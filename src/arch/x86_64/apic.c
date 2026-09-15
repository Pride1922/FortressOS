#include "apic.h"
#include "vmm.h"
#include "serial.h"

static volatile uint8_t *g_lapic_mmio = (volatile uint8_t *)LAPIC_VIRT_ADDR;
static volatile uint64_t g_spurious_count = 0;
static volatile uint64_t g_timer_ticks = 0;

static inline bool check_apic_cpuid(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    return (edx & (1 << 9)) != 0;
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t low = (uint32_t)val;
    uint32_t high = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" : : "a"(low), "d"(high), "c"(msr));
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

static void apic_spurious_handler(interrupt_frame_t *frame) {
    (void)frame;
    g_spurious_count++;
    /* Note: Intel SDM explicitly forbids sending EOI for spurious interrupts */
}

static void apic_timer_handler(interrupt_frame_t *frame) {
    (void)frame;
    g_timer_ticks++;
    lapic_eoi();
}

bool lapic_init(uintptr_t lapic_phys_addr) {
    if (!check_apic_cpuid()) {
        serial_puts("[FAIL] CPUID indicates APIC not supported\n");
        return false;
    }

    /* 1. Map LAPIC physical MMIO page to virtual address as uncacheable */
    uint64_t *pml4 = vmm_get_kernel_pml4_virt();
    int status = vmm_map_page(pml4, LAPIC_VIRT_ADDR, lapic_phys_addr,
                             PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT | PTE_NX);
    if (status != VMM_OK && status != VMM_ERR_ALREADY_MAPPED) {
        serial_puts("[FAIL] Failed to map LAPIC MMIO page into VMM\n");
        return false;
    }

    /* 2. Enable APIC via IA32_APIC_BASE MSR */
    uint64_t msr_val = rdmsr(IA32_APIC_BASE_MSR);
    if (!(msr_val & IA32_APIC_BASE_MSR_ENABLE)) {
        wrmsr(IA32_APIC_BASE_MSR, msr_val | IA32_APIC_BASE_MSR_ENABLE);
    }

    /* 3. Register Spurious Interrupt handler */
    idt_register_handler(APIC_SPURIOUS_VECTOR, apic_spurious_handler);

    /* 4. Configure Spurious Interrupt Vector Register (SVR) */
    lapic_write(APIC_REG_SVR, APIC_SVR_ENABLE | APIC_SPURIOUS_VECTOR);

    /* 5. Set Task Priority Register to 0 (accept all interrupt priority classes) */
    lapic_write(APIC_REG_TPR, 0);

    /* 6. Clear Error Status Register */
    lapic_write(APIC_REG_ESR, 0);
    lapic_write(APIC_REG_ESR, 0);

    /* 7. Initial EOI to clear any pre-existing in-service bit */
    lapic_eoi();

    serial_puts("[ OK ] Local APIC initialized (MMIO mapped, SVR=0x1FF, TPR=0x00)\n");
    return true;
}

void apic_timer_init(uint32_t target_hz) {
    if (target_hz == 0) {
        target_hz = 100;
    }

    /* 1. Register Timer IRQ Handler */
    idt_register_handler(APIC_TIMER_VECTOR, apic_timer_handler);

    /* 2. Set Divider to 16 */
    lapic_write(APIC_REG_TIMER_DIV, APIC_TIMER_DIV_16);

    /* 3. Mask timer during calibration */
    lapic_write(APIC_REG_LVT_TIMER, APIC_TIMER_MASKED);

    /* 4. Calibrate against PIT Channel 2 (10ms window) */
    uint16_t pit_count = 11932; /* 1193182 Hz / 100 = ~11932 ticks for 10ms */

    uint8_t port61 = inb(0x61);
    outb(0x61, (port61 & ~0x02) | 0x01); /* Enable gate, disable speaker output */

    /* Channel 2, LSB then MSB, Mode 0 (interrupt on terminal count), binary */
    outb(0x43, 0xB0);
    outb(0x42, (uint8_t)(pit_count & 0xFF));
    outb(0x42, (uint8_t)((pit_count >> 8) & 0xFF));

    /* Reset gate low then high to trigger countdown */
    port61 = inb(0x61);
    outb(0x61, port61 & ~0x01);
    outb(0x61, port61 | 0x01);

    /* Start LAPIC timer with max initial count */
    lapic_write(APIC_REG_TIMER_INITCNT, 0xFFFFFFFF);

    /* Poll until PIT Channel 2 OUT pin goes high (bit 5) */
    while (!(inb(0x61) & 0x20)) {
        __asm__ volatile("pause");
    }

    /* Read elapsed APIC ticks */
    uint32_t current_cnt = lapic_read(APIC_REG_TIMER_CURRCNT);
    lapic_write(APIC_REG_LVT_TIMER, APIC_TIMER_MASKED); /* Stop timer */
    uint32_t ticks_in_10ms = 0xFFFFFFFF - current_cnt;

    /* Restore port 0x61 */
    outb(0x61, port61 & ~0x03);

    serial_puts("[ OK ] APIC timer calibrated: ");
    serial_print_dec(ticks_in_10ms);
    serial_puts(" ticks per 10ms\n");

    /* Calculate count per tick for target_hz */
    uint32_t init_count = (uint32_t)(((uint64_t)ticks_in_10ms * 100) / target_hz);
    if (init_count == 0) {
        init_count = 10000;
    }

    /* 5. Start timer in periodic mode */
    lapic_write(APIC_REG_TIMER_DIV, APIC_TIMER_DIV_16);
    lapic_write(APIC_REG_LVT_TIMER, APIC_TIMER_PERIODIC | APIC_TIMER_VECTOR);
    lapic_write(APIC_REG_TIMER_INITCNT, init_count);

    serial_puts("[ OK ] APIC timer running (Periodic, Vector 0x20, Target ");
    serial_print_dec(target_hz);
    serial_puts(" Hz)\n");
}

uint64_t apic_timer_get_ticks(void) {
    return g_timer_ticks;
}

uint64_t lapic_get_spurious_count(void) {
    return g_spurious_count;
}
