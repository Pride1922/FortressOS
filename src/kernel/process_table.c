#include "process_table.h"
#include "spinlock.h"
#include "syscall_abi.h"

/* Separate from scheduler placement and the overwriteable kernel-test history. */
typedef struct {
    uint64_t pid, parent, pgid, sid;
    bool used, published, exited;
} process_record_t;
typedef struct {
    uint64_t pid, parent, pgid, code;
    bool used, done;
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
    uint64_t irq = spin_lock_irqsave(&g_process_lock);
    process_record_t *p = find(pid);
    if (p) p->exited = true;
    bool recorded = false;
    for (unsigned i=0; i<PROCESS_CAPACITY; ++i) {
        child_record_t *c = &children[i];
        if (!c->used) continue;
        if (c->pid == pid) { c->code=code; c->done=true; recorded=true; }
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
        if (status) *status=legacy ? c->code : ((c->code & 255) << 8);
        result=(int64_t)c->pid;
        c->used=false;
        goto out;
    }
    if (eligible) result=0;
out:
    spin_unlock_irqrestore(&g_process_lock, irq);
    return result;
}
