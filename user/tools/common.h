#ifndef STREAM_TOOLS_COMMON_H
#define STREAM_TOOLS_COMMON_H
#include "types.h"
#include "syscall_abi.h"
#include "vfs.h"

#define TOOL_BUFFER_SIZE 4096
#define TAIL_MAX_LINES 10
#define TAIL_LINE_BYTES 4096
#define TAIL_MAX_BYTES 65536
#define WC_LINES 1u
#define WC_WORDS 2u
#define WC_BYTES 4u

enum tool_kind { TOOL_CAT, TOOL_HEAD, TOOL_TAIL, TOOL_WC };
typedef struct {
    int first;
    uint64_t count;
    bool bytes;
    unsigned fields;
} tool_options_t;
typedef struct { uint64_t lines, words, bytes; } wc_counts_t;

extern unsigned char tool_buffer[TOOL_BUFFER_SIZE];
extern bool tool_output_failed;
long tool_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c);
size_t tool_length(const char *s);
bool tool_equal(const char *a, const char *b);
int tool_sys_error(const char *tool, const char *message, const char *operand, long error);
int tool_error(const char *tool, const char *message, const char *operand);
int tool_write(const char *tool, const void *data, size_t n);
/* 0: parsed, 1: --help, 2: usage error. */
int tool_options(enum tool_kind kind, int argc, char **argv, tool_options_t *out);
int tool_help(enum tool_kind kind);
int tool_inputs(const char *name, int argc, char **argv, int first,
                int (*consume)(int fd, const char *label, void *ctx), void *ctx);
static inline size_t tool_format_u64(char *buf, uint64_t val) {
    char tmp[32];
    size_t tpos = 0;
    do {
        tmp[tpos++] = (char)('0' + (val % 10));
        val /= 10;
    } while (val > 0);

    size_t out_len = tpos;
    for (size_t i = 0; i < out_len; i++) {
        buf[i] = tmp[tpos - 1 - i];
    }
    buf[out_len] = '\0';
    return out_len;
}

static inline size_t tool_format_i64(char *buf, int64_t val) {
    if (val < 0) {
        buf[0] = '-';
        uint64_t u = (uint64_t)(-(val + 1)) + 1;
        return 1 + tool_format_u64(buf + 1, u);
    }
    return tool_format_u64(buf, (uint64_t)val);
}

/* Transactional updates permit overflow injection without enormous input. */
bool wc_count_chunk(wc_counts_t *counts, bool *in_word, const unsigned char *p, size_t n);
bool wc_add_counts(wc_counts_t *sum, const wc_counts_t *add);
int cat_main(int argc, char **argv);
int head_main(int argc, char **argv);
int tail_main(int argc, char **argv);
int wc_main(int argc, char **argv);
int tar_main(int argc, char **argv);
int grep_main(int argc, char **argv);
int uniq_main(int argc, char **argv);
int xxd_main(int argc, char **argv);
int sort_main(int argc, char **argv);
int diff_main(int argc, char **argv);
int patch_main(int argc, char **argv);
int diskbench_main(int argc, char **argv);
int disk_main(int argc, char **argv);
int smpbench_main(int argc, char **argv);
int lockstat_main(int argc, char **argv);
static inline uint64_t tool_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)(hi) << 32) | lo;
}

#endif
