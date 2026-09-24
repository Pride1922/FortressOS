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

park:
    for (;;) __asm__ volatile("cli; hlt");
}

/* Read-only debugger checkpoint after all AP-local probes. */
__attribute__((noinline)) void smp_percpu_ready(void) {
    __asm__ volatile("" : : : "memory");
}

size_t smp_get_cpu_count(void) { return g_total_cpu_count; }
uint32_t smp_get_bsp_lapic_id(void) { return g_bsp_lapic_id; }

size_t smp_init(const acpi_madt_info_t *madt_info) {
    if (g_initialized) return g_total_cpu_count - 1;
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
