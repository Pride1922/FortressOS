#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "process_table.h"
#include "spinlock.h"
#include "syscall_abi.h"
static _Thread_local unsigned held;
uint64_t spin_lock_irqsave(spinlock_t *l) {
    assert(!held++); assert(!pthread_mutex_lock(&l->mutex)); return 0;
}
void spin_unlock_irqrestore(spinlock_t *l, uint64_t flags) {
    (void)flags; assert(held-- == 1); assert(!pthread_mutex_unlock(&l->mutex));
}
static process_snapshot_t row(uint64_t index) {
    process_snapshot_t p; assert(process_record_snapshot(index, &p)); return p;
}
static void begin(uint64_t pid, uint64_t parent, bool waitable, const char *name) {
    assert(!process_record_begin(pid,parent,waitable,0,0));
    process_record_set_name(pid,name); process_record_commit(pid);
}
static void ticks(uint64_t pid, uint64_t value) {
    process_tick_sample_t s={pid,value}; process_record_merge_ticks(&s,1);
}
static void *refresh(void *arg) {
    uint64_t pid=(uintptr_t)arg;
    for (unsigned i=0;i<2000;++i) {
        ticks(pid,i);
        process_snapshot_t s;
        (void)process_record_snapshot(1,&s);
    }
    return NULL;
}
int main(void) {
    process_snapshot_t out={.pid=999};
    assert(!process_record_snapshot(0,&out) && out.pid==999);
    assert(!process_record_snapshot(UINT64_MAX,&out));
    assert(!process_record_snapshot(0,NULL));
    begin(1,0,false,"parent");
    const char *names[]={"", "123456789012345", "1234567890123456", "12345678901234567890123456789012"};
    for (unsigned i=0;i<4;++i) {
        uint64_t pid=10+i;
        assert(!process_record_begin(pid,1,true,SPAWN_STAGED,0));
        process_record_set_name(pid,names[i]); ticks(pid,50);
        assert(!process_record_snapshot(1,&out));
        process_record_commit(pid);
        process_snapshot_t s=row(1);
        assert(s.pid==pid && s.parent==1 && s.pgid==1 && s.sid==1);
        assert(s.state==PROCESS_RUNNING && !s.cpu_ticks);
        size_t n=strlen(names[i]); if (n>15) n=15;
        assert(!memcmp(s.name,names[i],n));
        for (;n<16;++n) assert(!s.name[n]);
        process_record_set_name(pid,"changed");
        assert(!memcmp(row(1).name,s.name,16));
        process_record_abort(pid);
    }
    puts("PASS metadata: names, zero fill, publication, staged exclusion, abort");
    for (unsigned order=0;order<2;++order) {
        uint64_t pid=20+order; begin(pid,1,true,"child");
        signal_state_t signals;
        process_record_attach_signals(pid,&signals);
        assert(!process_signal_send(1,pid,SIGSTOP));
        assert(process_signal_take_control(pid)==SIGSTOP);
        assert(row(1).state==PROCESS_STOPPED);
        assert(!process_signal_send(1,pid,SIGCONT));
        assert(process_record_resume(pid));
        assert(row(1).state==PROCESS_RUNNING);
        ticks(pid,15); ticks(pid,3); assert(row(1).cpu_ticks==15);
        assert(process_record_exit_accounted(pid,7,0,30));
        ticks(pid,900); assert(row(1).cpu_ticks==30);
        assert(row(1).state==PROCESS_ZOMBIE);
        assert(process_record_group(pid)==SYSCALL_ESRCH);
        if (!order) process_record_forget(pid);
        assert(row(1).state==PROCESS_ZOMBIE);
        uint64_t status;
        assert(process_record_wait(1,pid,0,&status,false)==(int64_t)pid);
        assert(WIFEXITED(status) && WEXITSTATUS(status)==7);
        assert(!process_record_snapshot(1,&out));
        if (order) {
            assert(process_record_begin(pid,1,true,0,0)==SYSCALL_EINVAL);
            process_record_forget(pid);
        }
    }
    puts("PASS metadata: stop/continue, monotonic/final ticks, wait/reaper orders");
    for (unsigned i=0;i<PROCESS_CAPACITY-1;++i) {
        begin(100+i,1,true,"zombie");
        assert(process_record_exit_accounted(100+i,0,0,i));
        process_record_forget(100+i);
    }
    assert(process_record_begin(200,1,true,0,0)==SYSCALL_ENOMEM);
    assert(row(PROCESS_CAPACITY-1).state==PROCESS_ZOMBIE);
    assert(!process_record_snapshot(PROCESS_CAPACITY,&out));
    process_record_exit(1,0); process_record_forget(1);
    assert(!process_record_snapshot(0,&out));
    begin(300,0,false,"parent2");
    ticks(100,UINT64_MAX); assert(!row(0).cpu_ticks);
    begin(301,300,true,"race");
    pthread_t threads[4];
    for (unsigned i=0;i<4;++i) assert(!pthread_create(&threads[i],NULL,refresh,(void *)301));
    assert(process_record_exit_accounted(301,0,0,5000));
    process_record_forget(301);
    for (unsigned i=0;i<4;++i) assert(!pthread_join(threads[i],NULL));
    assert(row(1).cpu_ticks==5000);
    assert(process_record_wait(300,301,0,NULL,false)==301);
    begin(302,300,true,"live orphan");
    process_record_exit(300,0); process_record_forget(300);
    assert(row(0).pid==302);
    assert(!process_record_exit_accounted(302,0,0,12));
    assert(!process_record_snapshot(0,&out)); process_record_forget(302);
    puts("PASS metadata: capacity, parent discard, stale PID, concurrent refresh/finalize, orphan");
}
