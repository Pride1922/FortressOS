/* Actual staged-list functions extracted by the runner. IRQ/queue/cleanup
 * adapters establish concurrency/lifetime, not real scheduling or CR3 proof. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "thread.h"
#include "syscall_abi.h"
static _Thread_local tcb_t actor;
static _Thread_local unsigned held;
static struct { spinlock_t sched_lock; } scheduler_cpus[8];
static size_t g_total_sched_cpus=8;
static unsigned published, destroyed;
static tcb_t *staged_processes;
static spinlock_t staged_lock=SPINLOCK_RANKED(1,"staged");
static struct { size_t id; } cpu;
#define cpu_current() (&cpu)
tcb_t *thread_current(void) { return &actor; }
uint64_t spin_lock_irqsave(spinlock_t *l) {
    assert(!held++); assert(!pthread_mutex_lock(&l->mutex)); return 0;
}
void spin_unlock_irqrestore(spinlock_t *l,uint64_t f) {
    (void)f; assert(held--==1); assert(!pthread_mutex_unlock(&l->mutex));
}
static void destroy_staged(tcb_t *p) { assert(!held); __atomic_add_fetch(&destroyed,1,__ATOMIC_RELAXED); free(p); }
void process_record_commit(uint64_t pid) { (void)pid; assert(!held); }
static void runqueue_push_cpu_locked(size_t c,tcb_t *p) {
    assert(held && c==p->current_cpu);
    assert(p->cpu_affinity==-1 || (size_t)p->cpu_affinity==c);
    assert(p->cpus_allowed==(p->cpu_affinity<0 ? CPU_MASK_ALL : CPU_MASK_ONE(c)));
    __atomic_add_fetch(&published,1,__ATOMIC_RELAXED); free(p);
}
void smp_send_work_hint(size_t c) { assert(!held && c<8); }
#include "staged-functions.inc"
static void add(unsigned parent,unsigned group,int affinity) {
    tcb_t *p=calloc(1,sizeof(*p)); assert(p);
    p->parent_pid=parent; p->pgid=group;
    p->cpu_affinity=affinity; p->current_cpu=affinity<0 ? parent%8 : (size_t)affinity;
    p->cpus_allowed=affinity<0 ? CPU_MASK_ALL : CPU_MASK_ONE(affinity);
    uint64_t f=spin_lock_irqsave(&staged_lock);
    p->next=staged_processes; staged_processes=p;
    spin_unlock_irqrestore(&staged_lock,f);
}
static void *producer(void *arg) {
    unsigned id=(unsigned)(uintptr_t)arg; actor.tid=id;
    for (unsigned n=0;n<1000;n++) {
        for (unsigned i=0;i<4;i++) add(id,id,i%2 ? -1 : (int)(i+id)%8);
        if (n%3==0) cancel_staged_children(id);
        else assert(!process_group_release(id,n%3==1 ? GROUP_RELEASE : GROUP_CANCEL));
    }
    return NULL;
}
int main(void) {
    for (unsigned c=0;c<8;c++) scheduler_cpus[c].sched_lock=(spinlock_t)SPINLOCK_RANKED(1,"queue");
    actor.tid=1; add(1,77,-1); add(2,77,3);
    assert(process_group_release(77,GROUP_RELEASE)==SYSCALL_EPERM && !published);
    cancel_staged_children(1); cancel_staged_children(2);
    assert(process_group_release(77,GROUP_RELEASE)==SYSCALL_ESRCH);
    pthread_t threads[8];
    for (unsigned i=0;i<8;i++) assert(!pthread_create(&threads[i],NULL,producer,(void *)(uintptr_t)(i+1)));
    for (unsigned i=0;i<8;i++) assert(!pthread_join(threads[i],NULL));
    assert(!staged_processes && published+destroyed==32002);
    printf("PASS actual staged functions: 8 concurrent parents, 32002 exact releases/cancels, %u queued, %u destroyed; ownership rejection and explicit queue/mask preserved\n",published,destroyed);
}
