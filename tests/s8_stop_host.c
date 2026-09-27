/* Actual process metadata implementation with pthread locks. This does not
 * simulate or claim verification of context switching, IRQs, or runqueues. */
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include "process_table.c"

static _Thread_local bool held;
uint64_t spin_lock_irqsave(spinlock_t *lock) {
    assert(!held); assert(!pthread_mutex_lock(&lock->mutex)); held=true; return 0;
}
void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    (void)flags; assert(held); held=false; assert(!pthread_mutex_unlock(&lock->mutex));
}
static signal_state_t parent_state, child_state;
static void counters(uint64_t seq, uint64_t reported) {
    uint64_t irq=spin_lock_irqsave(&g_process_lock);
    bool found=false;
    for (unsigned i=0; i<PROCESS_CAPACITY; ++i) if (children[i].used && children[i].pid==2) {
        assert(children[i].event_seq==seq && children[i].reported_seq==reported);
        found=true;
    }
    assert(found); spin_unlock_irqrestore(&g_process_lock,irq);
}
static void stop(unsigned sig) {
    assert(!process_signal_send(1,2,sig));
    assert(process_signal_take_control(2)==sig);
}
static uint64_t collect(unsigned options) {
    uint64_t status=UINT64_MAX;
    assert(process_record_wait(1,2,options,&status,false)==2);
    return status;
}
static void *publish(void *sig) {
    assert(!process_signal_send(1,2,(uintptr_t)sig)); return NULL;
}
static void *exit_record(void *pid) {
    process_record_exit((uintptr_t)pid,7); return NULL;
}
int main(void) {
    assert(!process_record_begin(1,0,false,0,0));
    process_record_attach_signals(1,&parent_state);
    assert(!process_record_begin(2,1,true,0,0));
    process_record_attach_signals(2,&child_state);
    signal_action_t act={.handler=SIG_IGN};
    assert(process_signal_action(1,SIGCHLD,&act,NULL)==SYSCALL_EINVAL);
    act=(signal_action_t){.flags=2}; /* Unsupported SA_NOCLDWAIT/any flags. */
    assert(process_signal_action(1,SIGCHLD,&act,NULL)==SYSCALL_EINVAL);
    act=(signal_action_t){.handler=SIG_IGN};
    assert(process_signal_action(2,SIGSTOP,&act,NULL)==SYSCALL_EINVAL);
    uint64_t mask=SIGNAL_UNBLOCKABLE;
    assert(!process_signal_mask(2,SIG_BLOCK,&mask,NULL) && !child_state.blocked_mask);
    counters(0,0);
    stop(SIGSTOP); counters(1,0);
    assert(!signal_state_ready(&parent_state)); /* Default CHLD preserves report. */
    assert(process_record_wait(1,2,WNOHANG,NULL,false)==0);
    assert(process_record_wait(1,2,WUNTRACED,NULL,true)==0); /* Legacy exit-only. */
    counters(1,0);
    assert(collect(WUNTRACED)==((SIGSTOP<<8)|0x7f)); counters(1,1);
    assert(process_record_wait(1,2,WUNTRACED|WNOHANG,NULL,false)==0);
    stop(SIGSTOP); counters(1,1); /* Already stopped: no duplicate transition. */
    assert(!process_signal_send(1,2,SIGCONT)); counters(1,1); /* Publication alone. */
    assert(process_record_resume(2)); counters(2,1);
    assert(!process_record_resume(2));
    stop(SIGTSTP); counters(3,1);
    assert(collect(WUNTRACED)==((SIGTSTP<<8)|0x7f)); counters(3,3);
    /* Ignored and blocked CONT resumes, cancelling even blocked stop bits. */
    assert(!process_signal_action(2,SIGCONT,&act,NULL));
    mask=SIGNAL_BIT(SIGCONT)|SIGNAL_BIT(SIGTSTP);
    assert(!process_signal_mask(2,SIG_BLOCK,&mask,NULL));
    assert(!process_signal_send(1,2,SIGTSTP));
    assert(!process_signal_send(1,2,SIGCONT));
    assert(!(child_state.pending_mask & SIGNAL_STOPS));
    assert(process_record_resume(2)); counters(4,3);
    assert(process_record_wait(1,2,WUNTRACED|WNOHANG,NULL,false)==0); counters(4,3);
    assert(collect(WCONTINUED)==0xffff); counters(4,4);
    assert(!process_signal_send(1,2,SIGCONT) && !process_record_resume(2)); counters(4,4);
    mask=0; assert(!process_signal_mask(2,SIG_SETMASK,&mask,NULL));
    /* Ignored TSTP is retained but not delivered; CONT cancels it. */
    assert(!process_signal_action(2,SIGTSTP,&act,NULL));
    assert(!process_signal_send(1,2,SIGTSTP));
    assert(child_state.pending_mask & SIGNAL_BIT(SIGTSTP));
    assert(!signal_state_ready(&child_state) && !process_signal_take_control(2));
    assert(!process_signal_send(1,2,SIGCONT) && !(child_state.pending_mask & SIGNAL_STOPS));
    act=(signal_action_t){0}; assert(!process_signal_action(2,SIGTSTP,&act,NULL));
    /* Caught CHLD is coalesced, with durable state already readable. */
    act.handler=0x400000; assert(!process_signal_action(1,SIGCHLD,&act,NULL));
    stop(SIGTTIN); counters(5,4);
    assert(parent_state.pending_mask & SIGNAL_BIT(SIGCHLD));
    signal_action_t got;
    assert(process_signal_take_action(1,&got,NULL)==SIGCHLD && got.handler==act.handler);
    assert(collect(WUNTRACED)==((SIGTTIN<<8)|0x7f));
    /* A later stop cancels a not-yet-committed CONT, including handler bit. */
    assert(!process_signal_action(2,SIGCONT,&act,NULL));
    mask=SIGNAL_BIT(SIGCONT); assert(!process_signal_mask(2,SIG_BLOCK,&mask,NULL));
    assert(!process_signal_send(1,2,SIGCONT));
    assert(!process_signal_send(1,2,SIGTTOU));
    assert(!(child_state.pending_mask & SIGNAL_BIT(SIGCONT)));
    assert(!process_record_resume(2)); counters(5,5);
    /* Simultaneously pending CONT/KILL has exactly one resume claim and no
     * CONTINUED report. The scheduler's queue transitions need Ring 3 coverage. */
    pthread_t a,b;
    assert(!pthread_create(&a,NULL,publish,(void *)(uintptr_t)SIGCONT));
    assert(!pthread_create(&b,NULL,publish,(void *)(uintptr_t)SIGKILL));
    assert(!pthread_join(a,NULL) && !pthread_join(b,NULL));
    assert(process_record_resume(2) && !process_record_resume(2)); counters(5,5);
    assert(process_signal_take_control(2)==SIGKILL); /* Observed, not consumed. */
    assert(process_signal_take_action(2,NULL,NULL)==SIGKILL);
    assert(process_record_exit_signal(2,0,SIGKILL)); counters(6,5);
    assert(collect(0)==SIGKILL);
    assert(process_record_wait(1,2,~WNOHANG,NULL,false)==SYSCALL_EINVAL);
    assert(process_record_wait(1,2,WNOHANG,NULL,false)==SYSCALL_ECHILD);
    process_record_forget(2);
    /* No collection between stop/continue/stop: only the latest stop remains. */
    assert(!process_record_begin(104,1,true,0,0));
    process_record_attach_signals(104,&child_state);
    assert(!process_signal_send(1,104,SIGSTOP));
    assert(process_signal_take_control(104)==SIGSTOP);
    assert(!process_signal_send(1,104,SIGCONT) && process_record_resume(104));
    assert(!process_signal_send(1,104,SIGTTOU));
    assert(process_signal_take_control(104)==SIGTTOU);
    uint64_t irq=spin_lock_irqsave(&g_process_lock);
    for (unsigned i=0;i<PROCESS_CAPACITY;i++) if (children[i].used && children[i].pid==104)
        assert(children[i].event_seq==3 && children[i].reported_seq==0);
    spin_unlock_irqrestore(&g_process_lock,irq);
    uint64_t latest;
    assert(process_record_wait(1,104,WCONTINUED|WNOHANG,&latest,false)==0);
    assert(process_record_wait(1,104,WUNTRACED,&latest,false)==104 && latest==((SIGTTOU<<8)|0x7f));
    process_record_exit(104,0); process_record_forget(104);
    assert(process_record_wait(1,104,0,NULL,false)==104);
    /* Exit supersedes uncollected stop and continue; raw SYS_WAIT unaffected. */
    for (uint64_t pid=3; pid<103; ++pid) {
        assert(!process_record_begin(pid,1,true,0,0));
        process_record_attach_signals(pid,&child_state);
        assert(!process_signal_send(1,pid,SIGSTOP));
        assert(process_signal_take_control(pid)==SIGSTOP);
        if (pid&1) {
            assert(!process_signal_send(1,pid,SIGCONT)); assert(process_record_resume(pid));
        }
        assert(process_record_exit(pid,0x1234));
        uint64_t status;
        assert(process_record_wait(1,pid,WUNTRACED|WCONTINUED,&status,pid&1)==(int64_t)pid);
        assert(status==((pid&1) ? 0x1234 : 0x3400));
        process_record_forget(pid);
    }
    /* Parent exit discards its reservations, without dangling signal state. */
    assert(!process_record_begin(103,1,true,0,0));
    process_record_attach_signals(103,&child_state);
    assert(!process_signal_send(1,103,SIGSTOP));
    assert(process_signal_take_control(103)==SIGSTOP);
    process_record_exit(1,0); process_record_forget(1);
    assert(!process_record_exit(103,7)); process_record_forget(103);
    assert(process_record_wait(1,103,WNOHANG,NULL,false)==SYSCALL_ECHILD);
    /* Exit publication racing parent destruction must neither dereference a
     * detached signal state nor leave an uncollectable reservation behind. */
    for (uint64_t pid=200;pid<264;pid+=2) {
        signal_state_t par,sub;
        assert(!process_record_begin(pid,0,false,0,0));
        process_record_attach_signals(pid,&par);
        act=(signal_action_t){.handler=0x400000};
        assert(!process_signal_action(pid,SIGCHLD,&act,NULL));
        assert(!process_record_begin(pid+1,pid,true,0,0));
        process_record_attach_signals(pid+1,&sub);
        assert(!pthread_create(&a,NULL,exit_record,(void *)(uintptr_t)pid));
        assert(!pthread_create(&b,NULL,exit_record,(void *)(uintptr_t)(pid+1)));
        assert(!pthread_join(a,NULL) && !pthread_join(b,NULL));
        assert(process_record_wait(pid,pid+1,WNOHANG,NULL,false)==SYSCALL_ECHILD);
        process_record_forget(pid); process_record_forget(pid+1);
    }
    puts("PASS S8 stop metadata: transitions, counters, filtering, cancellation, kill priority, CHLD, reuse, parent exit (no scheduler/IRQ claim)");
}
