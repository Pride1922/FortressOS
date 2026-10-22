/* Copied constructor only: no mount publication or trust flag changes. */
static unsigned pre_calls;
static uint64_t pre_marker_byte,pre_owner_byte;
static void pre_capture(ext4_mount_t *fs){
    uint64_t flags=spin_lock_irqsave(&e4_lock);e4_inode_t inode;
    assert(!e4_inode(fs,ids[0],&inode));fs->rw_ino=0;assert(!e4_mapping(&inode));
    const e4_extent_t *extent=e4_find(&inode,0);assert(extent);
    pre_marker_byte=(uint64_t)extent->physical*fs->bs+480;
    uint32_t group=(ids[0]-1)/fs->ipg,bit=(ids[0]-1)%fs->ipg;
    pre_owner_byte=(uint64_t)e4_u32(fs->gd[group].raw+8)*fs->bs+bit*256+2;
    spin_unlock_irqrestore(&e4_lock,flags);
}
static int pg_pre_validate(ext4_engine_t *engine){
    pre_calls++;pg_view view;
    spin_debug_assert_unheld();
    return validate_engine(engine->fs,engine,&view)?-VFS_EIO:0;
}
static void pre_negative(block_dev_t *dev,const uint8_t *pending,unsigned op){
    ext4_fault_disk_t *d=dev->priv;pg_binding expected=binding;
    uint32_t generation=marker_generation;
    uint8_t *snapshot=malloc(d->bytes);assert(snapshot);
    for(unsigned test=0;test<7;test++){
        reset(d,pending);vfs_init();unsigned calls=pre_calls;
        if(test==0)binding.marker_sha256[0]^=1;
        if(test==1)binding.uuid[0][0]^=1;
        if(test==2)binding.generation[0]++;
        if(test==3)marker_generation++;
        if(test==4)binding.profile[0]^=1;
        if(test>=5){
            uint64_t offset=test==5?pre_marker_byte:pre_owner_byte;
            assert(offset<d->bytes);d->stable[offset]^=1;d->volatile_bytes[offset]^=1;
        }
        memcpy(snapshot,d->stable,d->bytes);
        ext4_mount_t *fs=NULL;const char *stage=NULL;
        assert(e4_mount_journal_admitted(dev,"/mnt",true,&fs,&stage,false)==-VFS_EIO);
        assert(!fs && !e4_active && !e4_engine_busy);
        if(test<6)assert(stage&&!strcmp(stage,"grant-preview")&&pre_calls==calls+1);
        else assert(stage&&!strcmp(stage,"recovery-preview")&&pre_calls==calls);
        assert(!d->writes&&!d->flushes&&!d->events);
        assert(!memcmp(d->stable,snapshot,d->bytes));
        binding=expected;marker_generation=generation;
        teardown();
    }
    free(snapshot);
    reset(d,pending);vfs_init();ext4_mount_t *fs=NULL;unsigned calls=pre_calls;
    assert(!e4_mount_journal_admitted(dev,"/mnt",true,&fs,NULL,false));
    assert(pre_calls==calls+1&&d->writes);discover();
    {uint64_t flags=spin_lock_irqsave(&e4_lock);pg_view v;
    assert(!validate(fs,&v));assert(v.sequence==(op?3u:2u));
    spin_unlock_irqrestore(&e4_lock,flags);}
    teardown();
    printf("PRE-REPLAY op=%u negative=7 zero-write-flush positive=1 grant=6 filesystem=1\n",op);
}

