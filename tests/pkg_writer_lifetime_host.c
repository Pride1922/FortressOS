#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include "pkg_writer_candidate.h"
#include "process_table.h"
static creds_t credentials[2];
static pthread_barrier_t started,finish;
static pw_key owned_operation;
static void *worker(void *unused){
    (void)unused;pthread_barrier_wait(&started);pthread_barrier_wait(&finish);
    assert(pw_complete(owned_operation));return NULL;
}
static pw_key birth(uint64_t pid,unsigned slot){
    assert(!process_record_begin(pid,0,false,0,0));creds_init_root(&credentials[slot]);
    assert(process_record_bind_creds(pid,&credentials[slot]));process_record_commit(pid);
    pw_key k;assert(pw_register(pid,&k));return k;
}
static void death(uint64_t pid,pw_key k){
    /* Return value reports a waitable child record, not exit success. */
    (void)process_record_exit(pid,0);process_snapshot_t p;
    assert(!process_record_snapshot_pid(pid,&p)||p.state==PROCESS_ZOMBIE);
    assert(pw_detach(k));process_record_forget(pid);
}
int main(void){
    assert(!pthread_barrier_init(&started,NULL,2)&&!pthread_barrier_init(&finish,NULL,2));
    unsigned cases=0;
    for(unsigned i=0;i<128;i++){
        uint64_t a=1000+2*i,b=a+1,session,other=UINT64_MAX;pw_key owner=birth(a,0),child=birth(b,1),op;
        assert(!pw_begin(owner,false,&other)&&other==UINT64_MAX);
        assert(pw_begin(owner,true,&session));assert(!pw_begin(child,true,&other));
        assert(!pw_enter(child,session,&op));assert(pw_enter(owner,session,&owned_operation));
        assert(pw_references(owner)==3);pthread_t thread;assert(!pthread_create(&thread,NULL,worker,NULL));
        pthread_barrier_wait(&started);death(a,owner);assert(pw_references(owner)==2);
        assert(!pw_detach(owner)&&!pw_enter(owner,session,&op)&&!pw_begin(child,true,&other));
        pthread_barrier_wait(&finish);assert(!pthread_join(thread,NULL));assert(!pw_references(owner));
        assert(!pw_complete(owned_operation)); /* stale completion cannot drop a new ref */
        assert(!pw_begin(child,false,&other)); /* death did not clear durable escrow */
        assert(pw_begin(child,true,&other)&&other!=session);assert(!pw_close(owner,session));
        creds_t old=credentials[1],drop=old;drop.euid=1000;drop.cap_effective=0;
        assert(process_record_publish_creds(b,&old,&drop));
        assert(!pw_enter(child,other,&op));assert(pw_close(child,other)); /* cleanup after drop */
        assert(process_record_publish_creds(b,&drop,&old));assert(pw_begin(child,true,&other));
        assert(pw_enter(child,other,&op));assert(pw_exec(child));
        assert(!pw_enter(child,other,&owned_operation)&&!pw_begin(child,true,&session));
        assert(pw_complete(op));assert(pw_references(child)==1);death(b,child);assert(!pw_references(child));
        cases++;
    }
    /* Same numeric PID can be registered anew in this host adapter, but the old
     * owner key never names the new object. Kernel PID allocation is monotonic. */
    pw_key old=birth(99999,0);death(99999,old);pw_key next=birth(99999,0);uint64_t session;
    assert(old.generation!=next.generation&&!pw_begin(old,true,&session));assert(pw_begin(next,true,&session));
    assert(pw_close(next,session));death(99999,next);
    assert(!pthread_barrier_destroy(&started)&&!pthread_barrier_destroy(&finish));
    printf("PASS writer lifetimes=%u actual-process-registry private-owner-arena pthread-exclusion no-TCB-retention\n",cases);return 0;
}
