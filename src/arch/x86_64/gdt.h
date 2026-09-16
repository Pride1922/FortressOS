#ifndef FORTRESS_GDT_H
#define FORTRESS_GDT_H

#include "types.h"

#define GDT_KERNEL_CODE  0x08
#define GDT_KERNEL_DATA  0x10
#define GDT_USER_DATA    0x1B /* (0x18 | 3) */
#define GDT_USER_CODE    0x23 /* (0x20 | 3) */
#define GDT_TSS          0x28

/* Standard 8-byte GDT Entry */
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  flags_limit_high;
    uint8_t  base_high;
} __attribute__((packed)) gdt_entry_t;

/* 64-bit Task State Segment (104 bytes) */
typedef struct {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed)) tss_t;

/* 16-byte TSS Descriptor for x86_64 GDT */
typedef struct {
    uint16_t length;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  flags1;
    uint8_t  flags2;
    uint8_t  base_high_mid;
    uint32_t base_high;
    uint32_t reserved;
} __attribute__((packed)) tss_descriptor_t;

/* GDT Pointer passed to lgdt */
typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdt_ptr_t;

void gdt_init(void);

uintptr_t gdt_get_ist1_guard(void);
uintptr_t gdt_get_ist1_stack_top(void);

void     gdt_set_tss_rsp0(uint64_t rsp0);
uint64_t gdt_get_tss_rsp0(void);
extern uint64_t g_tss_rsp0;

#endif /* FORTRESS_GDT_H */
