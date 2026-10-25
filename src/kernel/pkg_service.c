#include "pkg_service.h"
#include "pkg_abi.h"
#include "thread.h"
#include "syscall.h"
#include "vmm.h"
#include "string.h"
#include "spinlock.h"
#include "vfs.h"
static const pkg_kernel_backend *binding;
static bool zero(const uint8_t *p,size_t n){for(size_t i=0;i<n;i++)if(p[i])return false;return true;}
static bool name_valid(const uint8_t p[32]) {
    size_t n=0;while(n<32&&p[n])n++;
    if(!n||!zero(p+n,32-n))return false;
    for(size_t i=0;i<n;i++) {
        if((p[i]>='a'&&p[i]<='z')||(p[i]>='0'&&p[i]<='9'))continue;
        if(p[i]!='-'||!i||i+1==n||p[i-1]=='-')return false;
    }
    return true;
}
bool pkg_service_bind(const pkg_kernel_backend *b) {
    spin_debug_assert_unheld();
    if(!b||!b->admitted||!b->admitted()||!b->transactions.context||
       !b->transactions.status||!b->transactions.recover||!b->mutate)return false;
    const pkg_kernel_backend *expected=NULL;
    return __atomic_compare_exchange_n(&binding,&expected,b,false,__ATOMIC_RELEASE,__ATOMIC_RELAXED);
}
void pkg_owner_exit(tcb_t *t) {
    if(!t||!t->pkg_owner.generation)return;
    spin_debug_assert_unheld();
    pw_key key=t->pkg_owner;
    if(!pw_detach(key)||pw_references(key))__builtin_trap();
    t->pkg_owner=(pw_key){0};
}
void pkg_owner_exec(tcb_t *t) {
    if(!t||!t->pkg_owner.generation)return;
    spin_debug_assert_unheld();
    if(!pw_exec(t->pkg_owner))__builtin_trap();
}
void pkg_owner_reap(tcb_t *t) {
    if(!t||!t->pkg_owner.generation)return;
    spin_debug_assert_unheld();__builtin_trap();
}
int64_t pkg_service_control(uintptr_t input,uintptr_t output,uint64_t bytes) {
    spin_debug_assert_unheld();
    if(bytes!=sizeof(pkg_request_v1))return SYSCALL_EINVAL;
    tcb_t *t=thread_current();uint64_t *pml4=vmm_get_active_pml4_virt();
    if(!t||!t->is_user||!vmm_validate_user_range(pml4,input,sizeof(pkg_request_v1),false)||
       !vmm_validate_user_range(pml4,output,sizeof(pkg_result_v1),true))return SYSCALL_EFAULT;
    pkg_request_v1 q;memcpy(&q,(void *)input,sizeof q);
    bool mutation=q.operation==PKG_INSTALL||q.operation==PKG_REMOVE;
    if(q.size!=sizeof q||q.version!=1||q.reserved||q.reserved64||
       q.operation<PKG_STATUS||q.operation>PKG_REMOVE||
       ((q.operation==PKG_STATUS||q.operation==PKG_ACQUIRE)?q.session!=0:q.session==0)||
       (mutation?!name_valid(q.package):!zero(q.package,sizeof q.package)))return SYSCALL_EINVAL;
    const pkg_kernel_backend *b=__atomic_load_n(&binding,__ATOMIC_ACQUIRE);
    if(!b||!b->admitted())return SYSCALL_EOPNOTSUPP;
    if(!t->pkg_owner.generation&&!pw_register(t->tid,&t->pkg_owner))return SYSCALL_EAGAIN;
    pb_result captured={0};int r;
    if(!mutation) {
        pb_request request={.size=sizeof request,.version=1,.operation=q.operation,.session=q.session};
        r=pb_control(&request,t->pkg_owner,true,&b->transactions,&captured);
    } else {
        pw_key operation;
        if(!pw_enter(t->pkg_owner,q.session,&operation))return SYSCALL_EACCES;
        pkg_transaction_status s={0};
        r=b->mutate(b->transactions.context,q.operation==PKG_REMOVE,q.package,&s);
        if(!r&&(!pkg_backend_snapshot_valid(&s)||s.phase!=PKG_TX_IDLE))r=-VFS_EIO;
        if(!pw_complete(operation))r=-VFS_EIO;
        if(!r) {
            captured=(pb_result){.size=sizeof captured,.version=1,.phase=s.phase,.session=q.session,
                .sequence=s.sequence,.account_sequence=s.account_sequence,.credential_generation=s.credential_generation};
            for(unsigned i=0;i<2;i++){captured.remaining[i]=s.remaining[i];captured.charged[i]=s.charged[i];}
        }
    }
    if(r)return syscall_from_vfs_error(r);
    pkg_result_v1 result={.size=sizeof result,.version=1,.phase=captured.phase,.session=captured.session,
        .sequence=captured.sequence,.account_sequence=captured.account_sequence,.credential_generation=captured.credential_generation};
    for(unsigned i=0;i<2;i++){result.remaining[i]=captured.remaining[i];result.charged[i]=captured.charged[i];}
    memcpy((void *)output,&result,sizeof result);return 0;
}
