#ifndef FORTRESS_RTC_H
#define FORTRESS_RTC_H

#include "types.h"

/* CMOS RTC Hardware Ports */
#define RTC_PORT_INDEX 0x70
#define RTC_PORT_DATA  0x71

/* CMOS RTC Standard Registers */
#define RTC_REG_SECONDS      0x00
#define RTC_REG_MINUTES      0x02
#define RTC_REG_HOURS        0x04
#define RTC_REG_WEEKDAY      0x06
#define RTC_REG_DAY          0x07
#define RTC_REG_MONTH        0x08
#define RTC_REG_YEAR         0x09
#define RTC_REG_STATUS_A     0x0A
#define RTC_REG_STATUS_B     0x0B
#define RTC_REG_CENTURY      0x32

/* Status Register A Flags */
#define RTC_STATUS_A_UIP     (1u << 7) /* Update In Progress */

/* Status Register B Flags */
#define RTC_STATUS_B_24HR    (1u << 1) /* 1 = 24-hour mode, 0 = 12-hour mode */
#define RTC_STATUS_B_BINARY  (1u << 2) /* 1 = Binary format, 0 = BCD format */

typedef struct {
    uint32_t year;    /* 4-digit year, e.g. 2026 */
    uint8_t  month;   /* 1 .. 12 */
    uint8_t  day;     /* 1 .. 31 */
    uint8_t  hour;    /* 0 .. 23 */
    uint8_t  minute;  /* 0 .. 59 */
    uint8_t  second;  /* 0 .. 60 */
} rtc_datetime_t;

/* Initialize RTC driver (checks optional ACPI FADT century register). */
void rtc_init(uint8_t fadt_century_reg);

/* Read atomic, stable calendar date and time from CMOS RTC.
 * Returns true if successful and values are in valid ranges. */
bool rtc_read_datetime(rtc_datetime_t *dt);

/* Convert Gregorian date/time to Unix epoch seconds (seconds since 1970-01-01 00:00:00 UTC).
 * Pure arithmetic helper, callable in kernel and host test suites. */
uint64_t rtc_datetime_to_epoch(const rtc_datetime_t *dt);

/* Convenience: reads RTC and returns epoch seconds. Returns false on failure. */
bool rtc_get_epoch_seconds(uint64_t *out_epoch_sec);

#endif /* FORTRESS_RTC_H */
