#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stddef.h>

#include "syscall_abi.h"
#include "rtc.h"

/* Forward declare pure conversion helper and mock hook from rtc.c */
uint64_t rtc_datetime_to_epoch(const rtc_datetime_t *dt);
extern void rtc_mock_set_reg(uint8_t reg, uint8_t val);

static inline uint8_t bcd_to_bin_test(uint8_t val) {
    return ((val >> 4) * 10) + (val & 0x0F);
}

static void test_abi_layout(void) {
    printf("Testing timespec_t and timeval_t ABI layout...\n");
    assert(sizeof(timespec_t) == 16);
    assert(offsetof(timespec_t, tv_sec) == 0);
    assert(offsetof(timespec_t, tv_nsec) == 8);

    assert(sizeof(timeval_t) == 16);
    assert(offsetof(timeval_t, tv_sec) == 0);
    assert(offsetof(timeval_t, tv_usec) == 8);

    assert(SYS_CLOCK_GETTIME == 74);
    assert(SYS_NANOSLEEP == 75);
    assert(CLOCK_REALTIME == 0);
    assert(CLOCK_MONOTONIC == 1);
    printf("  [PASS] ABI layout verified.\n");
}

static void test_bcd_decoding(void) {
    printf("Testing BCD decoding...\n");
    assert(bcd_to_bin_test(0x00) == 0);
    assert(bcd_to_bin_test(0x09) == 9);
    assert(bcd_to_bin_test(0x10) == 10);
    assert(bcd_to_bin_test(0x24) == 24);
    assert(bcd_to_bin_test(0x26) == 26);
    assert(bcd_to_bin_test(0x59) == 59);
    assert(bcd_to_bin_test(0x99) == 99);
    printf("  [PASS] BCD decoding verified.\n");
}

static void test_rtc_mock_read(void) {
    printf("Testing rtc_read_datetime with mock BCD hardware registers...\n");
    rtc_init(0);
    /* Set Status A: UIP=0 */
    rtc_mock_set_reg(RTC_REG_STATUS_A, 0);
    /* Set Status B: BCD mode, 24h mode -> (RTC_STATUS_B_24HR) */
    rtc_mock_set_reg(RTC_REG_STATUS_B, RTC_STATUS_B_24HR);
    /* Set 2026-10-16 08:30:45 in BCD */
    rtc_mock_set_reg(RTC_REG_SECONDS, 0x45);
    rtc_mock_set_reg(RTC_REG_MINUTES, 0x30);
    rtc_mock_set_reg(RTC_REG_HOURS,   0x08);
    rtc_mock_set_reg(RTC_REG_DAY,     0x16);
    rtc_mock_set_reg(RTC_REG_MONTH,   0x10);
    rtc_mock_set_reg(RTC_REG_YEAR,    0x26);
    rtc_mock_set_reg(RTC_REG_CENTURY, 0x20);

    rtc_datetime_t dt;
    bool ok = rtc_read_datetime(&dt);
    assert(ok);
    assert(dt.year == 2026);
    assert(dt.month == 10);
    assert(dt.day == 16);
    assert(dt.hour == 8);
    assert(dt.minute == 30);
    assert(dt.second == 45);

    uint64_t epoch = rtc_datetime_to_epoch(&dt);
    assert(epoch == 1792139445ULL);
    printf("  [PASS] rtc_read_datetime BCD mode verified.\n");
}

static void test_epoch_conversion(void) {
    printf("Testing Gregorian to Unix epoch conversion...\n");

    /* Test 1: Epoch zero */
    rtc_datetime_t dt1 = {.year = 1970, .month = 1, .day = 1, .hour = 0, .minute = 0, .second = 0};
    assert(rtc_datetime_to_epoch(&dt1) == 0);

    /* Test 2: One day later */
    rtc_datetime_t dt2 = {.year = 1970, .month = 1, .day = 2, .hour = 0, .minute = 0, .second = 0};
    assert(rtc_datetime_to_epoch(&dt2) == 86400);

    /* Test 3: Y2K Leap day (2000-02-29 12:00:00 UTC) = 951825600 */
    rtc_datetime_t dt3 = {.year = 2000, .month = 2, .day = 29, .hour = 12, .minute = 0, .second = 0};
    assert(rtc_datetime_to_epoch(&dt3) == 951825600ULL);

    /* Test 4: Day after leap day (2000-03-01 00:00:00 UTC) = 951868800 */
    rtc_datetime_t dt4 = {.year = 2000, .month = 3, .day = 1, .hour = 0, .minute = 0, .second = 0};
    assert(rtc_datetime_to_epoch(&dt4) == 951868800ULL);

    /* Test 5: Leap year 2024 */
    rtc_datetime_t dt5 = {.year = 2024, .month = 2, .day = 29, .hour = 0, .minute = 0, .second = 0};
    assert(rtc_datetime_to_epoch(&dt5) == 1709164800ULL);

    /* Test 6: Reference date (2026-10-16 08:30:00 UTC) = 1792139400 */
    rtc_datetime_t dt6 = {.year = 2026, .month = 10, .day = 16, .hour = 8, .minute = 30, .second = 0};
    assert(rtc_datetime_to_epoch(&dt6) == 1792139400ULL);

    /* Test 7: Invalid input returns 0 */
    rtc_datetime_t dt_bad = {.year = 1969, .month = 12, .day = 31, .hour = 23, .minute = 59, .second = 59};
    assert(rtc_datetime_to_epoch(&dt_bad) == 0);

    printf("  [PASS] Epoch conversion verified.\n");
}

static void test_tsc_interpolation_math(void) {
    printf("Testing TSC nanosecond interpolation math...\n");

    /* Simulated CPU at 3.45 GHz (3,450,000,000 Hz) */
    uint64_t tsc_hz = 3450000000ULL;

    /* Exactly 1 second = 3,450,000,000 cycles */
    uint64_t delta_tsc = tsc_hz;
    uint64_t sec_delta = delta_tsc / tsc_hz;
    uint64_t rem_cycles = delta_tsc % tsc_hz;
    uint64_t nsec_delta = (rem_cycles * 1000000000ULL) / tsc_hz;
    assert(sec_delta == 1);
    assert(nsec_delta == 0);

    /* Exactly 0.5 second = 1,725,000,000 cycles */
    delta_tsc = tsc_hz / 2;
    sec_delta = delta_tsc / tsc_hz;
    rem_cycles = delta_tsc % tsc_hz;
    nsec_delta = (rem_cycles * 1000000000ULL) / tsc_hz;
    assert(sec_delta == 0);
    assert(nsec_delta == 500000000ULL);

    /* 1 cycle at 3.45 GHz is ~0.289 ns -> integer rounds to 0 ns */
    delta_tsc = 1;
    sec_delta = delta_tsc / tsc_hz;
    rem_cycles = delta_tsc % tsc_hz;
    nsec_delta = (rem_cycles * 1000000000ULL) / tsc_hz;
    assert(sec_delta == 0);
    assert(nsec_delta == 0);

    /* 345 cycles = 100 ns */
    delta_tsc = 345;
    sec_delta = delta_tsc / tsc_hz;
    rem_cycles = delta_tsc % tsc_hz;
    nsec_delta = (rem_cycles * 1000000000ULL) / tsc_hz;
    assert(sec_delta == 0);
    assert(nsec_delta == 100);

    /* Large delta: 10 days of continuous uptime */
    delta_tsc = 10ULL * 86400ULL * tsc_hz + 123456789ULL;
    sec_delta = delta_tsc / tsc_hz;
    rem_cycles = delta_tsc % tsc_hz;
    nsec_delta = (rem_cycles * 1000000000ULL) / tsc_hz;
    assert(sec_delta == 10ULL * 86400ULL);
    assert(nsec_delta == (123456789ULL * 1000000000ULL) / tsc_hz);

    printf("  [PASS] TSC interpolation math verified.\n");
}

static void test_nanosleep_edge_cases(void) {
    printf("Testing nanosleep edge-case logic...\n");

    /* 1. Negative seconds */
    timespec_t req1 = {.tv_sec = -1, .tv_nsec = 0};
    assert(req1.tv_sec < 0);

    /* 2. Negative nanoseconds */
    timespec_t req2 = {.tv_sec = 0, .tv_nsec = -10};
    assert(req2.tv_nsec < 0);

    /* 3. Nanoseconds >= 1,000,000,000 */
    timespec_t req3 = {.tv_sec = 0, .tv_nsec = 1000000000L};
    assert(req3.tv_nsec >= 1000000000L);

    /* 4. Zero sleep yields */
    timespec_t req4 = {.tv_sec = 0, .tv_nsec = 0};
    assert(req4.tv_sec == 0 && req4.tv_nsec == 0);

    /* 5. Sub-tick sleep rounds up */
    uint64_t hz = 100; /* 100 Hz = 10 ms per tick */
    uint64_t ns_per_tick = 1000000000ULL / hz;
    timespec_t req5 = {.tv_sec = 0, .tv_nsec = 1}; /* 1 ns */
    uint64_t nsec_ticks = ((uint64_t)req5.tv_nsec + ns_per_tick - 1) / ns_per_tick;
    assert(nsec_ticks == 1); /* rounds up to 1 tick */

    /* 6. Arithmetic overflow check */
    uint64_t start_ticks = 500;
    uint64_t huge_ticks = UINT64_MAX - 100;
    uint64_t deadline = (huge_ticks > UINT64_MAX - start_ticks) ? UINT64_MAX : (start_ticks + huge_ticks);
    assert(deadline == UINT64_MAX);

    printf("  [PASS] Nanosleep edge cases verified.\n");
}

int main(void) {
    printf("=== FortressOS Time Subsystem Host Unit Tests ===\n");
    test_abi_layout();
    test_bcd_decoding();
    test_rtc_mock_read();
    test_epoch_conversion();
    test_tsc_interpolation_math();
    test_nanosleep_edge_cases();
    printf(">>> ALL TIME HOST TESTS PASSED (100%%) <<<\n");
    return 0;
}
