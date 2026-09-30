#include "types.h"
#include "syscall_abi.h"

/* Static BSS storage to strictly respect the 512-byte Ring 3 stack budget. */
static sysinfo_t s_info;
static char s_buf[256];

static long sys_write(int fd, const void *buf, size_t count) {
    long nr = SYS_WRITE;
    __asm__ volatile("syscall" : "+a"(nr) : "D"((uintptr_t)fd), "S"((uintptr_t)buf), "d"((uintptr_t)count)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}

static long sys_sysinfo(sysinfo_t *buf) {
    long nr = SYS_SYSINFO;
    __asm__ volatile("syscall" : "+a"(nr) : "D"((uintptr_t)buf)
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

static void buf_append_str(char *buf, size_t *pos, const char *str) {
    size_t i = 0;
    while (str[i] != '\0') {
        buf[(*pos)++] = str[i++];
    }
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

static void buf_append_u64_right(char *buf, size_t *pos, uint64_t val, size_t width) {
    size_t digits = u64_digits(val);
    size_t pad = (width > digits) ? (width - digits) : 0;
    for (size_t i = 0; i < pad; i++) {
        buf[(*pos)++] = ' ';
    }
    size_t start = *pos;
    if (val == 0) {
        buf[(*pos)++] = '0';
    } else {
        uint64_t temp = val;
        while (temp > 0) {
            buf[(*pos)++] = (char)('0' + (temp % 10));
            temp /= 10;
        }
        /* Reverse digits */
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

static void buf_append_u64(char *buf, size_t *pos, uint64_t val) {
    buf_append_u64_right(buf, pos, val, 0);
}

static void buf_append_2digits(char *buf, size_t *pos, uint64_t val) {
    buf[(*pos)++] = (char)('0' + ((val / 10) % 10));
    buf[(*pos)++] = (char)('0' + (val % 10));
}

int sysinfo_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    long ret = sys_sysinfo(&s_info);
    if (ret < 0) {
        static const char err_msg[] = "sysinfo: error reading system information\n";
        write_all(2, err_msg, sizeof(err_msg) - 1);
        return 1;
    }

    uint64_t hz = s_info.tick_hz ? s_info.tick_hz : 100;
    uint64_t total_sec = s_info.uptime_ticks / hz;
    uint64_t hours = total_sec / 3600;
    uint64_t mins = (total_sec % 3600) / 60;
    uint64_t secs = total_sec % 60;

    uint64_t total_mib = s_info.total_ram_bytes / (1024ULL * 1024ULL);
    uint64_t free_mib = s_info.free_ram_bytes / (1024ULL * 1024ULL);
    uint64_t used_mib = (total_mib >= free_mib) ? (total_mib - free_mib) : 0;

    size_t ram_width = u64_digits(total_mib);
    if (ram_width < 4) {
        ram_width = 4;
    }

    size_t pos = 0;
    buf_append_str(s_buf, &pos, "FortressOS \xe2\x80\x94 system information\n");

    buf_append_str(s_buf, &pos, "  CPUs:        ");
    buf_append_u64(s_buf, &pos, (uint64_t)s_info.cpu_count);
    buf_append_str(s_buf, &pos, "\n");

    buf_append_str(s_buf, &pos, "  Uptime:      ");
    if (hours < 100) {
        buf_append_2digits(s_buf, &pos, hours);
    } else {
        buf_append_u64(s_buf, &pos, hours);
    }
    buf_append_str(s_buf, &pos, ":");
    buf_append_2digits(s_buf, &pos, mins);
    buf_append_str(s_buf, &pos, ":");
    buf_append_2digits(s_buf, &pos, secs);
    buf_append_str(s_buf, &pos, "\n");

    buf_append_str(s_buf, &pos, "  RAM total:   ");
    buf_append_u64_right(s_buf, &pos, total_mib, ram_width);
    buf_append_str(s_buf, &pos, " MiB\n");

    buf_append_str(s_buf, &pos, "  RAM free:    ");
    buf_append_u64_right(s_buf, &pos, free_mib, ram_width);
    buf_append_str(s_buf, &pos, " MiB\n");

    buf_append_str(s_buf, &pos, "  RAM used:    ");
    buf_append_u64_right(s_buf, &pos, used_mib, ram_width);
    buf_append_str(s_buf, &pos, " MiB\n");

    buf_append_str(s_buf, &pos, "  Processes:   ");
    buf_append_u64(s_buf, &pos, (uint64_t)s_info.task_count);
    buf_append_str(s_buf, &pos, "\n");

    if (write_all(1, s_buf, pos) != 0) {
        return 1;
    }

    return 0;
}
