#ifndef FORTRESS_TIME_H
#define FORTRESS_TIME_H

#include "types.h"
#include "syscall_abi.h"

/* Initialize kernel timekeeping from CMOS RTC and calibrated TSC */
void time_init(void);
/* Caller enforces CAP_SYS_ADMIN; atomic wall-clock correction, no RTC writes. */
int time_set_realtime(const timespec_t *ts);

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

/* Convert raw TSC cycle count into nanoseconds using calibrated TSC frequency */
uint64_t time_tsc_to_ns(uint64_t cycles);

struct tcb;
/* Per-TCB CPU-time accounting hooks */
void time_account_user_to_kernel(struct tcb *t);
void time_account_kernel_to_user(struct tcb *t);
void time_account_switch_out(struct tcb *outgoing);
void time_account_switch_in(struct tcb *incoming);
void time_account_enter_user(void);

#endif /* FORTRESS_TIME_H */
