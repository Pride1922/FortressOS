#include "process_table.h"
#include "spinlock.h"
#include "syscall_abi.h"

/* Separate from scheduler placement and the overwriteable kernel-test history. */
typedef struct {
    uint64_t pid, parent, pgid, sid;
    bool used, published, exited;
    signal_state_t *signals;
} process_record_t;
typedef struct {
    uint64_t pid, parent, pgid, code;
    bool used, done;
    unsigned signal;
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
    for (unsigned i = 0; i < PROCESS_CAPACITY; ++i)
        if (processes[i].used && !processes[i].exited &&
            processes[i].pgid == pgid && processes[i].sid == sid) return true;
    return false;
}
static void changed(void) { __atomic_add_fetch(&sequence, 1, __ATOMIC_RELEASE); }
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
    *p = (process_record_t){.pid=pid, .parent=parent, .pgid=group, .sid=sid, .used=true};
    if (waitable) *ch = (child_record_t){.pid=pid, .parent=parent, .pgid=group, .used=true};
    error = 0;
out:
    spin_unlock_irqrestore(&g_process_lock, irq);
    return error;
}
void process_record_abort(uint64_t pid) {
    uint64_t irq = spin_lock_irqsave(&g_process_lock);
    process_record_t *p = find(pid);
    if (p) p->used = false;
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
    if (p) { p->exited = true; p->signals = NULL; }
    bool recorded = false;
    for (unsigned i=0; i<PROCESS_CAPACITY; ++i) {
        child_record_t *c = &children[i];
        if (!c->used) continue;
        if (c->pid == pid) { c->code=signal ? 0 : code; c->signal=signal; c->done=true; recorded=true; }
        if (c->parent == pid) c->used=false;
    }
    changed();
    spin_unlock_irqrestore(&g_process_lock, irq);
    return recorded;
}
void process_record_forget(uint64_t pid) {
    uint64_t irq = spin_lock_irqsave(&g_process_lock);
    process_record_t *p = find(pid);
    if (p) p->used=false;
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
        if (!c->done) continue;
        if (status) *status=c->signal ? (legacy ? 128+c->signal : c->signal) :
                                     (legacy ? c->code : ((c->code & 255) << 8));
        result=(int64_t)c->pid;
        c->used=false;
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
        if (src) {
            state->blocked_mask=src->blocked_mask;
            state->ignored_mask=src->ignored_mask;
            for (unsigned i=1;i<32;i++) state->action_masks[i]=src->action_masks[i];
        }
        p->signals=state;
    }
    spin_unlock_irqrestore(&g_process_lock,irq);
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
        if (sig && !(p->signals->ignored_mask & SIGNAL_BIT(sig)))
            __atomic_fetch_or(&p->signals->pending_mask,SIGNAL_BIT(sig),__ATOMIC_RELEASE);
    }
out:
    spin_unlock_irqrestore(&g_process_lock,irq);
    return result;
}
int64_t process_signal_action(uint64_t pid, uint64_t sig, const signal_action_t *act, signal_action_t *old) {
    if (!sig || sig>31 || !(SIGNAL_SUPPORTED & SIGNAL_BIT(sig))) return SYSCALL_EINVAL;
    if (act && (act->flags || act->reserved || (act->mask & ~SIGNAL_SUPPORTED))) return SYSCALL_EINVAL;
    if (act && sig==SIGKILL) return SYSCALL_EINVAL;
    if (act && act->handler>SIG_IGN) return SYSCALL_EOPNOTSUPP;
    uint64_t irq=spin_lock_irqsave(&g_process_lock);
    process_record_t *p=find(pid);
    int result=SYSCALL_ESRCH;
    if (p && !p->exited && p->signals) {
        signal_state_t *s=p->signals;
        if (old) *old=(signal_action_t){.handler=(s->ignored_mask&SIGNAL_BIT(sig)) ? SIG_IGN:SIG_DFL,
                                      .mask=s->action_masks[sig]};
        if (act) {
            s->action_masks[sig]=act->mask & ~SIGNAL_UNBLOCKABLE;
            uint64_t ignored=s->ignored_mask;
            if (act->handler==SIG_IGN) ignored|=SIGNAL_BIT(sig); else ignored&=~SIGNAL_BIT(sig);
            __atomic_store_n(&s->ignored_mask,ignored,__ATOMIC_RELEASE);
            if (act->handler==SIG_IGN) __atomic_fetch_and(&s->pending_mask,~SIGNAL_BIT(sig),__ATOMIC_RELEASE);
        }
        result=0;
    }
    spin_unlock_irqrestore(&g_process_lock,irq);
    return result;
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
