/* Actual normal transport/owner/backend/process registry; host VMM and storage
 * adapters. Scheduler, hardware copies and filesystem behavior are separate gates. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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
static bool fail_allocation;
static unsigned allocations,source_calls;
static uint8_t source_catalog[72],source_artifact[128];
static vfs_node_t source_nodes[2];
static file_t source_files[2];
static bool short_source,growing_source;
file_t *fd_get(tcb_t *t,int fd){assert(t==current);return fd>=10&&fd<=11?&source_files[fd-10]:NULL;}
int vfs_close(file_t *f){assert(f->ref_count>1);f->ref_count--;return 0;}
int vfs_metadata(vfs_node_t *n,vfs_metadata_t *m){
    *m=(vfs_metadata_t){.type=VFS_FILE,.size=n==source_nodes?sizeof source_catalog:sizeof source_artifact};return 0;
}
int64_t vfs_read(file_t *f,void *out,size_t count){
    unsigned i=f->node==source_nodes?0:1;size_t n=i?sizeof source_artifact:sizeof source_catalog;
    assert(f!=&source_files[i]);
    if(short_source)return 0;
    if(f->offset==n){if(growing_source){*(uint8_t *)out=1;return 1;}return 0;}
    n-=f->offset;if(n>count)n=count;if(n>17)n=17;
    memcpy(out,(i?source_artifact:source_catalog)+f->offset,n);f->offset+=n;return (int64_t)n;
}
void *kmalloc_large(size_t n){if(fail_allocation)return NULL;allocations++;return malloc(n);}
void kfree(void *p){if(p){assert(allocations);allocations--;free(p);}}
tcb_t *thread_current(void){return current;}
uint64_t *vmm_get_active_pml4_virt(void){return NULL;}
bool vmm_validate_user_range(uint64_t *p,uintptr_t address,size_t length,bool writing){
    assert(!p);range_checks++;assert(writing?length==80:(length==64||length==96||length==112));
    return address&&!(writing?reject_write:reject_read);
}
int64_t syscall_from_vfs_error(int64_t r){
    if(!r)return 0;
    if(r==-VFS_EAGAIN)return SYSCALL_EAGAIN;
    if(r==-VFS_EIO)return SYSCALL_EIO;
    if(r==-VFS_EBADF)return SYSCALL_EBADF;
    if(r==-VFS_EACCES)return SYSCALL_EACCES;
    if(r==-VFS_ENOMEM)return SYSCALL_ENOMEM;
    if(r==-VFS_EINVAL)return SYSCALL_EINVAL;
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
static int source_install(void *ctx,const uint8_t name[32],const uint8_t *catalog,size_t cn,
                          const uint8_t *artifact,size_t an,pkg_transaction_status *out){
    assert(ctx==&reads&&!memcmp(name,"field-kit",10));
    assert(cn==sizeof source_catalog&&an==sizeof source_artifact);
    assert(catalog!=source_catalog&&artifact!=source_artifact&&allocations==1);
    assert(!memcmp(catalog,source_catalog,cn)&&!memcmp(artifact,source_artifact,an));
    /* Mutating original userspace backing cannot change the admitted copy. */
    source_catalog[0]^=1;source_artifact[0]^=1;
    assert(catalog[0]!=source_catalog[0]&&artifact[0]!=source_artifact[0]);
    source_catalog[0]^=1;source_artifact[0]^=1;
    pkg_request_v1 query={.size=64,.version=1,.operation=PKG_STATUS};pkg_result_v1 status_result;
    assert(pkg_service_control((uintptr_t)&query,(uintptr_t)&status_result,64)==SYSCALL_EAGAIN);
    source_calls++;snapshot(out);return fail_mutation?-VFS_EIO:0;
}
static int source_upgrade(void *ctx,const uint8_t name[32],const uint8_t *catalog,size_t cn,
    const uint8_t *artifact,size_t an,const uint8_t *previous,size_t pn,pkg_transaction_status *out) {
    assert(pn==sizeof source_artifact&&previous!=source_artifact&&previous!=artifact);
    assert(!memcmp(previous,source_artifact,pn));
    return source_install(ctx,name,catalog,cn,artifact,an,out);
}
static void birth(unsigned i){
    tasks[i].tid=100+i;tasks[i].is_user=true;creds_init_root(&credentials[i]);
    assert(!process_record_begin(tasks[i].tid,0,false,0,0));
    assert(process_record_bind_creds(tasks[i].tid,&credentials[i]));process_record_commit(tasks[i].tid);
}
int main(void){
    for(unsigned i=0;i<2;i++)source_files[i]=(file_t){.node=&source_nodes[i],.ref_count=1,.offset=57};
    birth(0);birth(1);current=&tasks[0];pkg_result_v1 result,old;
    memset(&result,0xa5,sizeof result);old=result;
    pkg_request_v1 q={.size=64,.version=1,.operation=PKG_STATUS};
    assert(pkg_service_control(0,0,63)==SYSCALL_EINVAL&&!range_checks);
    assert(pkg_service_control(0,(uintptr_t)&result,64)==SYSCALL_EFAULT);
    reject_write=true;assert(call(&q,&result)==SYSCALL_EFAULT);reject_write=false;
    assert(call(&q,&result)==SYSCALL_EOPNOTSUPP&&!tasks[0].pkg_owner.generation&&!reads&&!writes);
    assert(!memcmp(&old,&result,sizeof result));
    static const pkg_kernel_backend b={.transactions={&reads,status,recover},.admitted=ready,
        .mutate=mutate,.install_source=source_install,.upgrade_source=source_upgrade};
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
    pkg_request_v2 v2={.request={.size=96,.version=2,.operation=PKG_INSTALL_SOURCE,.session=session},
        .catalog_fd=10,.catalog_bytes=sizeof source_catalog,
        .artifact_fd=11,.artifact_bytes=sizeof source_artifact};
    memcpy(v2.request.package,"field-kit",10);old=result;
    v2.request.version=1;assert(pkg_service_control((uintptr_t)&v2,(uintptr_t)&result,96)==SYSCALL_EINVAL);
    v2.request.version=2;v2.catalog_bytes=0;
    assert(pkg_service_control((uintptr_t)&v2,(uintptr_t)&result,96)==SYSCALL_EINVAL);
    v2.catalog_bytes=PKG_SOURCE_CATALOG_MAX+1;
    assert(pkg_service_control((uintptr_t)&v2,(uintptr_t)&result,96)==SYSCALL_EINVAL);
    v2.catalog_bytes=sizeof source_catalog;v2.artifact_bytes=PKG_SOURCE_ARTIFACT_MAX+1;
    assert(pkg_service_control((uintptr_t)&v2,(uintptr_t)&result,96)==SYSCALL_EINVAL);
    v2.artifact_bytes=sizeof source_artifact;v2.artifact_fd=MAX_PROCESS_FDS;
    assert(pkg_service_control((uintptr_t)&v2,(uintptr_t)&result,96)==SYSCALL_EBADF);
    v2.artifact_fd=9;
    assert(pkg_service_control((uintptr_t)&v2,(uintptr_t)&result,96)==SYSCALL_EBADF&&!allocations);
    v2.artifact_fd=10;
    assert(pkg_service_control((uintptr_t)&v2,(uintptr_t)&result,96)==SYSCALL_EINVAL&&!allocations);
    assert(source_files[0].ref_count==1);
    v2.artifact_fd=11;source_nodes[0].is_stream=true;
    assert(pkg_service_control((uintptr_t)&v2,(uintptr_t)&result,96)==SYSCALL_EINVAL&&!allocations);
    source_nodes[0].is_stream=false;current=&tasks[1];
    assert(pkg_service_control((uintptr_t)&v2,(uintptr_t)&result,96)==SYSCALL_EACCES);
    current=&tasks[0];fail_allocation=true;
    assert(pkg_service_control((uintptr_t)&v2,(uintptr_t)&result,96)==SYSCALL_ENOMEM);
    assert(!source_calls&&!allocations&&!memcmp(&old,&result,sizeof result));fail_allocation=false;
    short_source=true;assert(pkg_service_control((uintptr_t)&v2,(uintptr_t)&result,96)==SYSCALL_EIO&&!source_calls&&!allocations);short_source=false;
    growing_source=true;assert(pkg_service_control((uintptr_t)&v2,(uintptr_t)&result,96)==SYSCALL_EIO&&!source_calls&&!allocations);growing_source=false;
    assert(!pkg_service_control((uintptr_t)&v2,(uintptr_t)&result,96)&&source_calls==1&&!allocations);
    old=result;fail_mutation=true;
    assert(pkg_service_control((uintptr_t)&v2,(uintptr_t)&result,96)==SYSCALL_EIO&&source_calls==2&&!allocations);
    assert(!memcmp(&old,&result,sizeof result));fail_mutation=false;bad_snapshot=true;
    assert(pkg_service_control((uintptr_t)&v2,(uintptr_t)&result,96)==SYSCALL_EIO&&source_calls==3&&!allocations);
    bad_snapshot=false;
    pkg_request_v3 v3={.source=v2,.previous_fd=11,.previous_bytes=sizeof source_artifact};
    v3.source.request.size=sizeof v3;v3.source.request.version=3;v3.source.request.operation=PKG_UPGRADE_SOURCE;
    assert(!pkg_service_control((uintptr_t)&v3,(uintptr_t)&result,sizeof v3)&&source_calls==4&&!allocations);
    v3.previous_fd=MAX_PROCESS_FDS;assert(pkg_service_control((uintptr_t)&v3,(uintptr_t)&result,sizeof v3)==SYSCALL_EBADF);
    v3.previous_fd=9;assert(pkg_service_control((uintptr_t)&v3,(uintptr_t)&result,sizeof v3)==SYSCALL_EBADF&&!allocations);
    v3.previous_fd=11;v3.previous_bytes=127;assert(pkg_service_control((uintptr_t)&v3,(uintptr_t)&result,sizeof v3)==SYSCALL_EINVAL);
    v3.previous_bytes=sizeof source_artifact;v3.source.request.reserved=1;
    assert(pkg_service_control((uintptr_t)&v3,(uintptr_t)&result,sizeof v3)==SYSCALL_EINVAL);v3.source.request.reserved=0;
    fail_mutation=true;assert(pkg_service_control((uintptr_t)&v3,(uintptr_t)&result,sizeof v3)==SYSCALL_EIO&&source_calls==5&&!allocations);fail_mutation=false;
    for(unsigned i=0;i<2;i++)assert(source_files[i].ref_count==1&&source_files[i].offset==57);
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
