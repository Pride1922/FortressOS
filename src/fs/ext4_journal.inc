/* Phase-6 read-only bootstrap. Allocation counters/bitmaps can be temporarily
 * inconsistent after a partial checkpoint; journal inode/map identity cannot.
 * Ordinary admission and production mount policy never use this mode. */
typedef struct {
    ext4_mount_t *fs;
    uint8_t sb[1024], inode[256];
    uint32_t inode_block, inode_offset;
    bool lifecycle;
} e4_journal_guard_t;
static void e4_journal_owner(void *context) {
    e4_journal_guard_t *g=context;
    if (g->fs->journal_mounted) spin_debug_assert_held(&e4_lock);
    else spin_debug_assert_unheld();
}
static bool e4_journal_home(void *context,uint32_t block,const uint8_t *bytes) {
    e4_journal_guard_t *g=context;ext4_mount_t *fs=g->fs;
    /* Include extent-tree nodes as well as journal data in self-write denial. */
    for (unsigned i=0;i<fs->journal_ranges;i++)
        if (block>=fs->journal_reserved[i].lo && block-fs->journal_reserved[i].lo<fs->journal_reserved[i].len) return false;
    if (block==g->inode_block && memcmp(bytes+g->inode_offset,g->inode,256)) return false;
    if (block==1024/fs->bs) {
        const uint8_t *sb=bytes+1024%fs->bs;
        for (unsigned i=0;i<sizeof(e4_journal_identity)/sizeof(e4_journal_identity[0]);i++)
            if (memcmp(sb+e4_journal_identity[i][0],g->sb+e4_journal_identity[i][0],e4_journal_identity[i][1])) return false;
        bool transition=g->lifecycle && (fs->journal_bootstrap || fs->state_transition);
        if (transition) {
            if ((e4_u32(sb+96)&~4u)!=(e4_u32(g->sb+96)&~4u) ||
                ((e4_u32(sb+96)&4) ? e4_u16(sb+58)!=0 : (e4_u16(sb+58)!=1 || e4_u32(sb+232)))) return false;
        } else if (e4_u32(sb+96)!=e4_u32(g->sb+96)) return false;
        uint32_t orphan=e4_u32(sb+232);
        if (orphan && (orphan<11 || orphan>fs->inodes || orphan==fs->journal_ino)) return false;
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
static int e4_journal_source(ext4_mount_t *fs,e4_journal_guard_t *guard,
                              uint32_t **map_out,jbd2_source_t *source) {
    int r=e4_admit(fs);if (r) return r;
    guard->fs=fs;
    if (!e4_bytes(fs,1024,guard->sb,1024)) return -VFS_EIO;
    uint8_t zero[16]={0};
    if (memcmp(guard->sb+208,zero,16) && memcmp(guard->sb+208,guard->sb+104,16)) return -VFS_EOPNOTSUPP;
    e4_inode_t inode;r=e4_inode(fs,fs->journal_ino,&inode);if (r) return r;
    if (inode.mode!=0x8000 || inode.size%fs->bs || inode.size/fs->bs<4 || inode.size/fs->bs>JBD2_MAP_MAX) return -VFS_EFBIG;
    unsigned group=(inode.ino-1)/fs->ipg,index=(inode.ino-1)%fs->ipg;
    uint64_t where=(uint64_t)e4_u32(fs->gd[group].raw+8)*fs->bs+(uint64_t)index*256;
    guard->inode_block=(uint32_t)(where/fs->bs);guard->inode_offset=(uint32_t)(where%fs->bs);
    if (!e4_bytes(fs,where,guard->inode,256)) return -VFS_EIO;
    r=e4_mapping(&inode);if (r) return r;
    unsigned length=(unsigned)(inode.size/fs->bs),at=0;
    uint32_t *map=kmalloc(length*sizeof(uint32_t));if (!map) return -VFS_ENOMEM;
    *map_out=map;
    for (unsigned i=0;i<inode.map->count;i++) {
        e4_extent_t x=inode.map->entries[i];
        if (x.logical!=at || x.unwritten || x.len>length-at) return -VFS_EIO;
        for (unsigned n=0;n<x.len;n++) map[at++]=x.physical+n;
    }
    if (at!=length) return -VFS_EIO;
    fs->journal_reserved=kmalloc(fs->physical_count*sizeof(e4_range_t));
    if (!fs->journal_reserved) return -VFS_ENOMEM;
    memcpy(fs->journal_reserved,fs->physical,fs->physical_count*sizeof(e4_range_t));
    fs->journal_ranges=fs->physical_count;
    *source=(jbd2_source_t){.block_size=fs->bs,.filesystem_blocks=fs->blocks,
        .journal_blocks=length,.journal_map=map,.validate_home=e4_journal_home,.context=guard,
        .assert_writer_owner=e4_journal_owner};
    memcpy(source->uuid,guard->sb+104,16);return 0;
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
    uint32_t *map=NULL;e4_journal_guard_t guard={0};jbd2_source_t source;
    int r=e4_journal_source(fs,&guard,&map,&source);
    if (!r) r=jbd2_analyze(dev,&source,out,report);
    kfree(map);e4_discard(fs);
    flags=spin_lock_irqsave(&e4_lock);e4_engine_busy=false;spin_unlock_irqrestore(&e4_lock,flags);
    return r;
}
