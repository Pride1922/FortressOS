/* Constructor-exclusive read-only certificate; no mounted globals or fences. */
static int capacity_preview(ext4_engine_t *engine,uint64_t out[2]){
    spin_debug_assert_unheld();ext4_mount_t *fs=engine->fs;
    if(fs->engine||fs->journal_mounted||fs->nodes||engine->tainted||engine->ready||
       engine->orphan_count||engine->orphan_recovery_required)return -VFS_EINVAL;
    e4_ns_workspace_t *w=kcalloc(1,sizeof(*w));if(!w)return -VFS_ENOMEM;
    e4_inode_t root;int r=e4_inode(fs,2,&root);
    if(!r&&(root.mode!=VFS_S_IFDIR||root.uid||root.gid||(root.full_mode&07777)!=0755))r=-VFS_EACCES;
    if(!r)r=e4_ns_child(fs,2,"etc",0755,&w->anchors[0]);
    if(!r)r=e4_ns_child(fs,2,"var",0755,&w->anchors[1]);
    if(!r)r=e4_ns_child(fs,w->anchors[1],"lib",0755,&w->anchors[2]);
    if(!r)r=e4_ns_child(fs,w->anchors[2],"fortress",0755,&w->anchors[3]);
    if(!r)r=e4_ns_child(fs,w->anchors[3],"accounts",0700,&w->anchors[4]);
    if(!r)r=capacity_certificate(fs,w,out);
    kfree(w);return r;
}
static int capacity_preview_budget(ext4_mount_t *fs,const uint64_t free[2],
                                   const uint64_t grant[2],const uint64_t required[2]){
    for(unsigned i=0;i<2;i++){
        uint64_t floor=i?EXT4_ACCOUNT_RESERVE_INODES:(EXT4_ACCOUNT_RESERVE_BYTES+fs->bs-1)/fs->bs;
        if(required[i]>floor)floor=required[i];
        if(free[i]<grant[i]||free[i]-grant[i]<floor)return -VFS_ENOSPC;
    }return 0;
}
/* Arithmetic boundary probes use local F/G inputs, never forged disk counters. */
static void capacity_preview_boundaries(ext4_mount_t *fs,const uint64_t required[2]){
    uint64_t grant[2]={8,1},free[2];
    for(unsigned i=0;i<2;i++){
        uint64_t floor=i?EXT4_ACCOUNT_RESERVE_INODES:(EXT4_ACCOUNT_RESERVE_BYTES+fs->bs-1)/fs->bs;
        free[i]=grant[i]+(required[i]>floor?required[i]:floor);
    }
    assert(!capacity_preview_budget(fs,free,grant,required));
    for(unsigned i=0;i<2;i++){
        grant[i]++;assert(capacity_preview_budget(fs,free,grant,required)==-VFS_ENOSPC);grant[i]--;
        uint64_t saved=free[i];free[i]=grant[i]-1;
        assert(capacity_preview_budget(fs,free,grant,required)==-VFS_ENOSPC);free[i]=saved;
    }
}
