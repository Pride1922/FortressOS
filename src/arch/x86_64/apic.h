#ifndef FORTRESS_APIC_H
#define FORTRESS_APIC_H

#include "types.h"
#include "acpi.h"
bool lapic_configure_nmi(const acpi_madt_info_t *info);
#include "idt.h"

/* Fixed Virtual Address for LAPIC MMIO Mapping */
#define LAPIC_VIRT_ADDR 0xFFFFFFFFE0000000ULL

/* LAPIC Register Offsets */
#define APIC_REG_ID             0x020
#define APIC_REG_VERSION        0x030
#define APIC_REG_TPR            0x080
#define APIC_REG_APR            0x090
#define APIC_REG_PPR            0x0A0
#define APIC_REG_EOI            0x0B0
#define APIC_REG_RRD            0x0C0
#define APIC_REG_LDR            0x0D0
#define APIC_REG_DFR            0x0E0
#define APIC_REG_SVR            0x0F0
#define APIC_REG_ESR            0x280
#define APIC_REG_ICR_LOW        0x300
#define APIC_REG_ICR_HIGH       0x310
#define APIC_REG_LVT_TIMER      0x320
#define APIC_REG_LVT_THERMAL    0x330
#define APIC_REG_LVT_PERF       0x340
#define APIC_REG_LVT_LINT0      0x350
#define APIC_REG_LVT_LINT1      0x360
#define APIC_REG_LVT_ERROR      0x370
#define APIC_REG_TIMER_INITCNT  0x380
#define APIC_REG_TIMER_CURRCNT  0x390
#define APIC_REG_TIMER_DIV      0x3E0

/* SVR Configuration */
#define APIC_SPURIOUS_VECTOR    0xFF
#define APIC_SVR_ENABLE         (1 << 8)

/* LVT Common Configuration */
#define APIC_LVT_MASKED         (1 << 16)

/* Timer Configuration */
#define APIC_TIMER_VECTOR       0x20  /* Vector 32 */
#define APIC_TIMER_PERIODIC     (1 << 17)
#define APIC_TIMER_DIV_16       0x03

/* MSR definitions */
#define IA32_APIC_BASE_MSR        0x1B
#define IA32_APIC_BASE_MSR_ENABLE (1ULL << 11)
#define IA32_APIC_BASE_MSR_X2APIC (1ULL << 10)

/* LAPIC Public API */
bool     lapic_is_supported(void);
bool     lapic_init(uintptr_t lapic_phys_addr);
uint32_t lapic_read(uint32_t reg);
void     lapic_write(uint32_t reg, uint32_t val);
void     lapic_eoi(void);

/* APIC Timer Public API */
bool     apic_timer_init(uint32_t target_hz);
void apic_timer_start(void);
void apic_timer_stop(void);
bool apic_timer_verify(void (*work)(void));
uint64_t apic_timer_get_ticks(void);
uint64_t lapic_get_spurious_count(void);

#endif /* FORTRESS_APIC_H */
