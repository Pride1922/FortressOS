/* Actual metadata, pthread locks only: no scheduler/IRQ claim. */
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include "process_table.h"
#include "spinlock.h"
#include "syscall_abi.h"
static _Thread_local bool held;
uint64_t spin_lock_irqsave(spinlock_t *l) {
    assert(!held); assert(!pthread_mutex_lock(&l->mutex)); held=true; return 0;
}
void spin_unlock_irqrestore(spinlock_t *l,uint64_t flags) {
    (void)flags; assert(held); held=false; assert(!pthread_mutex_unlock(&l->mutex));
}
static signal_state_t states[16];
static void spawn(unsigned pid,unsigned parent,unsigned group) {
    assert(!process_record_begin(pid,parent,parent!=0,SPAWN_SETPGROUP,group));
    process_record_attach_signals(pid,&states[pid]); process_record_commit(pid);
}
static void stop(unsigned pid) {
    assert(!process_signal_send(1,pid,SIGSTOP));
    assert(process_signal_take_control(pid)==SIGSTOP);
}
int main(void) {
    spawn(1,0,0); spawn(2,1,0); spawn(3,2,0); spawn(4,2,3);
    spawn(5,1,0); spawn(6,2,0);
    stop(3);
    assert(!signal_state_resume(&states[3]));
    process_group_ref_t ref={0};
    assert(!process_group_acquire(1,3,&ref));
    process_record_exit(2,0); process_record_forget(2);
    assert(signal_state_resume(&states[3]));
    assert(states[4].pending_mask & SIGNAL_BIT(SIGKILL)); /* whole group */
    assert(!states[5].pending_mask && !states[6].pending_mask);
    assert(process_record_group(3)==3 && process_record_group(6)==6);
    assert(process_record_wait(2,-1,WNOHANG,NULL,false)==SYSCALL_ECHILD);
    assert(process_record_resume(3));
    stop(6); /* Parent exit won the race before the child stopped. */
    assert(signal_state_resume(&states[6]));
    for (unsigned i=3;i<=6;++i) { process_record_exit(i,0); process_record_forget(i); }
    assert(process_group_release_ref(&ref));

    spawn(7,1,0); spawn(8,1,0); spawn(9,7,0); spawn(10,8,9);
    stop(9); process_record_exit(7,0); process_record_forget(7);
    assert(!(states[9].pending_mask & SIGNAL_BIT(SIGKILL))); /* second anchor */
    process_record_exit(8,0); process_record_forget(8);
    assert(states[9].pending_mask & SIGNAL_BIT(SIGKILL));
    assert(states[10].pending_mask & SIGNAL_BIT(SIGKILL));
    for (unsigned i=9;i<=10;++i) { process_record_exit(i,0); process_record_forget(i); }
    process_record_exit(1,0); process_record_forget(1);
    puts("PASS orphan metadata: group KILL, stop/exit orderings, live identities and other-parent anchor");
}
