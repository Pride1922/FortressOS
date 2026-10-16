#ifndef FORTRESS_TIME_H
#define FORTRESS_TIME_H

#include "types.h"
#include "syscall_abi.h"

/* Initialize kernel timekeeping from CMOS RTC and calibrated TSC */
void time_init(void);

/* Query kernel clock. Supports CLOCK_REALTIME and CLOCK_MONOTONIC.
 * Returns 0 on success, or -SYSCALL_EINVAL for unsupported clock IDs. */
int time_get_timespec(uint32_t clk_id, timespec_t *ts);

/* Return current wall-clock seconds since Unix epoch (for filesystems) */
uint64_t time_get_epoch_seconds(void);

/* Return current wall-clock timeval */
void time_get_timeval(timeval_t *tv);

/* Called by BSP timer interrupt handler on each 100 Hz tick to wake sleepers */
void time_timer_tick(void);

/* Sleep channel token for nanosleep */
const void *time_get_sleep_channel(void);

#endif /* FORTRESS_TIME_H */
