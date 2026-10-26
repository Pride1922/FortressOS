/* Actual sched_reap_dead() is extracted by the runner. These adapters test
 * final-reference lifetime and lock separation, not real CR3/TLB scheduling. */
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { pthread_mutex_t mutex; } spinlock_t;
#define SPINLOCK_RANKED(rank,name) { PTHREAD_MUTEX_INITIALIZER }
static _Thread_local unsigned held;
static uint64_t spin_lock_irqsave(spinlock_t *l) {
    assert(!held++);assert(!pthread_mutex_lock(&l->mutex));return 0;
}
static void spin_unlock_irqrestore(spinlock_t *l,uint64_t flags) {
    (void)flags;assert(held--==1);assert(!pthread_mutex_unlock(&l->mutex));
}
struct tcb;
typedef struct {
    struct tcb *leader;
    unsigned ref_count;
    uint64_t pid,exit_code,exit_signal,final_ticks,final_utime,final_stime,final_preempts;
} process_t;
typedef struct tcb {
    struct tcb *next;
    process_t *process;
    bool is_user;
    int stack_slot;
    uintptr_t kstack_base,cr3;
    unsigned handoff;
    uint64_t tid;
} tcb_t;
static struct { spinlock_t sched_lock; tcb_t *dead_threads; } scheduler_cpus[4];
static size_t g_total_sched_cpus=4;
static _Thread_local tcb_t actor;
#define g_current_thread (&actor)
static spinlock_t g_sched_lock=SPINLOCK_RANKED(1,"fallback");
#define MAX_EXIT_RECORDS 8
static struct { uint64_t pid,exit_code,total_ticks,preempt_count;bool valid; } g_exit_records[8];
static unsigned g_exit_records_head;
static const char child_wait_channel;
static unsigned stacks,spaces,records,forgotten,fds,freed;
static void handoff_abandon(unsigned *p) {(void)p;assert(!held);}
static void pkg_owner_reap(tcb_t *t) {(void)t;assert(!held);}
static void serial_puts(const char *s) {(void)s;abort();}
static void kstack_free(int slot,uintptr_t base) {
    assert(!held && slot>=0 && base);__atomic_add_fetch(&stacks,1,__ATOMIC_RELAXED);
}
static uintptr_t vmm_get_current_pml4(void) {return 0;}
static void vmm_destroy_pml4(uintptr_t root,bool recursive) {
    assert(!held && root && recursive);__atomic_add_fetch(&spaces,1,__ATOMIC_RELAXED);
}
static void fd_close_all(tcb_t *t) {
    assert(!held && t->process->ref_count==0);
    /* The leader's signal/credential metadata must still be alive. */
    assert(t->process->leader->tid==t->process->pid);
    __atomic_add_fetch(&fds,1,__ATOMIC_RELAXED);
}
static void cancel_staged_children(uint64_t pid) {assert(!held && pid);}
static bool process_record_exit_accounted_times(uint64_t pid,uint64_t code,
    unsigned signal,uint64_t ticks,uint64_t user,uint64_t system) {
    assert(!held && pid && code==139 && signal==11 && ticks==12 && user==13 && system==14);
    __atomic_add_fetch(&records,1,__ATOMIC_RELAXED);return true;
}
static void process_record_forget(uint64_t pid) {
    assert(!held && pid);__atomic_add_fetch(&forgotten,1,__ATOMIC_RELAXED);
}
static void sched_wake_all(const void *p) {assert(!held && p==&child_wait_channel);}
static void kfree(void *p) {assert(!held);__atomic_add_fetch(&freed,1,__ATOMIC_RELAXED);free(p);}
static void vmm_drain_deferred_destructions(void) {assert(!held);}
#include "reap-functions.inc"
static void *reap(void *unused) {(void)unused;actor.stack_slot=-1;sched_reap_dead();return NULL;}
int main(void) {
    for(unsigned i=0;i<4;i++)assert(!pthread_mutex_init(&scheduler_cpus[i].sched_lock.mutex,NULL));
    actor.stack_slot=-1;
    for(unsigned cycle=0;cycle<1000;cycle++) {
        uintptr_t root=0x1000ULL*(cycle+1);
        process_t *p=calloc(1,sizeof(*p));assert(p);
        tcb_t *t[4];
        for(unsigned i=0;i<4;i++) {
            t[i]=calloc(1,sizeof(*t[i]));assert(t[i]);
            *t[i]=(tcb_t){.process=p,.is_user=true,.stack_slot=(int)i,.kstack_base=i+1,.cr3=root,.tid=cycle*4+i+1};
        }
        *p=(process_t){.leader=t[0],.ref_count=4,.pid=t[0]->tid,.exit_code=139,.exit_signal=11,.final_ticks=12,.final_utime=13,.final_stime=14};
        for(unsigned i=0;i<4;i++)g_thread_join_table[i]=(thread_join_record_t){.tid=t[i]->tid,.owner_cr3=root,.exited=true};
        g_thread_join_table[4]=(thread_join_record_t){.tid=999999,.owner_cr3=root+0x10000000,.exited=true};
        /* Alternate leader-first, leader-last, and simultaneous reaping. */
        unsigned first=cycle%3==0?0:3;
        if(cycle%3!=2) {
            scheduler_cpus[first].dead_threads=t[first];sched_reap_dead();
            assert(records==cycle && p->ref_count==3);
            for(unsigned i=0;i<4;i++)if(i!=first)scheduler_cpus[i].dead_threads=t[i];
        } else for(unsigned i=0;i<4;i++)scheduler_cpus[i].dead_threads=t[i];
        pthread_t workers[4];
        for(unsigned i=0;i<4;i++)assert(!pthread_create(&workers[i],NULL,reap,NULL));
        for(unsigned i=0;i<4;i++)assert(!pthread_join(workers[i],NULL));
        assert(stacks==(cycle+1)*4 && spaces==cycle+1 && records==cycle+1);
        assert(fds==cycle+1 && forgotten==cycle+1 && freed==(cycle+1)*5);
        for(unsigned i=0;i<4;i++)assert(!g_thread_join_table[i].tid);
        assert(g_thread_join_table[4].tid==999999);
    }
    puts("PASS actual final reaper: 1000 concurrent groups, leader lifetime, one process status, unjoined records, lock separation");
}
