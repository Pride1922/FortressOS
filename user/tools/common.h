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
int tool_error(const char *tool, const char *message, const char *operand);
int tool_write(const char *tool, const void *data, size_t n);
/* 0: parsed, 1: --help, 2: usage error. */
int tool_options(enum tool_kind kind, int argc, char **argv, tool_options_t *out);
int tool_help(enum tool_kind kind);
int tool_inputs(const char *name, int argc, char **argv, int first,
                int (*consume)(int fd, const char *label, void *ctx), void *ctx);
/* Transactional updates permit overflow injection without enormous input. */
bool wc_count_chunk(wc_counts_t *counts, bool *in_word, const unsigned char *p, size_t n);
bool wc_add_counts(wc_counts_t *sum, const wc_counts_t *add);
int cat_main(int argc, char **argv);
int head_main(int argc, char **argv);
int tail_main(int argc, char **argv);
int wc_main(int argc, char **argv);
int tar_main(int argc, char **argv);
#endif
