#ifndef FORTRESS_HANDOFF_H
#define FORTRESS_HANDOFF_H
#include "types.h"

/* Bounded value transport; Phase 2A control/probes are its network consumer.
 * One registered caller per actor; no pointers/TCBs or callbacks cross ownership.
 * API calls require thread context with no other locks held. The sole owner
 * serializes claim/finish/pending/control turns; it never blocks in a step.
 * Predicates only acquire-read static storage, safe under scheduler locks.
 */
#define HANDOFF_SLOTS 64u
#define HANDOFF_CREDITS 32u
#define HANDOFF_DATA_CREDITS 24u
typedef struct { uint64_t generation; uint32_t slot; } handoff_token_t;
typedef struct { handoff_token_t token; bool active; } handoff_registration_t;
typedef struct {
    uint64_t actor, value, deadline;
    int64_t cancel_status; /* Typed unclaimed cancellation; zero keeps inert -4. */
    uint32_t kind;
    bool control;
} handoff_request_t;
typedef struct { int64_t status; uint64_t value; } handoff_reply_t;
typedef enum {
    HANDOFF_FREE, HANDOFF_PREPARED, HANDOFF_QUEUED, HANDOFF_CLAIMED,
    HANDOFF_PENDING, HANDOFF_COMPLETED, HANDOFF_RETIRED
} handoff_state_t;
enum { HANDOFF_OK=0, HANDOFF_WAIT=1, HANDOFF_STALE=-1,
       HANDOFF_UNAVAILABLE=-2, HANDOFF_EXHAUSTED=-3 };
typedef struct { unsigned live, queued, data, pending, retired; uint64_t reclaimed; } handoff_stats_t;

/* Boot initialization only, before any users; never reset an active pool.
 * start/fail require exclusive owner lifecycle coordination. Failure rejects
 * new admissions and cancels unclaimed work; claimed storage is retained until
 * the owner acknowledges it. There is no unsafe restart/quiescence shortcut.
 */
void handoff_start(void);
void handoff_fail(void);
/* Service readiness changed (input, resource, deadline or inert fixture gate).
 * Pending storage alone is not runnable work. Notify unlocked, in thread
 * context; consumer event/clock adapters must provide their own notifications. */
void handoff_notify(void);
int handoff_begin(handoff_registration_t *registration, handoff_request_t request);
/* FIFO within class; WAIT means sleep/retry, never public EAGAIN by itself. */
int handoff_publish(handoff_token_t token);
bool handoff_claim(handoff_token_t *token, handoff_request_t *request);
bool handoff_pending(handoff_token_t *token, handoff_request_t *request);
bool handoff_cancelled(handoff_token_t token);
bool handoff_finish(handoff_token_t token, handoff_reply_t reply, bool pending);
void handoff_cancel(handoff_token_t token);
void handoff_release(handoff_registration_t *registration);
void handoff_abandon(handoff_registration_t *registration);
int handoff_collect(handoff_token_t token, handoff_reply_t *reply);
/* <=8 acknowledgements per call; sole owner only. Caller release never frees. */
unsigned handoff_acknowledge(void);
handoff_stats_t handoff_stats(void);
bool handoff_ready(void *token);
uint64_t handoff_admission_snapshot(void);
bool handoff_admissible(void *sequence);
bool handoff_owner_ready(void *unused);
const void *handoff_channel(handoff_token_t token);
extern const unsigned handoff_owner_channel, handoff_admission_channel;
#ifdef TEST_SMP_MEMORY
/* Exhaustion fault injection, free pool only. Not present in release builds. */
void handoff_test_exhaust(unsigned slot, uint64_t generation, uint64_t ticket);
#endif
#endif
