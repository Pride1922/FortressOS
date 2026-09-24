#include "percpu.h"
#include "msr.h"
#include "serial.h"
cpu_local_t cpu_locals[MAX_DETECTED_CPUS];
volatile bool g_cpu_installed[MAX_DETECTED_CPUS];
void cpu_install(size_t id) {
    cpu_local_t *cpu = &cpu_locals[id];
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
