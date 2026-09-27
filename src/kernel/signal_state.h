#ifndef FORTRESS_SIGNAL_STATE_H
#define FORTRESS_SIGNAL_STATE_H
#include "signal_abi.h"
/*
 * signal_state_t — embedded in TCB.
 *
 * Synchronization:
 *   Scalar masks (pending_mask, blocked_mask, ignored_mask) are manipulated
 *   atomically for lock-free scheduler reads. All other fields require the
 *   process lock (g_process_lock, rank 1).
 *
 * Registry attachment lasts until exit/abort, which precedes TCB reclamation.
 *
 * active_frames: bounded LIFO stack of in-progress signal frames.
 *   Pushed on handler delivery; popped only on successful sigreturn commit.
 *   nesting_depth < SIGNAL_MAX_NESTING at all times; overflow terminates
 *   the process (not the kernel).
 *   Each entry records the frame virtual address (F) and a generation counter
 *   that matches the frame_id stored in the user signal frame. The generation
 *   is monotonically increasing per TCB, never wraps without detection.
 */

/* Maximum nested signal handler depth (per §3 spec: nesting limit 4). */
#define SIGNAL_MAX_NESTING 4

typedef struct {
    uintptr_t frame_addr;   /* F = align_down(S - 224, 16) for this frame */
    uint64_t  generation;   /* frame_id written into the user frame */
} signal_active_frame_t;

typedef struct {
    /* Atomic masks — observable by scheduler without the process lock. */
    uint64_t pending_mask;
    uint64_t blocked_mask;
    uint64_t ignored_mask;
    /* Process-lock publication, acquire-observed by owner scheduler. Separate
     * from the blockable/ignorable SIGCONT handler notification. */
    bool continue_requested;

    /* Per-signal action data (under process lock). */
    uint64_t action_masks[32];    /* extra mask to block during handler */
    uint64_t action_handlers[32]; /* 0=SIG_DFL, 1=SIG_IGN, >=2=handler addr */
    uint64_t action_flags[32];    /* SA_* flags (none supported yet; must be 0) */

    /* Active frame stack (under process lock). */
    signal_active_frame_t active_frames[SIGNAL_MAX_NESTING];
    unsigned              nesting_depth; /* 0..SIGNAL_MAX_NESTING */
    uint64_t              next_generation; /* monotonically increasing frame_id */
} signal_state_t;

static inline bool signal_state_ready(const signal_state_t *s) {
    return (__atomic_load_n(&s->pending_mask, __ATOMIC_ACQUIRE) &
            ~__atomic_load_n(&s->blocked_mask, __ATOMIC_ACQUIRE) &
            ~__atomic_load_n(&s->ignored_mask, __ATOMIC_ACQUIRE)) != 0;
}
static inline bool signal_state_resume(const signal_state_t *s) {
    return (__atomic_load_n(&s->pending_mask, __ATOMIC_ACQUIRE) & SIGNAL_BIT(SIGKILL)) ||
           __atomic_load_n(&s->continue_requested, __ATOMIC_ACQUIRE);
}
#endif
