#include "logger.h"
#include "common.h"
#include "../permissions_cli.h"
#include "vfs.h"

static const char s_hex_digits[] = "0123456789abcdef";

static inline bool is_leap_year(uint32_t year) {
    return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

static bool epoch_to_datetime(uint64_t epoch, uint32_t *out_year, uint8_t *out_month,
                             uint8_t *out_day, uint8_t *out_hour, uint8_t *out_min,
                             uint8_t *out_sec) {
    uint64_t days = epoch / 86400ULL;
    uint32_t sec_in_day = (uint32_t)(epoch % 86400ULL);

    *out_hour = (uint8_t)(sec_in_day / 3600);
    *out_min  = (uint8_t)((sec_in_day % 3600) / 60);
    *out_sec  = (uint8_t)(sec_in_day % 60);

    uint32_t year = 1970;
    for (;;) {
        uint32_t diy = is_leap_year(year) ? 366 : 365;
        if (days < diy) break;
        days -= diy;
        year++;
        if (year > 2200) return false;
    }
    *out_year = year;

    static const uint8_t days_in_months[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };

    uint8_t month = 1;
    for (int m = 0; m < 12; m++) {
        uint32_t dim = days_in_months[m];
        if (m == 1 && is_leap_year(year)) dim = 29;
        if (days < dim) {
            month = (uint8_t)(m + 1);
            break;
        }
        days -= dim;
    }
    *out_month = month;
    *out_day   = (uint8_t)(days + 1);
    return true;
}

size_t logger_sanitize(char *dst, size_t cap, const char *src, size_t src_len) {
    if (!dst || cap == 0) return 0;
    size_t out_pos = 0;

    for (size_t i = 0; i < src_len && src; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\0') break;
        if (c == '\r' || c == '\n') {
            if (out_pos + 1 < cap) {
                dst[out_pos++] = ' ';
            } else {
                break;
            }
        } else if (c < 0x20 || c >= 0x7f) {
            if (out_pos + 4 < cap) {
                dst[out_pos++] = '\\';
                dst[out_pos++] = 'x';
                dst[out_pos++] = s_hex_digits[(c >> 4) & 0x0f];
                dst[out_pos++] = s_hex_digits[c & 0x0f];
            } else {
                break;
            }
        } else {
            if (out_pos + 1 < cap) {
                dst[out_pos++] = (char)c;
            } else {
                break;
            }
        }
    }
    if (out_pos < cap) {
        dst[out_pos] = '\0';
    }
    return out_pos;
}

static size_t format_prefix(char *buf, size_t cap, int64_t real_sec, int64_t mono_sec, int64_t mono_nsec) {
    uint32_t year = 0;
    uint8_t month = 0, day = 0, hour = 0, min = 0, sec = 0;
    bool valid_utc = false;

    if (real_sec > 0 && epoch_to_datetime((uint64_t)real_sec, &year, &month, &day, &hour, &min, &sec)) {
        if (year >= 2026 && year <= 2100) {
            valid_utc = true;
        }
    }

    if (valid_utc) {
        /* Format: [YYYY-MM-DD HH:MM:SS UTC]  (26 chars + NUL) */
        if (cap < 27) return 0;
        size_t p = 0;
        buf[p++] = '[';
        buf[p++] = (char)('0' + (year / 1000) % 10);
        buf[p++] = (char)('0' + (year / 100) % 10);
        buf[p++] = (char)('0' + (year / 10) % 10);
        buf[p++] = (char)('0' + (year % 10));
        buf[p++] = '-';
        buf[p++] = (char)('0' + (month / 10) % 10);
        buf[p++] = (char)('0' + (month % 10));
        buf[p++] = '-';
        buf[p++] = (char)('0' + (day / 10) % 10);
        buf[p++] = (char)('0' + (day % 10));
        buf[p++] = ' ';
        buf[p++] = (char)('0' + (hour / 10) % 10);
        buf[p++] = (char)('0' + (hour % 10));
        buf[p++] = ':';
        buf[p++] = (char)('0' + (min / 10) % 10);
        buf[p++] = (char)('0' + (min % 10));
        buf[p++] = ':';
        buf[p++] = (char)('0' + (sec / 10) % 10);
        buf[p++] = (char)('0' + (sec % 10));
        buf[p++] = ' ';
        buf[p++] = 'U';
        buf[p++] = 'T';
        buf[p++] = 'C';
        buf[p++] = ']';
        buf[p++] = ' ';
        buf[p] = '\0';
        return p;
    }

    /* Monotonic uptime fallback: [   12.345s UPTIME]  (20 chars + NUL) */
    uint64_t s = mono_sec >= 0 ? (uint64_t)mono_sec : 0;
    uint64_t ns = mono_nsec >= 0 ? (uint64_t)mono_nsec : 0;
    uint64_t ms = (ns / 1000000ULL) % 1000ULL;

    char s_digits[32];
    size_t s_len = tool_format_u64(s_digits, s);

    size_t pad = (s_len < 5) ? (5 - s_len) : 0;
    size_t needed = 1 + pad + s_len + 1 + 3 + 1 + 9; /* '[' + pad + s + '.' + ms + 's' + ' UPTIME] ' */
    if (cap < needed + 1) return 0;

    size_t p = 0;
    buf[p++] = '[';
    for (size_t i = 0; i < pad; i++) {
        buf[p++] = ' ';
    }
    for (size_t i = 0; i < s_len; i++) {
        buf[p++] = s_digits[i];
    }
    buf[p++] = '.';
    buf[p++] = (char)('0' + (ms / 100) % 10);
    buf[p++] = (char)('0' + (ms / 10) % 10);
    buf[p++] = (char)('0' + (ms % 10));
    buf[p++] = 's';
    buf[p++] = ' ';
    buf[p++] = 'U';
    buf[p++] = 'P';
    buf[p++] = 'T';
    buf[p++] = 'I';
    buf[p++] = 'M';
    buf[p++] = 'E';
    buf[p++] = ']';
    buf[p++] = ' ';
    buf[p] = '\0';
    return p;
}

size_t logger_format_record(char *buf, size_t cap,
                            const char *tag,
                            const char *msg, size_t msg_len,
                            int64_t real_sec,
                            int64_t mono_sec, int64_t mono_nsec) {
    if (!buf || cap == 0) return 0;
    if (!tag || !*tag) tag = LOGGER_DEFAULT_TAG;

    size_t p = format_prefix(buf, cap, real_sec, mono_sec, mono_nsec);
    if (p == 0) return 0;

    /* Append sanitized tag */
    char clean_tag[64];
    size_t tag_len = logger_sanitize(clean_tag, sizeof(clean_tag), tag, tool_length(tag));

    /* Budget check: leave room for tag + ": " + at least "...\n" (4 bytes) */
    if (p + tag_len + 2 + 4 > cap) {
        /* Pathological tag size: truncate tag to fit */
        tag_len = (cap > p + 6) ? (cap - p - 6) : 0;
    }
    for (size_t i = 0; i < tag_len; i++) {
        buf[p++] = clean_tag[i];
    }
    buf[p++] = ':';
    buf[p++] = ' ';

    /* Measure expansion of message */
    size_t msg_sanitized_len = 0;
    for (size_t i = 0; i < msg_len && msg; i++) {
        unsigned char c = (unsigned char)msg[i];
        if (c == '\r' || c == '\n') msg_sanitized_len += 1;
        else if (c < 0x20 || c >= 0x7f) msg_sanitized_len += 4;
        else msg_sanitized_len += 1;
    }

    if (p + msg_sanitized_len + 1 <= cap) {
        /* Message fits completely without truncation */
        for (size_t i = 0; i < msg_len && msg; i++) {
            unsigned char c = (unsigned char)msg[i];
            if (c == '\r' || c == '\n') {
                buf[p++] = ' ';
            } else if (c < 0x20 || c >= 0x7f) {
                buf[p++] = '\\';
                buf[p++] = 'x';
                buf[p++] = s_hex_digits[(c >> 4) & 0x0f];
                buf[p++] = s_hex_digits[c & 0x0f];
            } else {
                buf[p++] = (char)c;
            }
        }
        buf[p++] = '\n';
    } else {
        /* Exceeds buffer limit: truncate with "...\n" (4 bytes) */
        size_t budget = (cap >= p + 4) ? (cap - p - 4) : 0;
        size_t added = 0;
        for (size_t i = 0; i < msg_len && msg; i++) {
            unsigned char c = (unsigned char)msg[i];
            size_t clen = (c == '\r' || c == '\n') ? 1 : ((c < 0x20 || c >= 0x7f) ? 4 : 1);
            if (added + clen > budget) {
                break;
            }
            if (c == '\r' || c == '\n') {
                buf[p++] = ' ';
            } else if (c < 0x20 || c >= 0x7f) {
                buf[p++] = '\\';
                buf[p++] = 'x';
                buf[p++] = s_hex_digits[(c >> 4) & 0x0f];
                buf[p++] = s_hex_digits[c & 0x0f];
            } else {
                buf[p++] = (char)c;
            }
            added += clen;
        }
        if (p + 4 <= cap) {
            buf[p++] = '.';
            buf[p++] = '.';
            buf[p++] = '.';
            buf[p++] = '\n';
        }
    }

    return p;
}

static int check_dir(const char *dir) {
    stat_ext_v1_t st;
    long r = permission_call4(SYS_STAT_EXT, (uintptr_t)dir, (uintptr_t)&st, sizeof(st), 1);
    if (r != 0) {
        if (r == SYSCALL_ENOENT || r == SYSCALL_EROFS) {
            return 1; /* Missing or read-only; caller may fall back */
        }
        if (r == SYSCALL_EACCES || r == SYSCALL_EPERM) {
            tool_error("logger", "permission denied accessing directory", dir);
            return -1; /* Fail closed */
        }
        tool_sys_error("logger", "cannot access directory", dir, r);
        return -1;
    }
    if (st.type != VFS_DIRECTORY && (st.mode & VFS_S_IFMT) != VFS_S_IFDIR) {
        tool_error("logger", "log destination path is not a directory", dir);
        return -1;
    }
    if (st.uid != 0) {
        tool_error("logger", "insecure log directory (not owned by root)", dir);
        return -1;
    }
    if (st.mode & 0002) {
        tool_error("logger", "insecure log directory (world-writable)", dir);
        return -1;
    }
    return 0; /* OK */
}

static int check_file(const char *file) {
    vfs_stat_t lst;
    long r = tool_syscall(SYS_LSTAT, (uintptr_t)file, (uintptr_t)&lst, 0);
    if (r == 0) {
        if ((lst.mode & VFS_S_IFMT) == VFS_S_IFLNK) {
            tool_error("logger", "insecure log file (symlink rejected)", file);
            return -1;
        }
    }
    stat_ext_v1_t fst;
    r = permission_call4(SYS_STAT_EXT, (uintptr_t)file, (uintptr_t)&fst, sizeof(fst), 1);
    if (r == 0) {
        if (fst.uid != 0) {
            tool_error("logger", "insecure log file (not owned by root)", file);
            return -1;
        }
        if (fst.mode & 0002) {
            tool_error("logger", "insecure log file (world-writable)", file);
            return -1;
        }
    } else if (r != SYSCALL_ENOENT) {
        if (r == SYSCALL_EACCES || r == SYSCALL_EPERM) {
            tool_error("logger", "permission denied accessing log file", file);
            return -1;
        }
    }
    return 0;
}

int logger_open_destination(void) {
    /* Step 1: Attempt primary destination /var/log/messages */
    int dir_status = check_dir(LOGGER_LOG_DIR_PRIMARY);
    if (dir_status == 0) {
        if (check_file(LOGGER_LOG_FILE_PRIMARY) == 0) {
            long fd = tool_syscall(SYS_OPEN, (uintptr_t)LOGGER_LOG_FILE_PRIMARY,
                                   VFS_O_CREAT | VFS_O_WRONLY | VFS_O_APPEND, 0);
            if (fd >= 0) {
                tool_syscall(SYS_FCHMOD, fd, 0640, 0);
                return (int)fd;
            }
            if (fd == SYSCALL_EACCES || fd == SYSCALL_EPERM) {
                tool_sys_error("logger", "cannot open log file", LOGGER_LOG_FILE_PRIMARY, fd);
                return -1;
            }
            if (fd != SYSCALL_ENOENT && fd != SYSCALL_EROFS) {
                tool_sys_error("logger", "cannot open log file", LOGGER_LOG_FILE_PRIMARY, fd);
                return -1;
            }
            /* Fall through on ENOENT or EROFS */
        } else {
            return -1;
        }
    } else if (dir_status < 0) {
        return -1; /* Fail closed */
    }

    /* Step 2: Attempt fallback destination /run/messages */
    if (check_dir(LOGGER_LOG_DIR_FALLBACK) != 0) {
        return -1;
    }
    if (check_file(LOGGER_LOG_FILE_FALLBACK) != 0) {
        return -1;
    }
    long fd = tool_syscall(SYS_OPEN, (uintptr_t)LOGGER_LOG_FILE_FALLBACK,
                           VFS_O_CREAT | VFS_O_WRONLY | VFS_O_APPEND, 0);
    if (fd >= 0) {
        tool_syscall(SYS_FCHMOD, fd, 0640, 0);
        return (int)fd;
    }
    tool_sys_error("logger", "cannot open fallback log file", LOGGER_LOG_FILE_FALLBACK, fd);
    return -1;
}

int logger_write_record(int fd, const char *buf, size_t len) {
    if (fd < 0 || !buf || len == 0 || len > LOGGER_BUFFER_SIZE) {
        return -1;
    }
    long w = tool_syscall(SYS_WRITE, fd, (uintptr_t)buf, len);
    if (w == (long)len) {
        return 0;
    }
    if (w >= 0 && (size_t)w < len) {
        tool_error("logger", "short write (storage exhausted)", NULL);
    } else {
        tool_sys_error("logger", "write failed", NULL, w);
    }
    return -1;
}

int logger_stream_stdin(int out_fd, const char *tag) {
    char in_buf[512];
    char line[LOGGER_BUFFER_SIZE];
    size_t line_len = 0;
    bool discarding = false;
    long n;

    while ((n = tool_syscall(SYS_READ, 0, (uintptr_t)in_buf, sizeof(in_buf))) > 0) {
        for (long i = 0; i < n; i++) {
            char c = in_buf[i];
            if (c == '\n') {
                if (discarding) {
                    discarding = false;
                    line_len = 0;
                } else {
                    if (line_len > 0 && line[line_len - 1] == '\r') {
                        line_len--;
                    }
                    line[line_len] = '\0';
                    char rec[LOGGER_BUFFER_SIZE];
                    timespec_t ts_real = {0}, ts_mono = {0};
                    tool_syscall(SYS_CLOCK_GETTIME, CLOCK_REALTIME, (uintptr_t)&ts_real, 0);
                    tool_syscall(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (uintptr_t)&ts_mono, 0);
                    size_t rlen = logger_format_record(rec, sizeof(rec), tag, line, line_len,
                                                      ts_real.tv_sec, ts_mono.tv_sec, ts_mono.tv_nsec);
                    if (logger_write_record(out_fd, rec, rlen) != 0) {
                        return 1;
                    }
                    line_len = 0;
                }
            } else {
                if (discarding) continue;
                if (line_len + 1 < sizeof(line)) {
                    line[line_len++] = c;
                } else {
                    /* Oversized line: format and emit what we have (truncated with '...') */
                    line[line_len] = '\0';
                    char rec[LOGGER_BUFFER_SIZE];
                    timespec_t ts_real = {0}, ts_mono = {0};
                    tool_syscall(SYS_CLOCK_GETTIME, CLOCK_REALTIME, (uintptr_t)&ts_real, 0);
                    tool_syscall(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (uintptr_t)&ts_mono, 0);
                    size_t rlen = logger_format_record(rec, sizeof(rec), tag, line, line_len,
                                                      ts_real.tv_sec, ts_mono.tv_sec, ts_mono.tv_nsec);
                    if (logger_write_record(out_fd, rec, rlen) != 0) {
                        return 1;
                    }
                    line_len = 0;
                    discarding = true;
                }
            }
        }
    }

    if (n < 0) {
        tool_sys_error("logger", "read error on stdin", NULL, n);
        return 1;
    }

    /* Unterminated input line before EOF: explicitly format and append \n */
    if (line_len > 0 && !discarding) {
        if (line[line_len - 1] == '\r') line_len--;
        line[line_len] = '\0';
        char rec[LOGGER_BUFFER_SIZE];
        timespec_t ts_real = {0}, ts_mono = {0};
        tool_syscall(SYS_CLOCK_GETTIME, CLOCK_REALTIME, (uintptr_t)&ts_real, 0);
        tool_syscall(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (uintptr_t)&ts_mono, 0);
        size_t rlen = logger_format_record(rec, sizeof(rec), tag, line, line_len,
                                          ts_real.tv_sec, ts_mono.tv_sec, ts_mono.tv_nsec);
        if (logger_write_record(out_fd, rec, rlen) != 0) {
            return 1;
        }
    }

    return 0;
}

int logger_main(int argc, char **argv) {
    const char *tag = LOGGER_DEFAULT_TAG;
    int first_msg = 0;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (tool_equal(arg, "--help") || tool_equal(arg, "-h")) {
            const char *help =
                "usage: logger [-t tag] [message...]\n"
                "Log messages to the system audit timeline (/var/log/messages).\n"
                "\n"
                "Options:\n"
                "  -t tag        Specify the message tag (default: \"logger\")\n"
                "      --help    Display this help and exit\n";
            return tool_write("logger", help, tool_length(help));
        }
        if (tool_equal(arg, "--")) {
            first_msg = i + 1;
            break;
        }
        if (tool_equal(arg, "-t")) {
            if (i + 1 >= argc) {
                tool_error("logger", "option requires an argument: -t", NULL);
                return 1;
            }
            tag = argv[++i];
            continue;
        }
        if (arg[0] == '-' && arg[1] == 't') {
            tag = arg + 2;
            if (!*tag) {
                tool_error("logger", "option requires an argument: -t", NULL);
                return 1;
            }
            continue;
        }
        if (arg[0] == '-') {
            tool_error("logger", "unrecognized option", arg);
            return 1;
        }
        first_msg = i;
        break;
    }

    int out_fd = logger_open_destination();
    if (out_fd < 0) {
        return 1;
    }

    int ret = 0;
    if (first_msg == 0 || first_msg >= argc) {
        /* Streaming mode from stdin */
        ret = logger_stream_stdin(out_fd, tag);
    } else {
        /* Positional message arguments */
        char msg_buf[LOGGER_BUFFER_SIZE];
        size_t msg_len = 0;
        for (int i = first_msg; i < argc; i++) {
            const char *part = argv[i];
            if (i > first_msg && msg_len + 1 < sizeof(msg_buf)) {
                msg_buf[msg_len++] = ' ';
            }
            while (*part && msg_len + 1 < sizeof(msg_buf)) {
                msg_buf[msg_len++] = *part++;
            }
        }
        msg_buf[msg_len] = '\0';

        char rec[LOGGER_BUFFER_SIZE];
        timespec_t ts_real = {0}, ts_mono = {0};
        tool_syscall(SYS_CLOCK_GETTIME, CLOCK_REALTIME, (uintptr_t)&ts_real, 0);
        tool_syscall(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (uintptr_t)&ts_mono, 0);

        size_t rlen = logger_format_record(rec, sizeof(rec), tag, msg_buf, msg_len,
                                          ts_real.tv_sec, ts_mono.tv_sec, ts_mono.tv_nsec);
        ret = (logger_write_record(out_fd, rec, rlen) == 0) ? 0 : 1;
    }

    tool_syscall(SYS_CLOSE, out_fd, 0, 0);
    return ret;
}
