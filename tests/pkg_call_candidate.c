#include "pkg_call_candidate.h"
#include "thread.h"
#include "process_table.h"
#include "spinlock.h"
/* Included with the admission candidate so host lock-depth assertions also
 * cover filesystem -> admission/binding nesting. No production linkage. */
struct pc_call {
    uint64_t pid,generation;
    pm_key key;
    pm_view view;
    void *context;
    unsigned state; /* atomic: CANCEL=1, COMMIT=2; no spinlock at commit boundary */
    bool live,pinned;
};
static struct pc_call pc_calls[PM_CAPACITY];
static uint64_t pc_serial;
static spinlock_t pc_guard=SPINLOCK_RANKED(1,"private retained call");
static bool pc_alive(uint64_t pid){
    process_snapshot_t p;
    return process_record_snapshot_pid(pid,&p)&&p.state==PROCESS_RUNNING;
}
bool pc_start(tcb_t *self,pm_key key,uint64_t mount,void *context){
    spin_debug_assert_unheld();
    if(!self||self!=thread_current()||self->package_call||!context||!pc_alive(self->tid))return false;
    pm_view view;if(!pm_describe(key,mount,&view))return false;
    uint64_t irq=spin_lock_irqsave(&pc_guard);struct pc_call *call=NULL;
    bool duplicate=false;
    for(unsigned i=0;i<PM_CAPACITY;i++)if(pc_calls[i].live&&
        (pc_calls[i].pid==self->tid||(pc_calls[i].key.slot==key.slot&&pc_calls[i].key.generation==key.generation)))duplicate=true;
    if(!duplicate&&pc_serial!=UINT64_MAX)for(unsigned i=0;i<PM_CAPACITY;i++)if(!pc_calls[i].live){
        call=&pc_calls[i];
        *call=(struct pc_call){.pid=self->tid,.generation=++pc_serial,.key=key,.view=view,.context=context,.live=true};break;
    }
    spin_unlock_irqrestore(&pc_guard,irq);
    if(!call)return false;
    self->package_call=call;
    /* Close the snapshot/registration race: exit either finds the published
     * record or this check observes death. Neither path frees an owned binding. */
    if(!pc_alive(self->tid))pc_cancel_pid(self->tid);
    return true;
}
static struct pc_call *pc_self(tcb_t *self){
    return self&&self==thread_current()&&self->package_call&&self->package_call->pid==self->tid?self->package_call:NULL;
}
bool pc_pin(tcb_t *self){
    spin_debug_assert_unheld();struct pc_call *c=pc_self(self);
    if(!c||c->pinned||pc_cancelled(self))return false;
    if(!pm_pin(c->key,c->view.mount))return false;
    c->pinned=true;return !pc_cancelled(self);
}
const pm_view *pc_view(tcb_t *self){struct pc_call *c=pc_self(self);return c?&c->view:NULL;}
void *pc_context(tcb_t *self){struct pc_call *c=pc_self(self);return c?c->context:NULL;}
bool pc_cancelled(tcb_t *self){struct pc_call *c=pc_self(self);return !c||(__atomic_load_n(&c->state,__ATOMIC_ACQUIRE)&1u);}
bool pc_commit_begin(tcb_t *self){
    struct pc_call *c=pc_self(self);unsigned expected=0;
    return c&&c->pinned&&__atomic_compare_exchange_n(&c->state,&expected,2u,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
}
void pc_commit_end(tcb_t *self){
    struct pc_call *c=pc_self(self);assert(c);
    unsigned old=__atomic_fetch_and(&c->state,~2u,__ATOMIC_ACQ_REL);assert(old&2u);
}
bool pc_cancel_current(tcb_t *self){
    struct pc_call *c=pc_self(self);if(!c)return false;
    __atomic_fetch_or(&c->state,1u,__ATOMIC_ACQ_REL);return true;
}
void pc_cancel_pid(uint64_t pid){
    spin_debug_assert_unheld();uint64_t irq=spin_lock_irqsave(&pc_guard);
    for(unsigned i=0;i<PM_CAPACITY;i++)if(pc_calls[i].live&&pc_calls[i].pid==pid)
        __atomic_fetch_or(&pc_calls[i].state,1u,__ATOMIC_ACQ_REL);
    spin_unlock_irqrestore(&pc_guard,irq);
}
bool pc_end(tcb_t *self){
    spin_debug_assert_unheld();struct pc_call *c=pc_self(self);
    if(!c||(__atomic_load_n(&c->state,__ATOMIC_ACQUIRE)&2u))return false;
    pm_key key=c->key;bool pinned=c->pinned;self->package_call=NULL;
    uint64_t irq=spin_lock_irqsave(&pc_guard);c->live=false;
    spin_unlock_irqrestore(&pc_guard,irq);
    bool unpinned=!pinned||pm_unpin(key),finished=unpinned&&pm_finish(key);return finished;
}
unsigned pc_count(void){
    spin_debug_assert_unheld();uint64_t irq=spin_lock_irqsave(&pc_guard);unsigned n=0;
    for(unsigned i=0;i<PM_CAPACITY;i++)n+=pc_calls[i].live;
    spin_unlock_irqrestore(&pc_guard,irq);return n;
}
