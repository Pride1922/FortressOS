#include "percpu.h"
#include "msr.h"
#include "serial.h"
cpu_local_t cpu_locals[MAX_DETECTED_CPUS];
volatile bool g_cpu_installed[MAX_DETECTED_CPUS];

/* Limine's documented PAT layout (PA0 WB, PA1 WT, PA2 UC-, PA3 UC, PA4 WP,
 * PA5 WC) with PA1 changed from WT to WC and PA6/PA7 pinned to UC-/UC.
 * PTE_PWT alone then selects WC, so the framebuffer never needs the 4K PAT
 * bit, which aliases PTE_HUGE. Nothing maps with PWT alone (the LAPIC uses
 * PCD|PWT = PA3 UC), so no live translation changes type and no WBINVD is
 * needed; the CR3 reload drops any TLB entry cached under the old value.
 * Programmed identically on every CPU: mixed PATs are undefined behaviour.
 * WC in the PAT wins over any MTRR type, including firmware UC ranges. */
#define IA32_PAT 0x277
#define FORTRESS_PAT 0x0007010500070106ULL

static bool cpu_has_pat(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    (void)a; (void)b; (void)c;
    return (d & (1u << 16)) != 0;
}

static void pat_install(size_t id) {
    if (!cpu_has_pat()) {
        if (id == 0) serial_puts("[WARN] CPU has no PAT: framebuffer stays uncached\n");
        return;
    }
    if (rdmsr(IA32_PAT) != FORTRESS_PAT) {
        uint64_t cr3;
        wrmsr(IA32_PAT, FORTRESS_PAT);
        __asm__ volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");
    }
    if (id == 0 && !g_pat_wc) {
        g_pat_wc = true;
        serial_puts("[ OK ] PAT programmed: framebuffer write-combining (PA1=WC)\n");
    }
}

void cpu_install(size_t id) {
    cpu_local_t *cpu = &cpu_locals[id];
    pat_install(id);
    /* Users cannot set an upper-half GS base: no FSGSBASE or arch_prctl. */
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 &= ~(1ULL << 16);
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
    if (id == 0) cpu->nmi_uart_available = serial_is_available();
    cpu->self = cpu;
    cpu->id = id;
    wrmsr(0xC0000101, (uintptr_t)cpu); /* IA32_GS_BASE */
    wrmsr(0xC0000102, 0);             /* IA32_KERNEL_GS_BASE: user GS */
    g_cpu_installed[id] = true;
}
