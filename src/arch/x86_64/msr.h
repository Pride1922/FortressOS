#ifndef FORTRESS_MSR_H
#define FORTRESS_MSR_H

#include "types.h"

/* x86_64 Model Specific Registers (MSRs) */
#define IA32_APIC_BASE_MSR 0x0000001B
#define IA32_EFER_MSR      0xC0000080
#define IA32_STAR_MSR      0xC0000081
#define IA32_LSTAR_MSR     0xC0000082
#define IA32_CSTAR_MSR     0xC0000083
#define IA32_SFMASK_MSR    0xC0000084

/* EFER Bit Flags */
#define EFER_SCE           (1ULL << 0)  /* System Call Extensions */
#define EFER_LME           (1ULL << 8)  /* Long Mode Enable */
#define EFER_LMA           (1ULL << 10) /* Long Mode Active */
#define EFER_NXE           (1ULL << 11) /* No-Execute Enable */

/* RFLAGS Bit Masks for SFMASK */
#define RFLAGS_CF          (1ULL << 0)
#define RFLAGS_PF          (1ULL << 2)
#define RFLAGS_AF          (1ULL << 4)
#define RFLAGS_ZF          (1ULL << 6)
#define RFLAGS_SF          (1ULL << 7)
#define RFLAGS_TF          (1ULL << 8)
#define RFLAGS_IF          (1ULL << 9)
#define RFLAGS_DF          (1ULL << 10)
#define RFLAGS_OF          (1ULL << 11)
#define RFLAGS_NT          (1ULL << 14)
#define RFLAGS_RF          (1ULL << 16)
#define RFLAGS_AC          (1ULL << 18)
#define RFLAGS_ID          (1ULL << 21)

/* Standard Syscall SFMASK: masks IF, TF, DF, and arithmetic/status flags */
#define SYSCALL_SFMASK_DEFAULT (RFLAGS_CF | RFLAGS_PF | RFLAGS_AF | RFLAGS_ZF | \
                                RFLAGS_SF | RFLAGS_TF | RFLAGS_IF | RFLAGS_DF | \
                                RFLAGS_OF | RFLAGS_NT | RFLAGS_RF | RFLAGS_AC | RFLAGS_ID)

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t low = (uint32_t)val;
    uint32_t high = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" : : "a"(low), "d"(high), "c"(msr) : "memory");
}

#endif /* FORTRESS_MSR_H */
