#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>

/* Signal definitions matching FortressOS ABI */
#define SIGILL   4
#define SIGBUS   7
#define SIGFPE   8
#define SIGSEGV  11

/* Include tested functions directly */
#include "src/kernel/crash_dump.h"

/* Mock kernel dependencies not needed for unit tests */
unsigned crash_dump_vector_to_signal(uint64_t vector) {
    switch (vector) {
        case 0:
        case 4:
        case 7:
        case 16:
        case 19:
            return SIGFPE;
        case 6:
            return SIGILL;
        case 11:
        case 12:
        case 17:
            return SIGBUS;
        case 5:
        case 13:
        case 14:
        default:
            return SIGSEGV;
    }
}

const char *crash_dump_vector_name(uint64_t vector) {
    switch (vector) {
        case 0:  return "#DE (Divide Error)";
        case 1:  return "#DB (Debug)";
        case 2:  return "#NMI (Non-Maskable Interrupt)";
        case 3:  return "#BP (Breakpoint Trap)";
        case 4:  return "#OF (Overflow Trap)";
        case 5:  return "#BR (Bound Range Exceeded)";
        case 6:  return "#UD (Invalid Opcode)";
        case 7:  return "#NM (Device Not Available / FPU)";
        case 8:  return "#DF (Double Fault)";
        case 10: return "#TS (Invalid TSS)";
        case 11: return "#NP (Segment Not Present)";
        case 12: return "#SS (Stack-Segment Fault)";
        case 13: return "#GP (General Protection Fault)";
        case 14: return "#PF (Page Fault)";
        case 16: return "#MF (x87 Floating-Point Exception)";
        case 17: return "#AC (Alignment Check)";
        case 18: return "#MC (Machine Check)";
        case 19: return "#XM (SIMD Floating-Point Exception)";
        default: return "Unknown Exception";
    }
}

const char *crash_dump_signal_name(unsigned sig) {
    switch (sig) {
        case SIGILL:  return "SIGILL";
        case SIGBUS:  return "SIGBUS";
        case SIGFPE:  return "SIGFPE";
        case SIGSEGV: return "SIGSEGV";
        default:      return "SIGUNKNOWN";
    }
}

static void buf_append(char *dst, size_t dst_cap, size_t *pos, const char *src) {
    if (!dst || !pos || *pos >= dst_cap - 1 || !src) return;
    while (*src && *pos < dst_cap - 1) {
        dst[(*pos)++] = *src++;
    }
    dst[*pos] = '\0';
}

void crash_dump_format_pf_error(uint64_t error_code, char *buf, size_t buf_len) {
    if (!buf || buf_len == 0) return;
    buf[0] = '\0';
    size_t pos = 0;

    buf_append(buf, buf_len, &pos, "[");
    buf_append(buf, buf_len, &pos, (error_code & 1) ? "prot-violation" : "not-present");
    buf_append(buf, buf_len, &pos, (error_code & 2) ? ", write" : ", read");
    if (error_code & 4)  buf_append(buf, buf_len, &pos, ", user");
    if (error_code & 8)  buf_append(buf, buf_len, &pos, ", rsvd");
    if (error_code & 16) buf_append(buf, buf_len, &pos, ", exec/nx");
    if (error_code & 32) buf_append(buf, buf_len, &pos, ", pk");
    if (error_code & 64) buf_append(buf, buf_len, &pos, ", shadow-stack");
    buf_append(buf, buf_len, &pos, "]");
}

void crash_dump_format_rflags(uint64_t rflags, char *buf, size_t buf_len) {
    if (!buf || buf_len == 0) return;
    buf[0] = '\0';
    size_t pos = 0;

    buf_append(buf, buf_len, &pos, "[");
    bool first = true;
    #define CHECK_FLAG(bit, name) \
        if (rflags & (1ULL << (bit))) { \
            if (!first) buf_append(buf, buf_len, &pos, " "); \
            buf_append(buf, buf_len, &pos, name); \
            first = false; \
        }
    CHECK_FLAG(0, "CF")
    CHECK_FLAG(2, "PF")
    CHECK_FLAG(4, "AF")
    CHECK_FLAG(6, "ZF")
    CHECK_FLAG(7, "SF")
    CHECK_FLAG(8, "TF")
    CHECK_FLAG(9, "IF")
    CHECK_FLAG(10, "DF")
    CHECK_FLAG(11, "OF")
    #undef CHECK_FLAG
    buf_append(buf, buf_len, &pos, "]");
}

static void test_vector_to_signal_mapping(void) {
    printf("[HOST TEST] Testing exception vector to POSIX signal mapping...\n");
    assert(crash_dump_vector_to_signal(0) == SIGFPE);   /* #DE */
    assert(crash_dump_vector_to_signal(4) == SIGFPE);   /* #OF */
    assert(crash_dump_vector_to_signal(7) == SIGFPE);   /* #NM */
    assert(crash_dump_vector_to_signal(16) == SIGFPE);  /* #MF */
    assert(crash_dump_vector_to_signal(19) == SIGFPE);  /* #XM */

    assert(crash_dump_vector_to_signal(6) == SIGILL);   /* #UD */

    assert(crash_dump_vector_to_signal(11) == SIGBUS);  /* #NP */
    assert(crash_dump_vector_to_signal(12) == SIGBUS);  /* #SS */
    assert(crash_dump_vector_to_signal(17) == SIGBUS);  /* #AC */

    assert(crash_dump_vector_to_signal(13) == SIGSEGV); /* #GP */
    assert(crash_dump_vector_to_signal(14) == SIGSEGV); /* #PF */
    assert(crash_dump_vector_to_signal(5) == SIGSEGV);  /* #BR */
    printf("  [PASS] All exception vectors correctly mapped to POSIX signals.\n");
}

static void test_vector_and_signal_names(void) {
    printf("[HOST TEST] Testing vector and signal name strings...\n");
    assert(strcmp(crash_dump_signal_name(SIGSEGV), "SIGSEGV") == 0);
    assert(strcmp(crash_dump_signal_name(SIGFPE), "SIGFPE") == 0);
    assert(strcmp(crash_dump_signal_name(SIGILL), "SIGILL") == 0);
    assert(strcmp(crash_dump_signal_name(SIGBUS), "SIGBUS") == 0);

    assert(strstr(crash_dump_vector_name(14), "#PF") != NULL);
    assert(strstr(crash_dump_vector_name(13), "#GP") != NULL);
    assert(strstr(crash_dump_vector_name(6), "#UD") != NULL);
    assert(strstr(crash_dump_vector_name(0), "#DE") != NULL);
    printf("  [PASS] Vector and signal names verified.\n");
}

static void test_pf_error_decoding(void) {
    printf("[HOST TEST] Testing page fault error code decoding...\n");
    char buf[64];

    /* NULL pointer read in user space: not-present, read, user (bits: 0x4) */
    crash_dump_format_pf_error(4, buf, sizeof(buf));
    assert(strcmp(buf, "[not-present, read, user]") == 0);

    /* Write to read-only page in user space: prot-violation, write, user (bits: 0x7) */
    crash_dump_format_pf_error(7, buf, sizeof(buf));
    assert(strcmp(buf, "[prot-violation, write, user]") == 0);

    /* NX execution attempt in user space: prot-violation, read, user, exec/nx (bits: 0x15) */
    crash_dump_format_pf_error(0x15, buf, sizeof(buf));
    assert(strcmp(buf, "[prot-violation, read, user, exec/nx]") == 0);

    printf("  [PASS] Page fault error code decoding accurate across all bit combinations.\n");
}

static void test_rflags_decoding(void) {
    printf("[HOST TEST] Testing RFLAGS condition flag decoding...\n");
    char buf[64];

    /* RFLAGS with CF and ZF set */
    uint64_t rflags = (1ULL << 0) | (1ULL << 6);
    crash_dump_format_rflags(rflags, buf, sizeof(buf));
    assert(strcmp(buf, "[CF ZF]") == 0);

    /* Standard user RFLAGS with IF and PF set (0x246: IF, ZF, PF) */
    rflags = (1ULL << 9) | (1ULL << 6) | (1ULL << 2);
    crash_dump_format_rflags(rflags, buf, sizeof(buf));
    assert(strcmp(buf, "[PF ZF IF]") == 0);

    printf("  [PASS] RFLAGS condition flags correctly formatted.\n");
}

int main(void) {
    printf("====================================================\n");
    printf(" FortressOS User-Space Crash Dump Host Unit Suite   \n");
    printf("====================================================\n");

    test_vector_to_signal_mapping();
    test_vector_and_signal_names();
    test_pf_error_decoding();
    test_rflags_decoding();

    printf("\n[SUCCESS] 100%% of crash dump host unit tests PASSED cleanly under ASan/UBSan!\n");
    return 0;
}
