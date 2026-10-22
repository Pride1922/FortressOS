/* Private copied-TCB owner delivery; only thread_current() is host TLS. */
static tcb_t call_boot_task;
static _Thread_local tcb_t call_worker_task;
static vfs_node_t *call_sandbox[3];
static unsigned call_namespace_commits;
static void call_bootstrap(void){
    integration_birth(&call_boot_task,90);call_host_current=&call_boot_task;
    call_sandbox[0]=integration_create(ext4_prepared_root(mounted),"work",true);
    call_sandbox[1]=integration_create(call_sandbox[0],"a",true);
    call_sandbox[2]=integration_create(call_sandbox[0],"b",true);
}
static void call_worker_enter(unsigned id){
    integration_birth(&call_worker_task,1000+id);call_host_current=&call_worker_task;
}
static void call_worker_exit(void){
    assert(!call_worker_task.package_call);process_record_abort(call_worker_task.tid);call_host_current=NULL;
}
static int mutation_dispatch(ext4_engine_t *e){
    if(!pc_view(thread_current()))return -VFS_EACCES;
    if(!pc_commit_begin(thread_current()))return -VFS_EINTR;
    int result=call_budget_dispatch(e);
    if(!result&&mutation_current->namespace)call_namespace_commits++;
    pc_commit_end(thread_current());return result;
}
static void call_cancel_package(pw_key owner,uint64_t session,uint32_t ino){
    unsigned before=writes_seen,refs=pw_references(owner);pm_key key;mutation_call call;
    assert(!pm_package(mutation_mount,owner,session,&key)&&!mutation_enter(key,&call));
    pc_cancel_pid(thread_current()->tid);
    assert(mutation_write(ino,'!',1)==-VFS_EINTR&&writes_seen==before);
    mutation_leave(&call);assert(pw_references(owner)==refs&&!pc_count());
}
enum call_ns_op {CALL_CREATE,CALL_MKDIR,CALL_RENAME,CALL_REMOVE,CALL_TRUNCATE,CALL_WRITE};
static bool call_namespace_orphan(mutation_call *call){
    spin_debug_assert_held(&e4_lock);ext4_engine_t *e=mounted->engine;uint32_t ino=call->namespace_ino;
    if(!call->namespace||call->admission->kind!=PM_ORDINARY||!ino||ino==life_target||
       e->orphan_count!=1||e->orphans[0]!=ino||e4_ns_anchor(mounted,ino)||e4_account_inode(mounted,ino))return false;
    for(unsigned i=0;i<4;i++)if(ino==ids[i])return false;
    uint8_t *raw=raw_inode(mounted,ino);return raw&&e4_u32(raw+100)==call->namespace_generation;
}
/* Trusted finite workbench routes, not an arbitrary pathname confinement API.
 * Every component is checked; retained sandbox identities remain owned. No
 * sandbox-root move, symlink creation, external import or hard-link is exposed. */
static bool call_path(const char *p){
    if(!p||strncmp(p,"/work/",6))return false;
    const char *part=p+6;if(!((part[0]=='a'||part[0]=='b')&&part[1]=='/'))return false;
    part+=2;if(!*part)return false;
    while(*part){
        unsigned n=0;while(part[n]&&part[n]!='/'){
            if(n>=VFS_MAX_NAME-1||!((part[n]>='a'&&part[n]<='z')||(part[n]>='0'&&part[n]<='9')||part[n]=='-'))return false;
            n++;
        }
        if(!n)return false;
        part+=n;if(*part)part++;if(!*part&&part[-1]=='/')return false;
    }return true;
}
static bool call_parent_inside(const char *path,const creds_t *actor){
    char parent[VFS_MAX_PATH];size_t n=strlen(path);if(n>=sizeof parent)return false;
    memcpy(parent,path,n+1);char *last=NULL;for(char *p=parent;*p;p++)if(*p=='/')last=p;
    if(!last)return false;
    *last=0;int error;
    vfs_node_t *node=vfs_lookup_creds(parent,actor,&error);if(!node)return false;
    uint64_t irq=spin_lock_irqsave(&e4_lock);bool inside=false;vfs_node_t *at=node;
    for(unsigned depth=0;at&&depth<VFS_MAX_PATH;depth++,at=at->parent){
        if(at->permission_actor!=e4_permission_actor||!at->fs_private||((e4_node_t *)at)->inode.fs!=mounted)break;
        if(at==call_sandbox[0]){inside=true;break;}
    }
    spin_unlock_irqrestore(&e4_lock,irq);vfs_node_put(node);return inside;
}
static int64_t call_namespace(enum call_ns_op op,const char *from,const char *to){
    mutation_call *call=mutation_current;
    if(!call||call->admission->kind!=PM_ORDINARY||!call_path(from)||(to&&!call_path(to)))return -VFS_EACCES;
    creds_t actor;creds_init_root(&actor);
    if(!call_parent_inside(from,&actor)||(to&&!call_parent_inside(to,&actor)))return -VFS_EACCES;
    vfs_node_t *target=NULL;call->namespace_ino=call->namespace_generation=0;
    if(op==CALL_TRUNCATE||op==CALL_REMOVE){
        int error;target=vfs_lookup_creds(from,&actor,&error);if(!target)return error;
        uint64_t irq=spin_lock_irqsave(&e4_lock);
        e4_inode_t *in=target->fs_private;assert(in&&in->fs==mounted);
        call->namespace_ino=in->ino;call->namespace_generation=in->generation;
        spin_unlock_irqrestore(&e4_lock,irq);
    }
    call->namespace=true;int error=0;int64_t r;
    if(op==CALL_CREATE){
        file_t *f=vfs_open_mode_creds(from,VFS_O_RDWR|VFS_O_CREAT|VFS_O_EXCL,0600,&actor,&error);
        r=f?vfs_close(f):error;
    }else if(op==CALL_MKDIR)r=vfs_mkdir_creds(from,0700,&actor);
    else if(op==CALL_RENAME)r=vfs_rename_creds(from,to,&actor);
    else if(op==CALL_REMOVE)r=vfs_unlink_creds(from,&actor);
    else if(op==CALL_TRUNCATE){
        file_t *f=vfs_open_mode_creds(from,VFS_O_RDWR|VFS_O_TRUNC,0600,&actor,&error);r=f?vfs_close(f):error;
    }else if(op==CALL_WRITE){
        file_t *f=vfs_open_creds(from,VFS_O_WRONLY,&actor,&error);
        r=f?vfs_write_creds(f,"namespace-data\n",15,&actor):error;if(f)assert(!vfs_close(f));
    }else r=-VFS_EINVAL;
    call->namespace=false;if(target)vfs_node_put(target);return r;
}
static void call_run(enum call_ns_op op,const char *a,const char *b,int64_t expected){
    pm_key key;mutation_call call;assert(!pm_ordinary(mutation_mount,&key)&&!mutation_enter(key,&call));
    int64_t result=call_namespace(op,a,b);
    if(result!=expected)fprintf(stderr,"namespace op=%u from=%s to=%s got=%lld expected=%lld\n",op,a,b?b:"-",(long long)result,(long long)expected);
    assert(result==expected);mutation_leave(&call);
}
static pthread_barrier_t call_ns_started,call_ns_release;
static void *call_ns_worker(void *arg){
    unsigned id=*(unsigned *)arg;call_worker_enter(200+id);pm_key key;mutation_call call;
    assert(!pm_ordinary(mutation_mount,&key)&&!mutation_enter(key,&call));
    pthread_barrier_wait(&call_ns_started);pthread_barrier_wait(&call_ns_release);
    const char *path=id?"/work/b/parallel1":"/work/a/parallel0";
    assert(!call_namespace(CALL_CREATE,path,NULL));assert(call_namespace(CALL_WRITE,path,NULL)==15);
    mutation_leave(&call);call_worker_exit();return NULL;
}
static void call_namespace_checks(void){
    assert(!mutation_current&&!call_boot_task.package_call);
    unsigned before=writes_seen;assert(call_namespace(CALL_CREATE,"/work/a/no-context",NULL)==-VFS_EACCES&&writes_seen==before);
    call_run(CALL_CREATE,"/etc/forbidden",NULL,-VFS_EACCES);
    call_run(CALL_CREATE,"/work/a/../escape",NULL,-VFS_EACCES);
    call_run(CALL_CREATE,"/pkg/forbidden",NULL,-VFS_EACCES);
    assert(writes_seen==before);
    pm_key oom_key;mutation_call oom_call;unsigned count,pins;bool pending;
    assert(!pm_account(mutation_mount,&oom_key));fail_alloc=0;
    assert(mutation_enter(oom_key,&oom_call)==-VFS_ENOMEM);fail_alloc=-1;
    pm_counts(&count,&pins,&pending);assert(!count&&!pins&&!pending&&!pc_count()&&writes_seen==before);
    /* Cancellation precedes semantic commit: actual create/mkdir planners run,
     * reject EINTR, leave no output object and perform no device writes. */
    for(unsigned i=0;i<2;i++){
        pm_key key;mutation_call call;assert(!pm_ordinary(mutation_mount,&key)&&!mutation_enter(key,&call));
        pc_cancel_pid(call_boot_task.tid);assert(pc_cancelled(thread_current()));before=writes_seen;
        const char *path=i?"/work/a/cancelled-dir":"/work/a/cancelled-file";
        assert(call_namespace(i?CALL_MKDIR:CALL_CREATE,path,NULL)==-VFS_EINTR&&writes_seen==before);
        mutation_leave(&call);int error=0;vfs_node_t *n=vfs_lookup_creds(path,&call_boot_task.creds,&error);
        assert(!n&&error==-VFS_ENOENT);
    }
    call_run(CALL_CREATE,"/work/a/file",NULL,0);
    call_run(CALL_WRITE,"/work/a/file",NULL,15);
    call_run(CALL_RENAME,"/work/a/file","/work/b/kept",0);
    call_run(CALL_CREATE,"/work/a/victim",NULL,0);call_run(CALL_WRITE,"/work/a/victim",NULL,15);before=writes_seen;
    call_run(CALL_RENAME,"/work/b/kept","/work/a/victim",-VFS_EEXIST);assert(writes_seen==before);
    call_run(CALL_TRUNCATE,"/work/a/victim",NULL,0);call_run(CALL_REMOVE,"/work/a/victim",NULL,0);
    call_run(CALL_MKDIR,"/work/a/sub",NULL,0);call_run(CALL_CREATE,"/work/a/sub/leaf",NULL,0);
    call_run(CALL_RENAME,"/work/a/sub","/work/b/sub",0);
    call_run(CALL_REMOVE,"/work/b/sub/leaf",NULL,0);call_run(CALL_REMOVE,"/work/b/sub",NULL,0);
    assert(!pthread_barrier_init(&call_ns_started,NULL,3)&&!pthread_barrier_init(&call_ns_release,NULL,3));
    unsigned ids[2]={0,1};pthread_t workers[2];for(unsigned i=0;i<2;i++)assert(!pthread_create(&workers[i],NULL,call_ns_worker,&ids[i]));
    pthread_barrier_wait(&call_ns_started);pm_key priority;assert(!pm_account(mutation_mount,&priority)&&!pm_pin(priority,mutation_mount));
    pthread_barrier_wait(&call_ns_release);for(unsigned i=0;i<2;i++)assert(!pthread_join(workers[i],NULL));
    assert(pm_finish(priority));assert(!pthread_barrier_destroy(&call_ns_started)&&!pthread_barrier_destroy(&call_ns_release));
    /* Real lifecycle notification cancels an owned binding before TCB teardown.
     * The owner acknowledges/unwinds; neither remote exit nor reaper frees it. */
    tcb_t dying;integration_birth(&dying,7000);tcb_t *previous=call_host_current;call_host_current=&dying;
    pm_key key;mutation_call call;assert(!pm_ordinary(mutation_mount,&key)&&!mutation_enter(key,&call));
    (void)process_record_exit_accounted_times(dying.tid,0,0,0,0,0);before=writes_seen;
    assert(pc_cancelled(&dying)&&call_namespace(CALL_CREATE,"/work/a/dead",NULL)==-VFS_EINTR&&writes_seen==before);
    mutation_leave(&call);assert(!dying.package_call);process_record_forget(dying.tid);call_host_current=previous;
    for(unsigned i=0;i<3;i++)vfs_node_put(call_sandbox[i]);
    printf("PASS namespace create mkdir write truncate file-rename directory-move unlink rmdir cancellation=3 concurrent=2 commits=%u\n",call_namespace_commits);
    process_record_abort(call_boot_task.tid);call_host_current=NULL;
}
