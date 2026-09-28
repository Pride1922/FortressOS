#include "process_table.h"
#include "spinlock.h"
#include "syscall_abi.h"

/* Separate from scheduler placement and the overwriteable kernel-test history.
 * Members include unpublished spawn reservations. External references retain an
 * empty identity, but do not keep any TCB or child status alive. */
typedef struct {
    uint64_t pgid, sid, generation;
    uint32_t members;
    uint32_t refs; /* atomic; an owned ref can be cloned without taking a lock */
} process_group_t;
static process_group_t groups[PROCESS_GROUP_CAPACITY];
_Static_assert(__atomic_always_lock_free(sizeof(uint32_t), 0), "group refs must be IRQ-safe atomics");
typedef struct {
    uint64_t pid, parent, pgid, sid;
    bool used, published, exited, stopped;
    signal_state_t *signals;
    process_group_t *group;
} process_record_t;
typedef struct {
    uint64_t pid, parent, pgid, code;
    uint64_t identity_generation, event_seq, reported_seq;
    bool used, done;
    unsigned signal;
    enum { CHILD_NONE, CHILD_STOPPED, CHILD_CONTINUED, CHILD_EXITED, CHILD_SIGNALED } event;
} child_record_t;
static process_record_t processes[PROCESS_CAPACITY];
static child_record_t children[PROCESS_CAPACITY];
static spinlock_t g_process_lock = SPINLOCK_RANKED(1, "process");
static uint64_t sequence;
static process_record_t *find(uint64_t pid) {
    for (unsigned i = 0; i < PROCESS_CAPACITY; ++i)
        if (processes[i].used && processes[i].pid == pid) return &processes[i];
    return NULL;
}
static bool group_exists(uint64_t pgid, uint64_t sid) {
    for (unsigned i = 0; i < PROCESS_GROUP_CAPACITY; ++i)
        if (groups[i].members && groups[i].pgid == pgid && groups[i].sid == sid) return true;
    return false;
}
/* Process lock held. Reuse is allowed only with no members and no external
 * owners; generation persists across reuse. A retained empty numeric identity
 * reserves its PGID and cannot be resurrected into a different membership. */
static process_group_t *group_join(uint64_t pgid, uint64_t sid, bool create, int *error) {
    process_group_t *free_group = NULL;
    for (unsigned i=0; i<PROCESS_GROUP_CAPACITY; ++i) {
        process_group_t *g=&groups[i];
        uint32_t refs=__atomic_load_n(&g->refs, __ATOMIC_ACQUIRE);
        if (!g->members && !refs) {
            if (!free_group && g->generation != UINT64_MAX) free_group=g;
            continue;
        }
        if (g->pgid != pgid) continue;
        if (g->sid != sid || !g->members) { *error=SYSCALL_EPERM; return NULL; }
        ++g->members;
        return g;
    }
    if (!create) { *error=SYSCALL_EPERM; return NULL; }
    if (!free_group) { *error=SYSCALL_ENOMEM; return NULL; }
    free_group->pgid=pgid;
    free_group->sid=sid;
    ++free_group->generation; /* exhausted slots are never recycled */
    free_group->members=1;
    return free_group;
}
static void group_leave(process_record_t *p) {
    if (!p->group) return;
    if (!p->group->members) __builtin_trap();
    --p->group->members;
    p->group=NULL;
}
static process_group_t *group_ref_locked(const process_group_ref_t *ref) {
    if (!ref || !ref->generation || ref->slot >= PROCESS_GROUP_CAPACITY) return NULL;
    process_group_t *g=&groups[ref->slot];
    return g->generation == ref->generation &&
           __atomic_load_n(&g->refs, __ATOMIC_ACQUIRE) ? g : NULL;
}
int process_group_acquire(uint64_t sid, uint64_t pgid, process_group_ref_t *out) {
    if (!out || !sid || !pgid) return SYSCALL_EINVAL;
    uint64_t irq=spin_lock_irqsave(&g_process_lock);
    int result=SYSCALL_ESRCH;
    for (unsigned i=0; i<PROCESS_GROUP_CAPACITY; ++i) {
        process_group_t *g=&groups[i];
        if (!g->members || g->pgid != pgid) continue;
        if (g->sid != sid) { result=SYSCALL_EPERM; break; }
        uint32_t refs=__atomic_load_n(&g->refs, __ATOMIC_RELAXED);
        result=SYSCALL_ENOMEM;
        if (refs != UINT32_MAX && __atomic_compare_exchange_n(&g->refs, &refs,
                refs+1, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            *out=(process_group_ref_t){.generation=g->generation, .slot=i};
            result=0;
        }
        break;
    }
    spin_unlock_irqrestore(&g_process_lock, irq);
    return result;
}
bool process_group_try_retain(const process_group_ref_t *owned, process_group_ref_t *out) {
    if (!owned || !out || owned == out || !owned->generation || owned->slot >= PROCESS_GROUP_CAPACITY)
        return false;
    /* Source ownership prevents concurrent slot reuse. This is not a way to
     * reacquire stale copies; only acquire() may create a first external ref. */
    process_group_t *g=&groups[owned->slot];
    uint32_t refs=__atomic_load_n(&g->refs, __ATOMIC_ACQUIRE);
    if (g->generation != owned->generation || !refs || refs == UINT32_MAX) return false;
    if (!__atomic_compare_exchange_n(&g->refs, &refs, refs+1, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return false;
    *out=*owned;
    return true;
}
bool process_group_release_ref(process_group_ref_t *ref) {
    uint64_t irq=spin_lock_irqsave(&g_process_lock);
    process_group_t *g=group_ref_locked(ref);
    if (g) {
        __atomic_fetch_sub(&g->refs, 1, __ATOMIC_ACQ_REL);
        *ref=(process_group_ref_t){0};
    }
    spin_unlock_irqrestore(&g_process_lock, irq);
    return g != NULL;
}
static void changed(void) {
    if (sequence == UINT64_MAX) __builtin_trap();
    __atomic_add_fetch(&sequence, 1, __ATOMIC_RELEASE);
}
static uint64_t advance(uint64_t value) {
    /* Never reset a live sequence to match an outstanding snapshot. */
    if (value == UINT64_MAX) __builtin_trap();
    return value + 1;
}
/* All callers hold g_process_lock. Durable state and the parent wait sequence
 * precede signal publication. Default CHLD drops only the notification. */
static void child_publish(child_record_t *c) {
    c->event_seq = advance(c->event_seq);
    changed();
    process_record_t *parent = find(c->parent);
    if (parent && !parent->exited && parent->signals &&
        parent->signals->action_handlers[SIGCHLD] > SIG_IGN)
        __atomic_fetch_or(&parent->signals->pending_mask, SIGNAL_BIT(SIGCHLD), __ATOMIC_RELEASE);
}
static void stopped_locked(process_record_t *p, unsigned sig) {
    if (p->stopped) return;
    p->stopped = true;
    for (unsigned i=0; i<PROCESS_CAPACITY; ++i) {
        child_record_t *c = &children[i];
        if (c->used && c->pid == p->pid && !c->done) {
            c->event = CHILD_STOPPED;
            c->signal = sig;
            child_publish(c);
        }
    }
}
uint64_t process_record_sequence(void) { return __atomic_load_n(&sequence, __ATOMIC_ACQUIRE); }
int process_record_begin(uint64_t pid, uint64_t parent, bool waitable,
                         uint32_t flags, uint64_t pgid) {
    if (!pid || pid > 0x7fffffffffffffffULL ||
        (flags & ~SPAWN_V2_FLAGS) || (!(flags & SPAWN_SETPGROUP) && pgid) ||
        pgid > 0x7fffffffffffffffULL) return SYSCALL_EINVAL;
    uint64_t irq = spin_lock_irqsave(&g_process_lock);
    int error = SYSCALL_ENOMEM;
    process_record_t *p = NULL, *par = find(parent);
    child_record_t *ch = NULL;
    if (find(pid)) { error = SYSCALL_EINVAL; goto out; }
    if (waitable && (!par || par->exited)) { error = SYSCALL_ECHILD; goto out; }
    for (unsigned i = 0; i < PROCESS_CAPACITY; ++i) {
        if (!processes[i].used && !p) p = &processes[i];
        if (!children[i].used && !ch) ch = &children[i];
    }
    if (!p || (waitable && !ch)) goto out;
    uint64_t sid = par ? par->sid : pid;
    uint64_t group = par ? par->pgid : pid;
    if (flags & SPAWN_SETPGROUP) {
        group = pgid ? pgid : pid;
        if (group != pid && !group_exists(group, sid)) { error = SYSCALL_EPERM; goto out; }
    }
    process_group_t *identity=group_join(group, sid, group==pid, &error);
    if (!identity) goto out;
    *p = (process_record_t){.pid=pid, .parent=parent, .pgid=group, .sid=sid,
                           .used=true, .group=identity};
    if (waitable) *ch = (child_record_t){.pid=pid, .parent=parent, .pgid=group, .used=true,
        .identity_generation=advance(ch->identity_generation)};
    error = 0;
out:
    spin_unlock_irqrestore(&g_process_lock, irq);
    return error;
}
void process_record_abort(uint64_t pid) {
    uint64_t irq = spin_lock_irqsave(&g_process_lock);
    process_record_t *p = find(pid);
    if (p) { group_leave(p); p->used = false; }
    for (unsigned i=0; i<PROCESS_CAPACITY; ++i)
        if (children[i].used && children[i].pid == pid) children[i].used = false;
    changed();
    spin_unlock_irqrestore(&g_process_lock, irq);
}
void process_record_commit(uint64_t pid) {
    uint64_t irq = spin_lock_irqsave(&g_process_lock);
    process_record_t *p = find(pid);
    if (p) p->published = true;
    spin_unlock_irqrestore(&g_process_lock, irq);
}
bool process_record_exit(uint64_t pid, uint64_t code) {
    return process_record_exit_signal(pid, code, 0);
}
bool process_record_exit_signal(uint64_t pid, uint64_t code, unsigned signal) {
    uint64_t irq = spin_lock_irqsave(&g_process_lock);
    process_record_t *p = find(pid);
    if (p) { group_leave(p); p->exited = true; p->signals = NULL; }
    bool recorded = false;
    for (unsigned i=0; i<PROCESS_CAPACITY; ++i) {
        child_record_t *c = &children[i];
        if (!c->used) continue;
        if (c->pid == pid) {
            recorded=true;
            if (!c->done) {
                c->code=signal ? 0 : code; c->signal=signal; c->done=true;
                c->event=signal ? CHILD_SIGNALED : CHILD_EXITED;
                child_publish(c);
            }
        }
        if (c->parent == pid) c->used=false;
    }
    changed();
    spin_unlock_irqrestore(&g_process_lock, irq);
    return recorded;
}
void process_record_forget(uint64_t pid) {
    uint64_t irq = spin_lock_irqsave(&g_process_lock);
    process_record_t *p = find(pid);
    if (p) { group_leave(p); p->used=false; }
    spin_unlock_irqrestore(&g_process_lock, irq);
}
int64_t process_record_group(uint64_t pid) {
    uint64_t irq = spin_lock_irqsave(&g_process_lock);
    process_record_t *p = find(pid);
    int64_t result = p && !p->exited ? (int64_t)p->pgid : SYSCALL_ESRCH;
    spin_unlock_irqrestore(&g_process_lock, irq);
    return result;
}
uint64_t process_record_session(uint64_t pid) {
    uint64_t irq = spin_lock_irqsave(&g_process_lock);
    process_record_t *p = find(pid);
    uint64_t result = p ? p->sid : 0;
    spin_unlock_irqrestore(&g_process_lock, irq);
    return result;
}
int64_t process_record_setpgid(uint64_t caller, uint64_t pid, uint64_t pgid) {
    if (pid > 0x7fffffffffffffffULL || pgid > 0x7fffffffffffffffULL) return SYSCALL_EINVAL;
    if (!pid) pid=caller;
    if (!pgid) pgid=pid;
    uint64_t irq = spin_lock_irqsave(&g_process_lock);
    process_record_t *self=find(caller), *p=find(pid);
    int result=SYSCALL_ESRCH;
    if (!self || !p || p->exited || (pid != caller && p->parent != caller)) goto out;
    result=SYSCALL_EPERM;
    if (p->sid != self->sid || p->sid == p->pid) goto out;
    /* Spawn is exec-like. Parent may regroup a staged child, not an executing image. */
    if (pid != caller && p->published) goto out;
    if (pgid != pid && !group_exists(pgid, p->sid)) goto out;
    if (pgid != p->pgid) {
        process_group_t *identity=group_join(pgid, p->sid, pgid==pid, &result);
        if (!identity) goto out;
        group_leave(p);
        p->group=identity;
    }
    p->pgid=pgid;
    for (unsigned i=0; i<PROCESS_CAPACITY; ++i)
        if (children[i].used && children[i].pid == pid) children[i].pgid=pgid;
    changed();
    result=0;
out:
    spin_unlock_irqrestore(&g_process_lock, irq);
    return result;
}
int64_t process_record_wait(uint64_t parent, int64_t selector, uint32_t options,
                            uint64_t *status, bool legacy) {
    if ((options & ~(WNOHANG|WUNTRACED|WCONTINUED)) || selector == (-0x7fffffffffffffffLL-1))
        return SYSCALL_EINVAL;
    uint64_t irq=spin_lock_irqsave(&g_process_lock);
    process_record_t *p=find(parent);
    bool eligible=false;
    int64_t result=SYSCALL_ECHILD;
    for (unsigned i=0; i<PROCESS_CAPACITY; ++i) {
        child_record_t *c=&children[i];
        if (!c->used || c->parent != parent) continue;
        if (selector > 0 && c->pid != (uint64_t)selector) continue;
        if (!selector && (!p || c->pgid != p->pgid)) continue;
        if (selector < -1 && c->pgid != (uint64_t)-selector) continue;
        eligible=true;
        if (c->event_seq == c->reported_seq) continue;
        if (!c->done && (legacy ||
            (c->event == CHILD_STOPPED && !(options & WUNTRACED)) ||
            (c->event == CHILD_CONTINUED && !(options & WCONTINUED)))) continue;
        if (status) {
            if (c->event == CHILD_STOPPED) *status=((uint64_t)c->signal << 8) | 0x7f;
            else if (c->event == CHILD_CONTINUED) *status=0xffff;
            else *status=c->signal ? (legacy ? 128+c->signal : c->signal) :
                                    (legacy ? c->code : ((c->code & 255) << 8));
        }
        result=(int64_t)c->pid;
        c->reported_seq=c->event_seq;
        if (c->done) c->used=false;
        goto out;
    }
    if (eligible) result=0;
out:
    spin_unlock_irqrestore(&g_process_lock, irq);
    return result;
}

/* No registry signal pointer is returned to a caller. It is dereferenced only
 * under the process lock, and detached before any TCB can be destroyed. */
void process_record_attach_signals(uint64_t pid, signal_state_t *state) {
    uint64_t irq=spin_lock_irqsave(&g_process_lock);
    process_record_t *p=find(pid);
    if (p) {
        process_record_t *parent=find(p->parent);
        signal_state_t *src=parent && !parent->exited ? parent->signals : NULL;
        *state=(signal_state_t){0};
        state->ignored_mask=SIGNAL_BIT(SIGCHLD)|SIGNAL_BIT(SIGCONT);
        if (src) {
            /* Inherit mask and ignored disposition; reset caught handlers to SIG_DFL. */
            state->blocked_mask=src->blocked_mask;
            state->ignored_mask=src->ignored_mask;
            for (unsigned i=1;i<32;i++) {
                state->action_masks[i]=src->action_masks[i];
                /* exec-like semantics: caught handlers reset; ignored/default inherit */
                state->action_handlers[i]=
                    (src->action_handlers[i]==SIG_IGN) ? SIG_IGN : SIG_DFL;
                state->action_flags[i]=0;
            }
        }
        /* Caught handlers reset on exec; default CHLD/CONT ignore only their
         * notifications, regardless of the parent's effective ignore mask. */
        if (state->action_handlers[SIGCHLD]==SIG_DFL) state->ignored_mask |= SIGNAL_BIT(SIGCHLD);
        if (state->action_handlers[SIGCONT]==SIG_DFL) state->ignored_mask |= SIGNAL_BIT(SIGCONT);
        p->signals=state;
    }
    spin_unlock_irqrestore(&g_process_lock,irq);
}
/* Shared by numeric kill and retained-target publication. Process lock held;
 * owner-CPU timer servicing observes the same persistent signal mailbox. */
static void signal_publish(process_record_t *p, uint64_t sig) {
    signal_state_t *s=p->signals;
    if (!sig) return;
    if (sig == SIGCONT) {
        __atomic_fetch_and(&s->pending_mask, ~SIGNAL_STOPS, __ATOMIC_RELEASE);
        if (p->stopped) __atomic_store_n(&s->continue_requested, true, __ATOMIC_RELEASE);
    } else if (SIGNAL_BIT(sig) & SIGNAL_STOPS) {
        __atomic_fetch_and(&s->pending_mask, ~SIGNAL_BIT(SIGCONT), __ATOMIC_RELEASE);
        __atomic_store_n(&s->continue_requested, false, __ATOMIC_RELEASE);
    }
    if (!(s->ignored_mask & SIGNAL_BIT(sig)) || (SIGNAL_BIT(sig) & SIGNAL_STOPS))
        __atomic_fetch_or(&s->pending_mask, SIGNAL_BIT(sig), __ATOMIC_RELEASE);
}
int64_t process_group_signal(const process_group_ref_t *owned, uint64_t sig) {
    if (sig>31 || (sig && !(SIGNAL_SUPPORTED & SIGNAL_BIT(sig)))) return SYSCALL_EINVAL;
    uint64_t irq=spin_lock_irqsave(&g_process_lock);
    process_group_t *g=group_ref_locked(owned);
    int result=SYSCALL_ESRCH;
    if (g) for (unsigned i=0; i<PROCESS_CAPACITY; ++i) {
        process_record_t *p=&processes[i];
        if (!p->used || p->exited || !p->signals || p->group != g) continue;
        signal_publish(p, sig);
        result=0;
    }
    spin_unlock_irqrestore(&g_process_lock, irq);
    return result;
}
int64_t process_signal_send(uint64_t caller, int64_t selector, uint64_t sig) {
    if (sig>31 || (sig && !(SIGNAL_SUPPORTED & SIGNAL_BIT(sig))) ||
        selector == -1 || selector == (-0x7fffffffffffffffLL-1)) return SYSCALL_EINVAL;
    uint64_t irq=spin_lock_irqsave(&g_process_lock);
    process_record_t *self=find(caller);
    int result=SYSCALL_ESRCH;
    if (!self || self->exited) goto out;
    for (unsigned i=0;i<PROCESS_CAPACITY;i++) {
        process_record_t *p=&processes[i];
        if (!p->used || p->exited || !p->signals) continue;
        bool match=selector>0 ? p->pid==(uint64_t)selector :
                   p->pgid==(selector==0 ? self->pgid : (uint64_t)-selector);
        if (!match) continue;
        if (p->sid != self->sid) { if (result) result=SYSCALL_EPERM; continue; }
        result=0;
        signal_publish(p, sig);
    }
out:
    spin_unlock_irqrestore(&g_process_lock,irq);
    return result;
}
int64_t process_signal_action(uint64_t pid, uint64_t sig, const signal_action_t *act, signal_action_t *old) {
    if (!sig || sig>31 || !(SIGNAL_SUPPORTED & SIGNAL_BIT(sig))) return SYSCALL_EINVAL;
    if (act && (act->flags || act->reserved || (act->mask & ~SIGNAL_SUPPORTED))) return SYSCALL_EINVAL;
    /* SIGKILL and SIGSTOP cannot be caught or ignored (UNBLOCKABLE). */
    if (act && (sig==SIGKILL || sig==SIGSTOP)) return SYSCALL_EINVAL;
    if (act && sig==SIGCHLD && act->handler==SIG_IGN) return SYSCALL_EINVAL;
    /* Custom handler: handler >= 2 means user address. Validate canonical range. */
    if (act && act->handler>SIG_IGN) {
        /* User address: must be in lower canonical half, above page zero. */
        if (act->handler < 0x1000ULL || act->handler >= 0x0000800000000000ULL)
            return SYSCALL_EINVAL;
    }
    uint64_t irq=spin_lock_irqsave(&g_process_lock);
    process_record_t *p=find(pid);
    int result=SYSCALL_ESRCH;
    if (p && !p->exited && p->signals) {
        signal_state_t *s=p->signals;
        /* Report old action. */
        if (old) {
            uint64_t h=s->action_handlers[sig];
            *old=(signal_action_t){.handler=h, .mask=s->action_masks[sig],
                                  .flags=s->action_flags[sig]};
        }
        if (act) {
            s->action_masks[sig]   = act->mask & ~SIGNAL_UNBLOCKABLE;
            s->action_handlers[sig]= act->handler;
            s->action_flags[sig]   = act->flags;
            uint64_t ignored=s->ignored_mask;
            if (act->handler==SIG_IGN || (act->handler==SIG_DFL && (sig==SIGCHLD || sig==SIGCONT))) {
                ignored|=SIGNAL_BIT(sig);
                /* Pending ignored signal is discarded. */
                if (!(SIGNAL_BIT(sig) & SIGNAL_STOPS))
                    __atomic_fetch_and(&s->pending_mask,~SIGNAL_BIT(sig),__ATOMIC_RELEASE);
            } else {
                ignored&=~SIGNAL_BIT(sig);
            }
            __atomic_store_n(&s->ignored_mask,ignored,__ATOMIC_RELEASE);
        }
        result=0;
    }
    spin_unlock_irqrestore(&g_process_lock,irq);
    return result;
}
/* Returns the handler address stored for sig under the process lock.
 * Used by the delivery path to atomically snapshot the action. */
uint64_t process_signal_handler(uint64_t pid, uint64_t sig) {
    if (!sig || sig>31) return SIG_DFL;
    uint64_t irq=spin_lock_irqsave(&g_process_lock);
    process_record_t *p=find(pid);
    uint64_t h=SIG_DFL;
    if (p && !p->exited && p->signals)
        h=p->signals->action_handlers[sig];
    spin_unlock_irqrestore(&g_process_lock,irq);
    return h;
}
/* Take a pending deliverable signal and snapshot its action atomically.
 * Returns the signal number, or 0 if nothing deliverable.
 * On success, clears the pending bit and fills *out_action. */
static unsigned take_action(uint64_t pid, signal_action_t *out_action,
                            uint64_t *out_old_mask, bool control_only) {
    uint64_t irq=spin_lock_irqsave(&g_process_lock);
    process_record_t *p=find(pid);
    unsigned sig=0;
    if (p && !p->exited && p->signals) {
        signal_state_t *s=p->signals;
        uint64_t pending=s->pending_mask & ~s->blocked_mask & ~s->ignored_mask;
        if (pending & SIGNAL_BIT(SIGKILL)) sig=SIGKILL;
        else for (unsigned i=1;i<32;i++) {
            if (!(pending&SIGNAL_BIT(i))) continue;
            if (control_only && (!(SIGNAL_STOPS&SIGNAL_BIT(i)) || s->action_handlers[i]!=SIG_DFL)) continue;
            sig=i; break;
        }
        if (sig && !(control_only && sig==SIGKILL)) {
            __atomic_fetch_and(&s->pending_mask,~SIGNAL_BIT(sig),__ATOMIC_RELEASE);
            if ((SIGNAL_STOPS & SIGNAL_BIT(sig)) && s->action_handlers[sig]==SIG_DFL)
                stopped_locked(p, sig);
            if (out_old_mask) *out_old_mask=s->blocked_mask;
            if (out_action) {
                out_action->handler=s->action_handlers[sig];
                out_action->mask   =s->action_masks[sig];
                out_action->flags  =s->action_flags[sig];
                out_action->reserved=0;
            }
        }
    }
    spin_unlock_irqrestore(&g_process_lock,irq);
    return sig;
}
unsigned process_signal_take_action(uint64_t pid, signal_action_t *action, uint64_t *mask) {
    return take_action(pid, action, mask, false);
}
unsigned process_signal_take_control(uint64_t pid) {
    return take_action(pid, NULL, NULL, true);
}
bool process_record_resume(uint64_t pid) {
    uint64_t irq=spin_lock_irqsave(&g_process_lock);
    process_record_t *p=find(pid);
    bool resumed=false;
    if (p && !p->exited && p->stopped && p->signals) {
        signal_state_t *s=p->signals;
        bool killed=(s->pending_mask & SIGNAL_BIT(SIGKILL)) != 0;
        if (killed || s->continue_requested) {
            p->stopped=false;
            __atomic_store_n(&s->continue_requested, false, __ATOMIC_RELEASE);
            resumed=true;
            if (!killed) for (unsigned i=0; i<PROCESS_CAPACITY; ++i) {
                child_record_t *c=&children[i];
                if (c->used && !c->done && c->pid==pid) {
                    c->event=CHILD_CONTINUED; c->signal=0;
                    child_publish(c);
                }
            }
        }
    }
    spin_unlock_irqrestore(&g_process_lock,irq);
    return resumed;
}
int64_t process_signal_mask(uint64_t pid, uint64_t how, const uint64_t *mask, uint64_t *old) {
    if (mask && (how>SIG_SETMASK || (*mask & ~SIGNAL_SUPPORTED))) return SYSCALL_EINVAL;
    uint64_t irq=spin_lock_irqsave(&g_process_lock);
    process_record_t *p=find(pid);
    int result=SYSCALL_ESRCH;
    if (p && !p->exited && p->signals) {
        signal_state_t *s=p->signals;
        if (old) *old=s->blocked_mask;
        if (mask) {
            uint64_t value=how==SIG_BLOCK ? s->blocked_mask|*mask :
                           how==SIG_UNBLOCK ? s->blocked_mask&~*mask : *mask;
            __atomic_store_n(&s->blocked_mask,value & ~SIGNAL_UNBLOCKABLE,__ATOMIC_RELEASE);
        }
        result=0;
    }
    spin_unlock_irqrestore(&g_process_lock,irq);
    return result;
}
unsigned process_signal_take(uint64_t pid) {
    uint64_t irq=spin_lock_irqsave(&g_process_lock);
    process_record_t *p=find(pid);
    unsigned sig=0;
    if (p && !p->exited && p->signals) {
        signal_state_t *s=p->signals;
        uint64_t pending=s->pending_mask & ~s->blocked_mask & ~s->ignored_mask;
        if (pending & SIGNAL_BIT(SIGKILL)) sig=SIGKILL;
        else for (unsigned i=1;i<32;i++) if (pending&SIGNAL_BIT(i)) { sig=i; break; }
        if (sig) __atomic_fetch_and(&s->pending_mask,~SIGNAL_BIT(sig),__ATOMIC_RELEASE);
    }
    spin_unlock_irqrestore(&g_process_lock,irq);
    return sig;
}
/* Push an active-frame entry. Returns 0 on success, SYSCALL_ENOMEM on overflow. */
int process_signal_push_frame(uint64_t pid, uintptr_t frame_addr, uint64_t *out_generation) {
    uint64_t irq=spin_lock_irqsave(&g_process_lock);
    process_record_t *p=find(pid);
    int result=SYSCALL_ESRCH;
    if (p && !p->exited && p->signals) {
        signal_state_t *s=p->signals;
        if (s->nesting_depth >= SIGNAL_MAX_NESTING) {
            result=SYSCALL_ENOMEM; /* nesting overflow */
        } else {
            uint64_t gen=s->next_generation++;
            s->active_frames[s->nesting_depth].frame_addr=frame_addr;
            s->active_frames[s->nesting_depth].generation=gen;
            s->nesting_depth++;
            if (out_generation) *out_generation=gen;
            result=0;
        }
    }
    spin_unlock_irqrestore(&g_process_lock,irq);
    return result;
}
/* Pop the active-frame entry if the top generation matches expected_generation.
 * Returns 0 on success, SYSCALL_EINVAL on mismatch/empty. */
int process_signal_pop_frame(uint64_t pid, uint64_t expected_generation) {
    uint64_t irq=spin_lock_irqsave(&g_process_lock);
    process_record_t *p=find(pid);
    int result=SYSCALL_EINVAL;
    if (p && !p->exited && p->signals) {
        signal_state_t *s=p->signals;
        if (s->nesting_depth > 0 &&
            s->active_frames[s->nesting_depth-1].generation == expected_generation) {
            s->nesting_depth--;
            result=0;
        }
    }
    spin_unlock_irqrestore(&g_process_lock,irq);
    return result;
}
/* Atomically install a new blocked mask (for handler entry). */
int process_signal_set_mask(uint64_t pid, uint64_t new_mask) {
    uint64_t irq=spin_lock_irqsave(&g_process_lock);
    process_record_t *p=find(pid);
    int result=SYSCALL_ESRCH;
    if (p && !p->exited && p->signals) {
        __atomic_store_n(&p->signals->blocked_mask,
                         new_mask & ~SIGNAL_UNBLOCKABLE, __ATOMIC_RELEASE);
        result=0;
    }
    spin_unlock_irqrestore(&g_process_lock,irq);
    return result;
}
/* Verify that the top active-frame entry matches (frame_addr, generation).
 * Returns true only if the match is exact; false on any mismatch or empty stack.
 * No side effects — used as the pre-validation check in sys_sigreturn. */
bool process_signal_check_frame_id(uint64_t pid, uintptr_t frame_addr, uint64_t generation) {
    uint64_t irq=spin_lock_irqsave(&g_process_lock);
    process_record_t *p=find(pid);
    bool ok=false;
    if (p && !p->exited && p->signals) {
        signal_state_t *s=p->signals;
        ok = (s->nesting_depth > 0 &&
              s->active_frames[s->nesting_depth-1].generation == generation &&
              s->active_frames[s->nesting_depth-1].frame_addr == frame_addr);
    }
    spin_unlock_irqrestore(&g_process_lock,irq);
    return ok;
}
