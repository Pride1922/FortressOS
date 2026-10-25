#include "pkg_owner.h"
#include "process_table.h"
#include "spinlock.h"
/* Owned value objects, never TCB pointers. No process API under this lock.
 * Birth/exit/exec notifications must come from trusted lifecycle call sites;
 * admission supplies durable-state authority separately from owner lifetime. */
typedef struct {uint64_t pid,generation;unsigned refs;bool live;} pw_owner;
typedef struct {uint64_t generation;pw_key owner;bool active;} pw_operation;
static pw_owner owners[PROCESS_CAPACITY];
static pw_operation operations[PROCESS_CAPACITY];
static spinlock_t guard=SPINLOCK_RANKED(1,"package owner");
static uint64_t owner_serial,session_serial,operation_serial,current;
static pw_key writer;
static unsigned inflight;
static bool cancelled;
static pw_owner *find(pw_key k){
    return k.slot<PROCESS_CAPACITY&&k.generation&&owners[k.slot].refs&&
        owners[k.slot].generation==k.generation?&owners[k.slot]:NULL;
}
static bool same(pw_key a,pw_key b){return a.slot==b.slot&&a.generation==b.generation;}
static void retire(void){
    if(current&&cancelled&&!inflight){pw_owner *o=find(writer);o->refs--;current=0;writer=(pw_key){0};}
}
static bool privileged(pw_key k){
    uint64_t lock=spin_lock_irqsave(&guard);pw_owner *o=find(k);uint64_t pid=o&&o->live?o->pid:0;
    spin_unlock_irqrestore(&guard,lock);creds_t c;
    /* Immutable credential snapshot authorizes an admitted in-flight call;
     * a subsequent drop denies subsequent calls, not retroactive completion. */
    return pid&&process_record_creds(pid,&c)&&!c.euid&&(c.cap_effective&CAP_SYS_ADMIN);
}
bool pw_register(uint64_t pid,pw_key *out){
    process_snapshot_t p;if(!out||!pid||!process_record_snapshot_pid(pid,&p)||p.state!=PROCESS_RUNNING)return false;
    uint64_t lock=spin_lock_irqsave(&guard);bool ok=false;
    if(owner_serial==UINT64_MAX)goto done;
    for(unsigned i=0;i<PROCESS_CAPACITY;i++)if(owners[i].live&&owners[i].pid==pid)goto done;
    for(unsigned i=0;i<PROCESS_CAPACITY;i++)if(!owners[i].refs){
        owners[i]=(pw_owner){.pid=pid,.generation=++owner_serial,.refs=1,.live=true};
        *out=(pw_key){.generation=owner_serial,.slot=i};ok=true;break;
    }
done:spin_unlock_irqrestore(&guard,lock);return ok;
}
bool pw_authorized(pw_key k){return privileged(k);}
bool pw_recovery_step(pw_key k){
    if(!privileged(k))return false;
    uint64_t lock=spin_lock_irqsave(&guard);pw_owner *o=find(k);
    bool ok=o&&o->live&&current&&!cancelled&&inflight&&same(writer,k);
    spin_unlock_irqrestore(&guard,lock);return ok;
}
bool pw_begin(pw_key k,bool idle,uint64_t *out){
    if(!out||!idle||!privileged(k))return false;
    uint64_t lock=spin_lock_irqsave(&guard);pw_owner *o=find(k);bool ok=false;
    if(o&&o->live&&!current&&session_serial!=UINT64_MAX){
        writer=k;current=++session_serial;cancelled=false;o->refs++;*out=current;ok=true;
    }
    spin_unlock_irqrestore(&guard,lock);return ok;
}
bool pw_enter(pw_key k,uint64_t session,pw_key *out){
    if(!out||!privileged(k))return false;
    uint64_t lock=spin_lock_irqsave(&guard);pw_owner *o=find(k);bool ok=false;
    if(!o||!o->live||!current||current!=session||cancelled||!same(writer,k)||operation_serial==UINT64_MAX)goto done;
    for(unsigned i=0;i<PROCESS_CAPACITY;i++)if(!operations[i].active){
        operations[i]=(pw_operation){.generation=++operation_serial,.owner=k,.active=true};
        inflight++;o->refs++;*out=(pw_key){.generation=operation_serial,.slot=i};ok=true;break;
    }
done:spin_unlock_irqrestore(&guard,lock);return ok;
}
bool pw_complete(pw_key k){
    uint64_t lock=spin_lock_irqsave(&guard);bool ok=false;
    if(k.slot<PROCESS_CAPACITY){pw_operation *op=&operations[k.slot];
        if(op->active&&op->generation==k.generation){
            pw_owner *o=find(op->owner);o->refs--;op->active=false;inflight--;retire();ok=true;
        }
    }
    spin_unlock_irqrestore(&guard,lock);return ok;
}
bool pw_close(pw_key k,uint64_t session){
    uint64_t lock=spin_lock_irqsave(&guard);bool ok=current&&session==current&&same(writer,k);
    if(ok){cancelled=true;retire();}spin_unlock_irqrestore(&guard,lock);return ok;
}
bool pw_detach(pw_key k){
    uint64_t lock=spin_lock_irqsave(&guard);pw_owner *o=find(k);bool ok=o&&o->live;
    if(ok){o->live=false;o->refs--;if(current&&same(writer,k)){cancelled=true;retire();}}
    spin_unlock_irqrestore(&guard,lock);return ok;
}
bool pw_exec(pw_key k){
    uint64_t lock=spin_lock_irqsave(&guard);pw_owner *o=find(k);bool ok=o&&o->live;
    if(ok&&current&&same(writer,k)){cancelled=true;retire();}
    spin_unlock_irqrestore(&guard,lock);return ok;
}
unsigned pw_references(pw_key k){
    uint64_t lock=spin_lock_irqsave(&guard);pw_owner *o=find(k);unsigned refs=o?o->refs:0;
    spin_unlock_irqrestore(&guard,lock);return refs;
}
