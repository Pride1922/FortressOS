#ifndef PKG_CALL_CANDIDATE_H
#define PKG_CALL_CANDIDATE_H
#include "pkg_mutation_candidate.h"
struct tcb;
/* Copied-kernel candidate only. Current owner exclusively owns its TCB binding;
 * remote cancellation stores values and never retains or dereferences a TCB.
 * Successful start transfers an admission until end; failure retains the
 * caller's unpinned admission. Context is kernel-owned until end, never user data. */
bool pc_start(struct tcb *,pm_key,uint64_t mount,void *context);
bool pc_pin(struct tcb *);
const pm_view *pc_view(struct tcb *);
void *pc_context(struct tcb *);
bool pc_commit_begin(struct tcb *);
void pc_commit_end(struct tcb *);
bool pc_cancelled(struct tcb *);
void pc_cancel_pid(uint64_t pid);
/* Current owned binding only; atomic publication, safe at a commit boundary. */
bool pc_cancel_current(struct tcb *);
bool pc_end(struct tcb *);
unsigned pc_count(void);
#endif
