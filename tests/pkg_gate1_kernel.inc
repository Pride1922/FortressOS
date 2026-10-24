/* Only copied-kernel build. Wire 200 and test control 201 are private. */
#include "thread.h"
#include "process_table.h"
#include "syscall.h"
#include "vmm.h"
#include "percpu.h"
#include "string.h"
#include "pkg_backend_candidate.h"
const pkg_transaction_backend *gk_backend(void);
bool gk_root_admitted(void);
int64_t gk_storage_test(uint64_t);
static unsigned gk_exits,gk_reaps,gk_fail_copyout;
static uint64_t gk_cpus;
static void gk_check(bool ok){if(!ok)__builtin_trap();}
static pw_key gk_key(tcb_t *t){return (pw_key){.generation=t->pkg_owner.generation,.slot=t->pkg_owner.slot};}
void gk_owner_exit(tcb_t *t){
 spin_debug_assert_unheld();if(!t||!t->pkg_owner.generation)return;
 pw_key key=gk_key(t);gk_check(pw_detach(key));gk_check(!pw_references(key));t->pkg_owner.generation=0;t->pkg_owner.slot=0;
 __atomic_add_fetch(&gk_exits,1,__ATOMIC_RELEASE);
}
void gk_owner_reap(tcb_t *t){
 spin_debug_assert_unheld();gk_check(!t->pkg_owner.generation);
 if(t->pkg_tracked)__atomic_add_fetch(&gk_reaps,1,__ATOMIC_RELEASE);
}
int64_t gk_transport(uintptr_t input,uintptr_t output,uint64_t bytes){
 spin_debug_assert_unheld();tcb_t *t=thread_current();
 if(bytes!=sizeof(pb_request))return SYSCALL_EINVAL;
 uint64_t *pml4=vmm_get_active_pml4_virt();
 if(!t||!t->is_user || !vmm_validate_user_range(pml4,input,sizeof(pb_request),false) ||
    !vmm_validate_user_range(pml4,output,sizeof(pb_result),true))return SYSCALL_EFAULT;
 pb_request q;memcpy(&q,(void *)input,sizeof q);pb_result result;
 if(!t->pkg_owner.generation){
  pw_key key;if(!pw_register(t->tid,&key))return SYSCALL_EAGAIN;
  t->pkg_owner.generation=key.generation;t->pkg_owner.slot=key.slot;t->pkg_tracked=true;
 }
 __atomic_fetch_or(&gk_cpus,1ULL<<cpu_current()->id,__ATOMIC_RELAXED);
 int r=pb_control(&q,gk_key(t),gk_root_admitted(),gk_backend(),&result);
 if(r)return syscall_from_vfs_error(r);
 /* One-shot fault is private test control, after the durable result. Never
  * retry pb_control and never roll back an acquire/recover on copyout failure. */
 if(__atomic_exchange_n(&gk_fail_copyout,0,__ATOMIC_ACQ_REL))return SYSCALL_EFAULT;
 memcpy((void *)output,&result,sizeof result);return 0;
}
int64_t gk_test(uint64_t op){
 spin_debug_assert_unheld();tcb_t *t=thread_current();creds_t actor;
 if(op==0){thread_yield();return 0;}
 if(op==1)return __atomic_load_n(&gk_exits,__ATOMIC_ACQUIRE);
 if(op==2)return __atomic_load_n(&gk_reaps,__ATOMIC_ACQUIRE);
 if(op==3)return (int64_t)__atomic_load_n(&gk_cpus,__ATOMIC_ACQUIRE);
 if(!t||!process_record_creds(t->tid,&actor)||actor.euid||!(actor.cap_effective&CAP_SYS_ADMIN))return SYSCALL_EPERM;
 if(op==4){__atomic_store_n(&gk_fail_copyout,1,__ATOMIC_RELEASE);return 0;}
 return gk_storage_test(op);
}
