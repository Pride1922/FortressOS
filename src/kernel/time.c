#include "poll.h"
#include "time.h"
#include "rtc.h"
#include "serial.h"
#include "apic.h"
#include "thread.h"
#include "string.h"

typedef struct {
    uint64_t base_epoch_sec;   /* Wall-clock epoch seconds at sync point */
    uint64_t base_epoch_nsec;  /* Wall-clock nanoseconds at sync point (0..999999999) */
    uint64_t base_tsc;         /* TSC cycle count at sync point */
    uint64_t base_bsp_ticks;   /* BSP 100 Hz timer ticks at sync point */
    uint64_t boot_tsc;         /* TSC cycle count at kernel startup */
    uint64_t tsc_hz;           /* Calibrated TSC frequency in Hz */
    bool     tsc_valid;        /* True if invariant TSC calibration succeeded */
    bool     initialized;      /* True once time_init() has run */
} kernel_timekeeper_t;

static kernel_timekeeper_t g_timekeeper;
/* A single atomic correction publishes a coherent wall-clock update on SMP.
 * Boot origins/calibration never change; monotonic and sleep are independent. */
static int64_t g_realtime_correction_ns;
static const int g_sleep_channel = 0x51EE9; /* "SLEEP" */

const void *time_get_sleep_channel(void) {
    return &g_sleep_channel;
}

void time_timer_tick(void) {
    poll_wake_notify();
}

void time_init(void) {
    memset(&g_timekeeper, 0, sizeof(g_timekeeper));
    __atomic_store_n(&g_realtime_correction_ns,0,__ATOMIC_RELAXED);

    /* Read wall-clock calendar from CMOS RTC */
    rtc_datetime_t dt;
    uint64_t epoch_sec = 0;
    bool rtc_ok = rtc_read_datetime(&dt);
    if (rtc_ok) {
        epoch_sec = rtc_datetime_to_epoch(&dt);
    } else {
        /* Fallback timestamp (2024-10-15 00:00:00 UTC) if RTC is unreadable */
        epoch_sec = 1728950400ULL;
    }

    uint64_t tsc_now = apic_poll_clock_read();
    uint64_t tsc_hz = apic_poll_clock_hz();
    uint64_t ticks = apic_timer_get_bsp_ticks();

    g_timekeeper.base_epoch_sec = epoch_sec;
    g_timekeeper.base_epoch_nsec = 0;
    g_timekeeper.base_tsc = tsc_now;
    g_timekeeper.boot_tsc = tsc_now;
    g_timekeeper.base_bsp_ticks = ticks;
    g_timekeeper.tsc_hz = tsc_hz;
    g_timekeeper.tsc_valid = (tsc_hz >= 100000000ULL && tsc_hz <= 10000000000ULL);
    g_timekeeper.initialized = true;

    serial_puts("[TIME] Wall clock initialized: ");
    if (rtc_ok) {
        serial_print_dec(dt.year);
        serial_putc('-');
        if (dt.month < 10) serial_putc('0');
        serial_print_dec(dt.month);
        serial_putc('-');
        if (dt.day < 10) serial_putc('0');
        serial_print_dec(dt.day);
        serial_putc(' ');
        if (dt.hour < 10) serial_putc('0');
        serial_print_dec(dt.hour);
        serial_putc(':');
        if (dt.minute < 10) serial_putc('0');
        serial_print_dec(dt.minute);
        serial_putc(':');
        if (dt.second < 10) serial_putc('0');
        serial_print_dec(dt.second);
        serial_puts(" UTC");
    } else {
        serial_puts("(fallback baseline)");
    }
    serial_puts(" epoch=");
    serial_print_dec(epoch_sec);
    serial_puts("s tsc_hz=");
    serial_print_dec(tsc_hz);
    serial_puts(g_timekeeper.tsc_valid ? " (TSC-interpolated)\n" : " (tick-fallback)\n");
}

static int time_get_uncorrected(uint32_t clk_id, timespec_t *ts) {
    if (!ts) return SYSCALL_EFAULT;

    if (!g_timekeeper.initialized) {
        ts->tv_sec = 0;
        ts->tv_nsec = 0;
        return 0;
    }

    switch (clk_id) {
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE: {
        if (g_timekeeper.tsc_valid && g_timekeeper.tsc_hz > 0) {
            uint64_t now_tsc = apic_poll_clock_read();
            uint64_t delta_tsc = (now_tsc >= g_timekeeper.base_tsc)
                               ? (now_tsc - g_timekeeper.base_tsc) : 0;
            uint64_t sec_delta = delta_tsc / g_timekeeper.tsc_hz;
            uint64_t rem_cycles = delta_tsc % g_timekeeper.tsc_hz;
            uint64_t nsec_delta = (rem_cycles * 1000000000ULL) / g_timekeeper.tsc_hz;

            uint64_t total_sec = g_timekeeper.base_epoch_sec + sec_delta;
            uint64_t total_nsec = g_timekeeper.base_epoch_nsec + nsec_delta;
            if (total_nsec >= 1000000000ULL) {
                total_sec += total_nsec / 1000000000ULL;
                total_nsec %= 1000000000ULL;
            }
            ts->tv_sec = (int64_t)total_sec;
            ts->tv_nsec = (int64_t)total_nsec;
        } else {
            /* Fallback to 100 Hz BSP timer ticks */
            uint64_t ticks = apic_timer_get_bsp_ticks();
            uint64_t delta_ticks = (ticks >= g_timekeeper.base_bsp_ticks)
                                 ? (ticks - g_timekeeper.base_bsp_ticks) : 0;
            uint64_t sec_delta = delta_ticks / 100;
            uint64_t nsec_delta = (delta_ticks % 100) * 10000000ULL;
            uint64_t total_sec = g_timekeeper.base_epoch_sec + sec_delta;
            uint64_t total_nsec = g_timekeeper.base_epoch_nsec + nsec_delta;
            if (total_nsec >= 1000000000ULL) {
                total_sec += total_nsec / 1000000000ULL;
                total_nsec %= 1000000000ULL;
            }
            ts->tv_sec = (int64_t)total_sec;
            ts->tv_nsec = (int64_t)total_nsec;
        }
        return 0;
    }

    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_BOOTTIME: {
        if (g_timekeeper.tsc_valid && g_timekeeper.tsc_hz > 0) {
            uint64_t now_tsc = apic_poll_clock_read();
            uint64_t delta_tsc = (now_tsc >= g_timekeeper.boot_tsc)
                               ? (now_tsc - g_timekeeper.boot_tsc) : 0;
            uint64_t sec = delta_tsc / g_timekeeper.tsc_hz;
            uint64_t rem_cycles = delta_tsc % g_timekeeper.tsc_hz;
            uint64_t nsec = (rem_cycles * 1000000000ULL) / g_timekeeper.tsc_hz;
            ts->tv_sec = (int64_t)sec;
            ts->tv_nsec = (int64_t)nsec;
        } else {
            uint64_t ticks = apic_timer_get_bsp_ticks();
            ts->tv_sec = (int64_t)(ticks / 100);
            ts->tv_nsec = (int64_t)((ticks % 100) * 10000000ULL);
        }
        return 0;
    }

    default:
        return SYSCALL_EINVAL;
    }
}

int time_get_timespec(uint32_t clk_id, timespec_t *ts) {
    int r = time_get_uncorrected(clk_id, ts);
    if (!r && (clk_id == CLOCK_REALTIME || clk_id == CLOCK_REALTIME_COARSE)) {
        int64_t correction = __atomic_load_n(&g_realtime_correction_ns, __ATOMIC_ACQUIRE);
        ts->tv_sec += correction / 1000000000LL;
        ts->tv_nsec += correction % 1000000000LL;
        if (ts->tv_nsec < 0) {
            --ts->tv_sec;
            ts->tv_nsec += 1000000000LL;
        }
        if (ts->tv_nsec >= 1000000000LL) {
            ++ts->tv_sec;
            ts->tv_nsec -= 1000000000LL;
        }
    }
    return r;
}

int time_set_realtime(const timespec_t *ts) {
    /* Bounded NTP era policy: Unix epoch through 2100-12-31. No RTC writes. */
    if (!ts || ts->tv_sec < 0 || ts->tv_sec > 4133980799LL || ts->tv_nsec < 0 ||
        ts->tv_nsec >= 1000000000LL)
        return SYSCALL_EINVAL;
    timespec_t raw;
    int r = time_get_uncorrected(CLOCK_REALTIME, &raw);
    if (r)
        return r;
    int64_t delta = ts->tv_sec - raw.tv_sec;
    if (delta < -9000000000LL || delta > 9000000000LL)
        return SYSCALL_EINVAL;
    int64_t ns = delta * 1000000000LL + ts->tv_nsec - raw.tv_nsec;
    __atomic_store_n(&g_realtime_correction_ns, ns, __ATOMIC_RELEASE);
    return 0;
}

uint64_t time_get_epoch_seconds(void) {
    timespec_t ts;
    if (time_get_timespec(CLOCK_REALTIME, &ts) == 0) {
        return (uint64_t)ts.tv_sec;
    }
    return g_timekeeper.base_epoch_sec;
}

void time_get_timeval(timeval_t *tv) {
    if (!tv) return;
    timespec_t ts;
    time_get_timespec(CLOCK_REALTIME, &ts);
    tv->tv_sec = ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000;
}

uint64_t time_tsc_to_ns(uint64_t cycles) {
    if (!g_timekeeper.tsc_valid || g_timekeeper.tsc_hz == 0) {
        return cycles / 2;
    }
    uint64_t sec = cycles / g_timekeeper.tsc_hz;
    uint64_t rem = cycles % g_timekeeper.tsc_hz;
    return sec * 1000000000ULL + (rem * 1000000000ULL) / g_timekeeper.tsc_hz;
}

void time_account_user_to_kernel(struct tcb *t) {
    if (!t) return;
    if (t->in_user_mode) {
        uint64_t now = apic_poll_clock_read();
        if (now > t->last_tsc) {
            t->utime_tsc += (now - t->last_tsc);
        }
        t->in_user_mode = false;
        t->last_tsc = now;
    }
}

void time_account_kernel_to_user(struct tcb *t) {
    if (!t) return;
    if (!t->in_user_mode) {
        uint64_t now = apic_poll_clock_read();
        if (now > t->last_tsc) {
            t->stime_tsc += (now - t->last_tsc);
        }
        t->in_user_mode = true;
        t->last_tsc = now;
    }
}

void time_account_switch_out(struct tcb *outgoing) {
    if (!outgoing) return;
    uint64_t now = apic_poll_clock_read();
    if (now > outgoing->last_tsc) {
        uint64_t delta = now - outgoing->last_tsc;
        if (outgoing->in_user_mode) {
            outgoing->utime_tsc += delta;
        } else {
            outgoing->stime_tsc += delta;
        }
    }
    outgoing->last_tsc = now;
}

void time_account_switch_in(struct tcb *incoming) {
    if (!incoming) return;
    incoming->last_tsc = apic_poll_clock_read();
}

void time_account_enter_user(void) {
    tcb_t *curr = thread_current();
    if (curr) {
        time_account_kernel_to_user(curr);
    }
}
