/* Mounted actor adapters. e4_lock excludes metadata, identities and journals;
 * every denial precedes namespace_begin, allocation and journal staging. */
static vfs_metadata_t e4_value(const e4_inode_t *in) {
    return (vfs_metadata_t){.size=in->size,.mode=in->full_mode,.uid=in->uid,.gid=in->gid,
        .type=in->mode==VFS_S_IFDIR ? VFS_DIRECTORY : VFS_FILE,
        .mnt_flags=(in->fs->engine ? 0 : VFS_MNT_RDONLY)|VFS_MNT_NOSUID|VFS_MNT_NODEV};
}
static int e4_access_locked(vfs_node_t *node,unsigned mask,const creds_t *actor) {
    e4_node_t *n=(e4_node_t *)node;e4_inode_t in;int r=0;
    if (n->inode.fs->journal_mounted) {
        if (n->removed) return -VFS_ENOENT;
        r=e4_journal_refresh(node);in=n->inode;
    } else r=e4_inode(n->inode.fs,n->inode.ino,&in);
    if (r) return r;
    vfs_metadata_t value=e4_value(&in);return permission_access_value(&value,mask,actor);
}
static int e4_permission_actor(vfs_node_t *node,unsigned mask,const creds_t *actor) {
    uint64_t irq=spin_lock_irqsave(&e4_lock);int r=e4_access_locked(node,mask,actor);
    spin_unlock_irqrestore(&e4_lock,irq);return r;
}
static vfs_node_t *e4_lookup_actor(vfs_node_t *dir,const char *name,const creds_t *actor,int *error) {
    return e4_lookup_common(dir,name,error,true,actor);
}
static vfs_node_t *e4_create_actor(vfs_node_t *dir,const char *name,vfs_node_type_t type,
                                   uint32_t requested,const creds_t *actor,int *error) {
    ext4_mount_t *fs=((e4_inode_t *)dir->fs_private)->fs;
    if (!fs->journal_mounted) {
        if (error) *error=fs->engine ? -VFS_EOPNOTSUPP : -VFS_EROFS;
        return NULL;
    }
    vfs_create_attrs_t attrs={.mode=requested & 07777u};
    return e4_jcreate_common(dir,name,type,error,true,&attrs,actor);
}
static int e4_unlink_actor(vfs_node_t *dir,const char *name,bool require_directory,const creds_t *actor) {
    ext4_mount_t *fs=((e4_inode_t *)dir->fs_private)->fs;
    if (!fs->journal_mounted) return fs->engine ? -VFS_EOPNOTSUPP : -VFS_EROFS;
    return e4_junlink_common(dir,name,require_directory,actor);
}
static int e4_rename_actor(vfs_node_t *old,const char *name,vfs_node_t *dest,const char *next,
                           bool require_directory,const creds_t *actor) {
    if (dest->permission_actor!=e4_permission_actor) return -VFS_EOPNOTSUPP;
    ext4_mount_t *fs=((e4_inode_t *)old->fs_private)->fs;
    if (!fs->journal_mounted) return fs->engine ? -VFS_EOPNOTSUPP : -VFS_EROFS;
    return e4_jrename_common(old,name,dest,next,require_directory,actor);
}
static int e4_setattr_actor(vfs_node_t *node,bool chown,uint32_t mode,
                            creds_id_change_t uid,creds_id_change_t gid,const creds_t *actor) {
    uint64_t irq=spin_lock_irqsave(&e4_lock);
    e4_node_t *n=(e4_node_t *)node;ext4_mount_t *fs=n->inode.fs;
    int r=e4_state(node);
    if (!r && !fs->journal_mounted) r=-VFS_EOPNOTSUPP;
    if (!r) r=e4_journal_refresh(node);
    vfs_metadata_t next;
    if (!r) {
        vfs_metadata_t old=e4_value(&n->inode);
        r=chown ? permission_chown_value(&old,uid,gid,actor,&next) :
            permission_chmod_value(&old,mode,actor,&next);
    }
    if (!r) {
        ext4_engine_t *e=fs->engine;
        r=e4_namespace_begin(e);
        if (!r) {
            uint8_t *raw=e4_inode_image(e,n->inode.ino);
            if (!raw) r=e->error;
            else {
                e4_p16(raw,next.mode);e4_p16(raw+2,next.uid);e4_p16(raw+120,next.uid>>16);
                e4_p16(raw+24,next.gid);e4_p16(raw+122,next.gid>>16);
                e4_inode_sum(fs,n->inode.ino,raw);
            }
            r=e4_namespace_end(e,r);
            if (!r && e4_journal_refresh(node)) {e4_jtaint(fs);r=-VFS_EIO;}
        }
    }
    spin_unlock_irqrestore(&e4_lock,irq);return r;
}
static int e4_open_actor(vfs_node_t *node,unsigned mask,bool truncate,const creds_t *actor) {
    uint64_t irq=spin_lock_irqsave(&e4_lock);
    e4_node_t *n=(e4_node_t *)node;ext4_mount_t *fs=n->inode.fs;
    int r=e4_access_locked(node,mask,actor);
    if (!r && node->open && n->opens==UINT32_MAX) r=-VFS_EFBIG;
    if (!r && ((mask & VFS_MAY_WRITE) || truncate)) {
        r=e4_state(node);
        if (!r && !fs->journal_mounted) r=-VFS_EOPNOTSUPP;
    }
    if (!r && truncate) {
        r=e4_file_truncate_actor(fs->engine,n->inode.ino,0,actor);
        if ((!r || (!fs->engine->tainted && e4_mounted_orphan(fs,n->inode.ino))) &&
            e4_journal_refresh(node)) {e4_jtaint(fs);r=-VFS_EIO;}
    }
    if (!r && node->open) n->opens++;
    spin_unlock_irqrestore(&e4_lock,irq);return r;
}
static int64_t e4_write_actor(vfs_node_t *node,uint64_t *offset,bool append,
                              const void *buf,size_t len,const creds_t *actor) {
    uint64_t irq=spin_lock_irqsave(&e4_lock);int r=e4_state(node);int64_t result=r;
    e4_node_t *n=(e4_node_t *)node;ext4_mount_t *fs=n->inode.fs;
    if (!r && !fs->journal_mounted) result=-VFS_EOPNOTSUPP;
    else if (!r) {
        fs->orphan_access=n->removed ? n->inode.ino : 0;
        result=e4_file_write_actor(fs->engine,n->inode.ino,offset,append,buf,len,actor);
        fs->orphan_access=0;
        if (result>=0 && e4_journal_refresh(node)) {e4_jtaint(fs);result=-VFS_EIO;}
    }
    spin_unlock_irqrestore(&e4_lock,irq);return result;
}
