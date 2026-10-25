/* Actual normal transport/owner/backend/process registry; host VMM and storage
 * adapters. Scheduler, hardware copies and filesystem behavior are separate gates. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "pkg_service.h"
#include "process_table.h"
#include "pkg_abi.h"
#include "syscall.h"
#include "thread.h"
#include "vmm.h"
#include "vfs.h"
static tcb_t tasks[2];static tcb_t *current;
static creds_t credentials[2];
static bool admitted,reject_read,reject_write,mutating,fail_mutation,bad_snapshot;
static unsigned reads,writes,range_checks;
tcb_t *thread_current(void){return current;}
uint64_t *vmm_get_active_pml4_virt(void){return NULL;}
bool vmm_validate_user_range(uint64_t *p,uintptr_t address,size_t length,bool writing){
    assert(!p);range_checks++;assert(length==(writing?80:64));
    return address&&!(writing?reject_write:reject_read);
}
int64_t syscall_from_vfs_error(int64_t r){
    if(!r)return 0;
    if(r==-VFS_EAGAIN)return SYSCALL_EAGAIN;
    if(r==-VFS_EIO)return SYSCALL_EIO;
    if(r==-VFS_EBADF)return SYSCALL_EBADF;
    if(r==-VFS_EACCES)return SYSCALL_EACCES;
    assert(false);return SYSCALL_EINVAL;
}
static void snapshot(pkg_transaction_status *s){
    *s=(pkg_transaction_status){.phase=bad_snapshot?4:PKG_TX_IDLE,.sequence=61,
        .account_sequence=2,.credential_generation=3};
}
static bool ready(void){return admitted;}
static int status(void *context,pkg_transaction_status *s){
    assert(context==&reads);if(mutating)return -VFS_EAGAIN;reads++;snapshot(s);return 0;
}
static int recover(void *context,pkg_transaction_status *s){assert(context==&reads);snapshot(s);return 0;}
static int mutate(void *context,bool remove,const uint8_t name[32],pkg_transaction_status *s){
    (void)remove;assert(context==&reads&&!memcmp(name,"field-kit",10));writes++;
    mutating=true;pkg_request_v1 q={.size=64,.version=1,.operation=PKG_STATUS};pkg_result_v1 result;
    assert(pkg_service_control((uintptr_t)&q,(uintptr_t)&result,64)==SYSCALL_EAGAIN);
    mutating=false;snapshot(s);return fail_mutation?-VFS_EIO:0;
}
static long call(pkg_request_v1 *q,pkg_result_v1 *out){return pkg_service_control((uintptr_t)q,(uintptr_t)out,64);}
static void birth(unsigned i){
    tasks[i].tid=100+i;tasks[i].is_user=true;creds_init_root(&credentials[i]);
    assert(!process_record_begin(tasks[i].tid,0,false,0,0));
    assert(process_record_bind_creds(tasks[i].tid,&credentials[i]));process_record_commit(tasks[i].tid);
}
int main(void){
    birth(0);birth(1);current=&tasks[0];pkg_result_v1 result,old;
    memset(&result,0xa5,sizeof result);old=result;
    pkg_request_v1 q={.size=64,.version=1,.operation=PKG_STATUS};
    assert(pkg_service_control(0,0,63)==SYSCALL_EINVAL&&!range_checks);
    assert(pkg_service_control(0,(uintptr_t)&result,64)==SYSCALL_EFAULT);
    reject_write=true;assert(call(&q,&result)==SYSCALL_EFAULT);reject_write=false;
    assert(call(&q,&result)==SYSCALL_EOPNOTSUPP&&!tasks[0].pkg_owner.generation&&!reads&&!writes);
    assert(!memcmp(&old,&result,sizeof result));
    static const pkg_kernel_backend b={{&reads,status,recover},ready,mutate};
    assert(!pkg_service_bind(&b));admitted=true;assert(pkg_service_bind(&b));assert(!pkg_service_bind(&b));
    q.reserved=1;assert(call(&q,&result)==SYSCALL_EINVAL&&!reads);q.reserved=0;
    q.package[0]='x';assert(call(&q,&result)==SYSCALL_EINVAL);q.package[0]=0;
    assert(!call(&q,&result)&&result.size==80&&result.version==1&&!result.reserved&&!result.session);
    q.operation=PKG_ACQUIRE;assert(!call(&q,&result)&&result.session);uint64_t session=result.session;
    current=&tasks[1];q.operation=PKG_INSTALL;q.session=session;memcpy(q.package,"field-kit",10);
    assert(call(&q,&result)==SYSCALL_EACCES&&!writes);
    current=&tasks[0];q.package[0]='/';assert(call(&q,&result)==SYSCALL_EINVAL&&!writes);q.package[0]='f';
    q.package[31]='x';assert(call(&q,&result)==SYSCALL_EINVAL&&!writes);q.package[31]=0;
    creds_t saved=credentials[0],dropped=saved;dropped.cap_effective=0;
    assert(process_record_publish_creds(tasks[0].tid,&saved,&dropped));
    assert(call(&q,&result)==SYSCALL_EACCES&&!writes);
    assert(process_record_publish_creds(tasks[0].tid,&dropped,&saved));
    reject_write=true;assert(call(&q,&result)==SYSCALL_EFAULT&&!writes);reject_write=false;
    assert(!call(&q,&result)&&writes==1&&result.session==session&&result.phase==PKG_PHASE_IDLE);
    old=result;fail_mutation=true;assert(call(&q,&result)==SYSCALL_EIO&&writes==2&&!memcmp(&old,&result,sizeof result));fail_mutation=false;
    bad_snapshot=true;assert(call(&q,&result)==SYSCALL_EIO&&writes==3);bad_snapshot=false;
    pkg_owner_exec(current);assert(call(&q,&result)==SYSCALL_EACCES&&writes==3);
    q=(pkg_request_v1){.size=64,.version=1,.operation=PKG_ACQUIRE};assert(!call(&q,&result)&&result.session!=session);
    q.operation=PKG_RECOVER;q.session=result.session;assert(!call(&q,&result)&&result.phase==PKG_PHASE_IDLE);
    q.operation=PKG_RELEASE;assert(!call(&q,&result)&&!result.session);
    for(unsigned i=0;i<2;i++){
        pkg_owner_exit(&tasks[i]);pkg_owner_exit(&tasks[i]);pkg_owner_reap(&tasks[i]);
        (void)process_record_exit(tasks[i].tid,0);process_record_forget(tasks[i].tid);
    }
    puts("PASS normal package transport: disabled admission, ranges, shape, owner isolation, credentials, coherent mutation, no retry, exec/exit/reuse");
    return 0;
}
