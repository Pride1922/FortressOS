/* Private semantic-boundary cancellation and constructor recovery workbench. */
static uint32_t cancel_ino,cancel_generation;
static unsigned cancel_at,cancel_commits,cancel_blocked_writes;
static bool cancel_armed;
static bool cancel_preview_permit(ext4_engine_t *e){
    if(!e->orphan_count)return true;
    pg_view view;if(life_read(e->fs,&view))return false;
    if(e->orphan_count!=1||e->orphans[0]!=cancel_ino||!cancel_ino)return false;
    for(unsigned i=0;i<4;i++)if(cancel_ino==ids[i])return false;
    if(e4_u16(table+22)&&cancel_ino==e4_u32(table+260))return false;
    ext4_mount_t *fs=e->fs;uint8_t *raw=raw_inode(fs,cancel_ino);
    if(!raw||e4_u32(raw+100)!=cancel_generation||e4_u16(raw)!=(0100000|0600)||e4_u16(raw+26)>1)return false;
    e4_inode_t root,work,a;uint32_t ino;
    if(e4_inode(fs,2,&root)||e4_scan(&root,"work",0,NULL,&ino)<0||!ino||e4_inode(fs,ino,&work)||
       e4_scan(&work,"a",0,NULL,&ino)<0||!ino||e4_inode(fs,ino,&a))return false;
    uint32_t named=0;int found=e4_scan(&a,"recover",0,NULL,&named);
    if(e4_u16(raw+26)==1)return found>=0&&named==cancel_ino;
    return found==0&&!named;
}
static int cancel_dispatch(ext4_engine_t *e){
    if(!pc_view(thread_current()))return -VFS_EACCES;
    if(!pc_commit_begin(thread_current())){cancel_blocked_writes=writes_seen;return -VFS_EINTR;}
    int r=call_budget_dispatch(e);pc_commit_end(thread_current());
    if(!r&&cancel_armed&&++cancel_commits==cancel_at)assert(pc_cancel_current(thread_current()));
    return r;
}
static void cancel_context(const char *path,bool read){
    char context[1024];assert(snprintf(context,sizeof context,"%s.ctx",path)>0);
    FILE *f=fopen(context,read?"rb":"wb");assert(f);
    if(read){assert(fread(&binding,sizeof binding,1,f)==1&&fread(ids,sizeof ids,1,f)==1&&fread(&marker_generation,sizeof marker_generation,1,f)==1);
        assert(fread(&cancel_ino,sizeof cancel_ino,1,f)==1&&fread(&cancel_generation,sizeof cancel_generation,1,f)==1);}
    else {assert(fwrite(&binding,sizeof binding,1,f)==1&&fwrite(ids,sizeof ids,1,f)==1&&fwrite(&marker_generation,sizeof marker_generation,1,f)==1);
        assert(fwrite(&cancel_ino,sizeof cancel_ino,1,f)==1&&fwrite(&cancel_generation,sizeof cancel_generation,1,f)==1);}
    assert(!fclose(f));
}
int main(int argc,char **argv){
    (void)mutation_previous_main;(void)call_namespace_checks;
    assert(argc==4);setvbuf(stdout,NULL,_IONBF,0);unsigned sector=(unsigned)strtoul(argv[2],NULL,10);
    bool seed=!strcmp(argv[3],"seed"),recover=!strcmp(argv[3],"recover"),reject=!strcmp(argv[3],"reject");
    image=fopen(argv[1],"r+b");assert(image&&!fseeko(image,0,SEEK_END));size_t size=ftello(image);
    disk_bytes=malloc(size);assert(disk_bytes&&!fseeko(image,0,SEEK_SET)&&fread(disk_bytes,1,size,image)==size);
    block_dev_t dev={.sector_size=sector,.sector_count=size/sector,.read_sector=rd,.write_sector=wr,.flush=flush};
    if(!seed){cancel_context(argv[1],true);integration_pre_required=true;}
    if(reject)cancel_generation^=1;
    vfs_init();const char *stage;int prepared=ext4_prepare_root_diagnose(&dev,true,&mounted,&stage);
    if(reject){assert(prepared==-VFS_EIO&&!mounted&&!writes_seen&&!integration_flushes&&integration_pre_calls==1);
        assert(!fclose(image));free(disk_bytes);puts("PASS cancellation preview identity rejection zero-write-flush");return 0;}
    if(prepared)fprintf(stderr,"cancel constructor stage=%s status=%d\n",stage,prepared);
    assert(!prepared);ext4_account_namespace_report_t report;
    assert(!ext4_root_account_io(mounted,&backend)&&!ext4_root_account_namespace_admit(mounted,&report));
    if(seed)integration_seed();
    if(seed)call_bootstrap();else {
        integration_birth(&call_boot_task,90);call_host_current=&call_boot_task;
        const char *paths[]={"/work","/work/a","/work/b"};
        for(unsigned i=0;i<3;i++){int error;call_sandbox[i]=e4_ac_lookup(mounted,paths[i],&error);assert(call_sandbox[i]);}
    }
    assert(pm_configure(mutation_mount));integrated=life_active=true;
    pm_key key;mutation_call call;assert(!pm_account(mutation_mount,&key)&&!mutation_enter(key,&call));
    assert(!accounts_prepare(mounted));pg_integration_relax();mutation_leave(&call);integration_publish();
    if(seed){
        tcb_t parent;integration_birth(&parent,100);int fd=integration_acquire(&parent);assert(fd>=0);
        assert(!pm_package(mutation_mount,lease.owner,lease.session,&key)&&!mutation_enter(key,&call));
        call.create=true;assert(!life_create(mounted));call.create=false;
        assert(mutation_write(life_target,'K',mounted->bs*3)==(int64_t)mounted->bs*3);mutation_leave(&call);
        call_run(CALL_CREATE,"/work/a/recover",NULL,0);
        assert(!pm_ordinary(mutation_mount,&key)&&!mutation_enter(key,&call));call.namespace=true;
        int error;file_t *f=vfs_open_creds("/work/a/recover",VFS_O_WRONLY,&call_boot_task.creds,&error);assert(f);
        char *data=malloc(32768);assert(data);memset(data,'R',32768);size_t left=70*mounted->bs;
        while(left){size_t n=left>32768?32768:left;assert(vfs_write_creds(f,data,n,&call_boot_task.creds)==(int64_t)n);left-=n;}
        cancel_ino=((e4_node_t *)f->node)->inode.ino;cancel_generation=((e4_node_t *)f->node)->inode.generation;
        free(data);assert(!vfs_close(f));call.namespace=false;mutation_leave(&call);fd_close_all(&parent);process_record_abort(parent.tid);
        cancel_context(argv[1],false);
    }else if(!recover){
        bool remove=!strncmp(argv[3],"unlink",6);const char *equals=NULL;
        for(const char *p=argv[3];*p;p++)if(*p=='=')equals=p;
        cancel_at=equals?(unsigned)strtoul(equals+1,NULL,10):UINT32_MAX;
        assert(!pm_ordinary(mutation_mount,&key)&&!mutation_enter(key,&call));cancel_armed=true;
        int64_t r=call_namespace(remove?CALL_REMOVE:CALL_TRUNCATE,"/work/a/recover",NULL);cancel_armed=false;
        if((equals&&r!=-VFS_EINTR)||(!equals&&r))fprintf(stderr,"cancel op=%s status=%lld commits=%u orphans=%u\n",argv[3],(long long)r,cancel_commits,mounted->engine->orphan_count);
        if(equals){assert(r==-VFS_EINTR&&pc_cancelled(thread_current())&&cancel_commits==cancel_at&&writes_seen==cancel_blocked_writes);
            assert(mounted->engine->orphan_count==1&&mounted->engine->orphans[0]==cancel_ino);}
        else assert(!r&&!mounted->engine->orphan_count);
        mutation_leave(&call);assert(!pc_count());
        printf("PASS cancellation operation=%s commits=%u interrupted=%u\n",remove?"unlink":"truncate",cancel_commits,equals!=NULL);
        if(equals){assert(!fclose(image));free(disk_bytes);return 0;}
    }
    uint64_t irq=spin_lock_irqsave(&e4_lock);pg_view view;
    assert(!life_validate(mounted->engine,&view)&&view.charged[0]==3&&view.charged[1]==1&&view.remaining[0]==5&&!view.remaining[1]&&!mounted->engine->orphan_count);
    spin_unlock_irqrestore(&e4_lock,irq);assert(!ext4_freeze_and_sync(mounted));assert(!fclose(image));free(disk_bytes);
    puts("PASS cancellation recovery orphan=0 G=5,0 Q=3,1");return 0;
}
