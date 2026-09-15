#ifndef FORTRESS_IOAPIC_H
#define FORTRESS_IOAPIC_H

#include "types.h"
#include "acpi.h"

#define MAX_IOAPIC_CONTROLLERS 8
#define IOAPIC_VIRT_BASE       0xFFFFFFFFE0010000ULL

/* I/O APIC Direct Registers */
#define IOREGSEL               0x00
#define IOWIN                  0x10

/* I/O APIC Indirect Registers */
#define IOAPIC_REG_ID          0x00
#define IOAPIC_REG_VER         0x01
#define IOAPIC_REG_ARB         0x02
#define IOAPIC_REG_REDTBL_BASE 0x10

/* Redirection Table Entry Flags */
#define IOAPIC_REDTBL_MASK     (1 << 16)
#define IOAPIC_TRIGGER_LEVEL   (1 << 15)
#define IOAPIC_POLARITY_LOW    (1 << 13)

typedef struct {
    uint8_t   id;
    uintptr_t phys_base;
    uintptr_t virt_base;
    uint32_t  gsi_base;
    uint32_t  pin_count;
} ioapic_controller_t;

/* I/O APIC Public API: boot CPU only, interrupts disabled for all access. */
bool     ioapic_init(const acpi_madt_info_t *madt);
uint32_t ioapic_read(size_t controller_idx, uint8_t reg);
void     ioapic_write(size_t controller_idx, uint8_t reg, uint32_t val);
size_t   ioapic_get_controller_count(void);
uint32_t ioapic_get_pin_count(size_t controller_idx);
void     ioapic_mask_all(void);
bool     ioapic_route_gsi(uint32_t gsi, uint8_t vector, uint8_t dest_apic_id, bool level_triggered, bool active_low);

bool ioapic_route_isa(const acpi_madt_info_t *madt, uint8_t irq, uint8_t vector, uint8_t dest_apic_id);

#endif /* FORTRESS_IOAPIC_H */
