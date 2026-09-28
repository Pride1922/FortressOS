/* Actual metadata implementation; pthread lock adapter, no IRQ/scheduler claim. */
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include "process_table.c"

static _Thread_local bool held, cloning;
uint64_t spin_lock_irqsave(spinlock_t *lock) {
    assert(!held && !cloning);
    assert(!pthread_mutex_lock(&lock->mutex)); held=true; return 0;
}
void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    (void)flags; assert(held); held=false;
    assert(!pthread_mutex_unlock(&lock->mutex));
}
static process_group_ref_t acquire(uint64_t sid, uint64_t pgid) {
    process_group_ref_t ref={0};
    assert(!process_group_acquire(sid,pgid,&ref));
    return ref;
}
static void baseline(void) {
    for (unsigned i=0; i<PROCESS_CAPACITY; ++i) {
        assert(!processes[i].used && !children[i].used);
    }
    for (unsigned i=0; i<PROCESS_GROUP_CAPACITY; ++i) {
        assert(!groups[i].members && !groups[i].refs);
    }
}
static void lifecycle(void) {
    signal_state_t leader, member, peer;
    assert(!process_record_begin(1,0,false,0,0));
    assert(!process_record_begin(2,1,true,SPAWN_SETPGROUP,0));
    process_record_attach_signals(2,&leader);
    process_group_ref_t ref=acquire(1,2), event={0}, stale=ref;
    assert(process_group_acquire(99,2,&event)==SYSCALL_EPERM && !event.generation);
    assert(process_group_acquire(1,999,&event)==SYSCALL_ESRCH);
    assert(process_group_acquire(1,2,NULL)==SYSCALL_EINVAL);
    /* Queue retains while foreground ownership changes; no process lock. */
    cloning=true;
    assert(process_group_try_retain(&ref,&event));
    cloning=false;
    assert(process_group_release_ref(&ref) && !ref.generation);
    assert(!process_group_release_ref(&ref));
    assert(!process_group_signal(&event,SIGINT));
    assert(process_signal_take(2)==SIGINT);
    /* Same living PID can leave its namesake group; recreation must fail while
     * an old event owns that otherwise empty identity. No partial regroup. */
    assert(!process_record_setpgid(2,0,1));
    assert(process_group_signal(&event,SIGINT)==SYSCALL_ESRCH);
    assert(process_record_setpgid(2,0,0)==SYSCALL_EPERM);
    assert(process_record_group(2)==1);
    assert(process_signal_take(2)==0);
    assert(process_group_release_ref(&event));
    assert(!process_record_setpgid(2,0,0));
    ref=acquire(1,2);
    assert(ref.slot != stale.slot || ref.generation != stale.generation);
    assert(process_group_signal(&stale,SIGINT)==SYSCALL_ESRCH);
    assert(!process_group_release_ref(&stale));
    assert(!process_signal_take(2));
    /* A staged member alone preserves identity after the leader exits. */
    assert(!process_record_begin(3,1,true,SPAWN_SETPGROUP|SPAWN_STAGED,2));
    process_record_attach_signals(3,&member);
    assert(process_record_exit(2,0)); process_record_forget(2);
    assert(!process_record_begin(4,1,true,SPAWN_SETPGROUP,2));
    process_record_attach_signals(4,&peer);
    assert(!process_group_signal(&ref,SIGINT));
    assert(process_signal_take(3)==SIGINT && process_signal_take(4)==SIGINT);
    process_record_abort(4);
    assert(!process_group_signal(&ref,SIGTSTP));
    assert(process_signal_take_control(3)==SIGTSTP);
    assert(!process_group_signal(&ref,SIGCONT));
    assert(process_record_resume(3));
    assert(!process_group_signal(&ref,SIGINT));
    assert(process_signal_take(3)==SIGINT);
    process_record_abort(3);
    assert(process_group_signal(&ref,0)==SYSCALL_ESRCH);
    assert(process_group_signal(&ref,32)==SYSCALL_EINVAL);
    assert(process_group_release_ref(&ref));
    process_record_abort(1);
    /* Parent abort only owns its own child record; collect dead child status. */
    assert(process_record_wait(1,2,0,NULL,false)==2);
    baseline();
}
static void capacity(void) {
    assert(!process_record_begin(100,0,false,0,0));
    process_group_ref_t refs[PROCESS_GROUP_CAPACITY-1];
    for (unsigned i=0; i<PROCESS_GROUP_CAPACITY-1; ++i) {
        uint64_t pid=101+i;
        assert(!process_record_begin(pid,100,true,SPAWN_SETPGROUP|SPAWN_STAGED,0));
        refs[i]=acquire(100,pid);
        process_record_abort(pid);
    }
    assert(process_record_begin(1000,100,true,SPAWN_SETPGROUP,0)==SYSCALL_ENOMEM);
    assert(process_record_group(1000)==SYSCALL_ESRCH);
    assert(process_record_wait(100,1000,0,NULL,false)==SYSCALL_ECHILD);
    /* Joining an existing group does not allocate another group record. */
    assert(!process_record_begin(1001,100,true,0,0));
    assert(process_record_setpgid(1001,0,0)==SYSCALL_ENOMEM);
    assert(process_record_group(1001)==100);
    assert(process_group_release_ref(&refs[0]));
    assert(!process_record_setpgid(1001,0,0));
    process_record_abort(1001);
    for (unsigned i=1; i<PROCESS_GROUP_CAPACITY-1; ++i)
        assert(process_group_release_ref(&refs[i]));
    process_record_abort(100);
    baseline();
}
static process_group_ref_t shared;
static void *exit_worker(void *arg) {
    uint64_t pid=(uintptr_t)arg;
    process_record_exit(pid,0); process_record_forget(pid);
    return NULL;
}
static void *clone_worker(void *arg) {
    (void)arg;
    for (unsigned i=0; i<2000; ++i) {
        process_group_ref_t ref={0};
        cloning=true;
        bool ok=process_group_try_retain(&shared,&ref);
        cloning=false;
        if (!ok) { assert(!ref.generation); continue; }
        assert(!process_group_signal(&ref,SIGINT));
        assert(process_group_release_ref(&ref));
    }
    return NULL;
}
static void concurrency(void) {
    signal_state_t state;
    assert(!process_record_begin(2000,0,false,0,0));
    process_record_attach_signals(2000,&state);
    shared=acquire(2000,2000);
    pthread_t threads[4];
    for (unsigned i=0; i<4; ++i) assert(!pthread_create(&threads[i],NULL,clone_worker,NULL));
    for (unsigned i=0; i<4; ++i) assert(!pthread_join(threads[i],NULL));
    assert(groups[shared.slot].refs==1);
    assert(process_signal_take(2000)==SIGINT);
    /* Explicit overflow rejection must not wrap the reference count. */
    process_group_ref_t copy={0};
    __atomic_store_n(&groups[shared.slot].refs,UINT32_MAX,__ATOMIC_RELEASE);
    assert(!process_group_try_retain(&shared,&copy) && !copy.generation);
    assert(process_group_acquire(2000,2000,&copy)==SYSCALL_ENOMEM);
    __atomic_store_n(&groups[shared.slot].refs,1,__ATOMIC_RELEASE);
    pthread_t exiting;
    assert(!pthread_create(&exiting,NULL,exit_worker,(void *)(uintptr_t)2000));
    for (unsigned i=0; i<2000; ++i) {
        assert(process_group_try_retain(&shared,&copy));
        int64_t result=process_group_signal(&copy,SIGINT);
        assert(result==0 || result==SYSCALL_ESRCH);
        assert(process_group_release_ref(&copy));
    }
    assert(!pthread_join(exiting,NULL));
    assert(process_group_signal(&shared,SIGINT)==SYSCALL_ESRCH);
    assert(process_group_release_ref(&shared));
    baseline();
}
int main(void) {
    baseline(); lifecycle(); capacity(); concurrency();
    puts("PASS group lifetime: recreation, retained targets, staged/leader exit, rollback, capacity, refs (pthread adapter)");
}
