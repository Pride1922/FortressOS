#include "common.h"
#include "../timezone.h"

static const char *s_weekdays[7] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static const char *s_months[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

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
    bool utc=false;
    bool iso8601 = false;
    bool rfc2822 = false;
    bool print_epoch = false;

    for (int i = 1; i < argc; i++) {
        if (tool_equal(argv[i], "--help") || tool_equal(argv[i], "-h")) {
            const char *help =
                "usage: date [-u|--utc] [-I|--iso-8601] [-R|--rfc-2822] [+%s] [--help]\n"
                "Display current date and time (TZ, /etc/timezone, or UTC).\n"
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
            utc=true;
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
        size_t len=0;
        uint64_t magnitude=(uint64_t)ts.tv_sec;
        if(ts.tv_sec<0){buf[len++]='-';magnitude=(uint64_t)(-(ts.tv_sec+1))+1;}
        len += tool_format_u64(buf+len, magnitude);
        buf[len++] = '\n';
        return tool_write("date", buf, len);
    }

    uint32_t year = 0;
    uint8_t month = 0, day = 0, hour = 0, min = 0, sec = 0, wday = 0;
    tz_result_t zone = {.abbreviation = "UTC"};
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
    char numeric[16];
    size_t np = 0;
    int off = zone.offset;
    numeric[np++] = off < 0 ? '-' : '+';
    if (off < 0)
        off = -off;
    append_two_digits(numeric, &np, off / 3600);
    numeric[np++] = ':';
    append_two_digits(numeric, &np, off / 60 % 60);
    if(off%60) {
        if(rfc2822&&!iso8601)return tool_error("date","RFC 2822 cannot represent a sub-minute zone offset",NULL);
        numeric[np++]=':';append_two_digits(numeric,&np,off%60);
    }
    numeric[np] = 0;

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
        if(!zone.offset)out[pos++]='Z';else append_str(out,&pos,numeric);
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
        out[pos++]=' ';for(size_t i=0;i<np;i++)if(numeric[i]!=':')out[pos++]=numeric[i];out[pos++]='\n';
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
        out[pos++]=' ';append_str(out,&pos,zone.abbreviation);
        if(zone.offset && zone.abbreviation[0]=='U' && zone.abbreviation[1]=='T' && zone.abbreviation[2]=='C')append_str(out,&pos,numeric);
        out[pos++]=' ';
        append_four_digits(out, &pos, year);
        out[pos++] = '\n';
    }

    return tool_write("date", out, pos);
}
