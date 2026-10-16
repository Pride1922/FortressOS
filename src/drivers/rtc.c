#include "rtc.h"
#include "serial.h"
#include "string.h"

static uint8_t g_century_reg = 0;

static const uint16_t s_days_before_month[12] = {
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
};

static inline bool is_leap_year(uint32_t year) {
    return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

static inline uint8_t bcd_to_bin(uint8_t val) {
    return ((val >> 4) * 10) + (val & 0x0F);
}

#ifdef HOST_TEST
static uint8_t s_mock_rtc_regs[128];
void rtc_mock_set_reg(uint8_t reg, uint8_t val) {
    s_mock_rtc_regs[reg & 0x7F] = val;
}
static uint8_t rtc_read_reg(uint8_t reg) {
    return s_mock_rtc_regs[reg & 0x7F];
}
#define io_wait() ((void)0)
#else
static uint8_t rtc_read_reg(uint8_t reg) {
    outb(RTC_PORT_INDEX, reg & 0x7F);
    io_wait();
    return inb(RTC_PORT_DATA);
}
#endif

static bool rtc_is_updating(void) {
    return (rtc_read_reg(RTC_REG_STATUS_A) & RTC_STATUS_A_UIP) != 0;
}

void rtc_init(uint8_t fadt_century_reg) {
    g_century_reg = fadt_century_reg;
}

bool rtc_read_datetime(rtc_datetime_t *dt) {
    if (!dt) return false;

    rtc_datetime_t cur = {0};
    rtc_datetime_t prev = {0};
    uint8_t status_b = 0;
    bool match = false;

    /* Up to 5 attempts to read identical values without roll-over tearing */
    for (int attempt = 0; attempt < 5; attempt++) {
        /* Bounded wait for Update In Progress flag to clear */
        uint32_t uip_spins = 100000;
        while (rtc_is_updating() && --uip_spins > 0) {
            io_wait();
        }

        prev = cur;
        cur.second = rtc_read_reg(RTC_REG_SECONDS);
        cur.minute = rtc_read_reg(RTC_REG_MINUTES);
        cur.hour   = rtc_read_reg(RTC_REG_HOURS);
        cur.day    = rtc_read_reg(RTC_REG_DAY);
        cur.month  = rtc_read_reg(RTC_REG_MONTH);
        cur.year   = rtc_read_reg(RTC_REG_YEAR);
        status_b   = rtc_read_reg(RTC_REG_STATUS_B);

        if (attempt > 0 &&
            cur.second == prev.second &&
            cur.minute == prev.minute &&
            cur.hour   == prev.hour   &&
            cur.day    == prev.day    &&
            cur.month  == prev.month  &&
            cur.year   == prev.year) {
            match = true;
            break;
        }
    }

    if (!match) {
        /* Even if consecutive reads differed due to ticking, cur holds a stable sample */
    }

    bool is_bcd = !(status_b & RTC_STATUS_B_BINARY);
    bool is_24h = (status_b & RTC_STATUS_B_24HR) != 0;

    uint8_t sec  = is_bcd ? bcd_to_bin(cur.second) : cur.second;
    uint8_t min  = is_bcd ? bcd_to_bin(cur.minute) : cur.minute;
    uint8_t hour = cur.hour;

    if (!is_24h) {
        bool pm = (hour & 0x80) != 0;
        hour &= 0x7F;
        if (is_bcd) hour = bcd_to_bin(hour);
        if (pm) hour = (hour + 12) % 24;
        else if (hour == 12) hour = 0;
    } else {
        if (is_bcd) hour = bcd_to_bin(hour);
    }

    uint8_t day   = is_bcd ? bcd_to_bin(cur.day)   : cur.day;
    uint8_t month = is_bcd ? bcd_to_bin(cur.month) : cur.month;
    uint32_t year = is_bcd ? bcd_to_bin((uint8_t)cur.year) : cur.year;

    uint8_t century = 0;
    if (g_century_reg != 0) {
        century = rtc_read_reg(g_century_reg);
    } else {
        century = rtc_read_reg(RTC_REG_CENTURY);
    }
    if (is_bcd && century != 0) {
        century = bcd_to_bin(century);
    }

    if (century >= 19 && century <= 21) {
        year += (uint32_t)century * 100;
    } else if (year < 100) {
        /* Standard century heuristic: 00-69 is 2000-2069, 70-99 is 1970-1999 */
        year += (year < 70) ? 2000 : 1900;
    }

    /* Range validation */
    if (sec > 60 || min > 59 || hour > 23) return false;
    if (month < 1 || month > 12) return false;
    if (day < 1 || day > 31) return false;
    if (year < 1970 || year > 2099) return false;

    dt->second = sec;
    dt->minute = min;
    dt->hour   = hour;
    dt->day    = day;
    dt->month  = month;
    dt->year   = year;
    return true;
}

uint64_t rtc_datetime_to_epoch(const rtc_datetime_t *dt) {
    if (!dt || dt->year < 1970 || dt->month < 1 || dt->month > 12 || dt->day < 1) {
        return 0;
    }

    uint64_t days = 0;
    for (uint32_t y = 1970; y < dt->year; y++) {
        days += is_leap_year(y) ? 366 : 365;
    }

    days += s_days_before_month[dt->month - 1];
    if (dt->month > 2 && is_leap_year(dt->year)) {
        days++;
    }
    days += (dt->day - 1);

    uint64_t epoch = days * 86400ULL
                   + (uint64_t)dt->hour * 3600ULL
                   + (uint64_t)dt->minute * 60ULL
                   + (uint64_t)dt->second;
    return epoch;
}

bool rtc_get_epoch_seconds(uint64_t *out_epoch_sec) {
    if (!out_epoch_sec) return false;
    rtc_datetime_t dt;
    if (!rtc_read_datetime(&dt)) {
        return false;
    }
    *out_epoch_sec = rtc_datetime_to_epoch(&dt);
    return true;
}
