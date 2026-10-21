#include "timeline_log.h"
#if __has_include("vfs.h")
#include "vfs.h"
#elif __has_include("src/fs/vfs.h")
#include "src/fs/vfs.h"
#endif

#if defined(TIMELINE_LOG_HOST_TEST)
extern long timeline_test_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d);
#define timeline_sys timeline_test_syscall
#elif defined(CRYPTO_HOST_TEST)
__attribute__((weak)) long timeline_test_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d);
static inline long timeline_sys(long nr, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    if (timeline_test_syscall) {
        return timeline_test_syscall(nr, a, b, c, d);
    }
    return 0;
}
#else
static inline long timeline_sys(long nr, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    register uintptr_t r10 __asm__("r10") = d;
    register uintptr_t r8  __asm__("r8")  = 0;
    register uintptr_t r9  __asm__("r9")  = 0;
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory", "cc");
    return nr;
}
#endif

static const char s_hex_digits[] = "0123456789abcdef";

static int64_t s_last_diag_mono_sec = 0;
static uint32_t s_suppressed_diag_count = 0;

#ifdef TIMELINE_LOG_HOST_TEST
void timeline_log_reset_rate_limit(void) {
    s_last_diag_mono_sec = 0;
    s_suppressed_diag_count = 0;
}
#endif

static void timeline_emit_diag(const char *msg) {
    if (!msg) return;
    size_t len = 0;
    while (msg[len]) len++;
    if (len == 0) return;

    timespec_t ts_mono = {0};
    timeline_sys(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (uintptr_t)&ts_mono, 0, 0);
    int64_t now_sec = ts_mono.tv_sec;

    if (s_last_diag_mono_sec == 0 || now_sec >= s_last_diag_mono_sec + TIMELINE_DIAG_RATE_LIMIT_SEC) {
        if (s_suppressed_diag_count > 0) {
            char sup[64];
            size_t spos = 0;
            const char *sup_hdr = "timeline_log: (suppressed ";
            while (*sup_hdr) sup[spos++] = *sup_hdr++;
            uint32_t cnt = s_suppressed_diag_count;
            char tmp[16];
            size_t tlen = 0;
            do {
                tmp[tlen++] = (char)('0' + (cnt % 10));
                cnt /= 10;
            } while (cnt > 0);
            while (tlen > 0) sup[spos++] = tmp[--tlen];
            const char *sup_ftr = " error messages)\n";
            while (*sup_ftr) sup[spos++] = *sup_ftr++;
            timeline_sys(SYS_WRITE, 2, (uintptr_t)sup, spos, 0);
            s_suppressed_diag_count = 0;
        }
        timeline_sys(SYS_WRITE, 2, (uintptr_t)msg, len, 0);
        s_last_diag_mono_sec = now_sec;
    } else {
        s_suppressed_diag_count++;
    }
}

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
        buf[p++] = 'T';
        buf[p++] = (char)('0' + (hour / 10) % 10);
        buf[p++] = (char)('0' + (hour % 10));
        buf[p++] = ':';
        buf[p++] = (char)('0' + (min / 10) % 10);
        buf[p++] = (char)('0' + (min % 10));
        buf[p++] = ':';
        buf[p++] = (char)('0' + (sec / 10) % 10);
        buf[p++] = (char)('0' + (sec % 10));
        buf[p++] = 'Z';
        buf[p++] = ']';
        buf[p++] = ' ';
        buf[p] = '\0';
        return p;
    }

    /* Monotonic uptime fallback: [   12.345s UPTIME]  */
    uint64_t s = mono_sec >= 0 ? (uint64_t)mono_sec : 0;
    uint64_t ns = mono_nsec >= 0 ? (uint64_t)mono_nsec : 0;
    uint64_t ms = (ns / 1000000ULL) % 1000ULL;

    char s_digits[32];
    size_t s_len = 0;
    uint64_t s_val = s;
    do {
        s_digits[s_len++] = (char)('0' + (s_val % 10));
        s_val /= 10;
    } while (s_val > 0);
    for (size_t i = 0; i < s_len / 2; i++) {
        char tmp = s_digits[i];
        s_digits[i] = s_digits[s_len - 1 - i];
        s_digits[s_len - 1 - i] = tmp;
    }

    size_t pad = (s_len < 5) ? (5 - s_len) : 0;
    size_t needed = 1 + pad + s_len + 1 + 3 + 1 + 9;
    if (cap < needed + 1) return 0;

    size_t p = 0;
    buf[p++] = '[';
    for (size_t i = 0; i < pad; i++) buf[p++] = ' ';
    for (size_t i = 0; i < s_len; i++) buf[p++] = s_digits[i];
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

static size_t timeline_vformat_msg(char *dst, size_t cap, const char *fmt, __builtin_va_list ap) {
    if (!dst || cap == 0) return 0;
    if (!fmt) { dst[0] = '\0'; return 0; }

    size_t out = 0;
    while (*fmt && out + 1 < cap) {
        if (*fmt != '%') {
            dst[out++] = *fmt++;
            continue;
        }
        fmt++; /* skip '%' */
        if (*fmt == '%') {
            dst[out++] = '%';
            fmt++;
            continue;
        }

        bool is_long = false;
        if (*fmt == 'l') {
            is_long = true;
            fmt++;
            if (*fmt == 'l') {
                fmt++;
            }
        }

        char spec = *fmt++;
        if (spec == 's') {
            const char *s = __builtin_va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s && out + 1 < cap) {
                dst[out++] = *s++;
            }
        } else if (spec == 'c') {
            int c = __builtin_va_arg(ap, int);
            dst[out++] = (char)c;
        } else if (spec == 'd' || spec == 'i') {
            int64_t val = is_long ? __builtin_va_arg(ap, int64_t) : (int64_t)__builtin_va_arg(ap, int);
            char tmp[32];
            size_t tlen = 0;
            if (val < 0) {
                if (out + 1 < cap) dst[out++] = '-';
                uint64_t u = (uint64_t)(-(val + 1)) + 1;
                do {
                    tmp[tlen++] = (char)('0' + (u % 10));
                    u /= 10;
                } while (u > 0);
            } else {
                uint64_t u = (uint64_t)val;
                do {
                    tmp[tlen++] = (char)('0' + (u % 10));
                    u /= 10;
                } while (u > 0);
            }
            while (tlen > 0 && out + 1 < cap) {
                dst[out++] = tmp[--tlen];
            }
        } else if (spec == 'u') {
            uint64_t u = is_long ? __builtin_va_arg(ap, uint64_t) : (uint64_t)__builtin_va_arg(ap, unsigned int);
            char tmp[32];
            size_t tlen = 0;
            do {
                tmp[tlen++] = (char)('0' + (u % 10));
                u /= 10;
            } while (u > 0);
            while (tlen > 0 && out + 1 < cap) {
                dst[out++] = tmp[--tlen];
            }
        } else if (spec == 'x' || spec == 'X') {
            uint64_t u = is_long ? __builtin_va_arg(ap, uint64_t) : (uint64_t)__builtin_va_arg(ap, unsigned int);
            const char *digits = (spec == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
            char tmp[32];
            size_t tlen = 0;
            do {
                tmp[tlen++] = digits[u & 0x0f];
                u >>= 4;
            } while (u > 0);
            while (tlen > 0 && out + 1 < cap) {
                dst[out++] = tmp[--tlen];
            }
        } else if (spec == 'p') {
            uintptr_t u = (uintptr_t)__builtin_va_arg(ap, void *);
            const char *digits = "0123456789abcdef";
            char tmp[32];
            size_t tlen = 0;
            if (out + 2 < cap) {
                dst[out++] = '0';
                dst[out++] = 'x';
            }
            do {
                tmp[tlen++] = digits[u & 0x0f];
                u >>= 4;
            } while (u > 0);
            while (tlen > 0 && out + 1 < cap) {
                dst[out++] = tmp[--tlen];
            }
        } else {
            if (out + 1 < cap) dst[out++] = '%';
            if (is_long && out + 1 < cap) dst[out++] = 'l';
            if (spec && out + 1 < cap) dst[out++] = spec;
        }
    }
    dst[out] = '\0';
    return out;
}

static size_t format_record(char *buf, size_t cap,
                            const char *tag,
                            const char *msg, size_t msg_len,
                            int64_t real_sec,
                            int64_t mono_sec, int64_t mono_nsec) {
    if (!buf || cap == 0) return 0;
    if (!tag || !*tag) tag = TIMELINE_DEFAULT_TAG;

    size_t p = format_prefix(buf, cap, real_sec, mono_sec, mono_nsec);
    if (p == 0) return 0;

    /* Append sanitized tag */
    size_t tag_len = 0;
    while (tag[tag_len]) tag_len++;
    if (p + tag_len + 2 + 4 > cap) {
        tag_len = (cap > p + 6) ? (cap - p - 6) : 0;
    }
    for (size_t i = 0; i < tag_len; i++) {
        unsigned char c = (unsigned char)tag[i];
        if (c < 0x20 || c >= 0x7f) c = '_';
        buf[p++] = (char)c;
    }
    buf[p++] = ':';
    buf[p++] = ' ';

    /* Measure sanitized expansion */
    size_t msg_sanitized_len = 0;
    for (size_t i = 0; i < msg_len && msg; i++) {
        unsigned char c = (unsigned char)msg[i];
        if (c == '\r' || c == '\n') msg_sanitized_len += 1;
        else if (c < 0x20 || c >= 0x7f) msg_sanitized_len += 4;
        else msg_sanitized_len += 1;
    }

    if (p + msg_sanitized_len + 1 <= cap) {
        /* Message fits completely */
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
        /* Truncate with "...\n" */
        size_t budget = (cap >= p + 4) ? (cap - p - 4) : 0;
        size_t added = 0;
        for (size_t i = 0; i < msg_len && msg; i++) {
            unsigned char c = (unsigned char)msg[i];
            size_t clen = (c == '\r' || c == '\n') ? 1 : ((c < 0x20 || c >= 0x7f) ? 4 : 1);
            if (added + clen > budget) break;
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

static int check_directory(const char *dir) {
    stat_ext_v1_t st;
    long r = timeline_sys(SYS_STAT_EXT, (uintptr_t)dir, (uintptr_t)&st, sizeof(st), 1);
    if (r != 0) {
        if (r == SYSCALL_ENOENT || r == SYSCALL_EROFS) {
            return 1; /* Eligible for fallback */
        }
        if (r == SYSCALL_EACCES || r == SYSCALL_EPERM) {
            return -2; /* Fail closed, no fallback */
        }
        return -1;
    }
    if (st.type != VFS_DIRECTORY && (st.mode & VFS_S_IFMT) != VFS_S_IFDIR) {
        return -1;
    }
    if (st.uid != 0) {
        return -1;
    }
    if (st.mode & 0002) {
        return -1;
    }
    return 0;
}

static int check_file(const char *file) {
    vfs_stat_t lst;
    long r = timeline_sys(SYS_LSTAT, (uintptr_t)file, (uintptr_t)&lst, 0, 0);
    if (r == 0) {
        if ((lst.mode & VFS_S_IFMT) == VFS_S_IFLNK) {
            return -1;
        }
    }
    stat_ext_v1_t fst;
    r = timeline_sys(SYS_STAT_EXT, (uintptr_t)file, (uintptr_t)&fst, sizeof(fst), 1);
    if (r == 0) {
        if (fst.uid != 0) {
            return -1;
        }
        if (fst.mode & 0002) {
            return -1;
        }
    } else if (r != SYSCALL_ENOENT) {
        if (r == SYSCALL_EACCES || r == SYSCALL_EPERM) {
            return -2;
        }
        if (r == SYSCALL_EROFS) {
            return 1;
        }
        return -1;
    }
    return 0;
}

static int open_destination(void) {
    /* Step 1: Primary destination /var/log/messages */
    int dret = check_directory(TIMELINE_LOG_DIR_PRIMARY);
    if (dret == 0) {
        int fret = check_file(TIMELINE_LOG_FILE_PRIMARY);
        if (fret == 0) {
            long fd = timeline_sys(SYS_OPEN, (uintptr_t)TIMELINE_LOG_FILE_PRIMARY,
                                   VFS_O_CREAT | VFS_O_WRONLY | VFS_O_APPEND, 0, 0);
            if (fd >= 0) {
                timeline_sys(SYS_FCHMOD, fd, 0640, 0, 0);
                return (int)fd;
            }
            if (fd != SYSCALL_ENOENT && fd != SYSCALL_EROFS) {
                timeline_emit_diag("timeline_log: failed to open primary destination\n");
                return -1;
            }
            /* Fall through on ENOENT or EROFS */
        } else if (fret < 0) {
            timeline_emit_diag("timeline_log: primary file validation failed\n");
            return -1;
        }
    } else if (dret < 0) {
        timeline_emit_diag("timeline_log: primary directory validation failed\n");
        return -1;
    }

    /* Step 2: Fallback destination /run/messages */
    if (check_directory(TIMELINE_LOG_DIR_FALLBACK) != 0) {
        timeline_emit_diag("timeline_log: fallback directory validation failed\n");
        return -1;
    }
    if (check_file(TIMELINE_LOG_FILE_FALLBACK) != 0) {
        timeline_emit_diag("timeline_log: fallback file validation failed\n");
        return -1;
    }
    long fd = timeline_sys(SYS_OPEN, (uintptr_t)TIMELINE_LOG_FILE_FALLBACK,
                           VFS_O_CREAT | VFS_O_WRONLY | VFS_O_APPEND, 0, 0);
    if (fd >= 0) {
        timeline_sys(SYS_FCHMOD, fd, 0640, 0, 0);
        return (int)fd;
    }
    timeline_emit_diag("timeline_log: failed to open fallback destination\n");
    return -1;
}

int timeline_vlog(const char *tag, const char *fmt, __builtin_va_list ap) {
    char msg_buf[TIMELINE_LOG_BUFFER_SIZE];
    size_t msg_len = timeline_vformat_msg(msg_buf, sizeof(msg_buf), fmt, ap);

    timespec_t ts_real = {0}, ts_mono = {0};
    timeline_sys(SYS_CLOCK_GETTIME, CLOCK_REALTIME, (uintptr_t)&ts_real, 0, 0);
    timeline_sys(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (uintptr_t)&ts_mono, 0, 0);

    char rec[TIMELINE_LOG_BUFFER_SIZE];
    size_t rlen = format_record(rec, sizeof(rec), tag, msg_buf, msg_len,
                                ts_real.tv_sec, ts_mono.tv_sec, ts_mono.tv_nsec);
    if (rlen == 0) return -1;

    int fd = open_destination();
    if (fd < 0) return -1;

    long w = timeline_sys(SYS_WRITE, fd, (uintptr_t)rec, rlen, 0);
    timeline_sys(SYS_CLOSE, fd, 0, 0, 0);

    if (w == (long)rlen) {
        return 0;
    }
    if (w >= 0 && (size_t)w < rlen) {
        timeline_emit_diag("timeline_log: short write (storage exhausted)\n");
    } else {
        timeline_emit_diag("timeline_log: write error\n");
    }
    return -1;
}

int timeline_log(const char *tag, const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int r = timeline_vlog(tag, fmt, ap);
    __builtin_va_end(ap);
    return r;
}
