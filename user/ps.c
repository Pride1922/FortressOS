#include "types.h"
#include "syscall_abi.h"

/* Static BSS storage to strictly respect the 512-byte Ring 3 stack budget. */
static proc_info_t s_proc_info;
static char s_out_buf[128];

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

static void format_i64_right(char *dst, size_t width, int64_t val) {
    uint64_t u = (val < 0) ? (uint64_t)(-(val + 1)) + 1 : (uint64_t)val;
    size_t pos = width;
    if (u == 0) {
        if (pos > 0) dst[--pos] = '0';
    } else {
        while (u > 0 && pos > 0) {
            dst[--pos] = (char)('0' + (u % 10));
            u /= 10;
        }
    }
    if (val < 0 && pos > 0) {
        dst[--pos] = '-';
    }
    while (pos > 0) {
        dst[--pos] = ' ';
    }
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

int ps_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    static const char header[] = "  PID  PPID  PGID   SID  STATE    NAME\n";
    if (write_all(1, header, sizeof(header) - 1) != 0) {
        return 1;
    }

    for (uint64_t idx = 0; idx < PROC_INFO_MAX; ++idx) {
        long ret = sys_procinfo(idx, &s_proc_info);
        if (ret == 0) {
            break;
        }
        if (ret < 0) {
            static const char err_msg[] = "ps: error reading process info\n";
            write_all(2, err_msg, sizeof(err_msg) - 1);
            return 1;
        }

        format_i64_right(&s_out_buf[0], 5, s_proc_info.pid);
        format_i64_right(&s_out_buf[5], 6, s_proc_info.ppid);
        format_i64_right(&s_out_buf[11], 6, s_proc_info.pgid);
        format_i64_right(&s_out_buf[17], 6, s_proc_info.sid);
        s_out_buf[23] = ' ';
        s_out_buf[24] = ' ';

        const char *st = state_to_str(s_proc_info.state);
        size_t slen = str_len(st, 9);
        for (size_t i = 0; i < slen; ++i) {
            s_out_buf[25 + i] = st[i];
        }
        for (size_t i = slen; i < 9; ++i) {
            s_out_buf[25 + i] = ' ';
        }

        size_t nlen = str_len(s_proc_info.name, sizeof(s_proc_info.name));
        size_t pos = 34;
        for (size_t i = 0; i < nlen; ++i) {
            s_out_buf[pos++] = s_proc_info.name[i];
        }
        s_out_buf[pos++] = '\n';

        if (write_all(1, s_out_buf, pos) != 0) {
            return 1;
        }
    }

    return 0;
}
