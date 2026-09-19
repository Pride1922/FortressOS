#ifndef FORTRESS_PCI_H
#define FORTRESS_PCI_H

#include "types.h"
#include "boot_info.h"

/* PCI Legacy Port I/O Addresses */
#define PCI_CONFIG_ADDRESS_PORT 0xCF8
#define PCI_CONFIG_DATA_PORT    0xCFC

/* Common PCI Configuration Registers */
#define PCI_REG_VENDOR_ID       0x00
#define PCI_REG_DEVICE_ID       0x02
#define PCI_REG_COMMAND         0x04
#define PCI_REG_STATUS          0x06
#define PCI_REG_REVISION_ID     0x08
#define PCI_REG_PROG_IF         0x09
#define PCI_REG_SUBCLASS        0x0A
#define PCI_REG_CLASS           0x0B
#define PCI_REG_CACHE_LINE_SIZE 0x0C
#define PCI_REG_LATENCY_TIMER   0x0D
#define PCI_REG_HEADER_TYPE     0x0E
#define PCI_REG_BIST            0x0F
#define PCI_REG_BAR0            0x10
#define PCI_REG_BAR1            0x14
#define PCI_REG_BAR2            0x18
#define PCI_REG_BAR3            0x1C
#define PCI_REG_BAR4            0x20
#define PCI_REG_BAR5            0x24
#define PCI_REG_INTERRUPT_LINE  0x3C
#define PCI_REG_INTERRUPT_PIN   0x3D

/* PCI Command Register Bits */
#define PCI_COMMAND_IO_SPACE     (1U << 0)
#define PCI_COMMAND_MEMORY_SPACE (1U << 1)
#define PCI_COMMAND_BUS_MASTER   (1U << 2)
#define PCI_COMMAND_INT_DISABLE  (1U << 10)

/* PCI Header Types */
#define PCI_HEADER_TYPE_NORMAL   0x00
#define PCI_HEADER_TYPE_BRIDGE   0x01
#define PCI_HEADER_TYPE_CARDBUS  0x02
#define PCI_HEADER_TYPE_MULTIFN  0x80

/* PCI Base Classes */
#define PCI_CLASS_UNCLASSIFIED   0x00
#define PCI_CLASS_STORAGE        0x01
#define PCI_CLASS_NETWORK        0x02
#define PCI_CLASS_DISPLAY        0x03
#define PCI_CLASS_MULTIMEDIA     0x04
#define PCI_CLASS_MEMORY         0x05
#define PCI_CLASS_BRIDGE         0x06
#define PCI_CLASS_COMMUNICATION  0x07
#define PCI_CLASS_SYSTEM_PERIPH  0x08
#define PCI_CLASS_INPUT_DEVICE   0x09
#define PCI_CLASS_DOCKING        0x0A
#define PCI_CLASS_PROCESSOR      0x0B
#define PCI_CLASS_SERIAL_BUS     0x0C
#define PCI_CLASS_WIRELESS       0x0D

/* PCI Storage Subclasses & Interfaces */
#define PCI_SUBCLASS_STORAGE_SCSI  0x00
#define PCI_SUBCLASS_STORAGE_IDE   0x01
#define PCI_SUBCLASS_STORAGE_FLOPPY 0x02
#define PCI_SUBCLASS_STORAGE_IPI   0x03
#define PCI_SUBCLASS_STORAGE_RAID  0x04
#define PCI_SUBCLASS_STORAGE_ATA   0x05
#define PCI_SUBCLASS_STORAGE_SATA  0x06
#define PCI_SUBCLASS_STORAGE_SAS   0x07
#define PCI_SUBCLASS_STORAGE_NVME  0x08
#define PCI_PROGIF_STORAGE_NVME    0x02

/* USB controller interfaces (serial-bus class). */
#define PCI_SUBCLASS_USB           0x03
#define PCI_PROGIF_USB_XHCI        0x30

/* PCI BAR Flags */
#define PCI_BAR_IO_SPACE         0x01
#define PCI_BAR_MEM_TYPE_MASK    0x06
#define PCI_BAR_MEM_TYPE_32      0x00
#define PCI_BAR_MEM_TYPE_64      0x04
#define PCI_BAR_MEM_PREFETCH     0x08
#define PCI_BAR_MEM_ADDR_MASK    (~0x0FULL)
#define PCI_BAR_IO_ADDR_MASK     (~0x03ULL)

/* Port I/O Helpers for 32-bit and 16-bit access */
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port) : "memory");
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port) : "memory");
    return ret;
}

/* ACPI MCFG Structure (PCI Express Memory Mapped Configuration Space) */
typedef struct {
    uint64_t base_address;       /* 64-bit base physical address of ECAM allocation */
    uint16_t pci_segment_group;  /* PCI Segment Group number */
    uint8_t  start_bus_number;   /* Start PCI bus number decoded by this host bridge */
    uint8_t  end_bus_number;     /* End PCI bus number decoded by this host bridge */
    uint32_t reserved;
} __attribute__((packed)) acpi_mcfg_allocation_t;

/* Discovered PCI Device Representation */
typedef struct {
    uint16_t segment;
    uint8_t  bus;
    uint8_t  device;
    uint8_t  function;

    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision_id;
    uint8_t  header_type;

    uint64_t bar[6];        /* Decoded physical base address */
    bool     bar_is_io[6];  /* true if I/O space, false if Memory space */
    bool     bar_is_64[6];  /* true if 64-bit Memory BAR */
    bool     bar_prefetch[6]; /* true if prefetchable memory */
} pci_device_t;

#define MAX_PCI_DEVICES 64
#define MAX_PCI_SEGMENTS 16

/* Public PCI Subsystem API */
void pci_init(uintptr_t hhdm_offset);

uint32_t pci_read_config32(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t fn, uint16_t offset);
uint16_t pci_read_config16(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t fn, uint16_t offset);
uint8_t  pci_read_config8(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t fn, uint16_t offset);

void     pci_write_config32(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t fn, uint16_t offset, uint32_t val);
void     pci_write_config16(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t fn, uint16_t offset, uint16_t val);

size_t   pci_scan_all(pci_device_t *out_devices, size_t max_devices);
bool     pci_find_device(uint8_t class_code, uint8_t subclass, uint8_t prog_if, pci_device_t *out_device);
size_t   pci_find_all_devices(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                              pci_device_t *out_array, size_t max_count);
void     pci_print_inventory(const pci_device_t *devices, size_t count);
void     pci_print_bdf(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t fn);

/* Phase 9G.1a: report up to four xHCI controllers after pci_init, in unlocked boot
 * context. Configuration reads only: no BAR sizing, MMIO, command writes,
 * firmware handoff, reset or USB enumeration. Absence is nonfatal. */
void     pci_report_xhci(void);

bool     pci_is_mcfg_available(void);
size_t   pci_get_segment_count(void);

#endif /* FORTRESS_PCI_H */
