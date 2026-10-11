#include "accounts.h"
#include "syscall.h"
#include "heap.h"
#include "string.h"
#include "vmm.h"
#include "../../user/tools/userdb.h"
static account_io_t storage;
static vfs_node_t *namespace;
static bool enabled;
static unsigned busy;
static uint64_t generation=1,writer,token,next_token;
static bool writer_dead,latched;
/* One operation gate, never held as a spinlock and never waited on. Contention
 * returns EAGAIN. All state access is under this gate except the boot-only init.
 * Parser static scratch is serialized by the same gate. */
static bool enter(void) {return !__atomic_exchange_n(&busy,1,__ATOMIC_ACQUIRE);}
static void leave(void) {__atomic_store_n(&busy,0,__ATOMIC_RELEASE);}
static int coherent_namespace(vfs_node_t *root_namespace,const char *const bytes[3],const size_t sizes[3]) {
    static userdb_t db;memset(&db,0,sizeof(db));
    bool ok=db_passwd(&db,bytes[0],sizes[0]) && db_group(&db,bytes[1],sizes[1]) &&
        db_shadow(&db,bytes[2],sizes[2]) && db_validate(&db);
    const db_user_t *root=ok ? db_user_id(&db,0) : NULL;
    const db_shadow_t *shadow=root ? db_shadow_name(&db,root->name) : NULL;
    ok=root && !strcmp(root->name,"root") && !root->gid && !strcmp(root->home,"/root") &&
        shadow && (shadow->hash[0]=='!' || shadow->hash[0]=='*');
    creds_t actor;creds_init_root(&actor);
    for (unsigned i=0;i<db.nusers && ok;i++) {
        const db_user_t *u=&db.users[i];const db_shadow_t *s=db_shadow_name(&db,u->name);
        if (!s || !*s->hash || strcmp(u->shell,"/bin/shell")) {ok=false;break;}
        char home[128]="/home/";memcpy(home+6,u->name,strlen(u->name)+1);
        if (u->uid && (u->uid<1000 || strcmp(home,u->home))) {ok=false;break;}
        int error;vfs_node_t *n=vfs_lookup_at_creds(root_namespace,u->home,&actor,&error);vfs_metadata_t m;
        ok=n && !vfs_metadata(n,&m) && m.type==VFS_DIRECTORY && m.uid==u->uid && m.gid==u->gid && (m.mode & 07777)==0700;
        vfs_node_put(n);
    }
    db_wipe(&db,sizeof(db));return ok ? 0 : -VFS_EINVAL;
}
static int coherent(void *context,const char *const bytes[3],const size_t sizes[3]) {
    (void)context;return coherent_namespace(namespace,bytes,sizes);
}
static int coherent_detached(void *context,const char *const bytes[3],const size_t sizes[3]) {
    vfs_node_t *root=ext4_prepared_root(context);
    return root ? coherent_namespace(root,bytes,sizes) : -VFS_EINVAL;
}
static account_workspace_t *workspace(void) {return kcalloc(1,sizeof(account_workspace_t));}
static void release_workspace(account_workspace_t *w) {if (w) {account_redo_wipe(w);kfree(w);}}
int accounts_provision_detached(ext4_mount_t *fs,const char *const bytes[3],const size_t sizes[3]) {
    if (!fs || !bytes || !sizes || !ext4_prepared_root(fs)) return -VFS_EINVAL;
    for (unsigned i=0;i<3;i++) if (!bytes[i] || !sizes[i] || sizes[i]>ACCOUNT_FILE_MAX) return -VFS_EINVAL;
    if (!enter()) return -VFS_EAGAIN;
    int r=storage.context || enabled || writer ? -VFS_EACCES : 0;
    account_io_t io={0};account_workspace_t *w=NULL;
    if (!r) r=coherent_detached(fs,bytes,sizes);
    if (!r) r=ext4_root_account_io(fs,&io);
    io.validate=coherent_detached;
    if (!r) {w=workspace();if (!w) r=-VFS_ENOMEM;}
    if (!r) r=account_redo_recover(&io,w);
    if (!r) {
        for (unsigned i=0;i<3;i++) {memcpy(w->files[i],bytes[i],sizes[i]);w->lengths[i]=sizes[i];}
        r=account_redo_update(&io,w,1);
    }
    release_workspace(w);leave();return r;
}
int accounts_prepare(ext4_mount_t *fs) {
    namespace=ext4_prepared_root(fs);if (!namespace) return -VFS_EINVAL;
    int r=ext4_root_account_io(fs,&storage);if (r) return r;
    storage.validate=coherent;account_workspace_t *w=workspace();if (!w) return -VFS_ENOMEM;
    r=account_redo_recover(&storage,w);release_workspace(w);enabled=!r;return r;
}
void accounts_disable(void) {__atomic_store_n(&latched,true,__ATOMIC_RELEASE);}
void accounts_abandon(void) {enabled=false;storage=(account_io_t){0};namespace=NULL;}
static bool range(uint64_t p,size_t n,bool output) {
    return p && vmm_validate_user_range(vmm_get_active_pml4_virt(),p,n,output);
}
static int read_set(account_workspace_t *w) {
    for (unsigned i=0;i<3;i++) {
        int r=storage.read(storage.context,i,w->files[i],ACCOUNT_FILE_MAX,&w->lengths[i]);if (r) return r;
    }
    const char *p[3]={w->files[0],w->files[1],w->files[2]};return coherent(storage.context,p,w->lengths);
}
static bool empty(const account_request_t *r) {
    return !r->passwd && !r->group && !r->shadow && !r->passwd_bytes && !r->group_bytes && !r->shadow_bytes && !r->flags;
}
static void retire_dead(void) {
    if (!__atomic_exchange_n(&writer_dead,false,__ATOMIC_ACQUIRE)) return;
    account_workspace_t *w=workspace();size_t n;
    int r=w ? storage.read(storage.context,ACCOUNT_PENDING,w->intent,sizeof(w->intent),&n) : -VFS_ENOMEM;
    enabled=r==-VFS_ENOENT && !read_set(w) && !storage.sync(storage.context) && !__atomic_load_n(&latched,__ATOMIC_ACQUIRE);
    __atomic_store_n(&writer,0,__ATOMIC_RELEASE);token=0;release_workspace(w);
}
int64_t accounts_control(unsigned op,uintptr_t address,size_t size,const creds_t *actor,uint64_t pid) {
    if (size!=sizeof(account_request_t) || op>ACCOUNT_STATUS || op==ACCOUNT_RECOVER) return SYSCALL_EINVAL;
    if (!range(address,size,true)) return SYSCALL_EFAULT;
    account_request_t req;memcpy(&req,(void *)address,size);
    if (req.size!=size || req.version!=1) return SYSCALL_EINVAL;
    bool priv=actor && !actor->euid && (actor->cap_effective & CAP_SYS_ADMIN);
    if ((op==ACCOUNT_BEGIN || op==ACCOUNT_UPDATE || op==ACCOUNT_FINISH) && !priv) return SYSCALL_EPERM;
    if (op==ACCOUNT_SNAPSHOT && (req.flags & ACCOUNT_WITH_SHADOW) &&
        (!actor || actor->euid || !(actor->cap_effective & CAP_DAC_READ_SEARCH))) return SYSCALL_EACCES;
    /* Namespace identity is published at boot and survives authentication
     * failure. It must remain queryable during a writer operation, otherwise
     * installed configuration consumers could choose live fallback paths. */
    if (op==ACCOUNT_STATUS) {
        if (!empty(&req) || req.token || req.generation) return SYSCALL_EINVAL;
        if (!storage.context) return SYSCALL_EOPNOTSUPP;
        req.generation=__atomic_load_n(&generation,__ATOMIC_ACQUIRE);
        memcpy((void *)address,&req,sizeof(req));return 0;
    }
    if (!enter()) return SYSCALL_EAGAIN;
    retire_dead();
    if (__atomic_load_n(&latched,__ATOMIC_ACQUIRE)) enabled=false;
    int64_t result=0;account_workspace_t *w=NULL;
    if (!storage.context) {result=SYSCALL_EOPNOTSUPP;goto done;}
    if (op==ACCOUNT_CHECK) {
        if (!empty(&req) || req.token) result=SYSCALL_EINVAL;
        else if (!enabled || writer || req.generation!=generation) result=SYSCALL_EAGAIN;
    } else if (op==ACCOUNT_BEGIN) {
        if (!empty(&req) || req.token) result=SYSCALL_EINVAL;
        else if (!enabled || writer || (req.generation && req.generation!=generation)) result=SYSCALL_EAGAIN;
        else if (next_token==UINT64_MAX || generation==UINT64_MAX) result=SYSCALL_EFBIG;
        else {
            int r=ext4_root_account_headroom(storage.context);
            if (r) result=syscall_from_vfs_error(r);
            else {__atomic_store_n(&writer,pid,__ATOMIC_RELEASE);token=++next_token;req.token=token;req.generation=generation;}
        }
    } else if (op==ACCOUNT_FINISH) {
        if (!empty(&req) || !writer || writer!=pid || req.token!=token || req.generation!=generation) result=SYSCALL_EINVAL;
        else {
            w=workspace();if (!w) {result=SYSCALL_ENOMEM;goto done;}
            size_t n;int r=storage.read(storage.context,ACCOUNT_PENDING,w->intent,sizeof(w->intent),&n);
            if (r!=-VFS_ENOENT) result=r ? syscall_from_vfs_error(r) : SYSCALL_EAGAIN;
            else if ((r=read_set(w)) || (r=storage.sync(storage.context))) result=syscall_from_vfs_error(r);
            else {__atomic_store_n(&writer,0,__ATOMIC_RELEASE);token=0;enabled=!__atomic_load_n(&latched,__ATOMIC_ACQUIRE);}
        }
    } else if (op==ACCOUNT_SNAPSHOT || op==ACCOUNT_UPDATE) {
        uint64_t pointers[3]={req.passwd,req.group,req.shadow};
        uint32_t sizes[3]={req.passwd_bytes,req.group_bytes,req.shadow_bytes};
        unsigned count=op==ACCOUNT_UPDATE || (req.flags & ACCOUNT_WITH_SHADOW) ? 3 : 2;
        if (op==ACCOUNT_SNAPSHOT ? req.flags>1 || req.token || req.generation :
            req.flags || !writer || writer!=pid || req.token!=token || req.generation!=generation) {result=SYSCALL_EINVAL;goto done;}
        if (count==2 && (req.shadow || req.shadow_bytes)) {result=SYSCALL_EINVAL;goto done;}
        for (unsigned i=0;i<count;i++) {
            if (op==ACCOUNT_SNAPSHOT ? sizes[i]!=ACCOUNT_FILE_MAX : !sizes[i] || sizes[i]>ACCOUNT_FILE_MAX) {result=SYSCALL_EINVAL;goto done;}
            if (!range(pointers[i],sizes[i],op==ACCOUNT_SNAPSHOT)) {result=SYSCALL_EFAULT;goto done;}
        }
        if (!enabled) {result=SYSCALL_EIO;goto done;}
        if (op==ACCOUNT_SNAPSHOT && writer) {result=SYSCALL_EAGAIN;goto done;}
        w=workspace();if (!w) {result=SYSCALL_ENOMEM;goto done;}
        int r=0;
        if (op==ACCOUNT_SNAPSHOT) {
            r=read_set(w);
            if (!r) {
                for (unsigned i=0;i<count;i++) memcpy((void *)pointers[i],w->files[i],w->lengths[i]);
                req.passwd_bytes=w->lengths[0];req.group_bytes=w->lengths[1];req.shadow_bytes=count==3 ? w->lengths[2] : 0;req.generation=generation;
            }
        } else {
            for (unsigned i=0;i<3;i++) {memcpy(w->files[i],(void *)pointers[i],sizes[i]);w->lengths[i]=sizes[i];}
            const char *p[3]={w->files[0],w->files[1],w->files[2]};r=coherent(storage.context,p,w->lengths);
            if (!r && generation==UINT64_MAX) {result=SYSCALL_EFBIG;goto done;}
            if (!r) {
                /* Invalidate outstanding decisions even when an uncertain
                 * operation publishes new bytes and returns an error. */
                enabled=false;__atomic_add_fetch(&generation,1,__ATOMIC_RELEASE);r=account_redo_update(&storage,w,generation);
                if (!r) {req.generation=generation;enabled=!__atomic_load_n(&latched,__ATOMIC_ACQUIRE);}
            }
        }
        if (r) result=syscall_from_vfs_error(r);
    }
done:
    if (!result) memcpy((void *)address,&req,sizeof(req));
    release_workspace(w);leave();return result;
}
void accounts_process_exit(uint64_t pid) {
    if (!pid || __atomic_load_n(&writer,__ATOMIC_ACQUIRE)!=pid) return;
    __atomic_store_n(&writer_dead,true,__ATOMIC_RELEASE);
    if (enter()) {retire_dead();leave();}
}
int64_t accounts_seed(uintptr_t address,size_t length,const creds_t *actor) {
    if (!length || length>256) return SYSCALL_EINVAL;
    if (!actor || actor->euid || !(actor->cap_effective & CAP_SYS_ADMIN)) return SYSCALL_EPERM;
    if (!range(address,length,true)) return SYSCALL_EFAULT;
    uint8_t bytes[256]={0};uint32_t a,b,c,d,max;
    __asm__ volatile("cpuid":"=a"(max),"=b"(b),"=c"(c),"=d"(d):"a"(0),"c"(0));
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1),"c"(0));bool rand=(c & (1u<<30))!=0,seed=false;
    if (max>=7) {__asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(7),"c"(0));seed=(b & (1u<<18))!=0;}
    for (size_t offset=0;offset<length;offset+=8) {
        uint64_t value=0;unsigned char ok=0;
        if (seed) for (unsigned i=0;i<64 && !ok;i++) __asm__ volatile("rdseed %0; setc %1":"=r"(value),"=qm"(ok)::"cc");
        if (rand) for (unsigned i=0;i<64 && !ok;i++) __asm__ volatile("rdrand %0; setc %1":"=r"(value),"=qm"(ok)::"cc");
        if (!ok) {db_wipe(bytes,sizeof(bytes));return SYSCALL_EIO;}
        size_t n=length-offset<8 ? length-offset : 8;memcpy(bytes+offset,&value,n);value=0;
    }
    memcpy((void *)address,bytes,length);db_wipe(bytes,sizeof(bytes));return 0;
}
