#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include "pkg_mutation_candidate.c"
#include "process_table.h"
static pthread_barrier_t running,release;
static uint64_t mount_id;
typedef struct {pm_key key;} worker_arg;
static void *ordinary(void *arg){
    pm_key key=((worker_arg *)arg)->key;const pm_view *v=pm_pin(key,mount_id);
    assert(v&&v->kind==PM_ORDINARY&&v->mount==mount_id);
    pthread_barrier_wait(&running);pthread_barrier_wait(&release);
    assert(v->kind==PM_ORDINARY&&pm_unpin(key)&&pm_finish(key));return NULL;
}
int main(void){
    assert(!pthread_barrier_init(&running,NULL,5)&&!pthread_barrier_init(&release,NULL,5));
    for(unsigned round=0;round<128;round++){
        mount_id=round+1;assert(pm_configure(mount_id));
        creds_t creds;creds_init_root(&creds);uint64_t pid=1000+round,session;
        assert(!process_record_begin(pid,0,false,0,0)&&process_record_bind_creds(pid,&creds));process_record_commit(pid);
        pw_key owner;assert(pw_register(pid,&owner)&&pw_begin(owner,true,&session));
        pthread_t threads[4];worker_arg args[4];
        for(unsigned i=0;i<4;i++){assert(!pm_ordinary(mount_id,&args[i].key));assert(!pthread_create(&threads[i],NULL,ordinary,&args[i]));}
        pthread_barrier_wait(&running);
        pm_key waiting,account,denied={.generation=UINT64_MAX};assert(!pm_ordinary(mount_id,&waiting));
        assert(!pm_account(mount_id,&account)&&!pm_pin(account,mount_id));
        assert(!pm_pin(waiting,mount_id)&&pm_ordinary(mount_id,&denied)==-VFS_EAGAIN&&denied.generation==UINT64_MAX);
        assert(pm_package(mount_id,owner,session,&denied)==-VFS_EAGAIN&&pw_references(owner)==2);
        pm_key cancelled=account;assert(pm_finish(account)&&!pm_account(mount_id,&account));
        assert(!pm_pin(cancelled,mount_id));
        assert(!pm_configure(mount_id+1000));pthread_barrier_wait(&release);
        for(unsigned i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));
        const pm_view *v=pm_pin(account,mount_id);assert(v&&v->kind==PM_ACCOUNT);
        assert(!pm_pin(waiting,mount_id)&&!pm_finish(account));
        assert(pm_unpin(account)&&pm_finish(account)&&!pm_finish(account));
        assert(!pm_pin(waiting,mount_id+1)&&pm_pin(waiting,mount_id));
        assert(pm_unpin(waiting)&&pm_finish(waiting)&&!pm_pin(waiting,mount_id));
        pm_key package;assert(!pm_package(mount_id,owner,session,&package));v=pm_pin(package,mount_id);assert(v&&v->kind==PM_PACKAGE);
        assert(pw_close(owner,session)&&pw_detach(owner));(void)process_record_exit(pid,0);process_record_forget(pid);
        assert(pw_references(owner)==2&&pm_package(mount_id,owner,session,&denied)==-VFS_EACCES);
        assert(v->kind==PM_PACKAGE&&!pm_finish(package));assert(pm_unpin(package)&&pm_finish(package)&&!pw_references(owner));
        pm_key pool[PM_CAPACITY-1];for(unsigned i=0;i<PM_CAPACITY-1;i++)assert(!pm_ordinary(mount_id,&pool[i]));
        assert(pm_ordinary(mount_id,&denied)==-VFS_EAGAIN&&!pm_account(mount_id,&account));
        assert(pm_pin(account,mount_id)&&pm_unpin(account)&&pm_finish(account));
        for(unsigned i=0;i<PM_CAPACITY-1;i++)assert(pm_finish(pool[i]));
        unsigned count,pins;bool pending;pm_counts(&count,&pins,&pending);assert(!count&&!pins&&!pending);
    }
    assert(!pthread_barrier_destroy(&running)&&!pthread_barrier_destroy(&release));
    puts("PASS mutation admission rounds=128 concurrent-workers=4 priority pool stale mount owner-death drained");return 0;
}
