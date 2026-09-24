#include "smp.h"
#include "limine.h"
#include "serial.h"
#include "percpu.h"
#include "gdt.h"
#include "idt.h"
#include "vmm.h"
#include "msr.h"
#include "apic.h"
#include "spinlock.h"
#include "thread.h"
#include "pmm.h"

/* SMP_DESIGN.md SM2 note: the original draft assumed a hand-rolled
 * INIT-SIPI-SIPI trampoline in identity-mapped sub-1MiB memory. Limine
 * (already load-bearing for this kernel's boot) performs that exact
 * sequence itself as part of its documented SMP boot protocol before
 * this kernel's entry point is ever reached, and parks every AP waiting
 * on its own `goto_address` field. Reimplementing INIT-SIPI-SIPI by hand
 * here would duplicate a sequence Limine already owns and has to get
 * right for its own boot to work, for no benefit. This request is what
 * that reuse looks like in practice.
 */
__attribute__((used, section(".requests")))
static volatile struct limine_smp_request smp_request = {
    .id = LIMINE_SMP_REQUEST,
    .revision = 0,
    .response = NULL
};

/* No wall-clock source is safely readable this early (the APIC timer is
 * configured but not yet started -- see kmain). Bounded instruction-count
 * spin instead; calibrated generously and expected to be revisited once
 * real hardware/QEMU timing evidence exists (see the Piece 1 test doc). */
#define SMP_AP_REPORT_TIMEOUT_SPINS 200000000ULL

static acpi_madt_info_t ap_madt; /* BSP snapshot, immutable before release. */
static bool g_initialized;
static size_t          g_total_cpu_count = 1;
static uint32_t        g_bsp_lapic_id = 0;

volatile uint32_t g_smp_sched_active = 0;

/* Static lifetime, no AP allocator calls. All guards are removed by BSP
 * before release; APs never edit mappings or acquire subsystem locks. */
static struct {
    uint8_t guard[4096];
    uint8_t stack[16384];
} __attribute__((aligned(4096))) ap_stacks[MAX_DETECTED_CPUS];
extern void smp_stack_enter(uintptr_t cr3, uintptr_t top, size_t id);
extern void smp_fault_probe(uintptr_t guard_top, uintptr_t *saved_rsp,
                            uintptr_t *saved_rip);

void smp_ap_entry(struct limine_smp_info *info) {
    /* Copy handoff data while Limine mappings are still active. Assembly
     * changes CR3 and RSP together without touching the old stack again. */
    size_t id = info->extra_argument;
    smp_stack_enter(vmm_get_kernel_pml4(),
                    (uintptr_t)ap_stacks[id].stack + 16384, id);
    __builtin_unreachable();
}

void smp_ap_local_entry(size_t id) {
    /* GS is installed before the shared kernel IDT becomes active. */
    gdt_init_cpu(id);
    gdt_set_tss_rsp0((uintptr_t)ap_stacks[id].stack + 16384);
    idt_load_cpu();
    cpu_local_t *cpu = cpu_current();
    uintptr_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    if (cpu != &cpu_locals[id] || rdmsr(0xC0000101) != (uintptr_t)cpu ||
        !gdt_cpu_is_local() || vmm_get_current_pml4() != vmm_get_kernel_pml4() ||
        !vmm_boot_memory_ready() || !pmm_high_memory_enabled() ||
        rsp < (uintptr_t)ap_stacks[id].stack || rsp >= cpu->rsp0 ||
        cpu->current_thread || cpu->preempt_count || cpu->irq_depth) goto park;
    cpu->probe = 2;
    __asm__ volatile("int $2" : : : "memory");
    if (cpu->probe != 0) goto park;
    cpu->probe = 8;
    smp_fault_probe((uintptr_t)ap_stacks[id].stack,
                    &cpu->recovery_rsp, &cpu->recovery_rip);
    if (cpu->probe != 0) goto park;
    if (!lapic_init_ap(&ap_madt)) goto park;
    __atomic_store_n(&cpu->online, 1, __ATOMIC_RELEASE);

    /* Piece 3: AP 1 test dispatch protocol (SM11c). APs >= 2 always park. */
    if (id == 1) {
        uint32_t mode = __atomic_load_n(&g_smp_lock_test.test_mode, __ATOMIC_ACQUIRE);
        if (mode != SMP_TEST_MODE_NONE) {
            __atomic_store_n(&g_smp_lock_test.ap_ready, 1, __ATOMIC_RELEASE);

            /* Wait for BSP signal to start test */
            bool started = false;
            for (uint64_t s = 0; s < SMP_AP_REPORT_TIMEOUT_SPINS; s++) {
                if (__atomic_load_n(&g_smp_lock_test.bsp_start, __ATOMIC_ACQUIRE)) {
                    started = true;
                    break;
                }
                __asm__ volatile("pause");
            }

            if (!started) {
                __atomic_store_n(&g_smp_lock_test.ap_ready, 2, __ATOMIC_RELEASE);
                goto park;
            }

            if (mode == SMP_TEST_MODE_CONTENTION) {
                /* Contention test: 100,000 increments on AP 1 */
                for (uint64_t i = 0; i < 100000; i++) {
                    uint64_t rflags = spin_lock_irqsave(&g_smp_lock_test.lock);
                    g_smp_lock_test.counter++;
                    spin_unlock_irqrestore(&g_smp_lock_test.lock, rflags);
                }
                __atomic_store_n(&g_smp_lock_test.ap_done, 1, __ATOMIC_RELEASE);
            } else if (mode == SMP_TEST_MODE_INVERSION) {
                /* Deliberate rank inversion on AP 1: acquire rank 2 then rank 1 */
                spinlock_t rank2 = SPINLOCK_RANKED(2, "ap1-rank2");
                spinlock_t rank1 = SPINLOCK_RANKED(1, "ap1-rank1");
                uint64_t f2 = spin_lock_irqsave(&rank2);
                uint64_t f1 = spin_lock_irqsave(&rank1); /* Will panic via spin_fatal() */
                spin_unlock_irqrestore(&rank1, f1);
                spin_unlock_irqrestore(&rank2, f2);
            }
        }
    }

    while (__atomic_load_n(&g_smp_sched_active, __ATOMIC_ACQUIRE) == 0) {
        __asm__ volatile("pause");
    }
    if (__atomic_load_n(&g_smp_sched_active, __ATOMIC_ACQUIRE) == 1) {
        sched_ap_start(id);
    }

park:
    for (;;) __asm__ volatile("cli; hlt");
}

void smp_start_schedulers(void) {
    sched_init_aps(g_total_cpu_count);
    __atomic_store_n(&g_smp_sched_active, 1, __ATOMIC_RELEASE);
}

/* Read-only debugger checkpoint after all AP-local probes. */
__attribute__((noinline)) void smp_percpu_ready(void) {
    __asm__ volatile("" : : : "memory");
}

size_t smp_get_cpu_count(void) { return g_total_cpu_count; }
uint32_t smp_get_bsp_lapic_id(void) { return g_bsp_lapic_id; }

smp_lock_test_mailbox_t g_smp_lock_test = {
    .test_mode = SMP_TEST_MODE_CONTENTION,
    .ap_ready = 0,
    .bsp_start = 0,
    .ap_done = 0,
    .counter = 0,
    .lock = SPINLOCK_RANKED(1, "smp-test-lock")
};

void smp_set_test_mode(uint32_t mode) {
    g_smp_lock_test.test_mode = mode;
}

bool smp_run_lock_tests(void) {
    uint32_t mode = g_smp_lock_test.test_mode;
    if (mode == SMP_TEST_MODE_ASSERT_HELD) {
        serial_puts("[TEST] SMP Piece 3: Running spin_debug_assert_held negative test on BSP...\n");
        spinlock_t unheld = SPINLOCK_RANKED(1, "unheld-lock");
        /* This must trigger fail() -> spin_fatal() and halt BSP! */
        spin_debug_assert_held(&unheld);
        serial_puts("       [FAIL] spin_debug_assert_held did not trap unheld lock!\n");
        return false;
    }

    if (g_total_cpu_count < 2) {
        serial_puts("       [ OK ] Single-CPU system: SMP lock contention test skipped.\n");
        return true;
    }

    if (mode == SMP_TEST_MODE_NONE) {
        serial_puts("       [ OK ] SMP lock test mode is NONE; AP 1 parked.\n");
        return true;
    }

    if (mode == SMP_TEST_MODE_CONTENTION) {
        serial_puts("[TEST] SMP Piece 3: Starting two-core lock contention test (BSP + AP 1)...\n");

        /* Wait for AP 1 to report ready */
        uint32_t ready = 0;
        for (uint64_t s = 0; s < SMP_AP_REPORT_TIMEOUT_SPINS; s++) {
            ready = __atomic_load_n(&g_smp_lock_test.ap_ready, __ATOMIC_ACQUIRE);
            if (ready != 0) break;
            __asm__ volatile("pause");
        }
        if (ready == 2) {
            serial_puts("       [FAIL] AP 1 handshake timed out waiting for BSP\n");
            return false;
        } else if (ready != 1) {
            serial_puts("       [FAIL] AP 1 not ready for lock contention test\n");
            return false;
        }

        /* Signal AP 1 and execute 100,000 increments on BSP concurrently */
        __atomic_store_n(&g_smp_lock_test.bsp_start, 1, __ATOMIC_RELEASE);
        for (uint64_t i = 0; i < 100000; i++) {
            uint64_t rflags = spin_lock_irqsave(&g_smp_lock_test.lock);
            g_smp_lock_test.counter++;
            spin_unlock_irqrestore(&g_smp_lock_test.lock, rflags);
        }

        /* Wait for AP 1 to complete its 100,000 increments */
        for (uint64_t s = 0; s < SMP_AP_REPORT_TIMEOUT_SPINS; s++) {
            if (__atomic_load_n(&g_smp_lock_test.ap_done, __ATOMIC_ACQUIRE)) break;
            __asm__ volatile("pause");
        }
        if (!__atomic_load_n(&g_smp_lock_test.ap_done, __ATOMIC_ACQUIRE)) {
            serial_puts("       [FAIL] AP 1 timed out during lock contention test\n");
            return false;
        }

        uint64_t total = g_smp_lock_test.counter;
        serial_puts("       Counter value: ");
        serial_print_dec(total);
        serial_puts(" (expected: 200000)\n");
        serial_puts("       Test lock contention count: ");
        serial_print_dec(g_smp_lock_test.lock.contention_count);
        serial_puts("\n");

        if (total != 200000) {
            serial_puts("       [FAIL] Lock contention count mismatch (missed updates)!\n");
            return false;
        }
        serial_puts("       [PASS] Two-core lock contention test passed (exact 200,000 updates, mutual exclusion verified)\n");
        return true;
    } else if (mode == SMP_TEST_MODE_INVERSION) {
        serial_puts("[TEST] SMP Piece 3: Triggering AP 1 rank inversion test...\n");

        uint32_t ready = 0;
        for (uint64_t s = 0; s < SMP_AP_REPORT_TIMEOUT_SPINS; s++) {
            ready = __atomic_load_n(&g_smp_lock_test.ap_ready, __ATOMIC_ACQUIRE);
            if (ready != 0) break;
            __asm__ volatile("pause");
        }
        if (ready == 2) {
            serial_puts("       [FAIL] AP 1 handshake timed out waiting for BSP\n");
            return false;
        } else if (ready != 1) {
            serial_puts("       [FAIL] AP 1 not ready for rank inversion test\n");
            return false;
        }

        /* Tell AP 1 to run the inverted acquisition */
        __atomic_store_n(&g_smp_lock_test.bsp_start, 1, __ATOMIC_RELEASE);

        /* Wait for AP 1 to panic and set cpu_locals[1].lock_panic */
        for (uint64_t s = 0; s < SMP_AP_REPORT_TIMEOUT_SPINS; s++) {
            if (__atomic_load_n(&cpu_locals[1].lock_panic, __ATOMIC_ACQUIRE)) break;
            __asm__ volatile("pause");
        }

        if (__atomic_load_n(&cpu_locals[1].lock_panic, __ATOMIC_ACQUIRE)) {
            for (volatile int d = 0; d < 50000; d++) __asm__ volatile("pause");
            serial_puts("       [PASS] AP 1 rank inversion caught and isolated via spin_panic_ap; BSP unharmed\n");
            return true;
        } else {
            serial_puts("       [FAIL] AP 1 did not record lock panic\n");
            return false;
        }
    }

    return true;
}

/* =========================================================================
 * SMP Piece 5: Cross-Core Coordination & IPIs (SMP_DESIGN.md, SM14-SM15)
 * ========================================================================= */

volatile uint64_t g_ipi_tlb_count[MAX_DETECTED_CPUS] = {0};
volatile uint64_t g_ipi_resched_count[MAX_DETECTED_CPUS] = {0};

typedef struct {
    spinlock_t lock;
    volatile uintptr_t virt_addr;
    volatile uintptr_t cr3;
    volatile uint32_t ack_mask;
} smp_tlb_shootdown_t;

static smp_tlb_shootdown_t g_smp_tlb_shootdown = {
    .lock = SPINLOCK_RANKED(3, "smp-tlb"),
    .virt_addr = 0,
    .cr3 = 0,
    .ack_mask = 0
};

static void smp_ipi_tlb_handler(interrupt_frame_t *frame) {
    (void)frame;
    cpu_local_t *cpu = cpu_current();
    size_t cid = cpu ? cpu->id : 0;
    if (cid < MAX_DETECTED_CPUS) {
        g_ipi_tlb_count[cid]++;
    }
    uintptr_t target_cr3 = g_smp_tlb_shootdown.cr3;
    uintptr_t target_va  = g_smp_tlb_shootdown.virt_addr;
    if (target_cr3 == 0 || target_cr3 == (vmm_get_current_pml4() & PTE_ADDR_MASK)) {
        if (target_va != 0) {
            __asm__ volatile("invlpg (%0)" : : "r"(target_va) : "memory");
        } else {
            __asm__ volatile("mov %0, %%cr3" : : "r"(vmm_get_current_pml4()) : "memory");
        }
    }
    __atomic_fetch_and(&g_smp_tlb_shootdown.ack_mask, ~(1U << cid), __ATOMIC_RELEASE);
}

static void smp_ipi_resched_handler(interrupt_frame_t *frame) {
    (void)frame;
    cpu_local_t *cpu = cpu_current();
    size_t cid = cpu ? cpu->id : 0;
    if (cid < MAX_DETECTED_CPUS) {
        g_ipi_resched_count[cid]++;
    }
}

static void smp_ipi_panic_handler(interrupt_frame_t *frame) {
    (void)frame;
    for (;;) {
        __asm__ volatile("cli; hlt" ::: "memory");
    }
}

void smp_ipi_init(void) {
    idt_register_hardware_handler(IPI_VECTOR_TLB, smp_ipi_tlb_handler);
    idt_register_hardware_handler(IPI_VECTOR_RESCHED, smp_ipi_resched_handler);
    idt_register_hardware_handler(IPI_VECTOR_PANIC, smp_ipi_panic_handler);
}

void smp_tlb_shootdown(uintptr_t virt_addr, uintptr_t cr3) {
    if (g_total_cpu_count <= 1) {
        if (virt_addr != 0) {
            __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
        } else {
            __asm__ volatile("mov %0, %%cr3" : : "r"(vmm_get_current_pml4()) : "memory");
        }
        return;
    }

    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0" : "=r"(rflags) : : "memory");
    bool irq_was_enabled = (rflags & (1ULL << 9)) != 0;

    for (;;) {
        __asm__ volatile("cli" ::: "memory");
        if (!__atomic_test_and_set(&g_smp_tlb_shootdown.lock.lock, __ATOMIC_ACQUIRE)) {
            spin_debug_acquire(&g_smp_tlb_shootdown.lock);
            g_smp_tlb_shootdown.lock.acquire_count++;
            break;
        }
        if (irq_was_enabled) {
            __asm__ volatile("sti; pause; cli" ::: "memory");
        } else {
            __asm__ volatile("pause" ::: "memory");
        }
    }

    uint32_t my_id = (uint32_t)cpu_current()->id;
    uint32_t target_mask = 0;
    for (size_t i = 0; i < g_total_cpu_count; i++) {
        if (i != my_id && __atomic_load_n(&cpu_locals[i].online, __ATOMIC_ACQUIRE)) {
            target_mask |= (1U << i);
        }
    }

    if (target_mask != 0) {
        g_smp_tlb_shootdown.virt_addr = virt_addr;
        g_smp_tlb_shootdown.cr3 = cr3;
        __atomic_store_n(&g_smp_tlb_shootdown.ack_mask, target_mask, __ATOMIC_RELEASE);

        lapic_send_ipi_all_excluding_self(IPI_VECTOR_TLB);

        if (virt_addr != 0) {
            __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
        } else {
            __asm__ volatile("mov %0, %%cr3" : : "r"(vmm_get_current_pml4()) : "memory");
        }

        uint64_t iters = 0;
        while (__atomic_load_n(&g_smp_tlb_shootdown.ack_mask, __ATOMIC_ACQUIRE) != 0) {
            __asm__ volatile("pause" ::: "memory");
            iters++;
            if (iters > 50000000ULL) {
                serial_raw_puts("[WARN] smp_tlb_shootdown: ACK timeout\n");
                break;
            }
        }
    } else {
        if (virt_addr != 0) {
            __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
        } else {
            __asm__ volatile("mov %0, %%cr3" : : "r"(vmm_get_current_pml4()) : "memory");
        }
    }

    spin_debug_release(&g_smp_tlb_shootdown.lock);
    __atomic_clear(&g_smp_tlb_shootdown.lock.lock, __ATOMIC_RELEASE);

    if (irq_was_enabled) {
        __asm__ volatile("sti" ::: "memory");
    }
}

void smp_send_resched(size_t cpu_id) {
    if (cpu_id >= MAX_DETECTED_CPUS || cpu_id == cpu_current()->id) return;
    if (!__atomic_load_n(&cpu_locals[cpu_id].online, __ATOMIC_ACQUIRE)) return;
    uint8_t lapic_id = (uint8_t)cpu_locals[cpu_id].lapic_id;
    lapic_send_ipi(lapic_id, IPI_VECTOR_RESCHED);
}

void smp_send_panic(void) {
    if (g_smp_lock_test.test_mode == SMP_TEST_MODE_INVERSION) return;
    lapic_send_ipi_all_excluding_self(IPI_VECTOR_PANIC);
}

size_t smp_init(const acpi_madt_info_t *madt_info) {
    if (g_initialized) return g_total_cpu_count - 1;
    if (!vmm_boot_memory_ready() || !pmm_high_memory_enabled() ||
        vmm_get_current_pml4() != vmm_get_kernel_pml4()) {
        serial_puts("[FAIL] SMP: AP release requires kernel CR3 and PMM readiness\n");
        for (;;) { __asm__ volatile("cli; hlt"); }
    }
    serial_puts("[SMP] Boot memory readiness verified before AP release\n");
    g_initialized = true;
    if (!smp_request.response || smp_request.response->cpu_count == 0) {
        serial_puts("[ OK ] No Limine SMP response; staying single-CPU\n");
        return 0;
    }

    struct limine_smp_response *resp = smp_request.response;
    g_bsp_lapic_id    = resp->bsp_lapic_id;
    g_total_cpu_count = 1;
    if (!madt_info || !resp->cpus || resp->cpu_count > MAX_DETECTED_CPUS ||
        resp->cpu_count != madt_info->enabled_cpu_count) {
        serial_puts("[FAIL] Invalid MADT/Limine CPU set size; staying single-CPU\n");
        return 0;
    }
    cpu_locals[0].lapic_id = g_bsp_lapic_id;
    cpu_locals[0].online = 1;

    /* SM1: ACPI MADT is the sole source of truth for who is expected to
     * exist. Cross-check every Limine-reported LAPIC ID against MADT's
     * enabled set; refuse to start anything MADT did not enumerate as
     * enabled, rather than trusting Limine's count on its own. */
    size_t matched = 0;
    for (uint64_t i = 0; i < resp->cpu_count; i++) {
        struct limine_smp_info *cpu = resp->cpus[i];
        if (!cpu || cpu->lapic_id > 255) {
            serial_puts("[FAIL] Missing or unsupported Limine CPU entry\n");
            return 0;
        }
        for (uint64_t k = 0; k < i; k++) {
            if (resp->cpus[k]->lapic_id == cpu->lapic_id) {
                serial_puts("[FAIL] Duplicate Limine LAPIC ID\n");
                return 0;
            }
        }
        for (size_t j = 0; j < madt_info->enabled_cpu_count; j++) {
            if (madt_info->enabled_cpu_apic_ids[j] == cpu->lapic_id) {
                matched++;
                break;
            }
        }
    }
    if (matched != resp->cpu_count) {
        serial_puts("[FAIL] Limine SMP response disagrees with ACPI MADT enabled CPU set (");
        serial_print_dec(matched);
        serial_puts(" of ");
        serial_print_dec(resp->cpu_count);
        serial_puts(" matched); staying single-CPU\n");
        g_total_cpu_count = 1;
        return 0;
    }
    serial_puts("[ OK ] Limine SMP response cross-checked against ACPI MADT: ");
    serial_print_dec(resp->cpu_count);
    serial_puts(" CPU(s) agree (BSP LAPIC ID ");
    serial_print_dec(g_bsp_lapic_id);
    serial_puts(")\n");

    bool bsp_found = false;
    for (size_t i = 0; i < resp->cpu_count; i++)
        if (resp->cpus[i]->lapic_id == g_bsp_lapic_id) bsp_found = true;
    if (!bsp_found) {
        serial_puts("[FAIL] BSP missing from Limine CPU set\n");
        return 0;
    }

    /* Prepare ALL mappings before the first AP release (no TLB shootdown
     * exists yet). The BSP is the only writer; storage remains pinned even
     * if an AP reports late after a timeout. */
    uint64_t *pml4 = vmm_get_kernel_pml4_virt();
    for (size_t id = 1; id < resp->cpu_count; id++) {
        if (vmm_unmap_page(pml4, (uintptr_t)ap_stacks[id].guard) != VMM_OK ||
            vmm_unmap_page(pml4, gdt_cpu_ist_guard(id, 1)) != VMM_OK ||
            vmm_unmap_page(pml4, gdt_cpu_ist_guard(id, 2)) != VMM_OK) {
            serial_puts("[FAIL] AP stack guard setup failed\n");
            return 0;
        }
    }
    /* Finish response parsing and handoff metadata before releasing any AP. */
    struct limine_smp_info *aps[MAX_DETECTED_CPUS];
    size_t ap_count = 0;
    for (size_t i = 0; i < resp->cpu_count; i++) {
        struct limine_smp_info *cpu = resp->cpus[i];
        if (cpu->lapic_id == g_bsp_lapic_id) continue;
        size_t id = ++ap_count;
        cpu_locals[id].lapic_id = cpu->lapic_id;
        cpu->extra_argument = id;
        cpu_locals[id].nmi_uart_available = cpu_locals[0].nmi_uart_available;
        aps[id - 1] = cpu;
    }
    ap_madt = *madt_info;
    smp_ipi_init();
    for (size_t id = 1; id <= ap_count; id++) {
        struct limine_smp_info *cpu = aps[id - 1];
        __atomic_store_n(&cpu->goto_address, smp_ap_entry, __ATOMIC_RELEASE);
        for (uint64_t spins = 0; spins < SMP_AP_REPORT_TIMEOUT_SPINS; spins++) {
            if (__atomic_load_n(&cpu_locals[id].online, __ATOMIC_ACQUIRE)) break;
            __asm__ volatile("pause");
        }
        if (!__atomic_load_n(&cpu_locals[id].online, __ATOMIC_ACQUIRE)) {
            serial_puts("[FAIL] AP local setup timed out; further releases stopped\n");
            g_total_cpu_count = id;
            return id - 1;
        }
        serial_puts("[ OK ] Per-CPU AP "); serial_print_dec(id);
        serial_puts(": GS/TSS/IST1 fault/IST2 probe passed\n");
    }
    g_total_cpu_count = 1 + ap_count;
    smp_percpu_ready();
    serial_puts("[ OK ] SMP Piece 2 per-CPU storage ready (APs parked)\n");

    if (ap_count == 0) {
        serial_puts("[ OK ] Single enabled CPU per MADT/Limine; no APs to start\n");
        return 0;
    }

    serial_puts("[ OK ] All ");
    serial_print_dec(ap_count);
    serial_puts(" application processor(s) online (parked, interrupts disabled)\n");
    return ap_count;
}
