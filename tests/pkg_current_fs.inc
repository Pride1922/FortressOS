/* Actual EXT4 adapter: bytes are verified separately by the pure current reader.
 * Caller owns stable filesystem exclusion (or unpublished boot construction). */
static int current_read_file(ext4_mount_t *fs,const char *path,unsigned mode,uint8_t *out,size_t *length){
    e4_inode_t in;int r=identity_path(fs,path,&in);if(r)return r;
    if(in.mode!=VFS_S_IFREG || in.uid || in.gid || !in.generation || (in.full_mode&07777)!=mode ||
        !in.size || in.size>8192 || e4_u16(in.extent+6))return -VFS_EACCES;
    unsigned group=(in.ino-1)/fs->ipg,index=(in.ino-1)%fs->ipg;uint8_t links[2];
    if(!e4_bytes(fs,(uint64_t)e4_u32(fs->gd[group].raw+8)*fs->bs+(uint64_t)index*256+26,links,2))return -VFS_EIO;
    if(e4_u16(links)!=1)return -VFS_EINVAL;
    r=e4_mapping(&in);if(r)return r;
    if(!in.map->count || in.map->count>4)return -VFS_EINVAL;
    size_t copied=0;uint64_t logical=0;
    for(unsigned i=0;i<in.map->count;i++){
        e4_extent_t x=in.map->entries[i];if(x.unwritten || x.logical!=logical)return -VFS_EINVAL;
        size_t take=(size_t)x.len*fs->bs;if(take>in.size-copied)take=(size_t)in.size-copied;
        if(!take || !e4_bytes(fs,(uint64_t)x.physical*fs->bs,out+copied,take))return -VFS_EIO;
        logical+=x.len;copied+=take;
    }
    if(copied!=in.size || logical!=(in.size+fs->bs-1)/fs->bs)return -VFS_EINVAL;
    *length=copied;return 0;
}
