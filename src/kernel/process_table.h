#ifndef FORTRESS_PROCESS_TABLE_H
#define FORTRESS_PROCESS_TABLE_H
#include "types.h"
#include "signal_state.h"
/* Global bounded identity/group/session and child reservation/status store.
 * Metadata APIs acquire the private ordinary rank-1 g_process_lock internally;
 * callers hold no locks. It never nests with scheduler/ext2/other rank-1 locks.
 * process_record_sequence() is an exception: an atomic acquire read usable
 * by scheduler wait predicates, paired with release publication under the
 * process lock. No TCB pointers escape this module; scheduler placement and
 * TCB lifetime remain scheduler-owned. process_group_try_retain() is the other
 * exception, documented below. PID allocation is monotonic; a living process
 * may recreate its namesake PGID after leaving it, so group generations differ. */
#define PROCESS_CAPACITY 64
/* Kernel-only value snapshots, not a user ABI. No pointers or ownership escape.
 * Enumeration excludes unpublished and collected records. Across calls it is
 * best-effort: churn can skip/repeat PIDs. Caller owns output/samples and holds
 * no locks. Tick merge never resurrects a PID or changes finalized accounting. */
enum process_snapshot_state { PROCESS_RUNNING = 1, PROCESS_STOPPED, PROCESS_ZOMBIE };
typedef struct {
    uint64_t pid, parent, pgid, sid, cpu_ticks;
    enum process_snapshot_state state;
    char name[16];
} process_snapshot_t;
typedef struct { uint64_t pid, cpu_ticks; } process_tick_sample_t;
void process_record_set_name(uint64_t pid, const char *name);
void process_record_merge_ticks(const process_tick_sample_t *samples, size_t count);
bool process_record_snapshot(uint64_t index, process_snapshot_t *out);
bool process_record_snapshot_pid(uint64_t pid, process_snapshot_t *out);
uint32_t process_record_count_enumerable(void);
/* Final accounting supplied by the exiting owner with local IRQs excluded.
 * No scheduler lock held. Legacy metadata-only exit helpers retain cached ticks. */
bool process_record_exit_accounted(uint64_t pid, uint64_t code, unsigned signal,
                                   uint64_t final_ticks);
/* Separate bounded group store: live/staged membership plus externally retained
 * empty groups. All implementation remains in process_table.c. No user ABI. */
#define PROCESS_GROUP_CAPACITY (PROCESS_CAPACITY * 2)
typedef struct {
    uint64_t generation; /* zero is an invalid/empty handle */
    uint32_t slot;
} process_group_ref_t;
/* Acquire by session and numeric PGID in thread context, with no locks held.
 * Only groups with members can be acquired. A successful call owns one ref;
 * out must not already own a ref. Release consumes it and clears the handle.
 * Empty retained groups cannot be joined/recreated; release the last reference
 * before the same numeric PGID may describe a new generation. */
int process_group_acquire(uint64_t sid, uint64_t pgid, process_group_ref_t *out);
bool process_group_release_ref(process_group_ref_t *ref);
/* Bounded, lock-free clone of an ALREADY OWNED reference. Source ownership must
 * remain valid for the whole call (e.g. BSP IRQ-excluded foreground handle).
 * No lookup, allocation, scheduling or logging; suitable for ingress publication.
 * out must be unowned and distinct from owned. One CAS attempt: false on
 * contention/overflow, out unchanged. Caller accounts
 * for a dropped event. Ordinary IRQs must not acquire/release/signal groups. */
bool process_group_try_retain(const process_group_ref_t *owned, process_group_ref_t *out);
/* Trusted kernel thread-context publication to the captured group, independent
 * of the current foreground group and of the original caller's lifetime.
 * Requires an owned reference; ESRCH for stale/empty targets. Signal 0 probes.
 * Future user-facing consumers must separately enforce session permissions. */
int64_t process_group_signal(const process_group_ref_t *owned, uint64_t sig);
int process_record_begin(uint64_t pid, uint64_t parent, bool waitable,
                         uint32_t flags, uint64_t pgid);
void process_record_abort(uint64_t pid);
void process_record_commit(uint64_t pid);
bool process_record_exit(uint64_t pid, uint64_t code);
/* Teardown complete: retain value-only zombie until wait/parent discard. */
void process_record_forget(uint64_t pid);
int64_t process_record_group(uint64_t pid);
uint64_t process_record_session(uint64_t pid);
int64_t process_record_setpgid(uint64_t caller, uint64_t pid, uint64_t pgid);
int64_t process_record_wait(uint64_t parent, int64_t selector, uint32_t options,
                            uint64_t *status, bool legacy);
uint64_t process_record_sequence(void);
void process_record_attach_signals(uint64_t pid, signal_state_t *state);
int64_t process_signal_send(uint64_t caller, int64_t selector, uint64_t sig);
int64_t process_signal_action(uint64_t pid, uint64_t sig, const signal_action_t *act, signal_action_t *old);
int64_t process_signal_mask(uint64_t pid, uint64_t how, const uint64_t *mask, uint64_t *old);
unsigned process_signal_take(uint64_t pid);
/* Exit releases child reservations and publishes KILL for unanchored stopped
 * groups. Live orphan identities remain until their own exit/reaper teardown.
 * Uses the process lock only; owner schedulers observe the published signals. */
bool process_record_exit_signal(uint64_t pid, uint64_t code, unsigned signal);
/* 2B additions: atomic take with full action snapshot, and handler read. */
unsigned process_signal_take_action(uint64_t pid, signal_action_t *out_action,
                                    uint64_t *out_old_mask);
uint64_t process_signal_handler(uint64_t pid, uint64_t sig);
/* Push/pop active frame entries under the process lock. */
int  process_signal_push_frame(uint64_t pid, uintptr_t frame_addr, uint64_t *out_generation);
int  process_signal_pop_frame(uint64_t pid, uint64_t expected_generation);
/* Install handler mask for delivery (call before returning to user). */
int  process_signal_set_mask(uint64_t pid, uint64_t new_mask);
/* Read-only check of top active-frame identity for sys_sigreturn validation. */
bool process_signal_check_frame_id(uint64_t pid, uintptr_t frame_addr, uint64_t generation);
/* Safe-boundary default STOP consumption / KILL observation. KILL stays pending
 * so interrupted syscalls unwind their bookkeeping before normal exit.
 * A returned stop has committed
 * durable STOPPED metadata; caller must park its continuation with local IRQs
 * disabled, then recheck on resume. No scheduler lock held on entry. */
unsigned process_signal_take_control(uint64_t pid);
/* Owner CPU only, with local IRQs disabled, outside its scheduler lock.
 * Claims a stopped task's CONT/KILL wake, publishes continued only for CONT.
 * Caller then unlinks STOPPED and enqueues READY under its scheduler lock. */
bool process_record_resume(uint64_t pid);
#endif
