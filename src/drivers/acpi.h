#ifndef FORTRESS_ACPI_H
#define FORTRESS_ACPI_H

#include "types.h"

#define MAX_ACPI_TABLE_SIZE (2 * 1024 * 1024) /* 2 MiB sanity limit */

/* RSDP Structure */
typedef struct {
    char     signature[8];       /* "RSD PTR " */
    uint8_t  checksum;           /* First 20 bytes checksum */
    char     oem_id[6];
    uint8_t  revision;           /* 0 = ACPI 1.0, 2 = ACPI 2.0+ */
    uint32_t rsdt_address;       /* 32-bit physical address of RSDT */

    /* Extended fields (ACPI 2.0+) */
    uint32_t length;             /* Total length of RSDP (36 bytes) */
    uint64_t xsdt_address;       /* 64-bit physical address of XSDT */
    uint8_t  extended_checksum;  /* Checksum of entire structure */
    uint8_t  reserved[3];
} __attribute__((packed)) acpi_rsdp_t;

/* Common ACPI SDT Header (36 bytes) */
typedef struct {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_header_t;

/* MADT Header (Signature "APIC") */
typedef struct {
    acpi_sdt_header_t header;
    uint32_t          lapic_address;  /* Default 32-bit physical address of LAPIC */
    uint32_t          flags;          /* Bit 0 = PCAT_COMPAT */
} __attribute__((packed)) acpi_madt_t;

/* MADT Entry Types */
#define MADT_TYPE_LOCAL_APIC          0
#define MADT_TYPE_IO_APIC             1
#define MADT_TYPE_INTERRUPT_OVERRIDE  2
#define MADT_TYPE_NMI                 4
#define MADT_TYPE_LAPIC_ADDR_OVERRIDE 5

typedef struct {
    uint8_t type;
    uint8_t length;
} __attribute__((packed)) acpi_madt_entry_t;

/* Type 0: Processor Local APIC */
typedef struct {
    acpi_madt_entry_t header;
    uint8_t           processor_id;
    uint8_t           apic_id;
    uint32_t          flags;          /* Bit 0: Enabled, Bit 1: Online Capable */
} __attribute__((packed)) acpi_madt_lapic_entry_t;

/* Type 1: I/O APIC */
typedef struct {
    acpi_madt_entry_t header;
    uint8_t           ioapic_id;
    uint8_t           reserved;
    uint32_t          ioapic_address; /* Physical address */
    uint32_t          gsi_base;       /* Global System Interrupt Base */
} __attribute__((packed)) acpi_madt_ioapic_entry_t;

/* Type 2: Interrupt Source Override */
typedef struct {
    acpi_madt_entry_t header;
    uint8_t           bus;            /* 0 = ISA */
    uint8_t           source_irq;
    uint32_t          gsi;
    uint16_t          flags;          /* Polarity & Trigger */
} __attribute__((packed)) acpi_madt_iso_entry_t;

/* Type 5: 64-bit Local APIC Address Override */
typedef struct {
    acpi_madt_entry_t header;
    uint16_t          reserved;
    uint64_t          lapic_address;  /* 64-bit physical address */
} __attribute__((packed)) acpi_madt_lapic_override_entry_t;

/* Extracted MADT Information Structure */
#define MAX_DETECTED_CPUS 64
#define MAX_DETECTED_IOAPICS 8
#define MAX_DETECTED_ISOS 16

typedef struct {
    uintptr_t lapic_phys_addr;
    bool      pcat_compat;
    size_t    enabled_cpu_count;
    uint8_t   enabled_cpu_apic_ids[MAX_DETECTED_CPUS];
    uint8_t   enabled_cpu_processor_ids[MAX_DETECTED_CPUS];
    size_t    online_capable_cpu_count;
    uint8_t   online_capable_cpu_apic_ids[MAX_DETECTED_CPUS];
    size_t    ioapic_count;
    struct {
        uint8_t   id;
        uintptr_t phys_addr;
        uint32_t  gsi_base;
    } ioapics[MAX_DETECTED_IOAPICS];
    size_t    iso_count;
    struct {
        uint8_t  bus;
        uint8_t  source_irq;
        uint32_t gsi;
        uint16_t flags;
    } isos[MAX_DETECTED_ISOS];
    bool      has_irq0_override;
    size_t    nmi_count;
    struct {
        uint8_t processor_id;
        uint8_t lint;
        uint16_t flags;
    } nmis[MAX_DETECTED_CPUS * 2];
    uint32_t  irq0_gsi;
} acpi_madt_info_t;

/* ACPI 2.0+ Generic Address Structure */
typedef struct {
    uint8_t address_space; /* 0 = System Memory (MMIO), 1 = System I/O */
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
} __attribute__((packed)) acpi_gas_t;

/* FADT Table (Signature "FACP") */
typedef struct {
    acpi_sdt_header_t header;       /* 0: "FACP" */
    uint32_t firmware_ctrl;        /* 36 */
    uint32_t dsdt;                 /* 40: 32-bit physical address of DSDT */
    uint8_t  reserved1;            /* 44 */
    uint8_t  preferred_pm_profile; /* 45 */
    uint16_t sci_int;              /* 46 */
    uint32_t smi_cmd;              /* 48: Port for SMI command */
    uint8_t  acpi_enable;          /* 52: Value to write to smi_cmd to enable ACPI */
    uint8_t  acpi_disable;         /* 53 */
    uint8_t  s4bios_req;           /* 54 */
    uint8_t  pstate_cnt;           /* 55 */
    uint32_t pm1a_evt_blk;         /* 56 */
    uint32_t pm1b_evt_blk;         /* 60 */
    uint32_t pm1a_cnt_blk;         /* 64: Port for PM1a Control Register */
    uint32_t pm1b_cnt_blk;         /* 68: Port for PM1b Control Register */
    uint32_t pm2_cnt_blk;          /* 72 */
    uint32_t pm_tmr_blk;           /* 76 */
    uint32_t gpe0_blk;             /* 80 */
    uint32_t gpe1_blk;             /* 84 */
    uint8_t  pm1_evt_len;          /* 88 */
    uint8_t  pm1_cnt_len;          /* 89 */
    uint8_t  pm2_cnt_len;          /* 90 */
    uint8_t  pm_tmr_len;           /* 91 */
    uint8_t  gpe0_blk_len;         /* 92 */
    uint8_t  gpe1_blk_len;         /* 93 */
    uint8_t  gpe1_base;            /* 94 */
    uint8_t  cst_cnt;              /* 95 */
    uint16_t p_lvl2_lat;           /* 96 */
    uint16_t p_lvl3_lat;           /* 98 */
    uint16_t flush_size;           /* 100 */
    uint16_t flush_stride;         /* 102 */
    uint8_t  duty_offset;          /* 104 */
    uint8_t  duty_width;           /* 105 */
    uint8_t  day_alrm;             /* 106 */
    uint8_t  mon_alrm;             /* 107 */
    uint8_t  century;              /* 108 */
    uint16_t iapc_boot_arch;       /* 109 */
    uint8_t  reserved2;            /* 111 */
    uint32_t flags;                /* 112 */
    acpi_gas_t reset_reg;          /* 116: Reset register (ACPI 2.0+) */
    uint8_t  reset_value;          /* 128: Value to write to reset_reg */
    uint16_t arm_boot_arch;        /* 129 */
    uint8_t  minor_version;        /* 131 */
    uint64_t x_firmware_ctrl;      /* 132 */
    uint64_t x_dsdt;               /* 140: 64-bit physical address of DSDT */
    acpi_gas_t x_pm1a_evt_blk;     /* 148 */
    acpi_gas_t x_pm1b_evt_blk;     /* 160 */
    acpi_gas_t x_pm1a_cnt_blk;     /* 172 */
    acpi_gas_t x_pm1b_cnt_blk;     /* 184 */
} __attribute__((packed)) acpi_fadt_t;

/* ACPI Public API */
bool acpi_ensure_mapped(uintptr_t phys_addr, size_t length);
bool acpi_init(uintptr_t rsdp_phys_addr, uintptr_t hhdm_offset);
uintptr_t acpi_get_hhdm_offset(void);
bool acpi_validate_checksum(const acpi_sdt_header_t *header);
acpi_sdt_header_t *acpi_find_table(const char *signature);
bool acpi_parse_madt(acpi_madt_info_t *out_info);
bool acpi_parse_madt_buffer(const void *buffer, size_t available, acpi_madt_info_t *out_info);

#endif /* FORTRESS_ACPI_H */
