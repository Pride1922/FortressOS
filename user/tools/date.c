#include "common.h"

static const char *s_weekdays[7] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static const char *s_months[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static inline bool is_leap_year(uint32_t year) {
    return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

static void epoch_to_datetime(uint64_t epoch, uint32_t *out_year, uint8_t *out_month,
                             uint8_t *out_day, uint8_t *out_hour, uint8_t *out_min,
                             uint8_t *out_sec, uint8_t *out_wday) {
    uint64_t days = epoch / 86400ULL;
    uint32_t sec_in_day = (uint32_t)(epoch % 86400ULL);

    *out_hour = (uint8_t)(sec_in_day / 3600);
    *out_min  = (uint8_t)((sec_in_day % 3600) / 60);
    *out_sec  = (uint8_t)(sec_in_day % 60);
    *out_wday = (uint8_t)((days + 4) % 7); /* 1970-01-01 was Thursday = 4 */

    uint32_t year = 1970;
    for (;;) {
        uint32_t diy = is_leap_year(year) ? 366 : 365;
        if (days < diy) break;
        days -= diy;
        year++;
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
}

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

int date_main(int argc, char **argv) {
    bool iso8601 = false;
    bool rfc2822 = false;
    bool print_epoch = false;

    for (int i = 1; i < argc; i++) {
        if (tool_equal(argv[i], "--help") || tool_equal(argv[i], "-h")) {
            const char *help =
                "usage: date [-u|--utc] [-I|--iso-8601] [-R|--rfc-2822] [+%s] [--help]\n"
                "Display current date and time.\n"
                "\n"
                "Options:\n"
                "  -u, --utc, --universal  print Universal Coordinated Time (UTC)\n"
                "  -I, --iso-8601          output date/time in ISO 8601 format\n"
                "  -R, --rfc-2822          output date/time in RFC 2822 format\n"
                "  +%s                     output seconds since 1970-01-01 00:00:00 UTC\n"
                "      --help              display this help and exit\n";
            return tool_write("date", help, tool_length(help));
        } else if (tool_equal(argv[i], "-u") || tool_equal(argv[i], "--utc") ||
                   tool_equal(argv[i], "--universal")) {
            /* FortressOS currently operates in UTC */
        } else if (tool_equal(argv[i], "-I") || tool_equal(argv[i], "--iso-8601")) {
            iso8601 = true;
        } else if (tool_equal(argv[i], "-R") || tool_equal(argv[i], "--rfc-2822")) {
            rfc2822 = true;
        } else if (tool_equal(argv[i], "+%s")) {
            print_epoch = true;
        } else {
            return tool_error("date", "unrecognized option", argv[i]);
        }
    }

    timespec_t ts;
    long ret = tool_syscall(SYS_CLOCK_GETTIME, CLOCK_REALTIME, (uintptr_t)&ts, 0);
    if (ret < 0) {
        return tool_sys_error("date", "cannot query system time", NULL, ret);
    }

    if (print_epoch) {
        char buf[32];
        size_t len = tool_format_u64(buf, (uint64_t)ts.tv_sec);
        buf[len++] = '\n';
        return tool_write("date", buf, len);
    }

    uint32_t year = 0;
    uint8_t month = 0, day = 0, hour = 0, min = 0, sec = 0, wday = 0;
    epoch_to_datetime((uint64_t)ts.tv_sec, &year, &month, &day, &hour, &min, &sec, &wday);

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
        out[pos++] = 'Z';
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
        append_str(out, &pos, " +0000\n");
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
        append_str(out, &pos, " UTC ");
        append_four_digits(out, &pos, year);
        out[pos++] = '\n';
    }

    return tool_write("date", out, pos);
}
