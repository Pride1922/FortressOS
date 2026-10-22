#include "pkg_mutation_candidate.h"
#include "spinlock.h"
#include "vfs.h"
typedef struct {pm_view view;pw_key retained;uint64_t generation;bool live,pinned;} pm_slot;
static pm_slot pm_slots[PM_CAPACITY];
static uint64_t pm_configured,pm_serial;
static unsigned pm_admitted,pm_pins,pm_account_slot=PM_CAPACITY;
static spinlock_t pm_guard=SPINLOCK_RANKED(1,"private mutation admission");
static pm_slot *pm_find(pm_key key){
    return key.slot<PM_CAPACITY&&pm_slots[key.slot].live&&pm_slots[key.slot].generation==key.generation?&pm_slots[key.slot]:NULL;
}
bool pm_configure(uint64_t mount){
    spin_debug_assert_unheld();uint64_t irq=spin_lock_irqsave(&pm_guard);
    bool ok=mount&&!pm_admitted;if(ok)pm_configured=mount;
    spin_unlock_irqrestore(&pm_guard,irq);return ok;
}
static int pm_request(uint64_t mount,enum pm_kind kind,pw_key retained,pm_key *out){
    uint64_t irq=spin_lock_irqsave(&pm_guard);int r=-VFS_EAGAIN;
    if(!out||!mount||mount!=pm_configured){r=-VFS_EINVAL;goto done;}
    if(pm_serial==UINT64_MAX||pm_account_slot!=PM_CAPACITY)goto done;
    /* One slot is always reserved for account priority, even if all ordinary
     * admitted operations are waiting to pin. A pending account blocks new pins
     * without revoking already pinned operations or waiting under a spinlock. */
    if(kind!=PM_ACCOUNT&&pm_admitted>=PM_CAPACITY-1)goto done;
    for(unsigned i=0;i<PM_CAPACITY;i++)if(!pm_slots[i].live){
        pm_slots[i]=(pm_slot){.view={mount,kind},.retained=retained,.generation=++pm_serial,.live=true};
        *out=(pm_key){.generation=pm_serial,.slot=i};pm_admitted++;
        if(kind==PM_ACCOUNT)pm_account_slot=i;
        r=0;break;
    }
done:spin_unlock_irqrestore(&pm_guard,irq);return r;
}
int pm_ordinary(uint64_t mount,pm_key *out){
    spin_debug_assert_unheld();return pm_request(mount,PM_ORDINARY,(pw_key){0},out);
}
int pm_account(uint64_t mount,pm_key *out){
    spin_debug_assert_unheld();return pm_request(mount,PM_ACCOUNT,(pw_key){0},out);
}
int pm_package(uint64_t mount,pw_key owner,uint64_t session,pm_key *out){
    spin_debug_assert_unheld();pw_key retained;
    if(!pw_enter(owner,session,&retained))return -VFS_EACCES;
    int r=pm_request(mount,PM_PACKAGE,retained,out);
    if(r&&!pw_complete(retained))return -VFS_EIO;
    return r;
}
bool pm_describe(pm_key key,uint64_t mount,pm_view *out){
    spin_debug_assert_unheld();uint64_t irq=spin_lock_irqsave(&pm_guard);pm_slot *s=pm_find(key);
    bool ok=out&&s&&!s->pinned&&s->view.mount==mount;if(ok)*out=s->view;
    spin_unlock_irqrestore(&pm_guard,irq);return ok;
}
const pm_view *pm_pin(pm_key key,uint64_t mount){
    spin_debug_assert_unheld();uint64_t irq=spin_lock_irqsave(&pm_guard);pm_slot *s=pm_find(key);const pm_view *view=NULL;
    if(s&&s->view.mount==mount&&!s->pinned&&
       (s->view.kind==PM_ACCOUNT?!pm_pins:pm_account_slot==PM_CAPACITY)){
        s->pinned=true;pm_pins++;view=&s->view;
    }
    spin_unlock_irqrestore(&pm_guard,irq);return view;
}
bool pm_unpin(pm_key key){
    spin_debug_assert_unheld();uint64_t irq=spin_lock_irqsave(&pm_guard);pm_slot *s=pm_find(key);bool ok=s&&s->pinned;
    if(ok){s->pinned=false;pm_pins--;}spin_unlock_irqrestore(&pm_guard,irq);return ok;
}
bool pm_finish(pm_key key){
    spin_debug_assert_unheld();pw_key retained={0};uint64_t irq=spin_lock_irqsave(&pm_guard);
    pm_slot *s=pm_find(key);bool ok=s&&!s->pinned;
    if(ok){retained=s->retained;*s=(pm_slot){0};pm_admitted--;if(pm_account_slot==key.slot)pm_account_slot=PM_CAPACITY;}
    spin_unlock_irqrestore(&pm_guard,irq);
    if(retained.generation&&!pw_complete(retained))return false;
    return ok;
}
void pm_counts(unsigned *count,unsigned *active,bool *pending){
    spin_debug_assert_unheld();uint64_t irq=spin_lock_irqsave(&pm_guard);
    *count=pm_admitted;*active=pm_pins;*pending=pm_account_slot!=PM_CAPACITY;
    spin_unlock_irqrestore(&pm_guard,irq);
}
