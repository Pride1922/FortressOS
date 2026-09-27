#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include "process_table.h"
#include "spinlock.h"
#include "syscall_abi.h"
uint64_t spin_lock_irqsave(spinlock_t *l) { assert(!pthread_mutex_lock(&l->mutex)); return 0; }
void spin_unlock_irqrestore(spinlock_t *l, uint64_t f) { (void)f; assert(!pthread_mutex_unlock(&l->mutex)); }
static void *worker(void *arg) {
    uint64_t base=(uintptr_t)arg;
    assert(!process_record_begin(base,0,false,0,0));
    for (unsigned i=1;i<=1000;i++) {
        uint64_t pid=base+i, status=999;
        assert(!process_record_begin(pid,base,true,SPAWN_SETPGROUP,0));
        assert(process_record_group(pid)==(int64_t)pid);
        process_record_commit(pid);
        assert(process_record_wait(base,-1,WNOHANG,&status,false)==0 && status==999);
        assert(process_record_exit(pid,137));
        process_record_forget(pid);
        assert(process_record_wait(base,-(int64_t)pid,0,&status,false)==(int64_t)pid);
        assert(WIFEXITED(status) && WEXITSTATUS(status)==137);
        assert(process_record_wait(base,pid,0,&status,false)==SYSCALL_ECHILD);
    }
    process_record_exit(base,0); process_record_forget(base);
    return NULL;
}
int main(void) {
    uint64_t status;
    assert(!process_record_begin(1,0,false,0,0));
    assert(!process_record_begin(2,0,false,0,0));
    assert(process_record_setpgid(1,0,0)==SYSCALL_EPERM);
    assert(process_record_begin(3,1,true,SPAWN_SETPGROUP,2)==SYSCALL_EPERM);
    assert(!process_record_begin(3,1,true,SPAWN_SETPGROUP,0));
    assert(!process_record_begin(4,1,true,SPAWN_SETPGROUP,3));
    assert(process_record_session(4)==1);
    process_record_commit(3);
    assert(process_record_setpgid(1,3,1)==SYSCALL_EPERM);
    assert(!process_record_setpgid(1,4,4));
    assert(!process_record_setpgid(4,0,3));
    assert(process_record_exit(3,0x1234)); process_record_forget(3);
    /* Leader exit cannot destroy a group still containing member 4. */
    assert(!process_record_begin(5,1,true,SPAWN_SETPGROUP,3));
    process_record_abort(5);
    assert(process_record_wait(1,3,0,&status,true)==3 && status==0x1234);
    assert(process_record_wait(1,INT64_MIN,0,&status,false)==SYSCALL_EINVAL);
    assert(process_record_wait(1,-1,16,&status,false)==SYSCALL_EINVAL);
    process_record_abort(4);
    /* Independently fill all 64 durable child records, reclaiming live slots. */
    for (unsigned i=0;i<64;i++) {
        assert(!process_record_begin(100+i,1,true,0,0));
        assert(process_record_exit(100+i,i)); process_record_forget(100+i);
    }
    assert(process_record_begin(200,1,true,0,0)==SYSCALL_ENOMEM);
    for (unsigned i=0;i<64;i++) assert(process_record_wait(1,100+i,0,&status,false)==100+i);
    assert(!process_record_begin(201,1,true,0,0));
    process_record_exit(1,0); process_record_forget(1);
    assert(process_record_wait(1,201,0,&status,false)==SYSCALL_ECHILD);
    assert(process_record_group(201)==1); /* Running orphan identity survives. */
    process_record_exit(201,0); process_record_forget(201);
    process_record_abort(2);
    pthread_t threads[4];
    for (uintptr_t i=0;i<4;i++) assert(!pthread_create(&threads[i],NULL,worker,(void *)(10000+i*2000)));
    for (unsigned i=0;i<4;i++) assert(!pthread_join(threads[i],NULL));
    puts("PASS S8 metadata: capacity, rollback, groups, status, orphan, 4000 concurrent lifecycles (pthread lock adapter)");
}
