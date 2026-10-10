/* Included after EXT2's locked namespace helpers. No process lock on entry. */
static vfs_metadata_t ext_value(const ext2_inode_t *in) {
    return (vfs_metadata_t){.size=in->size,.mode=in->mode,.uid=in->uid,.gid=in->gid,
        .type=(in->mode & VFS_S_IFMT)==VFS_S_IFDIR ? VFS_DIRECTORY : VFS_FILE,
        .mnt_flags=(in->fs->read_only ? VFS_MNT_RDONLY : 0)|VFS_MNT_NOSUID|VFS_MNT_NODEV};
}
static int ext_access_locked(vfs_node_t *node,unsigned mask,const creds_t *actor) {
    ext2_inode_t *bound=node->fs_private,current;
    if (bound->removed) return -VFS_ENOENT;
    if (!inode(bound->fs,bound->ino,&current)) return -VFS_EIO;
    vfs_metadata_t value=ext_value(&current);
    return permission_access_value(&value,mask,actor);
}
static int ext_permission_actor(vfs_node_t *node,unsigned mask,const creds_t *actor) {
    uint64_t irq=spin_lock_irqsave(&ext2_lock);int r=ext_access_locked(node,mask,actor);
    spin_unlock_irqrestore(&ext2_lock,irq);return r;
}
static vfs_node_t *ext_lookup_actor(vfs_node_t *dir,const char *name,const creds_t *actor,int *error) {
    return ext_lookup_common(dir,name,error,true,actor);
}
static vfs_node_t *ext_create_actor(vfs_node_t *dir,const char *name,vfs_node_type_t type,
                                    uint32_t requested,const creds_t *actor,int *error) {
    vfs_create_attrs_t attrs={.mode=requested & 07777u};
    return ext_create_common(dir,name,type,&attrs,error,true,actor);
}
static int ext_unlink_actor(vfs_node_t *dir,const char *name,bool require_directory,const creds_t *actor) {
    uint64_t irq=spin_lock_irqsave(&ext2_lock);int r=ext_unlink_locked(dir,name,require_directory,actor);
    spin_unlock_irqrestore(&ext2_lock,irq);return r;
}
static int ext_rename_actor(vfs_node_t *old,const char *name,vfs_node_t *dest,const char *next,
                            bool require_directory,const creds_t *actor) {
    if (dest->permission_actor!=ext_permission_actor) return -VFS_EOPNOTSUPP;
    return ext_rename_common(old,name,dest,next,require_directory,actor);
}
static int ext_setattr_actor(vfs_node_t *node,bool chown,uint32_t mode,
                             creds_id_change_t uid,creds_id_change_t gid,const creds_t *actor) {
    uint64_t irq=spin_lock_irqsave(&ext2_lock);
    ext2_inode_t *bound=node->fs_private,current;
    int r=bound->removed ? -VFS_ENOENT : bound->fs->tainted ? -VFS_EIO :
        !inode(bound->fs,bound->ino,&current) ? -VFS_EIO : 0;
    vfs_metadata_t next;
    if (!r) {
        vfs_metadata_t old=ext_value(&current);
        r=chown ? permission_chown_value(&old,uid,gid,actor,&next) :
            permission_chmod_value(&old,mode,actor,&next);
    }
    if (!r) {
        current.mode=next.mode;current.uid=next.uid;current.gid=next.gid;
        if (!write_inode_to_disk(&current) || !block_flush(bound->fs->dev)) {
            bound->fs->tainted=true;r=-VFS_EIO;
        } else {
            bound->mode=current.mode;bound->uid=current.uid;bound->gid=current.gid;
            node->mode=current.mode;node->uid=current.uid;node->gid=current.gid;
        }
    }
    spin_unlock_irqrestore(&ext2_lock,irq);return r;
}
static int ext_open_actor(vfs_node_t *node,unsigned mask,bool truncate,const creds_t *actor) {
    uint64_t irq=spin_lock_irqsave(&ext2_lock);
    ext2_inode_t *bound=node->fs_private,current;
    int r=ext_access_locked(node,mask,actor);
    if (!r && bound->opens==UINT32_MAX) r=-VFS_EFBIG;
    if (!r && ((mask & VFS_MAY_WRITE) || truncate))
        r=bound->fs->tainted ? -VFS_EIO : bound->fs->read_only ? -VFS_EROFS : 0;
    if (!r && truncate) {
        if (!inode(bound->fs,bound->ino,&current)) r=-VFS_EIO;
        else {
            vfs_metadata_t old=ext_value(&current);uint16_t prior=bound->mode;
            bound->mode=permission_content_mode(&old,actor);
            r=ext_truncate_locked(node,0);
            if (r) bound->mode=prior;else node->mode=bound->mode;
        }
    }
    if (!r) bound->opens++;
    spin_unlock_irqrestore(&ext2_lock,irq);return r;
}
static int64_t ext_write_actor(vfs_node_t *node,uint64_t *offset,bool append,
                               const void *buf,size_t len,const creds_t *actor) {
    uint64_t irq=spin_lock_irqsave(&ext2_lock);
    ext2_inode_t *bound=node->fs_private,current;
    int64_t r=bound->removed ? -VFS_ENOENT : bound->fs->tainted ? -VFS_EIO :
        bound->fs->read_only ? -VFS_EROFS : !inode(bound->fs,bound->ino,&current) ? -VFS_EIO : 0;
    uint64_t off=append ? bound->size : *offset;
    if (!r && off>bound->size) r=-VFS_EINVAL;
    if (!r && len) {
        vfs_metadata_t old=ext_value(&current);uint16_t prior=bound->mode;
        bound->mode=permission_content_mode(&old,actor);
        r=write_inode(bound,off,buf,len);
        if (r>0) { *offset=off+r;node->size=bound->size;node->mode=bound->mode; }
        else bound->mode=prior;
    }
    spin_unlock_irqrestore(&ext2_lock,irq);return r;
}
