/* Reuse the existing validated timezone engine and runtime policy. */
#include "../timezone.h"
#include "../tools/timezone_io.c"

long tool_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}

static int64_t cached_minute = INT64_MIN;
static tz_result_t cached_zone;
static bool cached_valid;

void desktop_clock_invalidate(void) { cached_minute = INT64_MIN; }
bool desktop_zone_valid(const char *name, int64_t seconds) {
    tz_result_t zone;
    return tz_name(name) && tz_spec(name, seconds, &zone);
}
bool desktop_clock(int64_t seconds, int64_t *local, tz_calendar_t *calendar) {
    /* Runtime zone policy is refreshed at minute boundaries and immediately
       after a setting change. Calendar conversion remains a pure per-tick call. */
    int64_t minute = seconds / 60;
    if (seconds % 60 < 0) --minute;
    if (minute != cached_minute) {
        cached_valid = tz_resolve(NULL, seconds, &cached_zone);
        cached_minute = minute;
    }
    if (!cached_valid || (cached_zone.offset > 0 && seconds > INT64_MAX-cached_zone.offset)
        || (cached_zone.offset < 0 && seconds < INT64_MIN-cached_zone.offset)) return false;
    *local = seconds + cached_zone.offset;
    return tz_calendar(*local, calendar);
}
