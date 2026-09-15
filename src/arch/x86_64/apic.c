#include "apic.h"
#include "vmm.h"
#include "serial.h"

static volatile uint8_t *g_lapic_mmio = (volatile uint8_t *)LAPIC_VIRT_ADDR;
static volatile uint64_t g_spurious_count = 0;
static volatile uint64_t g_timer_ticks = 0;
static uint32_t g_target_hz;

static inline bool check_apic_cpuid(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    return (edx & (1 << 9)) != 0;
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr) : "memory");
    return ((uint64_t)high << 32) | low;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t low = (uint32_t)val;
    uint32_t high = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" : : "a"(low), "d"(high), "c"(msr) : "memory");
}

bool lapic_is_supported(void) {
    return check_apic_cpuid();
}

uint32_t lapic_read(uint32_t reg) {
    return *((volatile uint32_t *)(g_lapic_mmio + reg));
}

void lapic_write(uint32_t reg, uint32_t val) {
    *((volatile uint32_t *)(g_lapic_mmio + reg)) = val;
}

void lapic_eoi(void) {
    lapic_write(APIC_REG_EOI, 0);
}

/* Handler only updates state; single-owner EOI is dispatched by IDT dispatcher */
static void apic_spurious_handler(interrupt_frame_t *frame) {
    (void)frame;
    g_spurious_count++;
    /* No serial I/O or EOI in spurious handler */
}

static void apic_timer_handler(interrupt_frame_t *frame) {
    (void)frame;
    g_timer_ticks++;
    /* No serial I/O or EOI in timer handler; IDT dispatcher owns EOI */
}

bool lapic_init(uintptr_t lapic_phys_addr) {
    if (!check_apic_cpuid()) {
        serial_puts("[FAIL] CPUID indicates APIC not supported\n");
        return false;
    }

    /* 1. Check if x2APIC is already active */
    uint64_t msr_val = rdmsr(IA32_APIC_BASE_MSR);
    if (msr_val & IA32_APIC_BASE_MSR_X2APIC) {
        serial_puts("[FAIL] x2APIC mode is active; MMIO access is unsupported. Revert to legacy xAPIC mode.\n");
        return false;
    }

    if ((lapic_phys_addr & 4095) ||
        (msr_val & 0x0000000FFFFFF000ULL) != lapic_phys_addr) {
        serial_puts("[FAIL] MADT and APIC MSR base mismatch\n");
        return false;
    }

    /* 2. Map LAPIC physical MMIO page to virtual address as uncacheable */
    uint64_t *pml4 = vmm_get_kernel_pml4_virt();
    int status = vmm_map_page(pml4, LAPIC_VIRT_ADDR, lapic_phys_addr,
                             PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT | PTE_NX);
    if (status != VMM_OK) {
        serial_puts("[FAIL] Failed to map LAPIC MMIO page into VMM\n");
        return false;
    }

    /* 3. Enable APIC via IA32_APIC_BASE MSR */
    if (!(msr_val & IA32_APIC_BASE_MSR_ENABLE)) {
        wrmsr(IA32_APIC_BASE_MSR, msr_val | IA32_APIC_BASE_MSR_ENABLE);
    }

    /* 4. Register Spurious Interrupt handler */
    idt_register_handler(APIC_SPURIOUS_VECTOR, apic_spurious_handler);

    /* 5. Configure Spurious Interrupt Vector Register (SVR) */
    lapic_write(APIC_REG_SVR, APIC_SVR_ENABLE | APIC_SPURIOUS_VECTOR);

    /* 6. Set Task Priority Register to 0 (accept all interrupt priority classes) */
    lapic_write(APIC_REG_TPR, 0);

    /* 7. Deliberately initialize and mask all unused LVT sources before STI */
    uint32_t max_lvt = (lapic_read(APIC_REG_VERSION) >> 16) & 255;
    if (max_lvt < 3) return false;
    lapic_write(APIC_REG_LVT_ERROR, APIC_LVT_MASKED);
    if (max_lvt >= 4) lapic_write(APIC_REG_LVT_PERF, APIC_LVT_MASKED);
    if (max_lvt >= 5) lapic_write(APIC_REG_LVT_THERMAL, APIC_LVT_MASKED);
    if (max_lvt >= 6) lapic_write(0x2F0, APIC_LVT_MASKED);
    lapic_write(APIC_REG_LVT_TIMER, APIC_LVT_MASKED);
    lapic_write(APIC_REG_TIMER_INITCNT, 0);
    lapic_write(APIC_REG_LVT_LINT0, APIC_LVT_MASKED);
    lapic_write(APIC_REG_LVT_LINT1, APIC_LVT_MASKED);

    /* 8. Clear Error Status Register */
    lapic_write(APIC_REG_ESR, 0);
    lapic_write(APIC_REG_ESR, 0);


    serial_puts("[ OK ] Local APIC initialized (MMIO mapped, xAPIC mode confirmed, LVT sources silenced, SVR=0x1FF, TPR=0x00)\n");
    return true;
}

/* PIT channel 2 is reserved for calibration/verification during early boot.
 * Preserve gate/speaker control; iteration limits only bound failure waits. */
static uint8_t pit_begin(uint16_t count) {
    uint8_t saved = inb(0x61);
    outb(0x61, saved & ~3u);
    outb(0x43, 0xB0);
    outb(0x42, (uint8_t)count);
    outb(0x42, (uint8_t)(count >> 8));
    return saved;
}
static bool pit_wait(void (*work)(void)) {
    for (uint32_t remaining = 10000000; remaining; --remaining) {
        if (inb(0x61) & 0x20) return true;
        if (work) work();
        __asm__ volatile("pause" ::: "memory");
    }
    return false;
}
bool apic_timer_init(uint32_t target_hz) {
    if (!target_hz || target_hz > 1000) return false;
    idt_register_hardware_handler(APIC_TIMER_VECTOR, apic_timer_handler);
    lapic_write(APIC_REG_TIMER_DIV, APIC_TIMER_DIV_16);
    lapic_write(APIC_REG_LVT_TIMER, APIC_LVT_MASKED | APIC_TIMER_VECTOR);
    uint8_t saved = pit_begin(11932);
    lapic_write(APIC_REG_TIMER_INITCNT, UINT32_MAX);
    outb(0x61, (saved & ~3u) | 1);
    bool completed = pit_wait(NULL);
    uint32_t elapsed = UINT32_MAX - lapic_read(APIC_REG_TIMER_CURRCNT);
    lapic_write(APIC_REG_TIMER_INITCNT, 0);
    outb(0x61, saved);
    if (!completed || elapsed < 1000 || elapsed > 100000000) {
        serial_puts("[FAIL] PIT calibration timeout or invalid count\n");
        return false;
    }
    uint64_t count = ((uint64_t)elapsed * 1193182) / ((uint64_t)11932 * target_hz);
    if (!count || count > UINT32_MAX) return false;
    g_target_hz = target_hz;
    g_timer_ticks = 0;
    lapic_write(APIC_REG_LVT_TIMER, APIC_LVT_MASKED | APIC_TIMER_PERIODIC | APIC_TIMER_VECTOR);
    lapic_write(APIC_REG_TIMER_INITCNT, (uint32_t)count);
    serial_puts("[ OK ] LAPIC calibrated against PIT; periodic timer remains masked\n");
    return true;
}
void apic_timer_start(void) {
    lapic_write(APIC_REG_LVT_TIMER, APIC_TIMER_PERIODIC | APIC_TIMER_VECTOR);
}
void apic_timer_stop(void) {
    lapic_write(APIC_REG_LVT_TIMER, APIC_LVT_MASKED | APIC_TIMER_VECTOR);
    lapic_write(APIC_REG_TIMER_INITCNT, 0);
}
bool apic_timer_verify(void (*work)(void)) {
    uint64_t before = g_timer_ticks;
    for (unsigned i = 0; i < 10; ++i) {
        uint8_t saved = pit_begin(59659); /* 50 ms hardware reference */
        outb(0x61, (saved & ~3u) | 1);
        bool ok = pit_wait(work);
        outb(0x61, saved);
        if (!ok) return false;
    }
    uint64_t delta = g_timer_ticks - before;
    serial_puts("[INFO] Timer ticks over ten PIT 50ms windows: ");
    serial_print_dec(delta);
    serial_puts("\n");
    /* Allow 30% for VM scheduling; this is a boot smoke test, not precision metrology. */
    return delta >= (uint64_t)g_target_hz * 35 / 100 &&
           delta <= (uint64_t)g_target_hz * 65 / 100;
}

uint64_t apic_timer_get_ticks(void) {
    return g_timer_ticks;
}

uint64_t lapic_get_spurious_count(void) {
    return g_spurious_count;
}
