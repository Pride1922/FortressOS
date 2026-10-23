/* Private trusted-origin workbench. The sidecar is supplied by the fixture,
 * never learned from an orphan's mode, a user pathname, or untrusted media.
 * This is NOT authenticated production provenance or a new on-disk ABI. */
typedef struct {
    uint32_t ino,generation,role,parent,count,entries;
    uint64_t size;
    uint8_t uuid[16];
    e4_range_t ranges[32];
    e4_extent_t extents[16];
} account_orphan_origin;
static account_orphan_origin account_origin;
static ext4_engine_t *account_preview_engine;
static uint64_t account_orphan_blocks,account_orphan_inodes;
static unsigned account_orphan_certificates;
static char account_new_files[3][ACCOUNT_FILE_MAX+1];
static void account_new_profile(void){
    integration_new[1]="root:x:0:root\n";integration_new[2]="root:*:::::::\n";
    for(unsigned i=0;i<3;i++){
        size_t n=strlen(integration_new[i]);memcpy(account_new_files[i],integration_new[i],n);
        while(n<ACCOUNT_FILE_MAX){
            size_t take=ACCOUNT_FILE_MAX-n;if(take>513)take=513;
            memset(account_new_files[i]+n,'p',take);
            if(take>1)account_new_files[i][n]='#';
            account_new_files[i][n+take-1]='\n';n+=take;
        }
        integration_new[i]=account_new_files[i];
    }
}

static int account_orphan_measure(ext4_engine_t *e,uint64_t *blocks){
    ext4_mount_t *fs=e->fs;account_orphan_origin *o=&account_origin;
    if(e->orphan_count!=1||!o->ino||e->orphans[0]!=o->ino||o->role>=ACCOUNT_ROLE_COUNT||
       o->size>ACCOUNT_FILE_MAX||o->count>32||o->entries>16)return -VFS_EIO;
    for(unsigned i=0;i<4;i++)if(o->ino==ids[i])return -VFS_EIO;
    pg_view v;if(life_read(fs,&v))return -VFS_EIO;
    if(e4_u16(table+22)&&o->ino==e4_u32(table+260))return -VFS_EIO;
    uint8_t sb[1024];if(!e4_bytes(fs,1024,sb,sizeof sb)||memcmp(sb+104,o->uuid,16))return -VFS_EIO;
    e4_inode_t in;fs->orphan_access=o->ino;int r=e4_inode(fs,o->ino,&in);fs->orphan_access=0;
    if(r||in.generation!=o->generation||in.size!=o->size||in.uid||in.gid||
       in.full_mode!=(VFS_S_IFREG|e4_ac_mode(o->role)))return -VFS_EIO;
    uint8_t *raw=raw_inode(fs,o->ino);if(!raw||e4_u16(raw+26))return -VFS_EIO;
    uint64_t sectors=e4_u32(raw+28);fs->rw_ino=0;r=e4_mapping(&in);if(r)return r;
    uint64_t logical=0,total=0;
    for(unsigned i=0;i<in.map->count;i++){
        e4_extent_t x=in.map->entries[i];
        if(x.unwritten||x.logical!=logical)return -VFS_EIO;
        if(i>=o->entries||x.logical!=o->extents[i].logical||x.physical!=o->extents[i].physical||
           x.len>o->extents[i].len)return -VFS_EIO;
        logical+=x.len;
    }
    if(logical>(in.size+fs->bs-1)/fs->bs)return -VFS_EIO;
    for(unsigned i=0;i<fs->physical_count;i++){
        e4_range_t x=fs->physical[i];bool contained=false;
        for(unsigned j=0;j<o->count;j++){
            e4_range_t old=o->ranges[j];
            if(x.lo>=old.lo&&(uint64_t)x.lo+x.len<=(uint64_t)old.lo+old.len)contained=true;
        }
        if(!contained)return -VFS_EIO;
        total+=x.len;
    }
    uint64_t d=(ACCOUNT_FILE_MAX+fs->bs-1)/fs->bs,max=o->role>=ACCOUNT_INTENT_TEMP?1:d+(d>4);
    if(total>max||total*(fs->bs/512)!=sectors)return -VFS_EIO;
    *blocks=total;return 0;
}
static bool account_orphan_permit(ext4_engine_t *e){
    account_preview_engine=e;
    if(!e->orphan_count)return !e->orphan_recovery_required;
    uint64_t blocks;return !account_orphan_measure(e,&blocks);
}
static int account_orphan_capacity(ext4_mount_t *fs,e4_ns_workspace_t *w,uint64_t left[2]){
    ext4_engine_t *e=fs->engine?fs->engine:account_preview_engine;
    if(!e||e->fs!=fs)return -VFS_EIO;
    if(!e->orphan_count)return 0;
    uint64_t blocks;int r=account_orphan_measure(e,&blocks);if(r)return r;
    account_orphan_origin *o=&account_origin;
    bool private_role=(o->role>=3&&o->role<=5)||o->role>=9;
    if(w->roles[o->role]||o->parent!=w->anchors[private_role?4:0])return -VFS_EIO;
    for(unsigned i=0;i<ACCOUNT_ROLE_COUNT;i++)if(w->roles[i]==o->ino)return -VFS_EIO;
    if(blocks>left[0]||!left[1])return -VFS_EIO;
    left[0]-=blocks;left[1]--;
    account_orphan_blocks=blocks;account_orphan_inodes=1;account_orphan_certificates++;
    return 0;
}
static void account_origin_file(const char *path,bool read){
    char name[1024];assert(snprintf(name,sizeof name,"%s.origin",path)>0);
    FILE *f=fopen(name,read?"rb":"wb");assert(f);
    assert((read?fread(&account_origin,sizeof account_origin,1,f):fwrite(&account_origin,sizeof account_origin,1,f))==1);
    if(read)assert(fgetc(f)==EOF);
    assert(!fclose(f));
}
static int account_orphan_remove(void *p,unsigned role){
    int r=mutation_headroom(p);if(r)return r;
    memset(&account_origin,0,sizeof account_origin);
    int error;vfs_node_t *node=e4_ac_lookup(p,e4_ac_paths[role],&error);
    if(node){
        uint64_t irq=spin_lock_irqsave(&e4_lock);e4_inode_t in;
        assert(!e4_inode(mounted,((e4_node_t *)node)->inode.ino,&in));
        mounted->rw_ino=0;assert(!e4_mapping(&in)&&mounted->physical_count<=32&&in.map->count<=16);
        account_origin=(account_orphan_origin){.ino=in.ino,.generation=in.generation,.role=role,.size=in.size,
            .parent=((e4_node_t *)node->parent)->inode.ino,.count=mounted->physical_count,.entries=in.map->count};
        memcpy(account_origin.ranges,mounted->physical,mounted->physical_count*sizeof(e4_range_t));
        memcpy(account_origin.extents,in.map->entries,in.map->count*sizeof(e4_extent_t));
        uint8_t sb[1024];assert(e4_bytes(mounted,1024,sb,sizeof sb));memcpy(account_origin.uuid,sb+104,16);
        spin_unlock_irqrestore(&e4_lock,irq);vfs_node_put(node);
    }else assert(error==-VFS_ENOENT);
    unsigned before=writes_seen;return account_orphan_after(mutation_after(backend.remove(p,role)),before);
}
static int account_orphan_after(int result,unsigned before){
    (void)before;
    if(pc_cancelled(thread_current())){
        assert(writes_seen==account_cancel_writes);
        if(mounted->engine->orphan_count){
            /* Recompute independently after unwinding the planner: reclaimed
             * blocks change R only when real allocation counters changed. */
            mutation_call *call=mutation_current;uint64_t r[2];
            uint64_t irq=spin_lock_irqsave(&e4_lock);
            memset(call->workspace,0,sizeof(*call->workspace));
            memcpy(call->workspace->anchors,mounted->account_namespace_anchors,sizeof call->workspace->anchors);
            assert(!capacity_certificate(mounted,call->workspace,r)&&!memcmp(r,call->left,sizeof r));
            spin_unlock_irqrestore(&e4_lock,irq);
        }
    }
    return result;
}
int main(int argc,char **argv){
    assert(argc==4);setvbuf(stdout,NULL,_IONBF,0);
    if(!strcmp(argv[3],"seed"))return account_callback_previous_main(argc,argv);
    account_new_profile();
    (void)account_cancel_at;(void)account_cancel_callbacks;(void)account_cancel_armed;
    unsigned sector=(unsigned)strtoul(argv[2],NULL,10);
    image=fopen(argv[1],"r+b");assert(image&&!fseeko(image,0,SEEK_END));size_t size=ftello(image);
    disk_bytes=malloc(size);assert(disk_bytes&&!fseeko(image,0,SEEK_SET)&&fread(disk_bytes,1,size,image)==size);
    block_dev_t dev={.sector_size=sector,.sector_count=size/sector,.read_sector=rd,.write_sector=wr,.flush=flush};
    cancel_context(argv[1],true);integration_pre_required=true;
    bool reject=!strncmp(argv[3],"reject",6),recover=!strncmp(argv[3],"recover",7);
    if(recover||reject)account_origin_file(argv[1],true);
    if(!strcmp(argv[3],"reject"))binding.marker_sha256[0]^=1;
    if(!strcmp(argv[3],"reject-generation"))account_origin.generation^=1;
    if(!strcmp(argv[3],"reject-unknown"))account_origin.ino=0;
    if(!strcmp(argv[3],"reject-role"))account_origin.role=ACCOUNT_ROLE_COUNT;
    if(!strcmp(argv[3],"reject-parent"))account_origin.parent=2;
    if(!strcmp(argv[3],"reject-map"))account_origin.count=0;
    if(!strcmp(argv[3],"reject-uuid"))account_origin.uuid[0]^=1;
    if(!strcmp(argv[3],"reject-package"))account_origin.ino=ids[0];
    vfs_init();const char *stage;int prepared=ext4_prepare_root_diagnose(&dev,true,&mounted,&stage);
    if(reject){
        assert(prepared&&!mounted&&!writes_seen&&!integration_flushes&&integration_pre_calls==1&&!e4_engine_busy);
        assert(!fclose(image));free(disk_bytes);puts("PASS account orphan rejection zero-write-flush");return 0;
    }
    if(prepared)fprintf(stderr,"account orphan constructor stage=%s status=%d\n",stage,prepared);
    assert(!prepared&&integration_pre_calls==1&&integration_capacity_calls==1);
    uint64_t occupied=account_orphan_blocks,occupied_inodes=account_orphan_inodes;
    ext4_account_namespace_report_t report;
    assert(!ext4_root_account_io(mounted,&backend)&&!ext4_root_account_namespace_admit(mounted,&report));
    uint64_t post[2];assert(!capacity_snapshot(post));
    assert(post[0]==integration_pre_capacity[0]+occupied&&post[1]==integration_pre_capacity[1]+occupied_inodes);
    integration_birth(&call_boot_task,90);call_host_current=&call_boot_task;
    assert(pm_configure(mutation_mount));integrated=life_active=true;
    const char *equals=NULL;for(const char *p=argv[3];*p;p++)if(*p=='=')equals=p;
    cancel_at=equals?(unsigned)strtoul(equals+1,NULL,10):UINT32_MAX;
    pm_key key;mutation_call call;assert(!pm_account(mutation_mount,&key)&&!mutation_enter(key,&call));
    if(recover)cancel_armed=true;
    int ready=accounts_prepare(mounted);cancel_armed=false;
    if(recover&&equals&&ready){
        assert(ready==-VFS_EINTR&&!enabled&&cancel_commits==cancel_at&&writes_seen==account_cancel_writes);
        pg_integration_relax();mutation_leave(&call);goto interrupted;
    }
    if(recover&&equals)assert(!cancel_commits&&!pc_cancelled(thread_current()));
    assert(!ready);pg_integration_relax();mutation_leave(&call);integration_publish();
    if(!recover){
        account_request_t request={.size=64,.version=1};
        assert(!pm_account(mutation_mount,&key)&&!mutation_control(key,&call_boot_task,ACCOUNT_BEGIN,&request));
        request.passwd=(uintptr_t)integration_new[0];request.group=(uintptr_t)integration_new[1];request.shadow=(uintptr_t)integration_new[2];
        request.passwd_bytes=strlen(integration_new[0]);request.group_bytes=strlen(integration_new[1]);request.shadow_bytes=strlen(integration_new[2]);
        cancel_armed=true;assert(!pm_account(mutation_mount,&key));
        int64_t result=mutation_control(key,&call_boot_task,ACCOUNT_UPDATE,&request);cancel_armed=false;
        if(equals){
            assert(result==SYSCALL_EINTR&&!enabled&&cancel_commits==cancel_at&&writes_seen==account_cancel_writes);
            account_request_t check={.size=64,.version=1,.generation=generation};unsigned before=writes_seen;
            assert(accounts_control(ACCOUNT_CHECK,(uintptr_t)&check,sizeof check,&call_boot_task.creds,90)==SYSCALL_EAGAIN&&before==writes_seen);
            goto interrupted;
        }
        assert(!result);request.passwd=request.group=request.shadow=0;request.passwd_bytes=request.group_bytes=request.shadow_bytes=0;
        assert(!pm_account(mutation_mount,&key)&&!mutation_control(key,&call_boot_task,ACCOUNT_FINISH,&request));
    }
    {uint64_t irq=spin_lock_irqsave(&e4_lock);pg_view view;
    assert(!life_validate(mounted->engine,&view)&&view.charged[0]==3&&view.charged[1]==1&&view.remaining[0]==5&&!view.remaining[1]&&!mounted->engine->orphan_count);
    spin_unlock_irqrestore(&e4_lock,irq);}
    assert(!ext4_freeze_and_sync(mounted));assert(!fclose(image));free(disk_bytes);
    printf("PASS account mid commits=%u interrupted=0 occupied=%llu,%llu certificates=%u\n",cancel_commits,(unsigned long long)occupied,(unsigned long long)occupied_inodes,account_orphan_certificates);return 0;
interrupted:
    {unsigned count,pins;bool priority;pm_counts(&count,&pins,&priority);assert(!count&&!pins&&!priority&&!pc_count());}
    account_origin_file(argv[1],false);
    char bytes[ACCOUNT_FILE_MAX];size_t n=0;bool forward=!backend.read(mounted,ACCOUNT_PENDING,bytes,sizeof bytes,&n);
    if(!forward&&!backend.read(mounted,ACCOUNT_PASSWD,bytes,sizeof bytes,&n))forward=n==strlen(integration_new[0])&&!memcmp(bytes,integration_new[0],n);
    printf("PASS account mid commits=%u interrupted=1 forward=%u orphans=%u role=%u blocks=%llu\n",cancel_commits,forward,mounted->engine->orphan_count,account_origin.role,(unsigned long long)account_orphan_blocks);
    assert(!fclose(image));free(disk_bytes);return 0;
}
