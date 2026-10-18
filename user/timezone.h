#ifndef FORTRESS_TIMEZONE_H
#define FORTRESS_TIMEZONE_H
#include "types.h"
/* Pure, bounded userland calendar/TZif engine. Seconds are always Unix UTC. */
typedef struct {
    int year, month, day, hour, minute, second, weekday;
} tz_calendar_t;
typedef struct {
    int32_t offset;
    bool daylight;
    char abbreviation[32];
} tz_result_t;
bool tz_calendar(int64_t seconds, tz_calendar_t *out);
bool tz_posix(const char *spec, int64_t seconds, tz_result_t *out);
bool tz_name(const char *name);
bool tz_tzif(const uint8_t *data, size_t size, int64_t seconds, tz_result_t *out);
/* Runtime adapter: TZ > /etc/timezone > opt-in /run/timezone > UTC.
 * Invalid explicit settings fail; they never silently select another zone. */
bool tz_resolve(const char *const *envp, int64_t seconds, tz_result_t *out);
/* Resolve one fixed/POSIX/IANA specification through the same bounded reader. */
bool tz_spec(const char *spec, int64_t seconds, tz_result_t *out);
#endif
