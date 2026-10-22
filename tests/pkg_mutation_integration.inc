/* Host thread delivery adapter only. Authority is the immutable pinned arena
 * view, not a TLS lane flag. Production needs a retained kernel call binding. */
#include "pkg_mutation_candidate.c"
typedef struct {
    pm_key key;const pm_view *admission;e4_ns_workspace_t *workspace;
    uint64_t left[2],grant[2],sequence;uint8_t grant_hash[32];
    uint32_t target;bool ready,create;
} mutation_call;
static _Thread_local mutation_call *mutation_current;
static const uint64_t mutation_mount=1;
static uint32_t mutation_ordinary_ino;
static pthread_barrier_t mutation_started,mutation_release,mutation_paused,mutation_continue;
static bool mutation_pause;
static int mutation_enter(pm_key key,mutation_call *call){
    spin_debug_assert_unheld();if(mutation_current)return -VFS_EAGAIN;
    const pm_view *view=pm_pin(key,mutation_mount);if(!view)return -VFS_EAGAIN;
    memset(call,0,sizeof(*call));call->key=key;call->admission=view;
    call->workspace=kcalloc(1,sizeof(*call->workspace));
    if(!call->workspace){assert(pm_unpin(key));return -VFS_ENOMEM;}
    mutation_current=call;return 0;
}
static void mutation_leave(mutation_call *call){
    spin_debug_assert_unheld();assert(mutation_current==call);mutation_current=NULL;
    kfree(call->workspace);assert(pm_unpin(call->key)&&pm_finish(call->key));
}
static int mutation_certificate(mutation_call *call,uint64_t r[2]){
    spin_debug_assert_held(&e4_lock);ext4_mount_t *fs=mounted;
    if(!fs->account_namespace_fenced||fs->engine->orphan_count||fs->engine->tainted)return -VFS_EIO;
    memset(call->workspace,0,sizeof(*call->workspace));
    memcpy(call->workspace->anchors,fs->account_namespace_anchors,sizeof call->workspace->anchors);
    return capacity_certificate(fs,call->workspace,r);
}
static int mutation_headroom(void *context){
    mutation_call *call=mutation_current;
    if(context!=mounted||!call||call->admission->kind!=PM_ACCOUNT)return -VFS_EACCES;
    uint64_t irq=spin_lock_irqsave(&e4_lock);readers_excluded=true;pg_view v;uint64_t r[2];uint8_t sb[1024];
    int status=life_read(mounted,&v)?-VFS_EIO:mutation_certificate(call,r);
    if(!status&&!e4_bytes(mounted,1024,sb,sizeof sb))status=-VFS_EIO;
    for(unsigned i=0;!status&&i<2;i++){
        uint64_t f=e4_u32(sb+12+4*i);
        if(f<v.remaining[i]||f-v.remaining[i]<r[i])status=-VFS_ENOSPC;
    }
    if(!status){memcpy(call->left,r,sizeof r);memcpy(call->grant,v.remaining,sizeof call->grant);
        call->sequence=v.sequence;hash(ledger,sizeof ledger,call->grant_hash);call->ready=true;}
    spin_unlock_irqrestore(&e4_lock,irq);return status;
}
static int mutation_after(int result){
    if(result)return result;
    mutation_call *call=mutation_current;uint64_t r[2];
    uint64_t irq=spin_lock_irqsave(&e4_lock);int checked=mutation_certificate(call,r);
    assert(!checked&&!memcmp(r,call->left,sizeof r));spin_unlock_irqrestore(&e4_lock,irq);return 0;
}
static int mutation_account_write(void *p,unsigned role,const void *bytes,size_t n){
    int r=mutation_headroom(p);if(!r)r=mutation_after(backend.write(p,role,bytes,n));
    if(!r&&mutation_pause&&role==ACCOUNT_STAGE_PASSWD){
        mutation_pause=false;spin_debug_assert_unheld();pthread_barrier_wait(&mutation_paused);pthread_barrier_wait(&mutation_continue);
    }return r;
}
static int mutation_account_remove(void *p,unsigned role){
    int r=mutation_headroom(p);return r?r:mutation_after(backend.remove(p,role));
}
static int mutation_account_rename(void *p,unsigned from,unsigned to){
    int r=mutation_headroom(p);return r?r:mutation_after(backend.rename(p,from,to));
}
static int mutation_storage(void *context,account_io_t *io){
    backend=*io;int r=mutation_headroom(context);if(r)return r;
    io->write=mutation_account_write;io->remove=mutation_account_remove;io->rename=mutation_account_rename;return 0;
}
static int mutation_dispatch(ext4_engine_t *e){
    spin_debug_assert_held(&e4_lock);mutation_call *call=mutation_current;
    if(!call||call->admission->mount!=mutation_mount||e!=mounted->engine)return -VFS_EACCES;
    if(call->admission->kind==PM_PACKAGE){
        if(!call->create&&call->target!=life_target)return -VFS_EACCES;
        return pg_tagged_commit(e);
    }
    pg_view view;if(life_read(e->fs,&view))return -VFS_EIO;
    uint8_t old[1024];if(!e4_bytes(e->fs,1024,old,sizeof old))return -VFS_EIO;
    uint8_t *next=e4_super(e);if(!next)return e->error;
    if(call->admission->kind==PM_ACCOUNT){
        uint8_t h[32];hash(ledger,sizeof ledger,h);
        if(!call->ready||view.sequence!=call->sequence||memcmp(h,call->grant_hash,sizeof h))return -VFS_EIO;
        uint64_t remain[2];
        for(unsigned i=0;i<2;i++){
            uint64_t f=e4_u32(next+12+4*i);
            int64_t required=(int64_t)call->left[i]+(int64_t)f-(int64_t)e4_u32(old+12+4*i);
            if(required<0||f<view.remaining[i]||f-view.remaining[i]<(uint64_t)required)return -VFS_ENOSPC;
            remain[i]=(uint64_t)required;
        }
        int r=pg_life_core_commit(e);if(!r){memcpy(call->left,remain,sizeof remain);capacity_commits++;}
        return r;
    }
    /* This candidate admits content changes to one prepared ordinary file.
     * General namespace mutations and arbitrary destinations remain excluded. */
    if(call->admission->kind!=PM_ORDINARY||call->target!=mutation_ordinary_ino)return -VFS_EACCES;
    uint64_t required[2];int r=mutation_certificate(call,required);if(r)return r;
    uint64_t free[2]={e4_u32(next+12),e4_u32(next+16)};
    r=capacity_preview_budget(e->fs,free,view.remaining,required);
    return r?r:pg_life_core_commit(e);
}
static int64_t mutation_write(uint32_t ino,char byte,size_t bytes){
    mutation_call *call=mutation_current;assert(call);call->target=ino;
    char data[4096*9];assert(bytes<=sizeof data);memset(data,byte,bytes);
    creds_t actor;creds_init_root(&actor);uint64_t irq=spin_lock_irqsave(&e4_lock),offset=0;
    int64_t r=e4_file_write_actor(mounted->engine,ino,&offset,true,data,bytes,&actor);
    spin_unlock_irqrestore(&e4_lock,irq);return r;
}
static int64_t mutation_control(pm_key key,tcb_t *proc,unsigned op,account_request_t *req){
    mutation_call call;int r=mutation_enter(key,&call);if(r)return syscall_from_vfs_error(r);
    if(call.admission->kind!=PM_ACCOUNT){mutation_leave(&call);return SYSCALL_EACCES;}
    int64_t result=accounts_control(op,(uintptr_t)req,sizeof(*req),&proc->creds,proc->tid);
    pg_integration_relax();mutation_leave(&call);return result;
}
typedef struct {pm_key key;uint32_t ino;char byte;} mutation_worker;
static void *mutation_writer(void *arg){
    mutation_worker *w=arg;mutation_call call;assert(!mutation_enter(w->key,&call));
    pthread_barrier_wait(&mutation_started);pthread_barrier_wait(&mutation_release);
    assert(mutation_write(w->ino,w->byte,mounted->bs)==(int64_t)mounted->bs);
    mutation_leave(&call);return NULL;
}
typedef struct {pm_key key;tcb_t *proc;account_request_t *request;} mutation_account_worker;
static void *mutation_account_update(void *arg){
    mutation_account_worker *w=arg;assert(!mutation_control(w->key,w->proc,ACCOUNT_UPDATE,w->request));return NULL;
}
int main(int argc,char **argv){
    (void)pg_capacity_commit; /* Historical baseline helper is not dispatched. */
    assert(argc==3);setvbuf(stdout,NULL,_IONBF,0);unsigned sector=(unsigned)strtoul(argv[2],NULL,10);
    image=fopen(argv[1],"r+b");assert(image&&!fseeko(image,0,SEEK_END));size_t size=ftello(image);
    disk_bytes=malloc(size);assert(disk_bytes&&!fseeko(image,0,SEEK_SET)&&fread(disk_bytes,1,size,image)==size);
    block_dev_t dev={.sector_size=sector,.sector_count=size/sector,.read_sector=rd,.write_sector=wr,.flush=flush};
    vfs_init();assert(!ext4_prepare_root(&dev,true,&mounted));ext4_account_namespace_report_t report;
    assert(!ext4_root_account_io(mounted,&backend)&&!ext4_root_account_namespace_admit(mounted,&report));
    integration_seed();vfs_node_t *ordinary=integration_create(ext4_prepared_root(mounted),"ordinary",false);
    mutation_ordinary_ino=((e4_node_t *)ordinary)->inode.ino;vfs_node_put(ordinary);
    assert(pm_configure(mutation_mount));integrated=life_active=true;pm_key key;
    assert(!pm_account(mutation_mount,&key));mutation_call boot;assert(!mutation_enter(key,&boot));
    assert(!accounts_prepare(mounted));pg_integration_relax();mutation_leave(&boot);integration_publish();
    tcb_t parent,child;integration_birth(&parent,100);integration_birth(&child,101);
    int fd=integration_acquire(&parent);assert(fd>=0);pw_key owner=lease.owner;uint64_t session=lease.session;
    unsigned no_writes=writes_seen;account_request_t wrong_request={.size=64,.version=1};
    assert(!pm_ordinary(mutation_mount,&key)&&mutation_control(key,&child,ACCOUNT_BEGIN,&wrong_request)==SYSCALL_EACCES&&writes_seen==no_writes);
    assert(!pm_package(mutation_mount,owner,session,&key));mutation_call create;assert(!mutation_enter(key,&create));
    create.create=true;assert(!life_create(mounted));mutation_leave(&create);
    uint32_t unit=life_target;
    /* Missing context, cross-role use and oversized grant spending reject before
     * the first device write, despite fully prepared planner images. */
    unsigned before=writes_seen;uint64_t irq=spin_lock_irqsave(&e4_lock),offset=0;char byte='!';
    assert(e4_file_write_actor(mounted->engine,mutation_ordinary_ino,&offset,false,&byte,1,NULL)==-VFS_EACCES);
    spin_unlock_irqrestore(&e4_lock,irq);assert(before==writes_seen);
    assert(!pm_ordinary(mutation_mount,&key));mutation_call wrong;assert(!mutation_enter(key,&wrong));
    assert(mutation_account_write(mounted,ACCOUNT_STAGE_PASSWD,"!",1)==-VFS_EACCES);
    assert(mutation_write(unit,'!',1)==-VFS_EACCES&&writes_seen==before);mutation_leave(&wrong);
    assert(!pm_package(mutation_mount,owner,session,&key));assert(!mutation_enter(key,&wrong));
    assert(mutation_write(mutation_ordinary_ino,'!',1)==-VFS_EACCES);
    assert(writes_seen==before);mutation_leave(&wrong);
    assert(!pthread_barrier_init(&mutation_started,NULL,5)&&!pthread_barrier_init(&mutation_release,NULL,5));
    mutation_worker workers[4];pthread_t threads[4];
    for(unsigned i=0;i<4;i++){
        if(i<2)assert(!pm_package(mutation_mount,owner,session,&workers[i].key));
        else assert(!pm_ordinary(mutation_mount,&workers[i].key));
        workers[i].ino=i<2?unit:mutation_ordinary_ino;workers[i].byte="PQNO"[i];
        assert(!pthread_create(&threads[i],NULL,mutation_writer,&workers[i]));
    }
    pthread_barrier_wait(&mutation_started);pm_key waiting,account,denied;
    assert(!pm_ordinary(mutation_mount,&waiting)&&!pm_account(mutation_mount,&account));
    assert(!pm_pin(account,mutation_mount)&&!pm_pin(waiting,mutation_mount));
    unsigned refs=pw_references(owner);assert(pm_package(mutation_mount,owner,session,&denied)==-VFS_EAGAIN&&pw_references(owner)==refs);
    assert(pm_ordinary(mutation_mount,&denied)==-VFS_EAGAIN);
    pthread_barrier_wait(&mutation_release);for(unsigned i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));
    account_request_t request={.size=64,.version=1};assert(!mutation_control(account,&child,ACCOUNT_BEGIN,&request));
    /* Seven more blocks after two charged blocks exceed the eight-block grant,
     * while remaining within the backend's 32 KiB single-call bound. */
    before=writes_seen;assert(!pm_package(mutation_mount,owner,session,&key)&&!mutation_enter(key,&wrong));
    assert(mutation_write(unit,'!',mounted->bs*7)==-VFS_EFBIG&&writes_seen==before);mutation_leave(&wrong);
    assert(pm_finish(waiting));
    assert(!pm_account(mutation_mount,&account));
    request.passwd=(uintptr_t)integration_new[0];request.group=(uintptr_t)integration_new[1];request.shadow=(uintptr_t)integration_new[2];
    request.passwd_bytes=strlen(integration_new[0]);request.group_bytes=strlen(integration_new[1]);request.shadow_bytes=strlen(integration_new[2]);
    assert(!pthread_barrier_init(&mutation_paused,NULL,2)&&!pthread_barrier_init(&mutation_continue,NULL,2));
    mutation_pause=true;mutation_account_worker update={account,&child,&request};pthread_t updater;
    assert(!pthread_create(&updater,NULL,mutation_account_update,&update));pthread_barrier_wait(&mutation_paused);
    before=writes_seen;assert(pm_ordinary(mutation_mount,&denied)==-VFS_EAGAIN&&pm_account(mutation_mount,&denied)==-VFS_EAGAIN);
    refs=pw_references(owner);assert(pm_package(mutation_mount,owner,session,&denied)==-VFS_EAGAIN&&pw_references(owner)==refs);
    int error;assert(!vfs_open_creds("/etc/passwd",VFS_O_RDONLY,&child.creds,&error)&&error==-VFS_EAGAIN&&writes_seen==before);
    pthread_barrier_wait(&mutation_continue);assert(!pthread_join(updater,NULL));
    request.passwd=request.group=request.shadow=0;request.passwd_bytes=request.group_bytes=request.shadow_bytes=0;
    assert(!pm_account(mutation_mount,&account)&&!mutation_control(account,&child,ACCOUNT_FINISH,&request));
    assert(!pm_package(mutation_mount,owner,session,&key));mutation_call retained;assert(!mutation_enter(key,&retained));
    (void)process_record_exit_accounted_times(parent.tid,0,0,0,0,0);fd_close_all(&parent);
    assert(pw_references(owner)==2&&pm_package(mutation_mount,owner,session,&denied)==-VFS_EACCES);
    assert(mutation_write(unit,'Z',mounted->bs)==(int64_t)mounted->bs);mutation_leave(&retained);assert(!pw_references(owner));
    process_record_forget(parent.tid);before=writes_seen;assert(integration_acquire(&child)<0&&writes_seen==before);
    unsigned count,pins;bool pending;pm_counts(&count,&pins,&pending);assert(!count&&!pins&&!pending);
    irq=spin_lock_irqsave(&e4_lock);pg_view view;assert(!life_validate(mounted->engine,&view)&&view.charged[0]==3&&view.charged[1]==1&&view.remaining[0]==5&&!view.remaining[1]);
    spin_unlock_irqrestore(&e4_lock,irq);process_record_abort(child.tid);
    assert(!ext4_freeze_and_sync(mounted));assert(!fclose(image));free(disk_bytes);
    assert(!pthread_barrier_destroy(&mutation_started)&&!pthread_barrier_destroy(&mutation_release)&&!pthread_barrier_destroy(&mutation_paused)&&!pthread_barrier_destroy(&mutation_continue));
    printf("PASS mutation filesystem block=%u sector=%u concurrent=4 account-priority paused-update owner-death G=5,0 Q=3,1\n",mounted->bs,sector);return 0;
}
