#include "pkg_service.h"
#include "pkg_abi.h"
#include "thread.h"
#include "syscall.h"
#include "vmm.h"
#include "string.h"
#include "spinlock.h"
#include "vfs.h"
#include "heap.h"
#include "console.h"
#include "serial.h"

static void srv_log(const char *msg) {
    serial_puts(msg);
}
static const pkg_kernel_backend *binding;
static unsigned source_busy;
/* The opened object stays pinned; a private file position avoids changing the
 * caller's shared descriptor offset. Sources are never userspace pointers. */
static int source_copy(file_t *file,uint8_t *out,size_t bytes) {
    vfs_metadata_t meta;
    if(!file->node||file->node->is_stream||vfs_metadata(file->node,&meta)||
       meta.type!=VFS_FILE||meta.size!=bytes)return -VFS_EINVAL;
    file_t view={.node=file->node,.flags=file->flags,.ref_count=1};
    size_t at=0;
    while(at<bytes){
        size_t n=bytes-at;if(n>4096)n=4096;
        int64_t got=vfs_read(&view,out+at,n);
        if(got<=0||(uint64_t)got>n)return got<0?(int)got:-VFS_EIO;
        at+=(size_t)got;
    }
    uint8_t extra;
    if(vfs_read(&view,&extra,1)!=0)return -VFS_EIO;
    return 0;
}
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
    if(bytes!=sizeof(pkg_request_v1)&&bytes!=sizeof(pkg_request_v2))return SYSCALL_EINVAL;
    tcb_t *t=thread_current();uint64_t *pml4=vmm_get_active_pml4_virt();
    if(!t||!t->is_user||!vmm_validate_user_range(pml4,input,bytes,false)||
       !vmm_validate_user_range(pml4,output,sizeof(pkg_result_v1),true))return SYSCALL_EFAULT;
    pkg_request_v2 source={0};memcpy(&source,(void *)input,bytes);
    pkg_request_v1 q=source.request;
    bool ingest=bytes==sizeof source;
    bool mutation=ingest||q.operation==PKG_INSTALL||q.operation==PKG_REMOVE;
    if(q.size!=bytes||q.version!=(ingest?2u:1u)||q.reserved||q.reserved64||
       (ingest?q.operation!=PKG_INSTALL_SOURCE:(q.operation<PKG_STATUS||q.operation>PKG_REMOVE))||
       ((q.operation==PKG_STATUS||q.operation==PKG_ACQUIRE)?q.session!=0:q.session==0)||
       (mutation?!name_valid(q.package):!zero(q.package,sizeof q.package)))return SYSCALL_EINVAL;
    if(ingest&&(!source.catalog_bytes||source.catalog_bytes>PKG_SOURCE_CATALOG_MAX||
       source.artifact_bytes<128||source.artifact_bytes>PKG_SOURCE_ARTIFACT_MAX))return SYSCALL_EINVAL;
    if(ingest&&(source.catalog_fd>=MAX_PROCESS_FDS||source.artifact_fd>=MAX_PROCESS_FDS))return SYSCALL_EBADF;
    const pkg_kernel_backend *b=__atomic_load_n(&binding,__ATOMIC_ACQUIRE);
    if(!b||!b->admitted())return SYSCALL_EOPNOTSUPP;
    if(ingest&&!b->install_source)return SYSCALL_EOPNOTSUPP;
    if(!t->pkg_owner.generation&&!pw_register(t->tid,&t->pkg_owner))return SYSCALL_EAGAIN;
    pb_result captured={0};int r;
    if(!mutation) {
        pb_request request={.size=sizeof request,.version=1,.operation=q.operation,.session=q.session};
        r=pb_control(&request,t->pkg_owner,true,&b->transactions,&captured);
    } else {
        pw_key operation;
        if(!pw_enter(t->pkg_owner,q.session,&operation))return SYSCALL_EACCES;
        pkg_transaction_status s={0};
        if(ingest) {
            srv_log("[PKG-SRV] starting ingest\n");
            /* One bounded memory credit, acquired only by an authorized retained
             * session. No spinlock across allocation, verification or storage. */
            unsigned expected=0;
            if(!__atomic_compare_exchange_n(&source_busy,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))r=-VFS_EAGAIN;
            else {
                file_t *catalog=fd_get(t,(int)source.catalog_fd),*artifact=fd_get(t,(int)source.artifact_fd);
                bool cat_held=vfs_file_get(catalog),art_held=vfs_file_get(artifact);
                size_t total=source.catalog_bytes+source.artifact_bytes;
                uint8_t *copy=cat_held&&art_held?kmalloc_large(total):NULL;
                if(!cat_held||!art_held)r=-VFS_EBADF;
                else if(!copy)r=-VFS_ENOMEM;
                else {
                    srv_log("[PKG-SRV] copying sources...\n");
                    r=source_copy(catalog,copy,source.catalog_bytes);
                    if(!r)r=source_copy(artifact,copy+source.catalog_bytes,source.artifact_bytes);
                    if(!r&&!pw_recovery_step(t->pkg_owner))r=-VFS_EACCES;
                    srv_log("[PKG-SRV] invoking b->install_source...\n");
                    if(!r)r=b->install_source(b->transactions.context,q.package,copy,source.catalog_bytes,
                                            copy+source.catalog_bytes,source.artifact_bytes,&s);
                    if(r) {
                        srv_log("[PKG-SRV] install_source failed!\n");
                    } else {
                        srv_log("[PKG-SRV] install_source succeeded!\n");
                    }
                    kfree(copy);
                }
                if(cat_held)vfs_close(catalog);
                if(art_held)vfs_close(artifact);
                __atomic_store_n(&source_busy,0,__ATOMIC_RELEASE);
            }
        } else r=b->mutate(b->transactions.context,q.operation==PKG_REMOVE,q.package,&s);
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
