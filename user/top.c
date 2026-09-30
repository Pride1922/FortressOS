#ifndef TOP_HOST_TEST
#include "types.h"
#include "syscall_abi.h"
#include "terminal.h"
#else
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "syscall_abi.h"
#include "terminal.h"
#endif

/* Freestanding helper implementations */
#ifdef TOP_HOST_TEST
extern long sys_write(int fd, const void *buf, size_t count);
extern long sys_procinfo(uint64_t index, proc_info_t *buf);
extern long sys_sysinfo(sysinfo_t *buf);
extern long sys_termctl(uint64_t op, uintptr_t ptr, size_t size);
extern long sys_input_read(void *buf, size_t count, int64_t timeout_ms);
#else
static long sys_write(int fd, const void *buf, size_t count) {
    long nr = SYS_WRITE;
    __asm__ volatile("syscall" : "+a"(nr) : "D"((uintptr_t)fd), "S"((uintptr_t)buf), "d"((uintptr_t)count)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}

static long sys_procinfo(uint64_t index, proc_info_t *buf) {
    long nr = SYS_PROCINFO;
    __asm__ volatile("syscall" : "+a"(nr) : "D"(index), "S"((uintptr_t)buf)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}

static long sys_sysinfo(sysinfo_t *buf) {
    long nr = SYS_SYSINFO;
    __asm__ volatile("syscall" : "+a"(nr) : "D"((uintptr_t)buf)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}

static long sys_termctl(uint64_t op, uintptr_t ptr, size_t size) {
    long nr = SYS_TERMCTL;
    __asm__ volatile("syscall" : "+a"(nr) : "D"(op), "S"(ptr), "d"(size)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}

static long sys_input_read(void *buf, size_t count, int64_t timeout_ms) {
    long nr = SYS_INPUT_READ;
    __asm__ volatile("syscall" : "+a"(nr) : "D"((uintptr_t)buf), "S"((uintptr_t)count), "d"((uintptr_t)timeout_ms)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}
#endif

static int write_all(int fd, const char *buf, size_t count) {
    while (count > 0) {
        long w = sys_write(fd, buf, count);
        if (w <= 0) {
            return -1;
        }
        buf += w;
        count -= (size_t)w;
    }
    return 0;
}

static size_t str_len(const char *s, size_t max) {
    size_t len = 0;
    while (len < max && s[len] != '\0') {
        len++;
    }
    return len;
}

static size_t u64_digits(uint64_t val) {
    if (val == 0) return 1;
    size_t count = 0;
    while (val > 0) {
        count++;
        val /= 10;
    }
    return count;
}

static void buf_append_str(char *buf, size_t *pos, size_t max, const char *str) {
    size_t i = 0;
    while (str[i] != '\0' && *pos < max) {
        buf[(*pos)++] = str[i++];
    }
}

static void buf_append_u64_right(char *buf, size_t *pos, size_t max, uint64_t val, size_t width) {
    size_t digits = u64_digits(val);
    size_t pad = (width > digits) ? (width - digits) : 0;
    for (size_t i = 0; i < pad && *pos < max; i++) {
        buf[(*pos)++] = ' ';
    }
    size_t start = *pos;
    if (val == 0) {
        if (*pos < max) buf[(*pos)++] = '0';
    } else {
        uint64_t temp = val;
        while (temp > 0 && *pos < max) {
            buf[(*pos)++] = (char)('0' + (temp % 10));
            temp /= 10;
        }
        if (*pos <= max) {
            size_t end = *pos - 1;
            while (start < end) {
                char t = buf[start];
                buf[start] = buf[end];
                buf[end] = t;
                start++;
                end--;
            }
        }
    }
}

static void buf_append_u64(char *buf, size_t *pos, size_t max, uint64_t val) {
    buf_append_u64_right(buf, pos, max, val, 0);
}

static void buf_append_2digits(char *buf, size_t *pos, size_t max, uint64_t val) {
    if (*pos < max) buf[(*pos)++] = (char)('0' + ((val / 10) % 10));
    if (*pos < max) buf[(*pos)++] = (char)('0' + (val % 10));
}

static const char *state_to_str(uint32_t state) {
    switch (state) {
        case PROC_STATE_RUNNING: return "RUNNING";
        case PROC_STATE_STOPPED: return "STOPPED";
        case PROC_STATE_ZOMBIE:  return "ZOMBIE";
        case PROC_STATE_DONE:    return "DONE";
        default:                 return "UNKNOWN";
    }
}

static void format_cpu_pct(char *buf, size_t *pos, size_t max, int64_t permille) {
    if (permille < 0) {
        buf_append_str(buf, pos, max, "unknown");
    } else {
        uint64_t whole = (uint64_t)(permille / 10);
        uint64_t frac = (uint64_t)(permille % 10);
        size_t wdigits = u64_digits(whole);
        size_t total_len = wdigits + 2; /* digits + '.' + frac */
        size_t pad = (total_len < 7) ? (7 - total_len) : 0;
        for (size_t i = 0; i < pad && *pos < max; i++) {
            buf[(*pos)++] = ' ';
        }
        buf_append_u64(buf, pos, max, whole);
        if (*pos < max) buf[(*pos)++] = '.';
        if (*pos < max) buf[(*pos)++] = (char)('0' + (frac % 10));
    }
}

/* Core delta calculation logic */
void top_compute_deltas(const sysinfo_t *prev_si, const sysinfo_t *curr_si,
                        const proc_info_t *prev_p, size_t prev_cnt,
                        const proc_info_t *curr_p, size_t curr_cnt,
                        int64_t *out_permille, bool has_prev) {
    bool interval_valid = false;
    uint64_t delta_uptime = 0;

    if (has_prev && prev_si && curr_si) {
        if (curr_si->uptime_ticks > prev_si->uptime_ticks) {
            delta_uptime = curr_si->uptime_ticks - prev_si->uptime_ticks;
            interval_valid = (delta_uptime > 0);
        }
    }

    for (size_t i = 0; i < curr_cnt; i++) {
        out_permille[i] = -1; /* default to unknown */

        if (!interval_valid) {
            continue;
        }

        int64_t pid = curr_p[i].pid;
        int prev_idx = -1;
        for (size_t j = 0; j < prev_cnt; j++) {
            if (prev_p[j].pid == pid) {
                prev_idx = (int)j;
                break;
            }
        }

        if (prev_idx < 0) {
            /* First sight of PID or churn -> unknown */
            continue;
        }

        uint64_t curr_ticks = curr_p[i].cpu_ticks;
        uint64_t prev_ticks = prev_p[prev_idx].cpu_ticks;

        if (curr_ticks < prev_ticks) {
            /* Decreasing counter -> unknown */
            continue;
        }

        uint64_t delta_ticks = curr_ticks - prev_ticks;
        uint64_t permille;
        if (delta_ticks > UINT64_MAX / 1000) {
            permille = (delta_ticks / delta_uptime) * 1000;
        } else {
            permille = (delta_ticks * 1000) / delta_uptime;
        }

        out_permille[i] = (int64_t)permille;
    }
}

/* Stable sort: primary CPU% permille descending; break ties deterministically by PID ascending */
void top_sort_processes(const proc_info_t *procs, const int64_t *permille,
                        size_t count, size_t *out_sorted) {
    for (size_t i = 0; i < count; i++) {
        out_sorted[i] = i;
    }

    for (size_t i = 0; i < count; i++) {
        for (size_t j = i + 1; j < count; j++) {
            size_t a = out_sorted[i];
            size_t b = out_sorted[j];
            int64_t pa = permille[a];
            int64_t pb = permille[b];
            bool swap = false;

            if (pb > pa) {
                swap = true;
            } else if (pa == pb) {
                /* Equal percentage: lower PID comes first */
                if (procs[b].pid < procs[a].pid) {
                    swap = true;
                }
            }

            if (swap) {
                out_sorted[i] = b;
                out_sorted[j] = a;
            }
        }
    }
}

static char s_line_buf[256];

/* Render a single top display frame into out_buf */
size_t top_render_frame(char *out_buf, size_t max_out,
                        const sysinfo_t *si, const proc_info_t *procs,
                        const int64_t *permille, const size_t *sorted,
                        size_t count, uint32_t cols, uint32_t rows,
                        bool interactive) {
    size_t pos = 0;
    uint32_t max_cols = cols ? cols : 80;
    uint32_t max_rows = rows ? rows : 25;
    uint32_t lines_printed = 0;

    if (interactive) {
        /* Clear screen and home cursor */
        buf_append_str(out_buf, &pos, max_out, "\033[2J\033[H");
    }

    char *line = s_line_buf;

    /* Line 1: Header - FortressOS top — up HH:MM:SS */
    if (lines_printed < max_rows) {
        size_t lpos = 0;
        buf_append_str(line, &lpos, sizeof(s_line_buf), "FortressOS top — up ");
        uint64_t hz = si->tick_hz ? si->tick_hz : 100;
        uint64_t total_sec = si->uptime_ticks / hz;
        uint64_t hours = total_sec / 3600;
        uint64_t mins = (total_sec % 3600) / 60;
        uint64_t secs = total_sec % 60;
        if (hours < 100) {
            buf_append_2digits(line, &lpos, sizeof(s_line_buf), hours);
        } else {
            buf_append_u64(line, &lpos, sizeof(s_line_buf), hours);
        }
        if (lpos < sizeof(s_line_buf)) line[lpos++] = ':';
        buf_append_2digits(line, &lpos, sizeof(s_line_buf), mins);
        if (lpos < sizeof(s_line_buf)) line[lpos++] = ':';
        buf_append_2digits(line, &lpos, sizeof(s_line_buf), secs);

        size_t llen = (lpos > max_cols) ? max_cols : lpos;
        for (size_t i = 0; i < llen && pos < max_out; i++) out_buf[pos++] = line[i];
        if (pos < max_out) out_buf[pos++] = '\n';
        lines_printed++;
    }

    /* Line 2: CPUs and Tasks */
    if (lines_printed < max_rows) {
        size_t lpos = 0;
        buf_append_str(line, &lpos, sizeof(s_line_buf), "CPUs: ");
        buf_append_u64(line, &lpos, sizeof(s_line_buf), si->cpu_count);
        buf_append_str(line, &lpos, sizeof(s_line_buf), "   Tasks: ");
        buf_append_u64(line, &lpos, sizeof(s_line_buf), si->task_count);

        size_t llen = (lpos > max_cols) ? max_cols : lpos;
        for (size_t i = 0; i < llen && pos < max_out; i++) out_buf[pos++] = line[i];
        if (pos < max_out) out_buf[pos++] = '\n';
        lines_printed++;
    }

    /* Line 3: Memory */
    if (lines_printed < max_rows) {
        size_t lpos = 0;
        uint64_t total_mib = si->total_ram_bytes / (1024 * 1024);
        uint64_t free_mib = si->free_ram_bytes / (1024 * 1024);
        uint64_t used_mib = (si->total_ram_bytes >= si->free_ram_bytes) ?
                            ((si->total_ram_bytes - si->free_ram_bytes) / (1024 * 1024)) : 0;
        buf_append_str(line, &lpos, sizeof(s_line_buf), "Mem:  ");
        buf_append_u64(line, &lpos, sizeof(s_line_buf), total_mib);
        buf_append_str(line, &lpos, sizeof(s_line_buf), " MiB total, ");
        buf_append_u64(line, &lpos, sizeof(s_line_buf), free_mib);
        buf_append_str(line, &lpos, sizeof(s_line_buf), " free, ");
        buf_append_u64(line, &lpos, sizeof(s_line_buf), used_mib);
        buf_append_str(line, &lpos, sizeof(s_line_buf), " used");

        size_t llen = (lpos > max_cols) ? max_cols : lpos;
        for (size_t i = 0; i < llen && pos < max_out; i++) out_buf[pos++] = line[i];
        if (pos < max_out) out_buf[pos++] = '\n';
        lines_printed++;
    }

    /* Line 4: Column headers */
    if (lines_printed < max_rows) {
        size_t lpos = 0;
        buf_append_str(line, &lpos, sizeof(s_line_buf), "  PID  STATE       CPU%  NAME");

        size_t llen = (lpos > max_cols) ? max_cols : lpos;
        for (size_t i = 0; i < llen && pos < max_out; i++) out_buf[pos++] = line[i];
        if (pos < max_out) out_buf[pos++] = '\n';
        lines_printed++;
    }

    /* Process rows */
    for (size_t r = 0; r < count && lines_printed < max_rows; r++) {
        size_t idx = sorted[r];
        size_t lpos = 0;

        /* PID (5 right-aligned) */
        buf_append_u64_right(line, &lpos, sizeof(s_line_buf), (uint64_t)procs[idx].pid, 5);
        buf_append_str(line, &lpos, sizeof(s_line_buf), "  ");

        /* STATE (7 left-aligned) */
        const char *st = state_to_str(procs[idx].state);
        size_t slen = str_len(st, 7);
        for (size_t i = 0; i < slen && lpos < sizeof(s_line_buf); i++) line[lpos++] = st[i];
        for (size_t i = slen; i < 7 && lpos < sizeof(s_line_buf); i++) line[lpos++] = ' ';
        buf_append_str(line, &lpos, sizeof(s_line_buf), "  ");

        /* CPU% (7 right-aligned or "unknown") */
        format_cpu_pct(line, &lpos, sizeof(s_line_buf), permille[idx]);
        buf_append_str(line, &lpos, sizeof(s_line_buf), "  ");

        /* NAME */
        size_t nlen = str_len(procs[idx].name, 16);
        for (size_t i = 0; i < nlen && lpos < sizeof(s_line_buf); i++) line[lpos++] = procs[idx].name[i];

        size_t llen = (lpos > max_cols) ? max_cols : lpos;
        for (size_t i = 0; i < llen && pos < max_out; i++) out_buf[pos++] = line[i];
        if (pos < max_out) out_buf[pos++] = '\n';
        lines_printed++;
    }

    if (pos < max_out) {
        out_buf[pos] = '\0';
    } else {
        out_buf[max_out - 1] = '\0';
    }

    return pos;
}

/* Static BSS storage respecting the 512-byte Ring 3 stack budget */
static sysinfo_t s_prev_sysinfo;
static sysinfo_t s_curr_sysinfo;
static proc_info_t s_prev_procs[PROC_INFO_MAX];
static proc_info_t s_curr_procs[PROC_INFO_MAX];
static size_t s_prev_count;
static size_t s_curr_count;
static bool s_has_prev;

static int64_t s_cpu_permille[PROC_INFO_MAX];
static size_t s_sorted_indices[PROC_INFO_MAX];

static char s_screen_buf[4096];
static char s_in_buf[32];
static terminal_info_t s_term;

int top_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    long term_res = sys_termctl(TERM_GET, (uintptr_t)&s_term, sizeof(s_term));
    long isatty_res = sys_termctl(TERM_ISATTY, 1, 0);
    bool interactive = (term_res == 0 && isatty_res == 1 && s_term.mode != TERM_PLAIN);

    s_has_prev = false;
    s_prev_count = 0;
    s_curr_count = 0;

    for (;;) {
        /* Update terminal dimensions in case of change */
        if (interactive) {
            long r = sys_termctl(TERM_GET, (uintptr_t)&s_term, sizeof(s_term));
            if (r != 0 || s_term.mode == TERM_PLAIN) {
                interactive = false;
            }
        }

        /* 1. SYS_SYSINFO */
        long ret = sys_sysinfo(&s_curr_sysinfo);
        if (ret < 0) {
            static const char err_msg[] = "top: error reading system info\n";
            write_all(2, err_msg, sizeof(err_msg) - 1);
            return 1;
        }

        /* 2. Enumerate SYS_PROCINFO into s_curr_procs (deduplicate within pass) */
        s_curr_count = 0;
        for (uint64_t idx = 0; idx < PROC_INFO_MAX; idx++) {
            proc_info_t info;
            long pret = sys_procinfo(idx, &info);
            if (pret == 0) break;
            if (pret < 0) break;
            if (info.pid <= 0) continue;

            int dup_idx = -1;
            for (size_t k = 0; k < s_curr_count; k++) {
                if (s_curr_procs[k].pid == info.pid) {
                    dup_idx = (int)k;
                    break;
                }
            }
            if (dup_idx >= 0) {
                s_curr_procs[dup_idx] = info;
            } else if (s_curr_count < PROC_INFO_MAX) {
                s_curr_procs[s_curr_count++] = info;
            }
        }

        /* 3. Compute CPU% deltas */
        top_compute_deltas(&s_prev_sysinfo, &s_curr_sysinfo,
                           s_prev_procs, s_prev_count,
                           s_curr_procs, s_curr_count,
                           s_cpu_permille, s_has_prev);

        /* 4. Sort processes: CPU% desc, tie break by PID asc */
        top_sort_processes(s_curr_procs, s_cpu_permille, s_curr_count, s_sorted_indices);

        /* 5. Render frame */
        uint32_t cols = s_term.cols ? s_term.cols : 80;
        uint32_t rows = s_term.rows ? s_term.rows : 25;
        size_t bytes = top_render_frame(s_screen_buf, sizeof(s_screen_buf),
                                        &s_curr_sysinfo, s_curr_procs,
                                        s_cpu_permille, s_sorted_indices,
                                        s_curr_count, cols, rows, interactive);

        if (write_all(1, s_screen_buf, bytes) != 0) {
            return 1;
        }

        /* Advance snapshots */
        s_prev_sysinfo = s_curr_sysinfo;
        for (size_t k = 0; k < s_curr_count; k++) {
            s_prev_procs[k] = s_curr_procs[k];
        }
        s_prev_count = s_curr_count;
        s_has_prev = true;

        /* If non-interactive / redirected, exit after one-shot summary */
        if (!interactive) {
            break;
        }

        /* 6. Timed input wait (~1000 ms) */
        long n = sys_input_read(s_in_buf, sizeof(s_in_buf), 1000);
        if (n > 0) {
            for (long k = 0; k < n; k++) {
                if (s_in_buf[k] == 'q' || s_in_buf[k] == 'Q') {
                    return 0;
                }
            }
        } else if (n == SYSCALL_EINTR) {
            /* Signal interrupted (e.g. Ctrl-C) */
            return 0;
        } else if (n < 0 && n != INPUT_LOST) {
            /* Unexpected fatal error reading input */
            return 1;
        }
    }

    return 0;
}
