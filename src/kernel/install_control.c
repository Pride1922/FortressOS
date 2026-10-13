#include "install_control.h"
#include "install_log.h"
#include "thread.h"
#include "syscall.h"
#include "vfs.h"
#include "vmm.h"
#include "heap.h"
#include "string.h"
#include "../../user/tools/digest.h"
static install_backend_t backend;
static bool mode,leased,valid;
static unsigned busy;
static uint64_t owner;
static file_t *lease_file;
static install_identity_v1_t selected;
static uint8_t io_bytes[4096];
static vfs_node_t lease_node;
static bool zero(const void *p,size_t n) {
    const uint8_t *b=p;for (size_t i=0;i<n;i++) if (b[i]) return false;return true;
}
void install_identity_digest(install_identity_v1_t *id) {
    digest_t d;digest_init(&d,DIGEST_SHA256);
    (void)digest_update(&d,id,128);digest_final(&d,id->digest);
}
static bool identity_valid(const install_identity_v1_t *id) {
    if (id->size!=sizeof(*id) || id->version!=1 || !id->generation || !id->nsid ||
        !id->sector_size || !id->sector_count || id->reserved0 ||
        !zero(id->reserved1,sizeof(id->reserved1)) || !zero(id->reserved2,sizeof(id->reserved2)) ||
        (id->flags & ~(INSTALL_ID_ELIGIBLE|INSTALL_ID_SOURCE|INSTALL_ID_MOUNTED))) return false;
    install_identity_v1_t copy=*id;install_identity_digest(&copy);
    return !memcmp(copy.digest,id->digest,32);
}
void install_control_boot(const install_backend_t *b,bool installer_mode) {
    backend=b ? *b : (install_backend_t){0};mode=installer_mode;
}
bool install_control_owned(void) {return __atomic_load_n(&leased,__ATOMIC_ACQUIRE);}
static void closed(vfs_node_t *node) {
    if (node!=&lease_node) return;
    valid=false;owner=0;lease_file=NULL;memset(&selected,0,sizeof(selected));
    __atomic_store_n(&leased,false,__ATOMIC_RELEASE);
}
static bool range(uintptr_t p,size_t n,bool write) {
    return p && vmm_validate_user_range(vmm_get_active_pml4_virt(),p,n,write);
}
static int current_identity(install_identity_v1_t *id) {
    if (!backend.identity) return -VFS_EOPNOTSUPP;
    memset(id,0,sizeof(*id));int r=backend.identity(backend.context,id);
    return r ? r<0 ? r : -VFS_EIO : identity_valid(id) ? 0 : -VFS_EIO;
}
static int lease_check(tcb_t *proc,int fd) {
    if (!proc || !leased || owner!=proc->tid || fd_get(proc,fd)!=lease_file || !lease_file) return -VFS_EBADF;
    if (!valid) return -VFS_EIO;
    install_identity_v1_t now;int r=current_identity(&now);
    if (!r && (memcmp(&now,&selected,sizeof(now)) || now.flags!=INSTALL_ID_ELIGIBLE)) r=-VFS_EIO;
    if (r) valid=false;
    return r;
}
int64_t install_control(unsigned op,uintptr_t input,size_t input_size,uintptr_t output,size_t output_size,const creds_t *actor) {
    size_t required=op==INSTALL_LOG ? sizeof(install_log_event_v1_t) : op==INSTALL_DEPLOY ? (input_size==sizeof(install_deploy_v2_t) ? sizeof(install_deploy_v2_t) : sizeof(install_deploy_v1_t)) : op==INSTALL_ENUM || op==INSTALL_FLUSH || op==INSTALL_RELEASE ? 4 :
        op==INSTALL_ACQUIRE ? sizeof(install_target_v1_t) : sizeof(install_io_v1_t);
    size_t out_size=op==INSTALL_DEPLOY ? sizeof(install_status_v1_t) : op==INSTALL_ENUM ? sizeof(install_identity_v1_t) : op==INSTALL_ACQUIRE ? 4 : 0;
    if (op>INSTALL_LOG || input_size!=required || output_size!=out_size || (!out_size && output)) return SYSCALL_EINVAL;
    if (!range(input,required,false)) return SYSCALL_EFAULT;
    union {uint32_t index;int32_t fd;install_target_v1_t target;install_io_v1_t io;install_deploy_v1_t deploy;install_deploy_v2_t deploy2;install_log_event_v1_t log;} request;
    memset(&request,0,sizeof(request));memcpy(&request,(void *)input,required);
    if (op==INSTALL_DEPLOY && (request.deploy.size!=required || request.deploy.version!=(required==sizeof(install_deploy_v2_t) ? 2u : 1u) ||
        request.deploy.flags>INSTALL_DRY_RUN || !zero(request.deploy.reserved,4) ||
        (request.deploy.flags==INSTALL_DRY_RUN && request.deploy.fd!=-1))) return SYSCALL_EINVAL;
    if (op==INSTALL_DEPLOY && request.deploy.version==2 && !install_username_valid(request.deploy2.username)) return SYSCALL_EINVAL;
    if (op==INSTALL_ACQUIRE && (request.target.size!=required || request.target.version!=1 ||
        request.target.flags!=INSTALL_CONFIRM_WHOLE_DISK || !zero(request.target.reserved,8))) return SYSCALL_EINVAL;
    if ((op==INSTALL_READ || op==INSTALL_WRITE) && (request.io.size!=required || request.io.version!=1 ||
        request.io.reserved0 || request.io.reserved1 || !request.io.count || request.io.count>8)) return SYSCALL_EINVAL;
    if (!actor || actor->euid || (actor->cap_effective & (CAP_SYS_ADMIN|CAP_SYS_RAWIO))!=(CAP_SYS_ADMIN|CAP_SYS_RAWIO)) return SYSCALL_EPERM;
    if (!mode || (op!=INSTALL_LOG && !backend.identity)) return SYSCALL_EOPNOTSUPP;
    if (out_size && !range(output,out_size,true)) return SYSCALL_EFAULT;
    if ((op==INSTALL_READ || op==INSTALL_WRITE) && !range(request.io.buffer,request.io.count*512,op==INSTALL_READ)) return SYSCALL_EFAULT;
    tcb_t *proc=thread_current();if (!proc || !proc->is_user || proc->current_cpu) return SYSCALL_EPERM;
    if (__atomic_exchange_n(&busy,1,__ATOMIC_ACQUIRE)) return SYSCALL_EAGAIN;
    int r=0;int64_t result=0;
    if (op==INSTALL_LOG) {
        r=install_log_event(&request.log);
    } else if (op==INSTALL_DEPLOY) {
        if (!backend.deploy) {r=-VFS_EOPNOTSUPP;goto done;}
        if (!request.deploy.flags) {r=lease_check(proc,request.deploy.fd);if (r) goto done;}
        install_status_v1_t status={.size=sizeof(status),.version=1};
        r=backend.deploy(backend.context,&request.deploy,&status);
        if (r>0) r=-VFS_EIO;
        if (r && !request.deploy.flags) valid=false;
        memcpy((void *)output,&status,sizeof(status));
    } else if (op==INSTALL_ENUM) {
        if (leased && backend.enumerate) {r=-VFS_EAGAIN;goto done;}
        if (request.index && !backend.enumerate) {result=0;goto done;}
        install_identity_v1_t id;
        if (backend.enumerate) {
            memset(&id,0,sizeof(id));r=backend.enumerate(backend.context,request.index,&id);
            if (!r) goto done;
            if (r==1) {install_identity_v1_t plain=id;memset(plain.reserved2,0,sizeof(plain.reserved2));r=identity_valid(&plain) ? 0 : -VFS_EIO;}
            else {r=r<0 ? r : -VFS_EIO;goto done;}
        } else r=current_identity(&id);
        if (!r) {memcpy((void *)output,&id,sizeof(id));result=1;}
    } else if (op==INSTALL_ACQUIRE) {
        if (leased) {r=-VFS_EAGAIN;goto done;}
        if (backend.select) {r=backend.select(backend.context,request.target.index);if (r) goto done;}
        else if (request.target.index) {r=-VFS_EINVAL;goto done;}
        install_identity_v1_t id;r=current_identity(&id);if (r) goto done;
        if (request.target.generation!=id.generation || memcmp(request.target.digest,id.digest,32)) {r=-VFS_EAGAIN;goto done;}
        if (id.flags!=INSTALL_ID_ELIGIBLE) {r=-VFS_EACCES;goto done;}
        if (id.sector_size!=512 || id.sector_count<324u*2048 || id.sector_count>UINT64_MAX/512 ||
            !backend.read || !backend.write || !backend.flush) {r=-VFS_EINVAL;goto done;}
        file_t *f=kcalloc(1,sizeof(*f));if (!f) {r=-VFS_ENOMEM;goto done;}
        f->node=&lease_node;f->ref_count=1;f->exclusive_owner=true;
        int fd=fd_alloc(proc,f);
        if (fd<0) {kfree(f);result=SYSCALL_EMFILE;goto done;}
        r=backend.flush(backend.context);
        if (r) {proc->fd_table[fd]=NULL;proc->fd_flags[fd]=0;kfree(f);r=r<0 ? r : -VFS_EIO;goto done;}
        lease_node=(vfs_node_t){.type=VFS_STREAM,.is_stream=true,.close=closed};
        selected=id;owner=proc->tid;valid=true;lease_file=f;
        proc->fd_flags[fd]=FD_FLAG_CLOEXEC;__atomic_store_n(&leased,true,__ATOMIC_RELEASE);
        memcpy((void *)output,&fd,sizeof(int32_t));
    } else if (op==INSTALL_RELEASE) {
        if (!leased || owner!=proc->tid || fd_get(proc,request.fd)!=lease_file || !lease_file) r=-VFS_EBADF;
        else (void)fd_free(proc,request.fd);
    } else {
        int fd=op==INSTALL_FLUSH ? request.fd : request.io.fd;
        r=lease_check(proc,fd);if (r) goto done;
        if (op==INSTALL_FLUSH) r=backend.flush(backend.context);
        else {
            uint64_t lba=request.io.lba;uint32_t count=request.io.count;
            if (lba>=selected.sector_count || count>selected.sector_count-lba) {r=-VFS_EINVAL;goto done;}
            if (op==INSTALL_WRITE) {
                memcpy(io_bytes,(void *)request.io.buffer,count*512);
                r=backend.write(backend.context,lba,count,io_bytes);
            } else {
                r=backend.read(backend.context,lba,count,io_bytes);
                if (!r) memcpy((void *)request.io.buffer,io_bytes,count*512);
            }
        }
        if (r) {valid=false;if (r>0) r=-VFS_EIO;}
    }
done:
    if (r) result=syscall_from_vfs_error(r);
    memset(&request,0,sizeof(request));
    memset(io_bytes,0,sizeof(io_bytes));__atomic_store_n(&busy,0,__ATOMIC_RELEASE);return result;
}
