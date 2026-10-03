/* Phase-6 read-only bootstrap. Allocation counters/bitmaps can be temporarily
 * inconsistent after a partial checkpoint; journal inode/map identity cannot.
 * Ordinary admission and production mount policy never use this mode. */
typedef struct {
    ext4_mount_t *fs;
    uint8_t sb[1024], inode[256];
    uint32_t inode_block, inode_offset;
} e4_journal_guard_t;
static bool e4_journal_home(void *context,uint32_t block,const uint8_t *bytes) {
    e4_journal_guard_t *g=context;ext4_mount_t *fs=g->fs;
    /* Include extent-tree nodes as well as journal data in self-write denial. */
    for (unsigned i=0;i<fs->physical_count;i++)
        if (block>=fs->physical[i].lo && block-fs->physical[i].lo<fs->physical[i].len) return false;
    if (block==g->inode_block && memcmp(bytes+g->inode_offset,g->inode,256)) return false;
    if (block==1024/fs->bs) {
        const uint8_t *sb=bytes+1024%fs->bs;
        for (unsigned i=0;i<sizeof(e4_journal_identity)/sizeof(e4_journal_identity[0]);i++)
            if (memcmp(sb+e4_journal_identity[i][0],g->sb+e4_journal_identity[i][0],e4_journal_identity[i][1])) return false;
        if (e4_u32(sb+96)!=e4_u32(g->sb+96)) return false;
        if (e4_u32(sb+1020)!=e4_crc(UINT32_MAX,sb,1020)) return false;
    }
    if (block>=fs->first+1 && block-fs->first-1<fs->gdt_blocks) {
        unsigned first=(block-fs->first-1)*(fs->bs/32);
        for (unsigned n=0;n<fs->bs/32 && first+n<fs->groups;n++) {
            uint8_t copy[32],number[4];memcpy(copy,bytes+n*32,32);
            if (memcmp(copy,fs->gd[first+n].raw,12)) return false;
            uint16_t stored=e4_u16(copy+30);copy[30]=copy[31]=0;e4_p32(number,first+n);
            if (stored!=(uint16_t)e4_crc(e4_crc(fs->seed,number,4),copy,32)) return false;
        }
    }
    return true;
}
int ext4_journal_analyze(block_dev_t *dev,jbd2_plan_t **out,jbd2_report_t *report) {
    spin_debug_assert_unheld();if (out) *out=NULL;
    if (!out || !dev || !dev->read_sector || (dev->sector_size!=512 && dev->sector_size!=4096) ||
        !dev->sector_count || dev->sector_count>UINT32_MAX || dev->sector_count>UINT64_MAX/dev->sector_size) return -VFS_EINVAL;
    ext4_mount_t *fs=kcalloc(1,sizeof(*fs));if (!fs) return -VFS_ENOMEM;
    uint64_t flags=spin_lock_irqsave(&e4_lock);
    if (e4_active || e4_engine_busy) { spin_unlock_irqrestore(&e4_lock,flags);e4_discard(fs);return -VFS_EEXIST; }
    e4_engine_busy=true;spin_unlock_irqrestore(&e4_lock,flags);
    fs->dev=dev;fs->bs=dev->sector_size;fs->blocks=(uint32_t)dev->sector_count;fs->journal_bootstrap=true;
    int r=e4_admit(fs);uint32_t *map=NULL;if (r) goto done;
    e4_journal_guard_t guard={.fs=fs};
    if (!e4_bytes(fs,1024,guard.sb,1024)) { r=-VFS_EIO;goto done; }
    uint8_t zero[16]={0};
    if (memcmp(guard.sb+208,zero,16) && memcmp(guard.sb+208,guard.sb+104,16)) { r=-VFS_EOPNOTSUPP;goto done; }
    e4_inode_t inode;r=e4_inode(fs,fs->journal_ino,&inode);if (r) goto done;
    if (inode.mode!=0x8000 || inode.size%fs->bs || inode.size/fs->bs<4 || inode.size/fs->bs>JBD2_MAP_MAX) { r=-VFS_EFBIG;goto done; }
    unsigned group=(inode.ino-1)/fs->ipg,index=(inode.ino-1)%fs->ipg;
    uint64_t where=(uint64_t)e4_u32(fs->gd[group].raw+8)*fs->bs+(uint64_t)index*256;
    guard.inode_block=(uint32_t)(where/fs->bs);guard.inode_offset=(uint32_t)(where%fs->bs);
    if (!e4_bytes(fs,where,guard.inode,256)) { r=-VFS_EIO;goto done; }
    r=e4_mapping(&inode);if (r) goto done;
    unsigned length=(unsigned)(inode.size/fs->bs),at=0;
    map=kmalloc(length*sizeof(uint32_t));if (!map) { r=-VFS_ENOMEM;goto done; }
    for (unsigned i=0;i<inode.map->count;i++) {
        e4_extent_t x=inode.map->entries[i];
        if (x.logical!=at || x.unwritten || x.len>length-at) { r=-VFS_EIO;goto done; }
        for (unsigned n=0;n<x.len;n++) map[at++]=x.physical+n;
    }
    if (at!=length) { r=-VFS_EIO;goto done; }
    jbd2_source_t source={.block_size=fs->bs,.filesystem_blocks=fs->blocks,
        .journal_blocks=length,.journal_map=map,.validate_home=e4_journal_home,.context=&guard};
    memcpy(source.uuid,guard.sb+104,16);
    r=jbd2_analyze(dev,&source,out,report);
done:
    kfree(map);e4_discard(fs);
    flags=spin_lock_irqsave(&e4_lock);e4_engine_busy=false;spin_unlock_irqrestore(&e4_lock,flags);
    return r;
}
