/* Included only by a generated, copied-source integration translation unit. */
static bool integrated,account_lane,tagged_lane,readers_excluded;
static unsigned integration_gate;
static uint64_t integration_compete_floor[2];
static int e4_semantic_commit(ext4_engine_t *e){
    if(!integrated)return pg_life_core_commit(e);
    if(account_lane)return pg_capacity_commit(e);
    if(tagged_lane)return pg_tagged_commit(e);
    pg_view v;if(life_read(e->fs,&v))return -VFS_EIO;
    uint8_t *sb=e4_super(e);if(!sb)return e->error;
    for(unsigned i=0;i<2;i++){
        uint64_t f=e4_u32(sb+12+4*i),a0=i?EXT4_ACCOUNT_RESERVE_INODES:(EXT4_ACCOUNT_RESERVE_BYTES+e->fs->bs-1)/e->fs->bs;
        uint64_t r=capacity_left[i]>a0?capacity_left[i]:a0;
        if(f<v.remaining[i]||f-v.remaining[i]<r)return -VFS_ENOSPC;
    }
    return pg_life_core_commit(e);
}
static bool pg_integration_pin_deny(struct vfs_node *node){
    if(!readers_excluded||!node||!node->fs_private||!node->parent||!node->parent->fs_private||
       node->parent->permission_actor!=e4_permission_actor)return false;
    e4_node_t *n=(e4_node_t *)node;ext4_mount_t *fs=n->inode.fs;
    if(fs!=mounted)return false;
    uint32_t parent=((e4_node_t *)node->parent)->inode.ino;
    for(unsigned role=0;role<ACCOUNT_ROLE_COUNT;role++){
        bool priv=(role>=3&&role<=5)||role>=9;
        const char *name=e4_ac_paths[role];for(const char *p=name;*p;p++)if(*p=='/')name=p+1;
        if(parent==fs->account_namespace_anchors[priv?4:0]&&!strcmp(node->name,name))return true;
    }return false;
}
static int pg_integration_headroom(void *context){
    if(context!=mounted)return -VFS_EIO;
    uint64_t lock=spin_lock_irqsave(&e4_lock);readers_excluded=true;pg_view v;
    int r=life_read(mounted,&v);spin_unlock_irqrestore(&e4_lock,lock);if(r)return -VFS_EIO;
    memcpy(capacity_escrow,v.remaining,sizeof capacity_escrow);return capacity_admit();
}
static int pg_integration_storage(void *context,account_io_t *io){
    backend=*io;int r=pg_integration_headroom(context);if(r)return r;
    io->write=capacity_write;io->remove=capacity_remove;io->rename=capacity_rename;return 0;
}
static void pg_integration_relax(void){
    uint64_t lock=spin_lock_irqsave(&e4_lock);readers_excluded=writer!=0||!enabled;
    spin_unlock_irqrestore(&e4_lock,lock);
}
/* These hooks are inserted after release of the actual process registry lock. */
typedef struct {uint64_t pid;pw_key key;unsigned phase;} integration_owner;
static integration_owner owner_map[PROCESS_CAPACITY];
static spinlock_t owner_map_guard=SPINLOCK_RANKED(1,"private owner map");
static pw_key integration_owner_key(uint64_t pid){
    uint64_t lock=spin_lock_irqsave(&owner_map_guard);pw_key key={0};
    for(unsigned i=0;i<PROCESS_CAPACITY;i++)if(owner_map[i].pid==pid&&owner_map[i].phase==2)key=owner_map[i].key;
    spin_unlock_irqrestore(&owner_map_guard,lock);return key;
}
void pg_integration_birth(uint64_t pid){
    uint64_t lock=spin_lock_irqsave(&owner_map_guard);unsigned slot=PROCESS_CAPACITY;
    for(unsigned i=0;i<PROCESS_CAPACITY;i++){
        if(owner_map[i].pid==pid){spin_unlock_irqrestore(&owner_map_guard,lock);return;}
        if(!owner_map[i].phase)slot=i;
    }
    if(slot==PROCESS_CAPACITY){spin_unlock_irqrestore(&owner_map_guard,lock);return;}
    owner_map[slot]=(integration_owner){.pid=pid,.phase=1};spin_unlock_irqrestore(&owner_map_guard,lock);
    pw_key key={0};bool registered=pw_register(pid,&key);
    lock=spin_lock_irqsave(&owner_map_guard);bool dead=owner_map[slot].phase==3;
    if(registered&&!dead){owner_map[slot].key=key;owner_map[slot].phase=2;}
    else owner_map[slot]=(integration_owner){0};
    spin_unlock_irqrestore(&owner_map_guard,lock);if(registered&&dead)(void)pw_detach(key);
}
void pg_integration_detach(uint64_t pid){
    uint64_t lock=spin_lock_irqsave(&owner_map_guard);pw_key key={0};
    for(unsigned i=0;i<PROCESS_CAPACITY;i++)if(owner_map[i].pid==pid){
        if(owner_map[i].phase==1)owner_map[i].phase=3;
        else if(owner_map[i].phase==2){key=owner_map[i].key;owner_map[i]=(integration_owner){0};}
    }
    spin_unlock_irqrestore(&owner_map_guard,lock);if(key.generation)(void)pw_detach(key);
}
typedef struct {pw_key owner;uint64_t session;} integration_lease;
static integration_lease lease;
static vfs_node_t lease_node;
static void integration_closed(vfs_node_t *node){
    integration_lease *l=node->fs_private;(void)pw_close(l->owner,l->session);
}
static bool integration_idle(void){
    uint64_t lock=spin_lock_irqsave(&e4_lock);pg_view v;
    bool ok=!life_read(mounted,&v)&&v.phase==PG_IDLE;
    spin_unlock_irqrestore(&e4_lock,lock);return ok;
}
static int integration_acquire(tcb_t *proc){
    if(__atomic_exchange_n(&integration_gate,1,__ATOMIC_ACQUIRE))return -1;
    int fd=-1;pw_key owner=integration_owner_key(proc->tid);uint64_t session;
    if(!enabled||!integration_idle()||!pw_begin(owner,true,&session))goto done;
    pw_key operation;if(!pw_enter(owner,session,&operation)){(void)pw_close(owner,session);goto done;}
    uint64_t r[2]={0};int result=capacity_snapshot(r);
    memcpy(integration_compete_floor,r,sizeof r);
    if(!result)result=compete_admit(mounted,u64(ledger+24),'I');
    if(result){(void)pw_close(owner,session);assert(pw_complete(operation));goto done;}
    lease=(integration_lease){owner,session};lease_node=(vfs_node_t){.type=VFS_STREAM,.is_stream=true,.close=integration_closed,.fs_private=&lease};
    file_t *file=kcalloc(1,sizeof(*file));
    if(!file){(void)pw_close(owner,session);assert(pw_complete(operation));goto done;}
    file->node=&lease_node;file->ref_count=1;file->exclusive_owner=true;fd=fd_alloc(proc,file);
    if(fd<0){kfree(file);(void)pw_close(owner,session);assert(pw_complete(operation));goto done;}
    proc->fd_flags[fd]=FD_FLAG_CLOEXEC;assert(pw_complete(operation));
done:__atomic_store_n(&integration_gate,0,__ATOMIC_RELEASE);return fd;
}
static int64_t integration_control(tcb_t *proc,unsigned op,account_request_t *req){
    if(__atomic_exchange_n(&integration_gate,1,__ATOMIC_ACQUIRE))return SYSCALL_EAGAIN;
    account_lane=capacity_active=true;
    int64_t r=accounts_control(op,(uintptr_t)req,sizeof(*req),&proc->creds,proc->tid);
    account_lane=capacity_active=false;pg_integration_relax();__atomic_store_n(&integration_gate,0,__ATOMIC_RELEASE);return r;
}
static void integration_birth(tcb_t *proc,uint64_t pid){
    memset(proc,0,sizeof(*proc));proc->tid=pid;proc->is_user=true;creds_init_root(&proc->creds);
    assert(!process_record_begin(pid,0,false,0,0));assert(process_record_bind_creds(pid,&proc->creds));process_record_commit(pid);
    assert(integration_owner_key(pid).generation);
}
static vfs_node_t *integration_create(vfs_node_t *parent,const char *name,bool dir){
    int error;creds_t root;creds_init_root(&root);vfs_create_attrs_t attrs={.mode=dir?0700:0600,.uid=0,.gid=0};
    vfs_node_t *node=e4_jcreate_common(parent,name,dir?VFS_DIRECTORY:VFS_FILE,&error,true,&attrs,&root);assert(node);return node;
}
static void integration_seed(void){
    vfs_node_t *pkg=integration_create(ext4_prepared_root(mounted),"pkg",true);
    const char *names[]={"enrollment.v1","grant.v1","ownership.v1","transaction.v1"};
    for(unsigned i=0;i<4;i++){vfs_node_t *node=integration_create(pkg,names[i],i==3);ids[i]=((e4_node_t *)node)->inode.ino;vfs_node_put(node);}vfs_node_put(pkg);
    integration_seed_bytes();
    uint8_t *buffers[]={marker,ledger,table};size_t sizes[]={sizeof marker,sizeof ledger,sizeof table};
    for(unsigned i=0;i<3;i++){
        uint64_t lock=spin_lock_irqsave(&e4_lock),offset=0;
        assert(e4_file_write_actor(mounted->engine,ids[i],&offset,false,buffers[i],sizes[i],NULL)==(int64_t)sizes[i]);
        spin_unlock_irqrestore(&e4_lock,lock);
    }assert(!ext4_sync(mounted));
}
static void integration_fixture_ledger(const uint8_t *bytes){
    uint64_t lock=spin_lock_irqsave(&e4_lock);assert(!e4_namespace_begin(mounted->engine));
    memcpy(ledger,bytes,sizeof ledger);assert(!life_stage(mounted->engine,1,ledger,sizeof ledger));
    mounted->engine->file_planning=false;assert(!pg_life_core_commit(mounted->engine));e4_semantic_invalidate(mounted->engine);
    spin_unlock_irqrestore(&e4_lock,lock);
}
static const char *integration_new[3]={"root:x:0:0:root2:/root:/bin/shell\n","root:x:0:\n","root:!:::::::\n"};
static void integration_publish(void){
    const char *names[]={"/etc","/etc/passwd","/etc/group","/etc/shadow"};vfs_node_t *nodes[4];
    for(unsigned i=0;i<4;i++){int error;nodes[i]=e4_ac_lookup(mounted,names[i],&error);assert(nodes[i]);}
    assert(!ext4_root_lock_accounts(mounted,nodes));for(unsigned i=0;i<4;i++)vfs_node_put(nodes[i]);assert(!ext4_publish_root(mounted));
}
int main(int argc,char **argv){
    assert(argc==4);setvbuf(stdout,NULL,_IONBF,0);unsigned sector=(unsigned)strtoul(argv[2],NULL,10);
    image=fopen(argv[1],"r+b");assert(image&&!fseeko(image,0,SEEK_END));size_t bytes=ftello(image);
    disk_bytes=malloc(bytes);assert(disk_bytes&&!fseeko(image,0,SEEK_SET)&&fread(disk_bytes,1,bytes,image)==bytes);
    block_dev_t dev={.sector_size=sector,.sector_count=bytes/sector,.read_sector=rd,.write_sector=wr,.flush=flush};
    vfs_init();assert(!ext4_prepare_root(&dev,true,&mounted));ext4_account_namespace_report_t report;
    assert(!ext4_root_account_io(mounted,&backend));assert(!ext4_root_account_namespace_admit(mounted,&report));
    life_path="/pkg/transaction.v1/unit";
    bool resume=!strncmp(argv[3],"resume",6);char context[1024];assert(snprintf(context,sizeof context,"%s.ctx",argv[1])>0);
    if(!resume){integration_seed();FILE *c=fopen(context,"wb");assert(c);assert(fwrite(&binding,sizeof binding,1,c)==1&&fwrite(ids,sizeof ids,1,c)==1&&fwrite(&marker_generation,sizeof marker_generation,1,c)==1);assert(!fclose(c));}
    else {FILE *c=fopen(context,"rb");assert(c&&fread(&binding,sizeof binding,1,c)==1&&fread(ids,sizeof ids,1,c)==1&&fread(&marker_generation,sizeof marker_generation,1,c)==1);assert(!fclose(c));}
    uint64_t allocated;uint64_t fixed_lock=spin_lock_irqsave(&e4_lock);
    for(unsigned i=0;i<3;i++)assert(!object(mounted,ids[i],i?binding.generation[i-1]:marker_generation,i==0?512:i==1?1024:8192,false,true,&allocated));
    assert(!object(mounted,ids[3],binding.generation[2],mounted->bs,true,true,&allocated));spin_unlock_irqrestore(&e4_lock,fixed_lock);
    life_active=integrated=true;uint64_t lock=spin_lock_irqsave(&e4_lock);assert(!pg_life_reconstruct(mounted->engine));spin_unlock_irqrestore(&e4_lock,lock);
    account_lane=capacity_active=true;assert(!accounts_prepare(mounted));account_lane=capacity_active=false;pg_integration_relax();
    integration_publish();pg_view view;assert(!life_read(mounted,&view));tcb_t parent,child;
    integration_birth(&parent,100);integration_birth(&child,101);
    if(!resume){
        int fd=integration_acquire(&parent);assert(fd>=0);uint64_t session=lease.session;pw_key owner=lease.owner,operation;
        assert(fd_dup(&parent,fd)==SYSCALL_EBADF&&fd_dup2(&parent,fd,fd)==SYSCALL_EBADF);
        fd_clone_table(&parent,&child);assert(!child.fd_table[fd]);assert(pw_enter(owner,session,&operation));
        (void)process_record_exit_accounted_times(parent.tid,0,0,0,0,0);fd_close_all(&parent);assert(pw_references(owner)==2);
        tagged_lane=true;assert(!life_create(mounted));file_t *unit=vfs_open(life_path,VFS_O_RDWR);assert(unit);
        memset(payload,'P',mounted->bs);assert(vfs_write(unit,payload,mounted->bs)==(int64_t)mounted->bs);assert(!vfs_close(unit));tagged_lane=false;
        assert(pw_complete(operation)&&!pw_references(owner));process_record_forget(parent.tid);
    }
    assert(!life_read(mounted,&view)&&view.phase==PG_RESERVED&&view.remaining[0]==7&&view.remaining[1]==0);
    unsigned before=writes_seen;assert(integration_acquire(&child)<0&&writes_seen==before);
    file_t *reader=vfs_open("/etc/passwd",VFS_O_RDONLY);assert(reader);
    account_request_t request={.size=64,.version=1};assert(integration_control(&child,ACCOUNT_BEGIN,&request)==SYSCALL_EAGAIN&&writes_seen==before);assert(!vfs_close(reader));
    assert(!integration_control(&child,ACCOUNT_BEGIN,&request));int error;
    assert(!vfs_open_creds("/etc/passwd",VFS_O_RDONLY,&child.creds,&error)&&error==-VFS_EAGAIN);
    request.passwd=(uintptr_t)integration_new[0];request.group=(uintptr_t)integration_new[1];request.shadow=(uintptr_t)integration_new[2];
    request.passwd_bytes=strlen(integration_new[0]);request.group_bytes=strlen(integration_new[1]);request.shadow_bytes=strlen(integration_new[2]);
    uint8_t saved[1024],bad[1024];assert(!life_read(mounted,&view));memcpy(saved,ledger,sizeof saved);memcpy(bad,saved,sizeof bad);bad[992]^=1;
    integration_fixture_ledger(bad);before=writes_seen;uint64_t old_generation=request.generation;
    assert(integration_control(&child,ACCOUNT_UPDATE,&request)==SYSCALL_EIO&&writes_seen==before&&request.generation==old_generation);
    integration_fixture_ledger(saved);
    bool interrupt=!strcmp(argv[3],"interrupt");if(interrupt)interrupt_role=ACCOUNT_TEMP_GROUP;
    int64_t result=integration_control(&child,ACCOUNT_UPDATE,&request);
    if(interrupt)assert(result==SYSCALL_EIO&&interrupted==1);
    else {assert(!result);request.passwd=request.group=request.shadow=0;request.passwd_bytes=request.group_bytes=request.shadow_bytes=0;assert(!integration_control(&child,ACCOUNT_FINISH,&request));}
    assert(!life_read(mounted,&view)&&view.remaining[0]==7&&view.remaining[1]==0&&!memcmp(ledger,saved,sizeof ledger));
    if(!interrupt&&strcmp(argv[3],"resume-keep")){
        tagged_lane=true;assert(!vfs_unlink(life_path));tagged_lane=false;assert(!idle_release(mounted));
        int fd=integration_acquire(&child);assert(fd>=0);pw_key owner=lease.owner;uint64_t session=lease.session;
        fd_close_cloexec(&child);pw_key operation;assert(!pw_enter(owner,session,&operation)&&pw_references(owner)==1);
        /* CLOEXEC cancels the volatile owner but preserves the new disk grant. */
        assert(!life_read(mounted,&view)&&view.phase==PG_RESERVED&&view.remaining[0]==8&&view.remaining[1]==1);
        assert(!idle_release(mounted));process_record_abort(child.tid);assert(!pw_references(owner));
    }else {fd_close_all(&child);(void)process_record_exit(child.tid,0);process_record_forget(child.tid);}
    if(resume)process_record_abort(parent.tid);
    assert(!ext4_freeze_and_sync(mounted));assert(!fclose(image));free(disk_bytes);
    printf("PASS integration block=%u sector=%u mode=%s service-generation=%llu capacity-commits=%u\n",mounted->bs,sector,argv[3],(unsigned long long)generation,capacity_commits);return 0;
}
