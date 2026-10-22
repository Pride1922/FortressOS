#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include "pkg_mutation_candidate.c"
#include "pkg_call_candidate.c"
static _Thread_local tcb_t *current_call_task;
tcb_t *thread_current(void){return current_call_task;}
static pthread_barrier_t call_entered,call_cancelled;
static void *cancel_call(void *arg){
    uint64_t pid=*(uint64_t *)arg;pthread_barrier_wait(&call_entered);
    pc_cancel_pid(pid);pthread_barrier_wait(&call_cancelled);return NULL;
}
int main(void){
    assert(pm_configure(1));
    assert(!pthread_barrier_init(&call_entered,NULL,2)&&!pthread_barrier_init(&call_cancelled,NULL,2));
    for(unsigned round=0;round<128;round++){
        tcb_t task={0},other={0};task.tid=5000+round;creds_init_root(&task.creds);
        assert(!process_record_begin(task.tid,0,false,0,0)&&process_record_bind_creds(task.tid,&task.creds));
        process_record_commit(task.tid);current_call_task=&task;
        int context=42;pm_key key,extra;
        assert(!pm_ordinary(1,&key)&&pm_pin(key,1)&&!pc_start(&task,key,1,&context));
        assert(!task.package_call&&pm_unpin(key)&&pm_finish(key));
        assert(!pm_ordinary(1,&key)&&pc_start(&task,key,1,&context));
        assert(pc_count()==1&&pc_context(&task)==&context&&pc_view(&task)->kind==PM_ORDINARY&&pc_pin(&task));
        assert(!pc_context(&other)&&!pc_end(&other));
        other.tid=task.tid+100000;other.package_call=task.package_call;current_call_task=&other;
        assert(!pc_context(&other)&&!pc_commit_begin(&other)&&!pc_end(&other));
        other.package_call=NULL;current_call_task=&task;
        assert(!pm_ordinary(1,&extra)&&!pc_start(&task,extra,1,&context)&&pm_finish(extra));
        pthread_t canceller;assert(!pthread_create(&canceller,NULL,cancel_call,&task.tid));
        assert(pc_commit_begin(&task)&&!pc_end(&task));
        pthread_barrier_wait(&call_entered);pthread_barrier_wait(&call_cancelled);
        assert(pc_cancelled(&task)&&!pc_end(&task));pc_commit_end(&task);
        assert(!pc_commit_begin(&task)&&pc_context(&task)==&context&&pc_end(&task));
        assert(!pthread_join(canceller,NULL)&&!task.package_call&&!pc_count());
        assert(!pm_pin(key,1)&&!pm_finish(key));
        assert(!pm_account(1,&key)&&pc_start(&task,key,1,&context));
        assert(pm_ordinary(1,&extra)==-VFS_EAGAIN);
        /* A queued, unpinned account binding is cancelled by real exit, then
         * acknowledged without a filesystem callback or retained priority. */
        (void)process_record_exit_accounted_times(task.tid,0,0,0,0,0);
        assert(pc_cancelled(&task)&&!pc_commit_begin(&task)&&pc_end(&task));
        assert(!pm_ordinary(1,&key)&&!pc_start(&task,key,1,&context)&&pm_finish(key));
        process_record_forget(task.tid);current_call_task=NULL;
        unsigned count,pins;bool pending;pm_counts(&count,&pins,&pending);assert(!count&&!pins&&!pending&&!pc_count());
    }
    assert(!pthread_barrier_destroy(&call_entered)&&!pthread_barrier_destroy(&call_cancelled));
    puts("PASS retained call rounds=128 cancellation-before-commit cancellation-during-commit exit-hook nested stale drained");return 0;
}
