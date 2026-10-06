/* Phase 8.5: explicit disposable fixture mount, ordinary ranked VFS exclusion.
 * Every superblock lifecycle image, including activation/clean, uses JBD2.
 * Recovery preview validates the result BEFORE the first recovery write. */
static bool e4_preview_sector(block_dev_t *dev,uint64_t lba,void *bytes) {
    return jbd2_preview_read_sector(dev->priv,lba,bytes);
}
static bool e4_preview_write(block_dev_t *dev,uint64_t lba,const void *bytes) {
    (void)dev;(void)lba;(void)bytes;return false;
}
static bool e4_preview_flush(block_dev_t *dev) { (void)dev;return false; }
static int e4_journal_mark(ext4_engine_t *e,bool clean) {
    if (e->tainted) return -VFS_EIO;
    if (e->ready || jbd2_writer_state(e->writer)!=JBD2_WRITE_IDLE) return -VFS_EAGAIN;
    if (clean && e->orphan_count) return -VFS_EAGAIN;
    e->file_planning=true;int r=e4_plan_begin(e);e->file_planning=false;if (r) return r;
    uint8_t *block=e4_stage(e,1024/e->fs->bs,E4_REFERENCE);
    if (!block) { r=e->error;ext4_engine_abort(e);return r; }
    uint8_t *sb=block+1024%e->fs->bs;
    e4_p16(sb+58,clean ? 1 : 0);e4_p32(sb+96,clean ? 0x42 : 0x46);
    e4_p32(sb+1020,e4_crc(UINT32_MAX,sb,1020));
    /* Lifecycle transactions own only the superblock image. A preceding
     * extent-tree replacement may have retired blocks; those revokes already
     * belong to its checkpointed transaction, never this zero-revoke plan. */
    e->revoke_count=0;
    e->fs->state_transition=true;e->ready=true;e->credits=(jbd2_credits_t){1,0,0};
    r=jbd2_writer_begin(e->writer,e->credits);
    if (!r) r=e4_transaction_commit(e);
    e->fs->state_transition=false;
    if (!r) {
        memcpy(((e4_journal_guard_t *)e->journal_guard)->sb,sb,1024);e->dirty=!clean;
    } else ext4_engine_abort(e);
    e4_semantic_invalidate(e);return r;
}
int ext4_mount_journal_fixture(block_dev_t *dev,const char *path,
                               ext4_journal_admission_t admission,ext4_mount_t **out) {
    spin_debug_assert_unheld();if (out) *out=NULL;
    if (!out || !dev || !path || strcmp(path,"/mnt") || !dev->read_sector ||
        (dev->sector_size!=512 && dev->sector_size!=4096) || !dev->sector_count ||
        dev->sector_count>UINT32_MAX || dev->sector_count>UINT64_MAX/dev->sector_size) return -VFS_EINVAL;
    if (!admission.disposable_fixture || !admission.writable || !admission.recovery ||
        !dev->write_sector || !dev->flush) return -VFS_EROFS;
    uint64_t flags=spin_lock_irqsave(&e4_lock);vfs_node_t *parent=vfs_lookup("/");
    int r=!parent ? -VFS_EINVAL : e4_active || e4_engine_busy || vfs_lookup(path) ? -VFS_EEXIST : 0;
    if (!r) e4_engine_busy=true;
    spin_unlock_irqrestore(&e4_lock,flags);if (r) return r;
    ext4_mount_t *bootstrap=kcalloc(1,sizeof(*bootstrap));
    e4_node_t *node=kcalloc(1,sizeof(*node));ext4_engine_t *engine=NULL;
    jbd2_plan_t *plan=NULL;uint32_t *map=NULL;e4_journal_guard_t guard={.lifecycle=true};
    r=-VFS_ENOMEM;if (!bootstrap || !node) goto fail;
    bootstrap->dev=dev;bootstrap->bs=dev->sector_size;bootstrap->blocks=(uint32_t)dev->sector_count;
    bootstrap->journal_bootstrap=true;jbd2_source_t source;
    r=e4_journal_source(bootstrap,&guard,&map,&source);
    if (!r) r=jbd2_analyze(dev,&source,&plan,NULL);
    kfree(map);map=NULL;e4_discard(bootstrap);bootstrap=NULL;if (r) goto fail;
    block_dev_t preview={.sector_size=dev->sector_size,.sector_count=dev->sector_count,
        .read_sector=e4_preview_sector,.write_sector=e4_preview_write,.flush=e4_preview_flush,.priv=plan};
    r=e4_open_journal_orphans_owned(&preview,true,&engine,true,true);if (r) goto fail;
    /* Constructor checks complete allocation/reference ownership and orphans.
     * Neither preview adapter nor any preflight is permitted to write. */
    e4_engine_dispose(engine);engine=NULL;
    r=jbd2_replay(plan,true);if (r) goto fail;
    jbd2_release(plan);plan=NULL;
    r=e4_open_journal_orphans_owned(dev,true,&engine,true,true);if (r) goto fail;
    ext4_mount_t *fs=engine->fs;
    fs->rw_map=kmalloc(sizeof(*fs->rw_map)+E4_MAP_MAX*sizeof(e4_extent_t));
    if (!fs->rw_map) { r=-VFS_ENOMEM;goto fail; }
    /* Activate dirty+RECOVER durably before orphan cleanup or publication.
     * A crash during activation has changed no namespace/allocation metadata;
     * explicit fixture mount always analyzes the journal, even if RECOVER=0. */
    r=e4_journal_mark(engine,false);if (r) goto fail;
    r=ext4_engine_orphan_recover(engine);if (r) goto fail;
    if (!block_flush(dev)) { engine->tainted=true;e4_cache_drop(fs,true);r=-VFS_EIO;goto fail; }
    fs->engine=engine;fs->journal_mounted=true;
    e4_inode_t root;
    r=e4_inode(fs,2,&root);if (r) goto fail;
    flags=spin_lock_irqsave(&e4_lock);
    if (e4_active || vfs_lookup(path)) { r=-VFS_EEXIST;spin_unlock_irqrestore(&e4_lock,flags);goto fail; }
    memcpy(node->node.name,"mnt",4);memcpy(node->node.path,"/mnt",5);node->node.parent=parent;e4_setup(node,&root);
    fs->cached[fs->nodes++]=node;node->node.next=parent->children;parent->children=&node->node;
    e4_active=fs;e4_engine_busy=false;*out=fs;
    spin_unlock_irqrestore(&e4_lock,flags);return 0;
fail:
    kfree(map);jbd2_release(plan);e4_discard(bootstrap);kfree(node);
    if (engine) { engine->fs->journal_mounted=false;e4_engine_dispose(engine); }
    flags=spin_lock_irqsave(&e4_lock);e4_engine_busy=false;spin_unlock_irqrestore(&e4_lock,flags);return r;
}
static bool e4_mounted_orphan(ext4_mount_t *fs,uint32_t ino) {
    return fs->journal_mounted && e4_orphan_index(fs->engine,ino)>=0;
}
static int e4_journal_refresh(vfs_node_t *node) {
    e4_node_t *n=(e4_node_t *)node;ext4_mount_t *fs=n->inode.fs;e4_inode_t in;
    if (n->removed && (!n->opens || !e4_mounted_orphan(fs,n->inode.ino))) return -VFS_ENOENT;
    fs->orphan_access=n->removed ? n->inode.ino : 0;int r=e4_inode(fs,n->inode.ino,&in);fs->orphan_access=0;
    if (!r && in.generation!=n->inode.generation) r=-VFS_EIO;
    if (!r) { n->inode=in;node->size=in.size; }return r;
}
static void e4_jtaint(ext4_mount_t *fs) { fs->engine->tainted=true;e4_cache_drop(fs,true); }
static int64_t e4_jwrite(vfs_node_t *node,uint64_t *off,bool append,const void *bytes,size_t len) {
    spin_debug_assert_unheld();uint64_t flags=spin_lock_irqsave(&e4_lock);int r=e4_state(node);int64_t n=r;
    if (!r) {
        e4_node_t *file=(e4_node_t *)node;ext4_mount_t *fs=file->inode.fs;
        fs->orphan_access=file->removed ? file->inode.ino : 0;
        n=ext4_engine_file_write(fs->engine,file->inode.ino,off,append,bytes,len);fs->orphan_access=0;
        if (n>=0 && e4_journal_refresh(node)) { e4_jtaint(fs);n=-VFS_EIO; }
    }
    spin_unlock_irqrestore(&e4_lock,flags);return n;
}
static int e4_jtruncate(vfs_node_t *node,uint64_t size) {
    spin_debug_assert_unheld();uint64_t flags=spin_lock_irqsave(&e4_lock);int r=e4_state(node);
    if (!r) {
        e4_node_t *file=(e4_node_t *)node;ext4_mount_t *fs=file->inode.fs;
        r=e4_orphan_pinned_truncate(fs->engine,file->inode.ino,size);
        /* A later cleanup staging failure can follow a durable shrink intent.
         * Its authoritative size is already published, even when reclamation
         * must be retried. Never retain the old VFS size on that healthy mount.
         * Uncertain I/O still taints and requires restart; do not infer a size. */
        if ((!r || (!fs->engine->tainted && e4_mounted_orphan(fs,file->inode.ino))) &&
            e4_journal_refresh(node)) { e4_jtaint(fs);r=-VFS_EIO; }
    }
    spin_unlock_irqrestore(&e4_lock,flags);return r;
}
static void e4_jclose(vfs_node_t *node) {
    spin_debug_assert_unheld();uint64_t flags=spin_lock_irqsave(&e4_lock);e4_node_t *file=(e4_node_t *)node;
    if (file->opens) file->opens--;
    ext4_mount_t *fs=file->inode.fs;
    if (!fs->frozen && !fs->engine->tainted && !file->opens && e4_mounted_orphan(fs,file->inode.ino))
        (void)e4_orphan_cleanup(fs->engine,file->inode.ino,false);
    /* close callback has no error result. A durable failed-cleanup intent stays
     * registered; sync/shutdown returns its retry or taint error. */
    spin_unlock_irqrestore(&e4_lock,flags);
}
static vfs_node_t *e4_jcreate(vfs_node_t *parent,const char *name,vfs_node_type_t type) {
    if (!e4_namespace_name(name) || (type!=VFS_FILE && type!=VFS_DIRECTORY)) {
        vfs_set_last_create_error(-VFS_EINVAL);return NULL;
    }
    e4_node_t *node=kcalloc(1,sizeof(*node));if (!node) { vfs_set_last_create_error(-VFS_ENOMEM);return NULL; }
    uint64_t flags=spin_lock_irqsave(&e4_lock);int r=e4_state(parent);
    ext4_mount_t *fs=((e4_inode_t *)parent->fs_private)->fs;uint32_t ino=0;
    if (!r && (fs->nodes==E4_NODES || strlen(parent->path)+strlen(name)+1>=VFS_MAX_PATH)) r=-VFS_EFBIG;
    if (!r) r=ext4_engine_namespace_create(fs->engine,((e4_inode_t *)parent->fs_private)->ino,name,type==VFS_DIRECTORY,&ino);
    e4_inode_t in;
    if (!r) {
        r=e4_inode(fs,ino,&in);
        if (!r) r=e4_journal_refresh(parent);
        if (r) e4_jtaint(fs); /* Known commit; failed cache publication is not rollback. */
    }
    if (!r) {
        e4_setup(node,&in);memcpy(node->node.name,name,strlen(name)+1);
        size_t length=strlen(parent->path);memcpy(node->node.path,parent->path,length);node->node.path[length]='/';
        memcpy(node->node.path+length+1,name,strlen(name)+1);node->node.parent=parent;
        node->node.next=parent->children;parent->children=&node->node;fs->cached[fs->nodes++]=node;
    }
    spin_unlock_irqrestore(&e4_lock,flags);
    if (r) { kfree(node);vfs_set_last_create_error(r);return NULL; }return &node->node;
}
static int e4_junlink(vfs_node_t *parent,const char *name) {
    if (!e4_namespace_name(name)) return -VFS_EINVAL;
    uint64_t flags=spin_lock_irqsave(&e4_lock);int r=e4_state(parent);uint32_t ino=0;e4_inode_t in;
    ext4_mount_t *fs=((e4_inode_t *)parent->fs_private)->fs;
    if (!r) { r=e4_journal_refresh(parent);if (!r) r=e4_scan(parent->fs_private,name,0,NULL,&ino); }
    if (r>=0 && !ino) r=-VFS_ENOENT;
    if (r>=0) r=e4_inode(fs,ino,&in);
    if (!r) r=ext4_engine_namespace_remove(fs->engine,((e4_inode_t *)parent->fs_private)->ino,name,in.mode==0x4000);
    /* Cleanup can fail after the durable name removal. Retire the cached name
     * on success or a still-registered orphan; do not expose a stale dentry. */
    if (!r || (ino && e4_orphan_index(fs->engine,ino)>=0)) {
        vfs_node_t **link=&parent->children;
        while (*link) {
            e4_node_t *node=(e4_node_t *)*link;
            if (node->inode.ino==ino) { node->removed=true;*link=node->node.next;node->node.next=NULL;break; }
            link=&(*link)->next;
        }
        if (e4_journal_refresh(parent)) { e4_jtaint(fs);if (!r) r=-VFS_EIO; }
    }
    spin_unlock_irqrestore(&e4_lock,flags);return r;
}
static int e4_jrename(vfs_node_t *old,const char *name,vfs_node_t *dest,const char *next) {
    if (!e4_namespace_name(name) || !e4_namespace_name(next)) return -VFS_EINVAL;
    uint64_t flags=spin_lock_irqsave(&e4_lock);int r=e4_state(old);if (!r) r=e4_state(dest);
    ext4_mount_t *fs=((e4_inode_t *)old->fs_private)->fs;
    if (!r && fs!=((e4_inode_t *)dest->fs_private)->fs) r=-VFS_EOPNOTSUPP;
    if (!r && strlen(dest->path)+strlen(next)+1>=VFS_MAX_PATH) r=-VFS_EFBIG;
    if (!r) r=ext4_engine_namespace_rename(fs->engine,((e4_inode_t *)old->fs_private)->ino,name,
                                         ((e4_inode_t *)dest->fs_private)->ino,next);
    if (!r) {
        vfs_node_t **link=&old->children;
        while (*link && strcmp((*link)->name,name)) link=&(*link)->next;
        if (*link) {
            vfs_node_t *node=*link;*link=node->next;node->next=dest->children;dest->children=node;node->parent=dest;
            memcpy(node->name,next,strlen(next)+1);size_t length=strlen(dest->path);
            memcpy(node->path,dest->path,length);node->path[length]='/';memcpy(node->path+length+1,next,strlen(next)+1);
        }
        if (e4_journal_refresh(old) || (dest!=old && e4_journal_refresh(dest))) { e4_jtaint(fs);r=-VFS_EIO; }
    }
    spin_unlock_irqrestore(&e4_lock,flags);return r;
}
static int e4_journal_drain(ext4_mount_t *fs,bool finish) {
    spin_debug_assert_held(&e4_lock);ext4_engine_t *e=fs->engine;
    if (e->tainted) return -VFS_EIO;
    if (e->ready || jbd2_writer_state(e->writer)!=JBD2_WRITE_IDLE) return -VFS_EAGAIN;
    if (!e->dirty) return block_flush(fs->dev) ? 0 : (e4_jtaint(fs),-VFS_EIO);
    int r=ext4_engine_orphan_recover(e);if (r) return r;
    if (finish && e->orphan_count) return -VFS_EAGAIN;
    if (!block_flush(fs->dev)) { e4_jtaint(fs);return -VFS_EIO; }
    /* Sync retains dirty+RECOVER and live pins. Clean marker is a final bounded
     * journal transaction only AFTER all work/checkpoints and this barrier. */
    return finish ? e4_journal_mark(e,true) : 0;
}
int ext4_sync_journal_fixture(void) {
    spin_debug_assert_unheld();uint64_t flags=spin_lock_irqsave(&e4_lock);
    ext4_mount_t *fs=e4_active && e4_active->journal_mounted ? e4_active : NULL;
    spin_unlock_irqrestore(&e4_lock,flags);return fs ? ext4_sync(fs) : -VFS_EROFS;
}
int ext4_freeze_journal_fixture(void) {
    spin_debug_assert_unheld();uint64_t flags=spin_lock_irqsave(&e4_lock);
    ext4_mount_t *fs=e4_active && e4_active->journal_mounted ? e4_active : NULL;
    spin_unlock_irqrestore(&e4_lock,flags);return fs ? ext4_freeze_and_sync(fs) : 0;
}
