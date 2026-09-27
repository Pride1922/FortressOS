#ifndef FORTRESS_PROCESS_TABLE_H
#define FORTRESS_PROCESS_TABLE_H
#include "types.h"
#include "signal_state.h"
/* Global bounded identity/group/session and child reservation/status store.
 * Metadata APIs acquire the private ordinary rank-1 g_process_lock internally;
 * callers hold no locks. It never nests with scheduler/ext2/other rank-1 locks.
 * process_record_sequence() is the exception: an atomic acquire read usable
 * by scheduler wait predicates, paired with release publication under the
 * process lock. No TCB pointers escape this module; scheduler placement and
 * TCB lifetime remain scheduler-owned. PID is monotonic, never recycled. */
#define PROCESS_CAPACITY 64
int process_record_begin(uint64_t pid, uint64_t parent, bool waitable,
                         uint32_t flags, uint64_t pgid);
void process_record_abort(uint64_t pid);
void process_record_commit(uint64_t pid);
bool process_record_exit(uint64_t pid, uint64_t code);
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
