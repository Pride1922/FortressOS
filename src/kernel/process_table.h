#ifndef FORTRESS_PROCESS_TABLE_H
#define FORTRESS_PROCESS_TABLE_H
#include "types.h"
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
#endif
