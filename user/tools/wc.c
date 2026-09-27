#include "common.h"
typedef struct { wc_counts_t total; unsigned fields; bool total_overflow; } wc_context_t;
#ifdef TOOL_HOST_TEST
extern void tool_test_wc_seed(wc_counts_t *counts, bool total);
#endif

bool wc_add_counts(wc_counts_t *sum, const wc_counts_t *add) {
    if (add->lines > UINT64_MAX - sum->lines || add->words > UINT64_MAX - sum->words ||
        add->bytes > UINT64_MAX - sum->bytes) return false;
    sum->lines += add->lines; sum->words += add->words; sum->bytes += add->bytes;
    return true;
}
bool wc_count_chunk(wc_counts_t *counts, bool *in_word, const unsigned char *p, size_t n) {
    wc_counts_t delta = {.bytes = n};
    bool word = *in_word;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = p[i];
        bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
        if (c == '\n') delta.lines++;
        if (!space && !word) delta.words++;
        word = !space;
    }
    if (!wc_add_counts(counts, &delta)) return false;
    *in_word = word;
    return true;
}
static int row(const wc_counts_t *counts, unsigned fields, const char *label) {
    char text[64]; /* Three uint64 decimals, separators and terminator. */
    size_t used = 0;
    const uint64_t values[] = {counts->lines, counts->words, counts->bytes};
    for (unsigned i = 0; i < 3; i++) {
        if (!(fields & (1u << i))) continue;
        if (used) text[used++] = ' ';
        size_t start = used;
        uint64_t value = values[i];
        do { text[used++] = (char)('0' + value % 10); value /= 10; } while (value);
        for (size_t a = start, b = used - 1; a < b; a++, b--) {
            char temp = text[a]; text[a] = text[b]; text[b] = temp;
        }
    }
    if (label) text[used++] = ' ';
    int r = tool_write("wc", text, used);
    if (!r && label) r = tool_write("wc", label, tool_length(label));
    if (!r) r = tool_write("wc", "\n", 1);
    return r;
}
static int count_input(int fd, const char *label, void *ctx) {
    wc_context_t *state = ctx;
    wc_counts_t counts = {0};
#ifdef TOOL_HOST_TEST
    tool_test_wc_seed(&counts, false);
#endif
    bool in_word = false;
    for (;;) {
        long n = tool_syscall(SYS_READ, fd, (uintptr_t)tool_buffer, TOOL_BUFFER_SIZE);
        if (n < 0) return tool_error("wc", "read error", label);
        if (!n) break;
        if (!wc_count_chunk(&counts, &in_word, tool_buffer, (size_t)n))
            return tool_error("wc", "counter overflow", NULL);
    }
    if (!state->total_overflow && !wc_add_counts(&state->total, &counts)) {
        state->total_overflow = true;
        tool_error("wc", "counter overflow", NULL);
    }
    return row(&counts, state->fields, label);
}
int wc_main(int argc, char **argv) {
    tool_options_t opts;
    int r = tool_options(TOOL_WC, argc, argv, &opts);
    if (r) return r == 1 ? tool_help(TOOL_WC) : r;
    wc_context_t state = {.fields = opts.fields};
#ifdef TOOL_HOST_TEST
    tool_test_wc_seed(&state.total, true);
#endif
    int result = tool_inputs("wc", argc, argv, opts.first, count_input, &state);
    if (tool_output_failed) return result;
    if (state.total_overflow) return 1;
    if (argc - opts.first > 1) {
        r = row(&state.total, opts.fields, "total");
        if (r) return r;
    }
    return result;
}
