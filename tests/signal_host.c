#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "process_table.h"
#include "spinlock.h"
#include "syscall_abi.h"
uint64_t spin_lock_irqsave(spinlock_t *l) { assert(!pthread_mutex_lock(&l->mutex)); return 0; }
void spin_unlock_irqrestore(spinlock_t *l, uint64_t f) { (void)f; assert(!pthread_mutex_unlock(&l->mutex)); }
static uint64_t target;
static bool done;
static void *sender(void *unused) {
    (void)unused;
    while (!__atomic_load_n(&done, __ATOMIC_ACQUIRE)) {
        uint64_t pid=__atomic_load_n(&target, __ATOMIC_ACQUIRE);
        if (!pid) continue;
        int64_t r=process_signal_send(1,pid,SIGKILL);
        assert(r==0 || r==SYSCALL_ESRCH);
    }
    return NULL;
}
int main(void) {
    signal_state_t parent, child, peer, foreign;
    assert(!process_record_begin(1,0,false,0,0));
    process_record_attach_signals(1,&parent);
    assert(!process_record_begin(2,0,false,0,0));
    process_record_attach_signals(2,&foreign);
    assert(process_signal_send(1,2,0)==SYSCALL_EPERM);
    assert(process_signal_send(1,999,0)==SYSCALL_ESRCH);
    assert(process_signal_send(1,-1,0)==SYSCALL_EINVAL);
    assert(process_signal_send(1,INT64_MIN,0)==SYSCALL_EINVAL);
    assert(process_signal_send(1,1,32)==SYSCALL_EINVAL);
    assert(process_signal_send(1,1,SIGQUIT)==SYSCALL_EINVAL); /* Reserved; STOP is supported in 2C. */
    assert(!process_signal_send(1,1,0) && !parent.pending_mask);
    uint64_t mask=SIGNAL_BIT(SIGTERM)|SIGNAL_BIT(SIGKILL), old;
    assert(!process_signal_mask(1,SIG_SETMASK,&mask,&old) && old==0);
    assert(parent.blocked_mask==SIGNAL_BIT(SIGTERM));
    mask=1ULL<<63;
    assert(process_signal_mask(1,SIG_BLOCK,&mask,NULL)==SYSCALL_EINVAL);
    signal_action_t act={.handler=SIG_IGN}, previous;
    assert(!process_signal_action(1,SIGINT,&act,&previous) && previous.handler==SIG_DFL);
    assert(process_signal_action(1,SIGKILL,&act,NULL)==SYSCALL_EINVAL);
    act.handler=0x400000;
    assert(!process_signal_action(1,SIGTERM,&act,NULL));
    assert(!process_signal_action(1,SIGTERM,NULL,&previous) && previous.handler==0x400000);
    assert(!process_signal_send(1,1,SIGINT) && !parent.pending_mask);
    assert(!process_signal_send(1,1,SIGTERM) && !signal_state_ready(&parent));
    assert(!process_record_begin(3,1,true,SPAWN_SETPGROUP,0));
    process_record_attach_signals(3,&child);
    assert(child.blocked_mask==parent.blocked_mask && child.ignored_mask==parent.ignored_mask);
    assert(!child.pending_mask); /* Pending signals never inherit. */
    assert(!process_record_begin(4,1,true,SPAWN_SETPGROUP,3));
    process_record_attach_signals(4,&peer);
    assert(!process_signal_send(1,-3,SIGTERM));
    assert(!process_signal_send(1,-3,SIGTERM)); /* Coalescing. */
    assert(child.pending_mask==SIGNAL_BIT(SIGTERM) && peer.pending_mask==SIGNAL_BIT(SIGTERM));
    mask=SIGNAL_BIT(SIGTERM);
    assert(!process_signal_mask(3,SIG_UNBLOCK,&mask,NULL));
    assert(signal_state_ready(&child) && process_signal_take(3)==SIGTERM);
    assert(process_signal_take(3)==0);
    act=(signal_action_t){.handler=SIG_IGN};
    assert(!process_signal_action(4,SIGTERM,&act,NULL) && !peer.pending_mask);
    assert(!process_signal_send(1,-3,SIGKILL));
    assert(process_signal_take(3)==SIGKILL && process_signal_take(4)==SIGKILL);
    assert(process_record_exit_signal(3,0,SIGKILL));
    process_record_forget(3);
    uint64_t status;
    assert(process_record_wait(1,3,0,&status,false)==3 && status==SIGKILL);
    assert(WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL && !WIFEXITED(status));
    assert(process_signal_send(1,3,0)==SYSCALL_ESRCH);
    assert(process_record_exit_signal(4,0,SIGTERM));
    process_record_forget(4);
    assert(process_record_wait(1,4,0,&status,true)==4 && status==128+SIGTERM);
    assert(!process_signal_send(1,0,SIGHUP));
    assert(process_signal_take(1)==SIGHUP);
    /* Sender races registry detachment and freeing the attached state. */
    pthread_t thread;
    assert(!pthread_create(&thread,NULL,sender,NULL));
    for (uint64_t pid=100;pid<2100;pid++) {
        signal_state_t *s=malloc(sizeof(*s)); assert(s);
        assert(!process_record_begin(pid,1,true,0,0));
        process_record_attach_signals(pid,s);
        __atomic_store_n(&target,pid,__ATOMIC_RELEASE);
        if (pid&1) {
            process_record_exit(pid,137); free(s); process_record_forget(pid);
            assert(process_record_wait(1,pid,0,&status,false)==(int64_t)pid && status==0x8900);
        } else { process_record_abort(pid); free(s); }
    }
    __atomic_store_n(&done,true,__ATOMIC_RELEASE);
    assert(!pthread_join(thread,NULL));
    process_record_abort(1); process_record_abort(2);
    puts("PASS S8 signals: masks, dispositions, targeting, inheritance, status, 2000 exit/abort signal races (pthread adapter)");
}
