#include "common.h"
#include "../timezone.h"
#include "creds.h"

static const char *s_weekdays[7] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static const char *s_months[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static const char *s_date_help =
    "Usage: date [OPTION]... [+FORMAT]\n"
    "       date set TZ=<Region/City>\n"
    "\n"
    "Options:\n"
    "  -u, --utc            Print in Coordinated Universal Time (UTC)\n"
    "  -I, --iso-8601       Print in ISO 8601 format (YYYY-MM-DD)\n"
    "  -R, --rfc-2822       Print in RFC 2822 format\n"
    "  --help               Show this help message\n"
    "\n"
    "Format specifiers (via +FORMAT):\n"
    "  %Y   Year (4 digits)\n"
    "  %m   Month (01..12)\n"
    "  %d   Day of month (01..31)\n"
    "  %H   Hour (00..23)\n"
    "  %M   Minute (00..59)\n"
    "  %S   Second (00..59)\n"
    "  %s   Seconds since epoch (1970-01-01)\n"
    "\n"
    "Examples:\n"
    "  date\n"
    "  date -u\n"
    "  date +\"%Y-%m-%d %H:%M:%S\"\n"
    "  sudo date set TZ=Europe/Brussels\n";

static void append_two_digits(char *buf, size_t *pos, uint32_t val) {
    buf[(*pos)++] = (char)('0' + ((val / 10) % 10));
    buf[(*pos)++] = (char)('0' + (val % 10));
}

static void append_four_digits(char *buf, size_t *pos, uint32_t val) {
    buf[(*pos)++] = (char)('0' + ((val / 1000) % 10));
    buf[(*pos)++] = (char)('0' + ((val / 100) % 10));
    buf[(*pos)++] = (char)('0' + ((val / 10) % 10));
    buf[(*pos)++] = (char)('0' + (val % 10));
}

static void append_str(char *buf, size_t *pos, const char *s) {
    while (*s) {
        buf[(*pos)++] = *s++;
    }
}

int date_main(int argc, char **argv, const char *const *envp) {
    bool utc = false;
    bool iso8601 = false;
    bool rfc2822 = false;
    const char *format = NULL;

    /* Handle 'date set TZ=<Region/City>' subcommand */
    if (argc >= 2 && tool_equal(argv[1], "set")) {
        if (argc != 3) {
            return tool_error("date", "usage: date set TZ=<Region/City>", NULL);
        }
        const char *arg = argv[2];
        if (arg[0] != 'T' || arg[1] != 'Z' || arg[2] != '=') {
            return tool_error("date", "usage: date set TZ=<Region/City>", argv[2]);
        }
        const char *tz = arg + 3;
        if (!*tz) {
            return tool_error("date", "missing timezone in TZ=<Region/City>", NULL);
        }

        /* Privilege check: root (EUID 0) or CAP_SYS_ADMIN */
        uint32_t real = 0, effective = 0, saved = 0;
        long uid_res = tool_syscall(SYS_GETRESUID, (uintptr_t)&real, (uintptr_t)&effective, (uintptr_t)&saved);
        long caps = tool_syscall(SYS_CAPGET, 0, 0, 0);
        bool is_root = (uid_res == 0 && effective == 0);
        bool has_cap = (caps >= 0 && ((uint64_t)caps & CAP_SYS_ADMIN));
        if (!is_root && !has_cap) {
            return tool_error("date", "permission denied: requires root or CAP_SYS_ADMIN", NULL);
        }

        /* Validate timezone name and availability in database */
        tz_result_t check_zone;
        if (!tz_name(tz) || !tz_spec(tz, 0, &check_zone)) {
            return tool_error("date", "invalid or unavailable timezone", tz);
        }

        /* Atomically write /etc/timezone via SYS_RENAME_REPLACE */
        char buf[160];
        size_t len = 0;
        while (tz[len] && len < sizeof(buf) - 2) {
            buf[len] = tz[len];
            len++;
        }
        buf[len++] = '\n';
        buf[len] = '\0';

        long fd = tool_syscall(SYS_OPEN, (uintptr_t)"/etc/timezone.tmp",
                               VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC, 0644);
        if (fd < 0) {
            return tool_sys_error("date", "cannot open temporary timezone file", "/etc/timezone.tmp", fd);
        }
        size_t written = 0;
        while (written < len) {
            long n = tool_syscall(SYS_WRITE, fd, (uintptr_t)(buf + written), len - written);
            if (n <= 0) {
                tool_syscall(SYS_CLOSE, fd, 0, 0);
                tool_syscall(SYS_UNLINK, (uintptr_t)"/etc/timezone.tmp", 0, 0);
                return tool_error("date", "failed to write temporary timezone file", "/etc/timezone.tmp");
            }
            written += (size_t)n;
        }
        tool_syscall(SYS_CLOSE, fd, 0, 0);

        long r = tool_syscall(SYS_RENAME_REPLACE, (uintptr_t)"/etc/timezone.tmp",
                              (uintptr_t)"/etc/timezone", 0);
        if (r < 0) {
            r = tool_syscall(SYS_RENAME, (uintptr_t)"/etc/timezone.tmp",
                             (uintptr_t)"/etc/timezone", 0);
        }
        if (r < 0) {
            tool_syscall(SYS_UNLINK, (uintptr_t)"/etc/timezone.tmp", 0, 0);
            return tool_sys_error("date", "failed to update /etc/timezone", "/etc/timezone", r);
        }

        char msg[160];
        size_t mlen = 0;
        append_str(msg, &mlen, "Timezone set to ");
        append_str(msg, &mlen, tz);
        msg[mlen++] = '.';
        msg[mlen++] = '\n';
        return tool_write("date", msg, mlen);
    }

    for (int i = 1; i < argc; i++) {
        if (tool_equal(argv[i], "--help") || tool_equal(argv[i], "-h")) {
            return tool_write("date", s_date_help, tool_length(s_date_help));
        } else if (tool_equal(argv[i], "-u") || tool_equal(argv[i], "--utc") ||
                   tool_equal(argv[i], "--universal")) {
            utc = true;
        } else if (tool_equal(argv[i], "-I") || tool_equal(argv[i], "--iso-8601")) {
            iso8601 = true;
        } else if (tool_equal(argv[i], "-R") || tool_equal(argv[i], "--rfc-2822")) {
            rfc2822 = true;
        } else if (argv[i][0] == '+') {
            if (format) {
                return tool_error("date", "multiple format strings not allowed", argv[i]);
            }
            format = argv[i] + 1;
        } else {
            return tool_error("date", "unrecognized option", argv[i]);
        }
    }

    /* Validate format specifiers if custom format string is supplied */
    bool needs_calendar = (format == NULL);
    if (format) {
        for (const char *p = format; *p; p++) {
            if (*p == '%') {
                p++;
                if (*p == 'Y' || *p == 'm' || *p == 'd' || *p == 'H' || *p == 'M' || *p == 'S') {
                    needs_calendar = true;
                } else if (*p == 's') {
                    /* epoch only */
                } else if (*p == '%') {
                    /* literal % */
                } else {
                    char bad[3] = {'%', *p, '\0'};
                    return tool_error("date", "unsupported format specifier", bad[1] ? bad : "%");
                }
            }
        }
    }

    timespec_t ts;
    long ret = tool_syscall(SYS_CLOCK_GETTIME, CLOCK_REALTIME, (uintptr_t)&ts, 0);
    if (ret < 0) {
        return tool_sys_error("date", "cannot query system time", NULL, ret);
    }

    uint32_t year = 0;
    uint8_t month = 0, day = 0, hour = 0, min = 0, sec = 0, wday = 0;
    tz_result_t zone = {.abbreviation = "UTC"};
    char numeric[16] = {0};
    size_t np = 0;

    if (needs_calendar) {
        if (!utc && !tz_resolve(envp, ts.tv_sec, &zone))
            return tool_error("date", "invalid or unavailable timezone", NULL);
        tz_calendar_t cal;
        if (ts.tv_sec < -62167125600LL || ts.tv_sec > 253402207199LL ||
            !tz_calendar(ts.tv_sec + zone.offset, &cal))
            return tool_error("date", "date outside supported calendar", NULL);
        year = cal.year;
        month = cal.month;
        day = cal.day;
        hour = cal.hour;
        min = cal.minute;
        sec = cal.second;
        wday = cal.weekday;

        int off = zone.offset;
        numeric[np++] = off < 0 ? '-' : '+';
        if (off < 0)
            off = -off;
        append_two_digits(numeric, &np, off / 3600);
        numeric[np++] = ':';
        append_two_digits(numeric, &np, off / 60 % 60);
        if (off % 60) {
            if (rfc2822 && !iso8601 && !format)
                return tool_error("date", "RFC 2822 cannot represent a sub-minute zone offset", NULL);
            numeric[np++] = ':';
            append_two_digits(numeric, &np, off % 60);
        }
        numeric[np] = 0;
    }

    if (format) {
        char out[512];
        size_t pos = 0;
        for (const char *p = format; *p; p++) {
            if (*p == '%') {
                p++;
                if (*p == 'Y') {
                    append_four_digits(out, &pos, year);
                } else if (*p == 'm') {
                    append_two_digits(out, &pos, month);
                } else if (*p == 'd') {
                    append_two_digits(out, &pos, day);
                } else if (*p == 'H') {
                    append_two_digits(out, &pos, hour);
                } else if (*p == 'M') {
                    append_two_digits(out, &pos, min);
                } else if (*p == 'S') {
                    append_two_digits(out, &pos, sec);
                } else if (*p == 's') {
                    uint64_t magnitude = (uint64_t)ts.tv_sec;
                    if (ts.tv_sec < 0) {
                        out[pos++] = '-';
                        magnitude = (uint64_t)(-(ts.tv_sec + 1)) + 1;
                    }
                    pos += tool_format_u64(out + pos, magnitude);
                } else if (*p == '%') {
                    out[pos++] = '%';
                }
            } else {
                out[pos++] = *p;
            }
            if (pos >= sizeof(out) - 64) {
                return tool_error("date", "formatted date exceeds buffer limit", NULL);
            }
        }
        out[pos++] = '\n';
        return tool_write("date", out, pos);
    }

    char out[128];
    size_t pos = 0;

    if (iso8601) {
        /* YYYY-MM-DDTHH:MM:SSZ */
        append_four_digits(out, &pos, year);
        out[pos++] = '-';
        append_two_digits(out, &pos, month);
        out[pos++] = '-';
        append_two_digits(out, &pos, day);
        out[pos++] = 'T';
        append_two_digits(out, &pos, hour);
        out[pos++] = ':';
        append_two_digits(out, &pos, min);
        out[pos++] = ':';
        append_two_digits(out, &pos, sec);
        if (!zone.offset)
            out[pos++] = 'Z';
        else
            append_str(out, &pos, numeric);
        out[pos++] = '\n';
    } else if (rfc2822) {
        /* Day, DD Mon YYYY HH:MM:SS +0000 */
        append_str(out, &pos, s_weekdays[wday]);
        append_str(out, &pos, ", ");
        append_two_digits(out, &pos, day);
        out[pos++] = ' ';
        append_str(out, &pos, s_months[month - 1]);
        out[pos++] = ' ';
        append_four_digits(out, &pos, year);
        out[pos++] = ' ';
        append_two_digits(out, &pos, hour);
        out[pos++] = ':';
        append_two_digits(out, &pos, min);
        out[pos++] = ':';
        append_two_digits(out, &pos, sec);
        out[pos++] = ' ';
        for (size_t i = 0; i < np; i++)
            if (numeric[i] != ':')
                out[pos++] = numeric[i];
        out[pos++] = '\n';
    } else {
        /* Standard: Day Mon DD HH:MM:SS UTC YYYY */
        append_str(out, &pos, s_weekdays[wday]);
        out[pos++] = ' ';
        append_str(out, &pos, s_months[month - 1]);
        out[pos++] = ' ';
        if (day < 10) {
            out[pos++] = ' ';
            out[pos++] = (char)('0' + day);
        } else {
            append_two_digits(out, &pos, day);
        }
        out[pos++] = ' ';
        append_two_digits(out, &pos, hour);
        out[pos++] = ':';
        append_two_digits(out, &pos, min);
        out[pos++] = ':';
        append_two_digits(out, &pos, sec);
        out[pos++] = ' ';
        append_str(out, &pos, zone.abbreviation);
        if (zone.offset && zone.abbreviation[0] == 'U' && zone.abbreviation[1] == 'T' &&
            zone.abbreviation[2] == 'C')
            append_str(out, &pos, numeric);
        out[pos++] = ' ';
        append_four_digits(out, &pos, year);
        out[pos++] = '\n';
    }

    return tool_write("date", out, pos);
}
