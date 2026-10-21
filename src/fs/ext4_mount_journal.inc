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
static void e4_mount_stage(const char **stage,const char *name) {
    if (stage) *stage=name;
}
static int e4_mount_journal_admitted(block_dev_t *dev,const char *path,
    bool admitted,ext4_mount_t **out,const char **stage,bool detached) {
    e4_mount_stage(stage,"arguments");
    spin_debug_assert_unheld();if (out) *out=NULL;
    if (!out || !dev || !path || (detached ? strcmp(path,"/") : strcmp(path,"/mnt")) || !dev->read_sector ||
        (dev->sector_size!=512 && dev->sector_size!=4096) || !dev->sector_count ||
        dev->sector_count>UINT32_MAX || dev->sector_count>UINT64_MAX/dev->sector_size) return -VFS_EINVAL;
    e4_mount_stage(stage,"writable-admission");
    if (!admitted ||
        !dev->write_sector || !dev->flush) return -VFS_EROFS;
    e4_mount_stage(stage,"exclusive-mount");
    uint64_t flags=spin_lock_irqsave(&e4_lock);vfs_node_t *parent=vfs_lookup_kernel("/");
    int r=!parent ? -VFS_EINVAL : e4_active || e4_engine_busy || (!detached && vfs_lookup_kernel(path)) ? -VFS_EEXIST : 0;
    if (!r) e4_engine_busy=true;
    spin_unlock_irqrestore(&e4_lock,flags);if (r) return r;
    e4_mount_stage(stage,"workspace-allocation");
    ext4_mount_t *bootstrap=kcalloc(1,sizeof(*bootstrap));
    e4_node_t *node=kcalloc(1,sizeof(*node));ext4_engine_t *engine=NULL;
    jbd2_plan_t *plan=NULL;uint32_t *map=NULL;e4_journal_guard_t guard={.lifecycle=true};
    r=-VFS_ENOMEM;if (!bootstrap || !node) goto fail;
    bootstrap->dev=dev;bootstrap->bs=dev->sector_size;bootstrap->blocks=(uint32_t)dev->sector_count;
    bootstrap->journal_bootstrap=true;jbd2_source_t source;
    e4_mount_stage(stage,"journal-source");
    r=e4_journal_source(bootstrap,&guard,&map,&source);
    if (!r) { e4_mount_stage(stage,"journal-analyze");r=jbd2_analyze(dev,&source,&plan,NULL); }
    kfree(map);map=NULL;e4_discard(bootstrap);bootstrap=NULL;if (r) goto fail;
    block_dev_t preview={.sector_size=dev->sector_size,.sector_count=dev->sector_count,
        .read_sector=e4_preview_sector,.write_sector=e4_preview_write,.flush=e4_preview_flush,.priv=plan};
    e4_mount_stage(stage,"recovery-preview");
    r=e4_open_journal_orphans_owned(&preview,true,&engine,true,true);if (r) goto fail;
    /* Constructor checks complete allocation/reference ownership and orphans.
     * Neither preview adapter nor any preflight is permitted to write. */
    e4_engine_dispose(engine);engine=NULL;
    e4_mount_stage(stage,"journal-replay");
    r=jbd2_replay(plan,true);if (r) goto fail;
    jbd2_release(plan);plan=NULL;
    e4_mount_stage(stage,"reopened-state");
    r=e4_open_journal_orphans_owned(dev,true,&engine,true,true);if (r) goto fail;
    ext4_mount_t *fs=engine->fs;
    e4_mount_stage(stage,"map-allocation");
    fs->rw_map=kmalloc(sizeof(*fs->rw_map)+E4_MAP_MAX*sizeof(e4_extent_t));
    if (!fs->rw_map) { r=-VFS_ENOMEM;goto fail; }
    /* Activate dirty+RECOVER durably before orphan cleanup or publication.
     * A crash during activation has changed no namespace/allocation metadata;
     * explicit fixture mount always analyzes the journal, even if RECOVER=0. */
    e4_mount_stage(stage,"journal-activation");
    r=e4_journal_mark(engine,false);if (r) goto fail;
    e4_mount_stage(stage,"orphan-recovery");
    r=ext4_engine_orphan_recover(engine);if (r) goto fail;
    e4_mount_stage(stage,"final-barrier");
    if (!block_flush(dev)) { engine->tainted=true;e4_cache_drop(fs,true);r=-VFS_EIO;goto fail; }
    fs->engine=engine;fs->journal_mounted=true;
    e4_inode_t root;
    e4_mount_stage(stage,"root-inode");
    r=e4_inode(fs,2,&root);if (r) goto fail;
    if (detached) {
        fs->detached=true;fs->trusted_root=true;
        memcpy(node->node.name,"/",2);memcpy(node->node.path,"/",2);e4_setup(node,&root);
        fs->cached[fs->nodes++]=node;*out=fs;
        e4_mount_stage(stage,"prepared");return 0;
    }
    e4_mount_stage(stage,"publication");
    flags=spin_lock_irqsave(&e4_lock);
    if (e4_active || vfs_lookup_kernel(path)) { r=-VFS_EEXIST;spin_unlock_irqrestore(&e4_lock,flags);goto fail; }
    memcpy(node->node.name,"mnt",4);memcpy(node->node.path,"/mnt",5);node->node.parent=parent;e4_setup(node,&root);
    fs->cached[fs->nodes++]=node;node->node.next=parent->children;parent->children=&node->node;
    e4_active=fs;e4_engine_busy=false;*out=fs;
    spin_unlock_irqrestore(&e4_lock,flags);e4_mount_stage(stage,"mounted");return 0;
fail:
    kfree(map);jbd2_release(plan);e4_discard(bootstrap);kfree(node);
    if (engine) { engine->fs->journal_mounted=false;e4_engine_dispose(engine); }
    flags=spin_lock_irqsave(&e4_lock);e4_engine_busy=false;spin_unlock_irqrestore(&e4_lock,flags);return r;
}
int ext4_mount_journal_rw(block_dev_t *dev,const char *path,ext4_mount_t **out) {
    return e4_mount_journal_admitted(dev,path,true,out,NULL,false);
}
int ext4_mount_journal_fixture_diagnose(block_dev_t *dev,const char *path,
    ext4_journal_admission_t admission,ext4_mount_t **out,const char **stage) {
    return e4_mount_journal_admitted(dev,path,
        admission.disposable_fixture && admission.writable && admission.recovery,out,stage,false);
}
int ext4_mount_journal_fixture(block_dev_t *dev,const char *path,
                               ext4_journal_admission_t admission,ext4_mount_t **out) {
    return ext4_mount_journal_fixture_diagnose(dev,path,admission,out,NULL);
}
static bool e4_mounted_orphan(ext4_mount_t *fs,uint32_t ino) {
    return fs->journal_mounted && e4_orphan_index(fs->engine,ino)>=0;
}
static int e4_journal_refresh(vfs_node_t *node) {
    e4_node_t *n=(e4_node_t *)node;ext4_mount_t *fs=n->inode.fs;e4_inode_t in;
    if (fs->namespace_uncertain && n->opens && n->inode.ino==fs->uncertain_victim.ino) {
        if (n->inode.generation!=fs->uncertain_victim.generation) return -VFS_EIO;
        n->inode=fs->uncertain_victim;return 0;
    }
    if (n->removed && (!n->opens || !e4_mounted_orphan(fs,n->inode.ino)))
        return n->replaced ? -VFS_EAGAIN : -VFS_ENOENT;
    fs->orphan_access=n->removed ? n->inode.ino : 0;int r=e4_inode(fs,n->inode.ino,&in);fs->orphan_access=0;
    if (!r && in.generation!=n->inode.generation) r=-VFS_EIO;
    if (!r) { n->inode=in;node->size=in.size;node->mode=in.full_mode;node->uid=in.uid;node->gid=in.gid; }return r;
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
    if (!fs->frozen && !fs->engine->tainted && !file->opens && e4_mounted_orphan(fs,file->inode.ino)) {
        int r;
#ifdef FORTRESS_EXT4_HEALTH_TEST
        /* Only compiled into the isolated health-probe kernel. No new ABI,
         * device fault, recovery admission or production filename behavior. */
        if (!fs->health_test_failed && !strcmp(node->name,"health-recovery")) {
            fs->health_test_failed=true;r=-VFS_ENOMEM;
        } else
#endif
        r=e4_orphan_cleanup(fs->engine,file->inode.ino,false);
        if (r) {
            if (!fs->cleanup_error) fs->cleanup_error=r;
            fs->engine->orphan_recovery_required=true;
        }
    }
    /* close callback has no error result. A durable failed-cleanup intent stays
     * registered; sync/shutdown returns its retry or taint error. */
    spin_unlock_irqrestore(&e4_lock,flags);
}
static vfs_node_t *e4_jcreate_account(vfs_node_t *parent,const char *name,vfs_node_type_t type,int *error,bool owned,const vfs_create_attrs_t *attrs,const creds_t *actor,bool account) {
    if (!e4_namespace_name(name) || (type!=VFS_FILE && type!=VFS_DIRECTORY)) {
        if (error) *error=-VFS_EINVAL;
        vfs_set_last_create_error(-VFS_EINVAL);return NULL;
    }
    e4_node_t *node=NULL;
    uint64_t flags=spin_lock_irqsave(&e4_lock);int r=e4_state_account(parent,account || e4_account_directory(parent));
    if (!r && !account && e4_account_name(parent,name)) r=-VFS_EROFS;
#if defined(FORTRESS_DAC_ENFORCED) || defined(TEST_PERMISSIONS_ENFORCEMENT)
    vfs_create_attrs_t derived;
    if (!r && actor) {
        r=e4_access_locked(parent,VFS_MAY_WRITE|VFS_MAY_EXEC,actor);
        if (!r) {
            vfs_metadata_t value=e4_value(parent->fs_private);
            r=permission_create_value(&value,type,attrs ? attrs->mode : 0644,actor,&derived);
            if (!r) attrs=&derived;
        }
    }
#else
    (void)actor;
#endif
    ext4_mount_t *fs=((e4_inode_t *)parent->fs_private)->fs;uint32_t ino=0;
    if (!r && (strlen(parent->path)+strlen(name)+1>=VFS_MAX_PATH || !e4_cache_room(fs))) r=-VFS_EFBIG;
    if (!r) {node=kcalloc(1,sizeof(*node));if (!node) r=-VFS_ENOMEM;}
    if (!r) {
        fs->account_allocation=account;
        fs->account_namespace_operation=account;
        r=ext4_engine_namespace_create_attrs(fs->engine,((e4_inode_t *)parent->fs_private)->ino,name,type==VFS_DIRECTORY,attrs,&ino);
        fs->account_namespace_operation=false;
        fs->account_allocation=false;
    }
    e4_inode_t in;
    if (!r) {
        r=e4_inode(fs,ino,&in);
        if (!r) r=e4_journal_refresh(parent);
        if (r) e4_jtaint(fs); /* Known commit; failed cache publication is not rollback. */
    }
    if (!r) {
        e4_setup(node,&in);node->refs=owned ? 1 : 0;node->legacy=!owned;memcpy(node->node.name,name,strlen(name)+1);
        size_t length=!strcmp(parent->path,"/") ? 0 : strlen(parent->path);memcpy(node->node.path,parent->path,length);node->node.path[length]='/';
        memcpy(node->node.path+length+1,name,strlen(name)+1);node->node.parent=parent;
        node->node.next=parent->children;parent->children=&node->node;fs->cached[fs->nodes++]=node;
    }
    spin_unlock_irqrestore(&e4_lock,flags);
    if (error) *error=r;
    if (r) { kfree(node);vfs_set_last_create_error(r);return NULL; }return &node->node;
}
static vfs_node_t *e4_jcreate_common(vfs_node_t *p,const char *n,vfs_node_type_t t,int *e,bool o,const vfs_create_attrs_t *a,const creds_t *c) {
    return e4_jcreate_account(p,n,t,e,o,a,c,false);
}
static vfs_node_t *e4_jcreate(vfs_node_t *parent,const char *name,vfs_node_type_t type) {
    return e4_jcreate_common(parent,name,type,NULL,false,NULL,NULL);
}
static vfs_node_t *e4_jcreate_ref(vfs_node_t *parent,const char *name,vfs_node_type_t type,int *error) {
    return e4_jcreate_common(parent,name,type,error,true,NULL,NULL);
}
static vfs_node_t *e4_jcreate_attrs(vfs_node_t *p,const char *n,vfs_node_type_t t,const vfs_create_attrs_t *a,int *error) {
    return e4_jcreate_common(p,n,t,error,true,a,NULL);
}
static int e4_junlink_account(vfs_node_t *parent,const char *name,bool require_directory,const creds_t *actor,bool account) {
    if (!e4_namespace_name(name)) return -VFS_EINVAL;
    uint64_t flags=spin_lock_irqsave(&e4_lock);int r=e4_state_account(parent,account || e4_account_directory(parent));uint32_t ino=0;e4_inode_t in;
    if (!r && !account && e4_account_name(parent,name)) r=-VFS_EROFS;
    ext4_mount_t *fs=((e4_inode_t *)parent->fs_private)->fs;
    if (!r) { r=e4_journal_refresh(parent);if (!r) r=e4_scan(parent->fs_private,name,0,NULL,&ino); }
    if (r>=0 && !ino) r=-VFS_ENOENT;
    if (r>=0) r=e4_inode(fs,ino,&in);
    if (!r && e4_ns_anchor(fs,ino)) r=-VFS_EROFS;
    if (!r && !account && e4_account_inode(fs,ino)) r=-VFS_EROFS;
    if (!r && require_directory && in.mode!=VFS_S_IFDIR) r=-VFS_ENOTDIR;
#if defined(FORTRESS_DAC_ENFORCED) || defined(TEST_PERMISSIONS_ENFORCEMENT)
    if (!r && actor) {
        r=e4_access_locked(parent,VFS_MAY_WRITE|VFS_MAY_EXEC,actor);
        if (!r) {
            vfs_metadata_t d=e4_value(parent->fs_private),v=e4_value(&in);
            r=permission_delete_value(&d,&v,actor);
        }
    }
#else
    (void)actor;
#endif
    bool attempted=!r;
    if (!r) {
        fs->account_namespace_operation=account;
        r=ext4_engine_namespace_remove(fs->engine,((e4_inode_t *)parent->fs_private)->ino,name,in.mode==0x4000);
        fs->account_namespace_operation=false;
    }
    /* Cleanup can fail after the durable name removal. Retire the cached name
     * on success or a still-registered orphan; do not expose a stale dentry. */
    if (!r || (attempted && ino && e4_orphan_index(fs->engine,ino)>=0)) {
        vfs_node_t **link=&parent->children;
        while (*link) {
            e4_node_t *node=(e4_node_t *)*link;
            if (node->inode.ino==ino) {
                node->removed=true;*link=node->node.next;node->node.next=NULL;
                e4_collect_removed(node);break;
            }
            link=&(*link)->next;
        }
        if (e4_journal_refresh(parent)) { e4_jtaint(fs);if (!r) r=-VFS_EIO; }
    }
    spin_unlock_irqrestore(&e4_lock,flags);return r;
}
static int e4_junlink_common(vfs_node_t *p,const char *n,bool d,const creds_t *c) {
    return e4_junlink_account(p,n,d,c,false);
}
static int e4_junlink(vfs_node_t *parent,const char *name) {
    return e4_junlink_common(parent,name,false,NULL);
}
static int e4_jrename_account(vfs_node_t *old,const char *name,vfs_node_t *dest,const char *next,bool require_directory,const creds_t *actor,bool account) {
    if (!e4_namespace_name(name) || !e4_namespace_name(next)) return -VFS_EINVAL;
    e4_move_cache_t *publication=NULL;
    uint64_t flags=spin_lock_irqsave(&e4_lock);int r=e4_state_account(old,account || e4_account_directory(old));
    if (!r) r=e4_state_account(dest,account || e4_account_directory(dest));
    if (!r && !account && (e4_account_name(old,name) || e4_account_name(dest,next))) r=-VFS_EROFS;
    ext4_mount_t *fs=((e4_inode_t *)old->fs_private)->fs;
    if (!r && fs!=((e4_inode_t *)dest->fs_private)->fs) r=-VFS_EOPNOTSUPP;
    if (!r && strlen(dest->path)+strlen(next)+1>=VFS_MAX_PATH) r=-VFS_EFBIG;
    uint32_t source=0;e4_inode_t in;
    if (!r) {r=e4_journal_refresh(old);if (!r) r=e4_scan(old->fs_private,name,0,NULL,&source);if (r>=0) r=source ? e4_inode(fs,source,&in) : -VFS_ENOENT;}
    if (!r && !account && e4_account_inode(fs,source)) r=-VFS_EROFS;
    if (!r && e4_ns_anchor(fs,source)) r=-VFS_EROFS;
    uint32_t victim=0;bool victim_scanned=false;
    if (!r && require_directory && in.mode!=VFS_S_IFDIR) r=-VFS_ENOTDIR;
    if(!r && in.mode==VFS_S_IFDIR && account)r=-VFS_EOPNOTSUPP;
#if defined(FORTRESS_DAC_ENFORCED) || defined(TEST_PERMISSIONS_ENFORCEMENT)
    if(!r && actor && in.mode==VFS_S_IFDIR && old!=dest) {
        vfs_metadata_t value=e4_value(&in);r=permission_access_value(&value,VFS_MAY_WRITE,actor);
    }
    if (!r && actor) {
        r=e4_access_locked(old,VFS_MAY_WRITE|VFS_MAY_EXEC,actor);
        if (!r) r=e4_access_locked(dest,VFS_MAY_WRITE|VFS_MAY_EXEC,actor);
        if (!r) {
            vfs_metadata_t d=e4_value(old->fs_private),v=e4_value(&in);
            r=permission_delete_value(&d,&v,actor);
        }
        if (!r) {r=e4_scan(dest->fs_private,next,0,NULL,&victim);if (r>=0) r=0;victim_scanned=true;}
        if (!r && victim) {
            e4_inode_t target;r=e4_inode(fs,victim,&target);
            if (!r) {vfs_metadata_t d=e4_value(dest->fs_private),v=e4_value(&target);r=permission_delete_value(&d,&v,actor);}
        }
    }
#else
    (void)actor;
#endif
    if (!r && !victim_scanned) {r=e4_scan(dest->fs_private,next,0,NULL,&victim);if (r>=0) r=0;}
    if (!r && !account && victim && e4_account_inode(fs,victim)) r=-VFS_EROFS;
    if (!r && victim && e4_ns_anchor(fs,victim)) r=-VFS_EROFS;
    if (!r && old==dest && !strcmp(name,next)) goto done;
    if(!r&&in.mode==VFS_S_IFDIR)r=e4_move_ancestry(fs,source,((e4_inode_t *)dest->fs_private)->ino);
    if(!r&&victim)r=-VFS_EEXIST;
    if(!r)r=e4_move_cache_prepare(fs,old,name,dest,next,source,in.mode==VFS_S_IFDIR,&publication);
    if (!r) {
        fs->account_allocation=account;
        fs->account_namespace_operation=account;
        r=ext4_engine_namespace_rename(fs->engine,((e4_inode_t *)old->fs_private)->ino,name,
                                     ((e4_inode_t *)dest->fs_private)->ino,next);
        fs->account_namespace_operation=false;
        fs->account_allocation=false;
    }
    if (!r) {
        if (account && ((e4_inode_t *)dest->fs_private)->ino==fs->account_inodes[0]) {
            const char *names[3]={"passwd","group","shadow"};
            for (unsigned i=0;i<3;i++) if (!strcmp(next,names[i])) fs->account_inodes[i+1]=source;
        }
        if (e4_journal_refresh(old) || (dest!=old && e4_journal_refresh(dest))) {
            e4_jtaint(fs);if(in.mode==VFS_S_IFDIR)fs->namespace_uncertain=true;r=-VFS_EIO;
        } else e4_move_cache_publish(fs,old,dest,next,publication);
    }
done:
    spin_unlock_irqrestore(&e4_lock,flags);kfree(publication);return r;
}
static int e4_jrename_common(vfs_node_t *o,const char *n,vfs_node_t *d,const char *s,bool r,const creds_t *c) {
    return e4_jrename_account(o,n,d,s,r,c,false);
}
static int e4_jrename(vfs_node_t *old,const char *name,vfs_node_t *dest,const char *next) {
    return e4_jrename_common(old,name,dest,next,false,NULL);
}
static int e4_journal_drain(ext4_mount_t *fs,bool finish) {
    spin_debug_assert_held(&e4_lock);ext4_engine_t *e=fs->engine;
    if (e->tainted || fs->quiesced) return -VFS_EIO;
    if (e->ready || jbd2_writer_state(e->writer)!=JBD2_WRITE_IDLE) return -VFS_EAGAIN;
    if (!e->dirty) return block_flush(fs->dev) ? 0 : (e4_jtaint(fs),-VFS_EIO);
    int r=ext4_engine_orphan_recover(e);if (r) return r;
    if (finish && e->orphan_count) return -VFS_EAGAIN;
    if (!block_flush(fs->dev)) { e4_jtaint(fs);return -VFS_EIO; }
    /* Sync retains dirty+RECOVER and live pins. Clean marker is a final bounded
     * journal transaction only AFTER all work/checkpoints and this barrier. */
    /* Report a lost close error once even if this retry reclaimed everything.
     * Do not mark clean on that call; shutdown/freeze can explicitly retry. */
    if (fs->cleanup_error) {int error=fs->cleanup_error;fs->cleanup_error=0;return error;}
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

int ext4_prepare_root(block_dev_t *dev,bool admitted,ext4_mount_t **out) {
    return ext4_prepare_root_diagnose(dev,admitted,out,NULL);
}
int ext4_prepare_root_diagnose(block_dev_t *dev,bool admitted,ext4_mount_t **out,const char **stage) {
    int r=vfs_boot_begin_root_preparation();
    if (r) { if (out) *out=NULL;return r; }
    r=e4_mount_journal_admitted(dev,"/",admitted,out,stage,true);
    if (r) vfs_boot_cancel_root_preparation();
    return r;
}
int ext4_prepare_installer_root(block_dev_t *dev,bool exclusive,ext4_mount_t **out,const char **stage) {
    if (!exclusive || !out) {if (out) *out=NULL;return -VFS_EACCES;}
    int r=e4_mount_journal_admitted(dev,"/",true,out,stage,true);
    if (!r) (*out)->installer_owned=true;
    return r;
}
int ext4_root_lock_accounts(ext4_mount_t *fs,vfs_node_t *nodes[4]) {
    if (!fs || !fs->detached || !nodes) return -VFS_EINVAL;
    uint64_t flags=spin_lock_irqsave(&e4_lock);int r=0;
    for (unsigned i=0;i<4;i++)
        if (!nodes[i] || !nodes[i]->fs_private || ((e4_inode_t *)nodes[i]->fs_private)->fs!=fs) r=-VFS_EINVAL;
    if (!r) for (unsigned i=0;i<4;i++) fs->account_inodes[i]=((e4_inode_t *)nodes[i]->fs_private)->ino;
    spin_unlock_irqrestore(&e4_lock,flags);return r;
}
int ext4_sync_installed_root(bool *handled) {
    spin_debug_assert_unheld();uint64_t flags=spin_lock_irqsave(&e4_lock);
    ext4_mount_t *fs=e4_active && e4_active->trusted_root ? e4_active : NULL;
    spin_unlock_irqrestore(&e4_lock,flags);*handled=fs!=NULL;return fs ? ext4_sync(fs) : 0;
}
int ext4_freeze_installed_root(bool *handled) {
    spin_debug_assert_unheld();uint64_t flags=spin_lock_irqsave(&e4_lock);
    ext4_mount_t *fs=e4_active && e4_active->trusted_root ? e4_active : NULL;
    spin_unlock_irqrestore(&e4_lock,flags);*handled=fs!=NULL;return fs ? ext4_freeze_and_sync(fs) : 0;
}
int ext4_quiesce_installed_root(bool (*stop)(void)) {
    spin_debug_assert_unheld();uint64_t flags=spin_lock_irqsave(&e4_lock);
    ext4_mount_t *fs=e4_active && e4_active->trusted_root ? e4_active : NULL;
    int r=!fs || !stop ? -VFS_EINVAL : fs->engine->tainted ? -VFS_EIO :
        !fs->frozen || fs->engine->dirty ? -VFS_EAGAIN : 0;
    if (!r) {fs->quiesced=true;if (!stop()) {e4_jtaint(fs);r=-VFS_EIO;}}
    spin_unlock_irqrestore(&e4_lock,flags);return r;
}
vfs_node_t *ext4_prepared_root(ext4_mount_t *fs) {
    return fs && fs->detached && fs->nodes ? &fs->cached[0]->node : NULL;
}
int ext4_publish_root(ext4_mount_t *fs) {
    if (!fs || !fs->detached || fs->installer_owned || e4_active) return -VFS_EINVAL;
    int r=vfs_boot_publish_root(&fs->cached[0]->node);
    if (r) return r;
    fs->detached=false;e4_active=fs;e4_engine_busy=false;return 0;
}
int ext4_discard_prepared_root(ext4_mount_t *fs) {
    if (!fs || !fs->detached) return -VFS_EINVAL;
    for (unsigned i=0;i<fs->nodes;i++)
        if (fs->cached[i]->refs || fs->cached[i]->opens || fs->cached[i]->legacy) return -VFS_EAGAIN;
    bool installer_owned=fs->installer_owned;
    fs->journal_mounted=false;e4_engine_dispose(fs->engine);e4_engine_busy=false;
    if (!installer_owned) vfs_boot_cancel_root_preparation();
    return 0;
}
