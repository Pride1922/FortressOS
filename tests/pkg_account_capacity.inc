/* Private fenced/no-reader profile, never called by production accounts. */
static bool capacity_active;
static uint64_t capacity_left[2],capacity_escrow[2];
static unsigned capacity_commits,capacity_checks;
/* Caller owns exclusion: mounted snapshot takes e4_lock; constructor preview
 * is unpublished and exclusively owned, with unlocked JBD2 preview reads. */
static int capacity_certificate(ext4_mount_t *fs,e4_ns_workspace_t *w,uint64_t out[2]){
    /* The constructor's full ownership audit precedes this private exclusive
     * workbench. Re-run the role/map certificate, not a quadratic global audit,
     * at each callback; all intervening mutations are the actual account code. */
    int r=e4_ns_directory(fs,w,0,2);if(r)return r;
    r=e4_ns_directory(fs,w,1,w->anchors[3]);if(r)return r;
    uint64_t d=(ACCOUNT_FILE_MAX+fs->bs-1)/fs->bs,t=d>4?1:0;
    uint64_t left[2]={9*(d+t)+2,ACCOUNT_ROLE_COUNT};
    for(unsigned role=0;role<ACCOUNT_ROLE_COUNT;role++)if(w->roles[role]){
        uint32_t ino=w->roles[role];e4_inode_t in;
        for(unsigned i=0;i<fs->nodes;i++)if(fs->cached[i]->inode.ino==ino&&fs->cached[i]->opens)return -VFS_EAGAIN;
        r=e4_inode(fs,ino,&in);if(r)return r;
        fs->rw_ino=0;r=e4_mapping(&in);if(r)return r;
        uint64_t blocks=0,logical=0;
        for(unsigned i=0;i<in.map->count;i++){
            e4_extent_t x=in.map->entries[i];
            if(x.unwritten||x.logical!=logical)return -VFS_EINVAL;
            logical+=x.len;
        }
        if(logical!=(in.size+fs->bs-1)/fs->bs)return -VFS_EINVAL;
        for(unsigned i=0;i<fs->physical_count;i++)blocks+=fs->physical[i].len;
        uint64_t max=role>=ACCOUNT_INTENT_TEMP?1:d+t;
        if(blocks>max||blocks>left[0]||!left[1])return -VFS_EINVAL;
        left[0]-=blocks;left[1]--;
    }
    memcpy(out,left,sizeof left);return 0;
}
static int capacity_snapshot(uint64_t out[2]){
    ext4_mount_t *fs=mounted;
    e4_ns_workspace_t *w=kcalloc(1,sizeof(*w));if(!w)return -VFS_ENOMEM;
    uint64_t lock=spin_lock_irqsave(&e4_lock);int r=-VFS_EINVAL;
    if(fs->account_namespace_fenced&&!fs->engine->tainted&&!fs->engine->ready&&
       !fs->engine->orphan_count&&!fs->engine->orphan_recovery_required){
        memcpy(w->anchors,fs->account_namespace_anchors,sizeof w->anchors);
        r=capacity_certificate(fs,w,out);if(!r)capacity_checks++;
    }
    spin_unlock_irqrestore(&e4_lock,lock);kfree(w);return r;
}
static int capacity_admit(void){
    uint64_t next[2];int r=capacity_snapshot(next);if(r)return r;
    uint8_t sb[1024];if(!e4_bytes(mounted,1024,sb,sizeof sb))return -VFS_EIO;
    uint64_t f[2]={e4_u32(sb+12),e4_u32(sb+16)};
    for(unsigned i=0;i<2;i++)if(f[i]<capacity_escrow[i]||f[i]-capacity_escrow[i]<next[i])return -VFS_ENOSPC;
    memcpy(capacity_left,next,sizeof next);return 0;
}
static int e4_semantic_commit(ext4_engine_t *e){
    if(!capacity_active)return pg_capacity_core_commit(e);
    uint8_t old[1024];if(!e4_bytes(e->fs,1024,old,sizeof old))return -VFS_EIO;
    uint8_t *next=e4_super(e);if(!next)return e->error;
    uint64_t remain[2];
    for(unsigned i=0;i<2;i++){
        uint64_t f=e4_u32(next+12+4*i);
        int64_t r=(int64_t)capacity_left[i]+(int64_t)f-(int64_t)e4_u32(old+12+4*i);
        if(r<0||f<capacity_escrow[i]||f-capacity_escrow[i]<(uint64_t)r)return -VFS_ENOSPC;
        remain[i]=(uint64_t)r;
    }
    int r=pg_capacity_core_commit(e);
    if(!r){memcpy(capacity_left,remain,sizeof remain);capacity_commits++;}return r;
}
static int capacity_after(int r){
    if(r)return r;
    uint64_t expected[2];memcpy(expected,capacity_left,sizeof expected);
    int checked=capacity_admit();assert(!checked&&!memcmp(expected,capacity_left,sizeof expected));return 0;
}
static int capacity_write(void *p,unsigned role,const void *b,size_t n){
    int r=capacity_admit();return r?r:capacity_after(write_role(p,role,b,n));
}
static int capacity_remove(void *p,unsigned role){
    int r=capacity_admit();return r?r:capacity_after(backend.remove(p,role));
}
static int capacity_rename(void *p,unsigned a,unsigned b){
    int r=capacity_admit();return r?r:capacity_after(backend.rename(p,a,b));
}
int main(int argc,char **argv){
    assert(argc==4);unsigned sector=(unsigned)strtoul(argv[2],NULL,10);
    image=fopen(argv[1],"r+b");assert(image&&!fseeko(image,0,SEEK_END));size_t bytes=ftello(image);
    disk_bytes=malloc(bytes);assert(disk_bytes&&!fseeko(image,0,SEEK_SET));assert(fread(disk_bytes,1,bytes,image)==bytes);
    block_dev_t dev={.sector_size=sector,.sector_count=bytes/sector,.read_sector=rd,.write_sector=wr,.flush=flush};
    vfs_init();assert(!ext4_prepare_root(&dev,true,&mounted));assert(!ext4_root_account_io(mounted,&backend));
    ext4_account_namespace_report_t ns;assert(!ext4_root_account_namespace_admit(mounted,&ns));
    uint64_t r[2];assert(!capacity_snapshot(r));uint8_t sb[1024];assert(e4_bytes(mounted,1024,sb,sizeof sb));
    uint64_t f[2]={e4_u32(sb+12),e4_u32(sb+16)};
    unsigned before=writes_seen;
    for(unsigned dim=0;dim<2;dim++){
        capacity_escrow[0]=8;capacity_escrow[1]=1;capacity_escrow[dim]=f[dim]-r[dim]+1;
        assert(capacity_admit()==-VFS_ENOSPC&&writes_seen==before);
    }
    capacity_escrow[0]=f[0]-r[0];capacity_escrow[1]=f[1]-r[1];assert(!capacity_admit());
    int error;vfs_node_t *held=e4_ac_lookup(mounted,"/etc/passwd",&error);assert(held&&!held->open(held));
    uint64_t unchanged[2]={UINT64_MAX,UINT64_MAX};assert(capacity_snapshot(unchanged)==-VFS_EAGAIN&&unchanged[0]==UINT64_MAX&&writes_seen==before);
    held->close(held);vfs_node_put(held);assert(!capacity_admit());
    account_io_t io=backend;io.write=capacity_write;io.remove=capacity_remove;io.rename=capacity_rename;io.validate=valid;
    capacity_active=true;int result;
    if(!strcmp(argv[3],"update")||!strcmp(argv[3],"interrupt")){
        assert(!account_redo_recover(&io,&workspace));
        for(unsigned i=0;i<3;i++){workspace.lengths[i]=ACCOUNT_FILE_MAX;memset(workspace.files[i],'A'+i,ACCOUNT_FILE_MAX);}
        if(!strcmp(argv[3],"interrupt"))interrupt_role=ACCOUNT_TEMP_GROUP;
        result=account_redo_update(&io,&workspace,7);
    }else result=account_redo_recover(&io,&workspace);
    if(!strcmp(argv[3],"interrupt"))assert(result==-VFS_EIO&&interrupted==1);
    else {assert(!result);assert(!account_redo_recover(&io,&workspace));assert(!capacity_admit());}
    capacity_active=false;assert(!ext4_freeze_and_sync(mounted));assert(!fclose(image));free(disk_bytes);
    printf("PASS capacity block=%u sector=%u initial_R=%llu,%llu commits=%u snapshots=%u mode=%s\n",mounted->bs,sector,
        (unsigned long long)r[0],(unsigned long long)r[1],capacity_commits,capacity_checks,argv[3]);return 0;
}
