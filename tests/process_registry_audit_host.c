#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "process_table.h"
#include "spinlock.h"
#include "syscall_abi.h"

/* Actual-code ordering regression; does not model IRQs/scheduling. */
static _Thread_local unsigned held;
static _Thread_local spinlock_t *held_lock;
static _Thread_local bool retiring;
static pthread_mutex_t barrier = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event = PTHREAD_COND_INITIALIZER;
static bool admitted, release_merge, retire_attempted;
static spinlock_t *merge_lock;
static uint64_t watched_pid;
static unsigned wakes;
void thread_wake_for_signal(uint64_t pid) {
    (void)pid; assert(!held); ++wakes;
}
uint64_t spin_lock_irqsave(spinlock_t *lock) {
    assert(!held++);
    if (retiring && !retire_attempted) {
        assert(!pthread_mutex_lock(&barrier));
        assert(lock==merge_lock);
        assert(pthread_mutex_trylock(&lock->mutex)==EBUSY);
        retire_attempted=true;
        assert(!pthread_cond_broadcast(&event));
        assert(!pthread_mutex_unlock(&barrier));
    }
    assert(!pthread_mutex_lock(&lock->mutex));
    held_lock=lock; return 0;
}
void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    (void)flags;
    assert(held-- == 1 && held_lock==lock);
    held_lock=NULL;
    assert(!pthread_mutex_unlock(&lock->mutex));
}
void process_registry_audit_merge_admitted(uint64_t pid) {
    if (pid!=watched_pid) return;
    assert(!pthread_mutex_lock(&barrier));
    merge_lock=held_lock; admitted=true;
    assert(!pthread_cond_broadcast(&event));
    while (!release_merge) assert(!pthread_cond_wait(&event,&barrier));
    assert(!pthread_mutex_unlock(&barrier));
}
static void begin(uint64_t pid, uint64_t parent) {
    assert(!process_record_begin(pid,parent,parent!=0,0,0));
    process_record_commit(pid);
}
static void *merge(void *arg) {
    process_tick_sample_t sample = {.pid=(uintptr_t)arg,.cpu_ticks=20};
    process_record_merge_ticks(&sample,1); return NULL;
}
static void *retire(void *arg) {
    uint64_t pid=(uintptr_t)arg; retiring=true;
    assert(process_record_exit_accounted(pid,0,0,30));
    if (pid==3) {
        process_record_forget(pid);
        assert(process_record_wait(1,pid,0,NULL,false)==(int64_t)pid);
        begin(4,1);
    }
    return NULL;
}
static void ordered_merge_exit(uint64_t pid) {
    pthread_t writer, exiter;
    assert(!pthread_mutex_lock(&barrier));
    watched_pid=pid; admitted=false; release_merge=false; retire_attempted=false;
    assert(!pthread_create(&writer,NULL,merge,(void *)(uintptr_t)pid));
    while (!admitted) assert(!pthread_cond_wait(&event,&barrier));
    assert(!pthread_create(&exiter,NULL,retire,(void *)(uintptr_t)pid));
    while (!retire_attempted) assert(!pthread_cond_wait(&event,&barrier));
    release_merge=true;
    assert(!pthread_cond_broadcast(&event));
    assert(!pthread_mutex_unlock(&barrier));
    assert(!pthread_join(writer,NULL)); assert(!pthread_join(exiter,NULL));
    watched_pid=0;
}
static void *actions(void *arg) {
    (void)arg;
    for (unsigned i=0;i<2000;++i) {
        signal_action_t a={.handler=i&1 ? 0x4000:0x5000,
            .mask=i&1 ? SIGNAL_BIT(SIGINT):SIGNAL_BIT(SIGTERM)};
        assert(!process_signal_action(1,SIGCHLD,&a,NULL));
    }
    return NULL;
}
static void *inherit_and_exit(void *arg) {
    (void)arg;
    for (unsigned i=0;i<1000;++i) {
        uint64_t pid=100+i;
        signal_state_t state;
        assert(!process_record_begin(pid,1,true,0,0));
        process_record_attach_signals(pid,&state);
        signal_action_t a;
        assert(!process_signal_action(1,SIGCHLD,NULL,&a));
        assert((a.handler==0x4000 && a.mask==SIGNAL_BIT(SIGINT)) ||
               (a.handler==0x5000 && a.mask==SIGNAL_BIT(SIGTERM)));
        assert(state.action_handlers[SIGCHLD]==SIG_DFL);
        assert(state.action_masks[SIGCHLD]==SIGNAL_BIT(SIGINT) ||
               state.action_masks[SIGCHLD]==SIGNAL_BIT(SIGTERM));
        process_record_commit(pid);
        assert(process_record_exit(pid,0)); /* reads parent's CHLD action */
        process_record_forget(pid);
        assert(process_record_wait(1,pid,0,NULL,false)==(int64_t)pid);
    }
    return NULL;
}

static creds_t ca,cb;
static void *credential_writer(void *unused) {
    (void)unused;
    for (unsigned i=0;i<20000;i++) {
        creds_t old;assert(process_record_creds(5000,&old));
        assert(process_record_publish_creds(5000,&old,i&1 ? &ca : &cb));
    }
    return NULL;
}
static void credential_tests(void) {
    creds_init_root(&ca);cb=ca;cb.uid=123;cb.euid=456;cb.suid=789;
    cb.ngroups=16;for(unsigned i=0;i<16;i++) cb.groups[i]=0x87654321+i;
    cb.cap_effective=CAP_KILL;cb.umask=0077;
    creds_t storage=ca,out={0};
    assert(!process_record_begin(5000,1,true,SPAWN_STAGED,0));
    assert(process_record_bind_creds(5000,&storage));
    assert(!process_record_bind_creds(5000,&storage));
    assert(!process_record_creds(5000,&out));
    process_record_commit(5000);assert(process_record_creds(5000,&out));
    creds_t bad=cb;bad.reserved=1;
    assert(!process_record_publish_creds(5000,&out,&bad));
    assert(process_record_publish_creds(5000,&out,&cb));
    assert(!process_record_publish_creds(5000,&out,&ca));
    pthread_t writer;assert(!pthread_create(&writer,NULL,credential_writer,NULL));
    for(unsigned i=0;i<20000;i++) {
        assert(process_record_creds(5000,&out));
        assert(!memcmp(&out,&ca,sizeof(out)) || !memcmp(&out,&cb,sizeof(out)));
    }
    assert(!pthread_join(writer,NULL));
    assert(process_record_exit(5000,0));assert(!process_record_creds(5000,&out));
    process_record_forget(5000);assert(process_record_wait(1,5000,0,NULL,false)==5000);
    assert(!process_record_begin(5001,1,true,SPAWN_STAGED,0));
    storage=ca;assert(process_record_bind_creds(5001,&storage));process_record_abort(5001);
    assert(!process_record_creds(5001,&out));
    assert(!process_record_publish_creds(5000,&ca,&cb));
    assert(!process_record_begin(5002,1,true,0,0));
    ca.cap_effective=0;assert(!creds_inherit(&ca,&storage));
    assert(process_record_bind_creds(5002,&storage));process_record_commit(5002);
    assert(process_record_creds(5002,&out) && !out.cap_effective && !out.euid);
    assert(process_record_exit(5002,0));process_record_forget(5002);
    assert(process_record_wait(1,5002,0,NULL,false)==5002);
    puts("PASS credential binding: 20000 coherent publications/snapshots, stale/invalid rollback, staged/exit/abort/reuse, root cap inheritance");
}

int main(void) {
    begin(1,0); begin(2,1); ordered_merge_exit(2);
    process_snapshot_t out;
    assert(process_record_snapshot_pid(2,&out) && out.cpu_ticks==30);
    process_record_forget(2);
    assert(process_record_wait(1,2,0,NULL,false)==2);
    begin(3,1); ordered_merge_exit(3);
    assert(process_record_snapshot_pid(4,&out) && !out.cpu_ticks);
    process_tick_sample_t stale={3,900}; process_record_merge_ticks(&stale,1);
    assert(process_record_snapshot_pid(4,&out) && !out.cpu_ticks);
    puts("PASS registry ordering: exit waits for admitted merge; final 30 retained; reused PID untouched");
    assert(process_signal_send(999,1,SIGKILL)==SYSCALL_ESRCH);
    assert(process_signal_send(3,1,SIGKILL)==SYSCALL_ESRCH);
    assert(process_signal_send(999,999,0)==SYSCALL_ESRCH); assert(!wakes);
    signal_state_t parent, child, staged;
    process_record_attach_signals(1,&parent); process_record_attach_signals(4,&child);
    assert(!process_record_begin(5,1,true,SPAWN_STAGED,0));
    process_record_attach_signals(5,&staged);
    assert(!process_signal_send(1,0,SIGINT));
    assert(wakes==3 && (parent.pending_mask&SIGNAL_BIT(SIGINT)) &&
           (child.pending_mask&SIGNAL_BIT(SIGINT)) && (staged.pending_mask&SIGNAL_BIT(SIGINT)));
    assert(!process_record_snapshot_pid(5,&out)); process_record_abort(5);
    assert(process_signal_mask(5,0,NULL,NULL)==SYSCALL_ESRCH);
    assert(!process_signal_send(1,1,0) && wakes==4);
    assert(process_record_exit(4,0));
    assert(process_signal_send(4,1,SIGINT)==SYSCALL_ESRCH && wakes==4);
    puts("PASS signal lookup/errors: absent/exited caller, self/group, staged target, unlocked wakes, abort detach");
    signal_action_t initial={.handler=0x4000,.mask=SIGNAL_BIT(SIGINT)};
    assert(!process_signal_action(1,SIGCHLD,&initial,NULL));
    pthread_t updater, inheritor;
    assert(!pthread_create(&updater,NULL,actions,NULL));
    assert(!pthread_create(&inheritor,NULL,inherit_and_exit,NULL));
    assert(!pthread_join(updater,NULL)); assert(!pthread_join(inheritor,NULL));
    puts("PASS concurrent action/CHLD/inheritance: coherent actions, caught reset, 1000 child lifecycles");
    credential_tests();
    return 0;
}
