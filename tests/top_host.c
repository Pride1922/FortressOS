#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#define TOP_HOST_TEST
#include "syscall_abi.h"
#include "terminal.h"

/* Forward declarations of top functions */
void top_compute_deltas(const sysinfo_t *prev_si, const sysinfo_t *curr_si,
                        const proc_info_t *prev_p, size_t prev_cnt,
                        const proc_info_t *curr_p, size_t curr_cnt,
                        int64_t *out_permille, bool has_prev);

void top_sort_processes(const proc_info_t *procs, const int64_t *permille,
                        size_t count, size_t *out_sorted);

size_t top_render_frame(char *out_buf, size_t max_out,
                        const sysinfo_t *si, const proc_info_t *procs,
                        const int64_t *permille, const size_t *sorted,
                        size_t count, uint32_t cols, uint32_t rows,
                        bool interactive);

int top_main(int argc, char **argv);

/* Mock syscall implementations for top_main testing */
static sysinfo_t s_mock_sysinfo;
static proc_info_t s_mock_procs[PROC_INFO_MAX];
static size_t s_mock_proc_count;
static terminal_info_t s_mock_term;
static int s_mock_isatty = 1;
static char s_mock_input_char = 'q';
static bool s_mock_write_fail = false;
static char s_captured_output[8192];
static size_t s_captured_len = 0;

long sys_write(int fd, const void *buf, size_t count) {
    (void)fd;
    if (s_mock_write_fail) {
        return -1;
    }
    if (s_captured_len + count < sizeof(s_captured_output)) {
        memcpy(s_captured_output + s_captured_len, buf, count);
        s_captured_len += count;
        s_captured_output[s_captured_len] = '\0';
    }
    return (long)count;
}

long sys_procinfo(uint64_t index, proc_info_t *buf) {
    if (index >= s_mock_proc_count) {
        return 0;
    }
    *buf = s_mock_procs[index];
    return 1;
}

long sys_sysinfo(sysinfo_t *buf) {
    *buf = s_mock_sysinfo;
    return 0;
}

long sys_termctl(uint64_t op, uintptr_t ptr, size_t size) {
    if (op == TERM_ISATTY) {
        return s_mock_isatty;
    }
    if (op == TERM_GET && size == sizeof(terminal_info_t)) {
        *(terminal_info_t *)ptr = s_mock_term;
        return 0;
    }
    return SYSCALL_EINVAL;
}

long sys_input_read(void *buf, size_t count, int64_t timeout_ms) {
    (void)timeout_ms;
    if (count > 0 && s_mock_input_char != '\0') {
        *(char *)buf = s_mock_input_char;
        return 1;
    }
    return 0;
}

/* Include user/top.c directly for testing */
#include "../user/top.c"

static void safe_copy_str(char *dst, const char *src, size_t max) {
    size_t i = 0;
    while (src[i] != '\0' && i + 1 < max) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void test_deltas_matching_and_churn(void) {
    sysinfo_t prev_si = { .uptime_ticks = 1000, .tick_hz = 100 };
    sysinfo_t curr_si = { .uptime_ticks = 1100, .tick_hz = 100 }; /* delta = 100 ticks = 1 sec */

    proc_info_t prev_p[4];
    memset(prev_p, 0, sizeof(prev_p));
    prev_p[0].pid = 1; prev_p[0].cpu_ticks = 100; safe_copy_str(prev_p[0].name, "shell", sizeof(prev_p[0].name));
    prev_p[1].pid = 2; prev_p[1].cpu_ticks = 200; safe_copy_str(prev_p[1].name, "busy", sizeof(prev_p[1].name));
    prev_p[2].pid = 3; prev_p[2].cpu_ticks = 300; safe_copy_str(prev_p[2].name, "idle", sizeof(prev_p[2].name));
    prev_p[3].pid = 4; prev_p[3].cpu_ticks = 400; safe_copy_str(prev_p[3].name, "dying", sizeof(prev_p[3].name));

    proc_info_t curr_p[4];
    memset(curr_p, 0, sizeof(curr_p));
    /* PID 1: used 10 ticks -> 10 / 100 = 10.0% = 100 permille */
    curr_p[0].pid = 1; curr_p[0].cpu_ticks = 110; safe_copy_str(curr_p[0].name, "shell", sizeof(curr_p[0].name));
    /* PID 2: used 50 ticks -> 50 / 100 = 50.0% = 500 permille */
    curr_p[1].pid = 2; curr_p[1].cpu_ticks = 250; safe_copy_str(curr_p[1].name, "busy", sizeof(curr_p[1].name));
    /* PID 3: used 0 ticks -> 0 / 100 = 0.0% = 0 permille */
    curr_p[2].pid = 3; curr_p[2].cpu_ticks = 300; safe_copy_str(curr_p[2].name, "idle", sizeof(curr_p[2].name));
    /* PID 5: brand new process (churn) -> should be -1 (unknown) */
    curr_p[3].pid = 5; curr_p[3].cpu_ticks = 50; safe_copy_str(curr_p[3].name, "spawned", sizeof(curr_p[3].name));

    int64_t permille[4];
    top_compute_deltas(&prev_si, &curr_si, prev_p, 4, curr_p, 4, permille, true);

    assert(permille[0] == 100); /* 10.0% */
    assert(permille[1] == 500); /* 50.0% */
    assert(permille[2] == 0);   /* 0.0% */
    assert(permille[3] == -1);  /* unknown */

    /* Test initial cycle: has_prev = false -> all unknown */
    top_compute_deltas(&prev_si, &curr_si, prev_p, 4, curr_p, 4, permille, false);
    for (int i = 0; i < 4; i++) {
        assert(permille[i] == -1);
    }

    puts("PASS top host: deltas matching, initial cycle, and PID churn");
}

static void test_invalid_intervals_and_decreasing_counter(void) {
    sysinfo_t prev_si = { .uptime_ticks = 1000, .tick_hz = 100 };
    proc_info_t prev_p[2];
    memset(prev_p, 0, sizeof(prev_p));
    prev_p[0].pid = 1; prev_p[0].cpu_ticks = 100;
    prev_p[1].pid = 2; prev_p[1].cpu_ticks = 200;

    proc_info_t curr_p[2];
    memset(curr_p, 0, sizeof(curr_p));
    curr_p[0].pid = 1; curr_p[0].cpu_ticks = 105;
    curr_p[1].pid = 2; curr_p[1].cpu_ticks = 190; /* Decreasing counter! */

    int64_t permille[2];

    /* 1. Decreasing counter -> unknown (-1) */
    sysinfo_t curr_si = { .uptime_ticks = 1100, .tick_hz = 100 };
    top_compute_deltas(&prev_si, &curr_si, prev_p, 2, curr_p, 2, permille, true);
    assert(permille[0] == 50); /* 5.0% */
    assert(permille[1] == -1); /* unknown because counter decreased */

    /* 2. Zero interval: curr_uptime == prev_uptime -> all unknown (-1) */
    curr_si.uptime_ticks = 1000;
    top_compute_deltas(&prev_si, &curr_si, prev_p, 2, curr_p, 2, permille, true);
    assert(permille[0] == -1);
    assert(permille[1] == -1);

    /* 3. Reversed interval: curr_uptime < prev_uptime -> all unknown (-1) */
    curr_si.uptime_ticks = 900;
    top_compute_deltas(&prev_si, &curr_si, prev_p, 2, curr_p, 2, permille, true);
    assert(permille[0] == -1);
    assert(permille[1] == -1);

    puts("PASS top host: decreasing counter, zero interval, and reversed interval");
}

static void test_overflow_bounds(void) {
    sysinfo_t prev_si = { .uptime_ticks = 0, .tick_hz = 100 };
    sysinfo_t curr_si = { .uptime_ticks = 100, .tick_hz = 100 };

    proc_info_t prev_p[1];
    memset(prev_p, 0, sizeof(prev_p));
    prev_p[0].pid = 1; prev_p[0].cpu_ticks = 0;

    proc_info_t curr_p[1];
    memset(curr_p, 0, sizeof(curr_p));
    /* Huge ticks delta that would overflow 1000 * delta if not guarded */
    curr_p[0].pid = 1; curr_p[0].cpu_ticks = UINT64_MAX / 500;

    int64_t permille[1];
    top_compute_deltas(&prev_si, &curr_si, prev_p, 1, curr_p, 1, permille, true);
    /* Should compute safely without wrap */
    assert(permille[0] > 0);

    puts("PASS top host: overflow bounds on arithmetic");
}

static void test_sorting_and_tie_breaking(void) {
    proc_info_t procs[5];
    memset(procs, 0, sizeof(procs));
    procs[0].pid = 10; /* 0.0% */
    procs[1].pid = 5;  /* 50.0% */
    procs[2].pid = 2;  /* 0.0% (same as PID 10 -> tie-break by PID asc) */
    procs[3].pid = 8;  /* unknown (-1) */
    procs[4].pid = 1;  /* unknown (-1) (tie-break by PID asc) */

    int64_t permille[5] = { 0, 500, 0, -1, -1 };
    size_t sorted[5];

    top_sort_processes(procs, permille, 5, sorted);

    /* Expected order:
     * 1st: PID 5 (500 permille) -> sorted index 1
     * 2nd: PID 2 (0 permille, smaller pid) -> sorted index 2
     * 3rd: PID 10 (0 permille, larger pid) -> sorted index 0
     * 4th: PID 1 (unknown -1, smaller pid) -> sorted index 4
     * 5th: PID 8 (unknown -1, larger pid) -> sorted index 3
     */
    assert(sorted[0] == 1); /* PID 5 */
    assert(sorted[1] == 2); /* PID 2 */
    assert(sorted[2] == 0); /* PID 10 */
    assert(sorted[3] == 4); /* PID 1 */
    assert(sorted[4] == 3); /* PID 8 */

    puts("PASS top host: sorting by CPU% desc and deterministic tie-breaking by PID asc");
}

static void test_formatting_and_dimensions(void) {
    sysinfo_t si = {
        .total_ram_bytes = 2048ULL * 1024 * 1024,
        .free_ram_bytes = 1980ULL * 1024 * 1024,
        .uptime_ticks = 25200, /* 00:04:12 */
        .tick_hz = 100,
        .cpu_count = 1,
        .task_count = 3
    };

    proc_info_t procs[3];
    memset(procs, 0, sizeof(procs));
    procs[0].pid = 1; procs[0].state = PROC_STATE_RUNNING; safe_copy_str(procs[0].name, "shell", sizeof(procs[0].name));
    procs[1].pid = 58; procs[1].state = PROC_STATE_RUNNING; safe_copy_str(procs[1].name, "hello", sizeof(procs[1].name));
    procs[2].pid = 12; procs[2].state = PROC_STATE_STOPPED; safe_copy_str(procs[2].name, "probe", sizeof(procs[2].name));

    int64_t permille[3] = { 3, 112, 0 }; /* 0.3%, 11.2%, 0.0% */
    size_t sorted[3] = { 1, 0, 2 }; /* PID 58, PID 1, PID 12 */

    char buf[2048];
    /* Interactive: includes \033[2J\033[H */
    size_t len = top_render_frame(buf, sizeof(buf), &si, procs, permille, sorted, 3, 80, 25, true);
    assert(len > 0);
    assert(strstr(buf, "\033[2J\033[H") != NULL);
    assert(strstr(buf, "FortressOS top — up 00:04:12") != NULL);
    assert(strstr(buf, "CPUs: 1   Tasks: 3") != NULL);
    assert(strstr(buf, "Mem:  2048 MiB total, 1980 free, 68 used") != NULL);
    assert(strstr(buf, "  PID  STATE       CPU%  NAME") != NULL);
    assert(strstr(buf, "   58  RUNNING     11.2  hello") != NULL);
    assert(strstr(buf, "    1  RUNNING      0.3  shell") != NULL);
    assert(strstr(buf, "   12  STOPPED      0.0  probe") != NULL);

    /* Plain / redirected: no \033[2J\033[H */
    len = top_render_frame(buf, sizeof(buf), &si, procs, permille, sorted, 3, 80, 25, false);
    assert(strstr(buf, "\033[2J") == NULL);
    assert(strstr(buf, "FortressOS top — up 00:04:12") != NULL);

    /* Row capping: rows = 5 -> 4 header lines + 1 process row */
    len = top_render_frame(buf, sizeof(buf), &si, procs, permille, sorted, 3, 80, 5, false);
    assert(strstr(buf, "hello") != NULL);
    assert(strstr(buf, "shell") == NULL); /* Capped */

    /* Tiny terminal: cols = 20, rows = 3 -> lines truncated at 20 chars */
    len = top_render_frame(buf, sizeof(buf), &si, procs, permille, sorted, 3, 20, 3, false);
    assert(len > 0);

    puts("PASS top host: frame rendering, escape codes, and row/column dimension capping");
}

static void test_main_interactive_and_redirected(void) {
    s_mock_sysinfo = (sysinfo_t){
        .total_ram_bytes = 2048ULL * 1024 * 1024,
        .free_ram_bytes = 1980ULL * 1024 * 1024,
        .uptime_ticks = 1000,
        .tick_hz = 100,
        .cpu_count = 1,
        .task_count = 1
    };
    s_mock_proc_count = 1;
    s_mock_procs[0].pid = 1;
    s_mock_procs[0].state = PROC_STATE_RUNNING;
    safe_copy_str(s_mock_procs[0].name, "shell", sizeof(s_mock_procs[0].name));

    /* 1. Redirected mode (isatty = 0): one-shot summary, no control sequences, exits 0 */
    s_mock_isatty = 0;
    s_captured_len = 0;
    int rc = top_main(0, NULL);
    assert(rc == 0);
    assert(strstr(s_captured_output, "\033[2J") == NULL);
    assert(strstr(s_captured_output, "FortressOS top — up") != NULL);
    assert(strstr(s_captured_output, "shell") != NULL);

    /* 2. Interactive mode (isatty = 1, key = 'q'): emits clear screen, exits 0 */
    s_mock_isatty = 1;
    s_mock_term.mode = TERM_MIRROR;
    s_mock_term.cols = 80;
    s_mock_term.rows = 25;
    s_mock_input_char = 'q';
    s_captured_len = 0;
    rc = top_main(0, NULL);
    assert(rc == 0);
    assert(strstr(s_captured_output, "\033[2J\033[H") != NULL);
    assert(strstr(s_captured_output, "FortressOS top — up") != NULL);

    /* 3. Write failure handling: returns 1 */
    s_mock_write_fail = true;
    rc = top_main(0, NULL);
    assert(rc == 1);
    s_mock_write_fail = false;

    puts("PASS top host: top_main interactive, redirected, and write failure handling");
}

int main(void) {
    test_deltas_matching_and_churn();
    test_invalid_intervals_and_decreasing_counter();
    test_overflow_bounds();
    test_sorting_and_tie_breaking();
    test_formatting_and_dimensions();
    test_main_interactive_and_redirected();
    puts("All top host tests PASS");
    return 0;
}
