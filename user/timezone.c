#include "timezone.h"

static bool leap(int y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }
bool tz_calendar(int64_t s, tz_calendar_t *o) {
    if (!o || s < -62167219200LL || s > 253402300799LL)
        return false;
    int64_t d = s / 86400, r = s % 86400;
    if (r < 0) {
        r += 86400;
        --d;
    }
    o->hour = r / 3600;
    o->minute = r / 60 % 60;
    o->second = r % 60;
    o->weekday = (int)((d + 4) % 7);
    if (o->weekday < 0)
        o->weekday += 7;
    /* Gregorian 400-year eras, bounded even at the supported endpoints. */
    int64_t z = d + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = (int)(yoe + era * 400);
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    o->day = doy - (153 * mp + 2) / 5 + 1;
    o->month = mp + (mp < 10 ? 3 : -9);
    o->year = y + (o->month <= 2);
    return true;
}
static int64_t days(int y, int m, int d) {
    y -= m <= 2;
    int e = (y >= 0 ? y : y - 399) / 400;
    unsigned a = y - e * 400;
    unsigned b = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    return (int64_t)e * 146097 + a * 365 + a / 4 - a / 100 + b - 719468;
}
static bool number(const char **p, int max, int *out) {
    const char *s = *p;
    int v = 0, n = 0;
    while (*s >= '0' && *s <= '9') {
        if (++n > 3)
            return false;
        v = v * 10 + *s++ - '0';
    }
    if (!n || v > max)
        return false;
    *p = s;
    *out = v;
    return true;
}
static bool hms(const char **p, int max, int *out) {
    int sign = 1, h, m = 0, s = 0;
    if (**p == '-' || **p == '+') {
        if (*(*p)++ == '-')
            sign = -1;
    }
    if (!number(p, max, &h))
        return false;
    if (**p == ':') {
        ++*p;
        if (!number(p, 59, &m))
            return false;
        if (**p == ':') {
            ++*p;
            if (!number(p, 59, &s))
                return false;
        }
    }
    *out = sign * (h * 3600 + m * 60 + s);
    return true;
}
static bool label(const char **p, char *dst) {
    bool quoted = **p == '<';
    if (quoted)
        ++*p;
    int n = 0;
    while (**p &&
           (quoted ? **p != '>' : ((**p >= 'A' && **p <= 'Z') || (**p >= 'a' && **p <= 'z')))) {
        char c = *(*p)++;
        if (n >= 31 || !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                         (quoted && (c == '+' || c == '-' || (c >= '0' && c <= '9')))))
            return false;
        dst[n++] = c;
    }
    if (n < 3)
        return false;
    dst[n] = 0;
    if (quoted) {
        if (**p != '>')
            return false;
        ++*p;
    }
    return true;
}
typedef struct {
    int kind, a, b, c, at;
} rule_t;
static bool rule(const char **p, rule_t *r) {
    *r = (rule_t){.at = 7200};
    if (**p == 'M') {
        ++*p;
        r->kind = 2;
        if (!number(p, 12, &r->a) || r->a < 1 || *(*p)++ != '.' || !number(p, 5, &r->b) ||
            r->b < 1 || *(*p)++ != '.' || !number(p, 6, &r->c))
            return false;
    } else {
        r->kind = **p == 'J';
        if (r->kind)
            ++*p;
        if (!number(p, r->kind ? 365 : 365, &r->a) || (r->kind && r->a < 1))
            return false;
    }
    if (**p == '/') {
        ++*p;
        if (!hms(p, 167, &r->at))
            return false;
    }
    return true;
}
static int64_t transition(int y, const rule_t *r, int offset) {
    int64_t d = days(y, 1, 1);
    if (r->kind == 2) {
        d = days(y, r->a, 1);
        int w = (int)((d + 4) % 7);
        if (w < 0)
            w += 7;
        int dom = 1 + (r->c - w + 7) % 7 + (r->b - 1) * 7;
        static const int md[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        int limit = md[r->a - 1] + (r->a == 2 && leap(y));
        if (dom > limit)
            dom -= 7;
        d += dom - 1;
    } else
        d += r->a - (r->kind ? 1 : 0) + (r->kind && r->a >= 60 && leap(y));
    return d * 86400 + r->at - offset;
}
bool tz_posix(const char *p, int64_t s, tz_result_t *out) {
    if (!p || !out)
        return false;
    tz_result_t t = {0};
    char dst[32] = {0};
    int east;
    if (!*p) {
        t.abbreviation[0] = 'U';
        t.abbreviation[1] = 'T';
        t.abbreviation[2] = 'C';
        *out = t;
        return true;
    }
    /* Bare signed offsets use ISO sign; named POSIX zones use POSIX sign. */
    if (*p == '+' || *p == '-') {
        if (!hms(&p, 24, &east) || *p || east > 86400 || east < -86400)
            return false;
        t.offset = east;
        t.abbreviation[0] = 'U';
        t.abbreviation[1] = 'T';
        t.abbreviation[2] = 'C';
        *out = t;
        return true;
    }
    if (!label(&p, t.abbreviation))
        return false;
    if (!*p) {
        if (t.abbreviation[0] != 'U' || t.abbreviation[1] != 'T' || t.abbreviation[2] != 'C' ||
            t.abbreviation[3])
            return false;
        *out = t;
        return true;
    }
    if (!hms(&p, 24, &east))
        return false;
    t.offset = -east;
    if (!*p) {
        *out = t;
        return true;
    }
    if (!label(&p, dst))
        return false;
    int dstoff = t.offset + 3600;
    if (*p != ',') {
        if (!hms(&p, 24, &east))
            return false;
        dstoff = -east;
    }
    rule_t a, b;
    if (*p++ != ',' || !rule(&p, &a) || *p++ != ',' || !rule(&p, &b) || *p)
        return false;
    tz_calendar_t c;
    if (!tz_calendar(s, &c))
        return false;
    /* Extended rule times and offsets can move an event across New Year.
     * Select the latest event from adjacent years rather than assuming both
     * transitions occur inside the UTC calendar year being displayed. */
    int64_t latest = INT64_MIN;
    for (int year = c.year - 1; year <= c.year + 1; year++) {
        int64_t start = transition(year, &a, t.offset), end = transition(year, &b, dstoff);
        if (start <= s && start > latest) {
            latest = start;
            t.daylight = true;
        }
        if (end <= s && end >= latest) {
            latest = end;
            t.daylight = false;
        }
    }
    if (t.daylight) {
        t.offset = dstoff;
        for (int i = 0; i < 32; i++)
            t.abbreviation[i] = dst[i];
    }
    *out = t;
    return true;
}
bool tz_name(const char *p) {
    if (!p || !*p || *p == '/')
        return false;
    int n = 0, part = 0;
    for (; *p; p++) {
        if (++n > 127)
            return false;
        if (*p == '/') {
            if (!part)
                return false;
            part = 0;
            continue;
        }
        if (*p == '.')
            return false;
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
              *p == '_' || *p == '+' || *p == '-'))
            return false;
        ++part;
    }
    return part > 0;
}
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static int64_t stamp(const uint8_t *p, int w) {
    uint64_t v = 0;
    for (int i = 0; i < w; i++)
        v = v << 8 | p[i];
    if (w == 4)
        return (int32_t)v;
    return v <= INT64_MAX ? (int64_t)v : -1 - (int64_t)(~v);
}
static bool block(const uint8_t *p, size_t n, int w, size_t *len) {
    if (n < 44 || p[0] != 'T' || p[1] != 'Z' || p[2] != 'i' || p[3] != 'f')
        return false;
    uint32_t ut = be32(p + 20), st = be32(p + 24), lp = be32(p + 28), tc = be32(p + 32),
             ty = be32(p + 36), ch = be32(p + 40);
    if (tc > 4096 || !ty || ty > 256 || !ch || ch > 4096 || lp || (ut && ut != ty) ||
        (st && st != ty))
        return false;
    *len = 44 + (size_t)tc * (w + 1) + (size_t)ty * 6 + ch + st + ut;
    return *len <= n;
}
bool tz_tzif(const uint8_t *p, size_t n, int64_t s, tz_result_t *out) {
    if (!p || !out)
        return false;
    size_t len;
    int w = 4;
    if (!block(p, n, w, &len))
        return false;
    if (p[4] == '2' || p[4] == '3' || p[4] == '4') {
        uint8_t version = p[4];
        p += len;
        n -= len;
        w = 8;
        if (!block(p, n, w, &len) || p[4] != version)
            return false;
    } else if (p[4] != 0)
        return false;
    uint32_t tc = be32(p + 32), ty = be32(p + 36), ch = be32(p + 40);
    const uint8_t *times = p + 44, *ix = times + tc * w, *types = ix + tc, *chars = types + ty * 6;
    int selected = 0;
    int64_t prev = 0;
    for (uint32_t i = 0; i < tc; i++) {
        int64_t v = stamp(times + i * w, w);
        if ((i && v <= prev) || ix[i] >= ty)
            return false;
        if (v <= s)
            selected = ix[i];
        prev = v;
    }
    for (uint32_t i = 0; i < ty; i++) {
        if (types[6 * i + 4] > 1 || types[6 * i + 5] >= ch)
            return false;
        uint32_t j = types[6 * i + 5];
        while (j < ch && chars[j]) {
            ++j;
        }
        if (j == ch)
            return false;
    }
    if (w == 8) {
        if (n < len + 2 || p[len] != '\n' || p[n - 1] != '\n')
            return false;
        char footer[256];
        size_t k = 0;
        while (len + 1 + k < n && p[len + 1 + k] != '\n') {
            if (k == 255)
                return false;
            footer[k] = (char)p[len + 1 + k];
            ++k;
        }
        if (len + 2 + k != n)
            return false;
        footer[k] = 0;
        if (k) {
            tz_result_t future;
            if (!tz_posix(footer, s, &future))
                return false;
            if (!tc || s > prev) {
                *out = future;
                return true;
            }
        }
    }
    tz_result_t t = {0};
    t.offset = (int32_t)be32(types + selected * 6);
    t.daylight = types[selected * 6 + 4];
    if (t.offset < -89999 || t.offset > 93599)
        return false;
    uint32_t j = types[selected * 6 + 5];
    size_t k = 0;
    while (chars[j]) {
        if (k == 31 || chars[j] < 33 || chars[j] > 126)
            return false;
        t.abbreviation[k++] = (char)chars[j++];
    }
    *out = t;
    return true;
}
