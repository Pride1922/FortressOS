/* EH-2: existing regular-file replacement. One metadata activation, no data
 * reclamation here. The mounted adapter publishes names before later cleanup. */
static int e4_replace_entry(ext4_engine_t *e,e4_inode_t *dir,const char *name,
                            uint32_t expected,uint32_t next) {
    ext4_mount_t *fs=e->fs;uint32_t found=0;dir->map=NULL;
    int r=e4_scan(dir,name,0,NULL,&found);if(r<0)return r;
    if(found!=expected)return -VFS_EIO;
    for(uint32_t l=0;l<dir->size/fs->bs;l++) {
        uint32_t physical;r=e4_dir_block(dir,l,&physical);if(r)return r;
        if(!e4_bytes_kind(fs,(uint64_t)physical*fs->bs,fs->scratch,fs->bs,E4_PR_DIRECTORY))return -VFS_EIO;
        for(unsigned at=0;at<fs->bs-12;) {
            uint8_t *d=fs->scratch+at;unsigned rec=e4_u16(d+4);
            if(e4_u32(d)==expected && d[6]==strlen(name) && !memcmp(d+8,name,d[6])) {
                uint8_t *b=e4_stage(e,physical,E4_REFERENCE);if(!b)return e->error;
                if(e4_u32(b+at)!=expected)return -VFS_EIO;
                e4_p32(b+at,next);e4_dir_sum(dir,b);return 0;
            }
            at+=rec;
        }
    }
    return -VFS_EIO;
}
static int e4_replace_stamp(ext4_engine_t *e,uint32_t ino,bool directory,uint64_t now) {
    uint8_t *raw=e4_inode_image(e,ino);if(!raw)return e->error;
    if(now>INT64_MAX || !ext4_inode_time_set(raw,EXT4_CTIME,(int64_t)now,0) ||
        (directory && !ext4_inode_time_set(raw,EXT4_MTIME,(int64_t)now,0)))return -VFS_EOPNOTSUPP;
    e4_inode_sum(e->fs,ino,raw);return 0;
}
static int e4_replace_activate(ext4_engine_t *e,uint32_t old_parent,const char *name,
                              uint32_t new_parent,const char *next,uint32_t source,uint32_t victim) {
    int r=e4_orphan_begin(e);if(r)return r;
    e4_inode_t old,dest,in;uint8_t *raw;uint32_t found=0;
    r=e4_namespace_parent(e,old_parent,&old);if(r)goto done;
    r=e4_namespace_parent(e,new_parent,&dest);if(r)goto done;
    r=e4_scan(&old,name,0,NULL,&found);if(r<0)goto done;
    if(found!=source) {r=-VFS_ENOENT;goto done;}
    r=e4_scan(&dest,next,0,NULL,&found);if(r<0)goto done;
    if(found!=victim) {r=-VFS_ENOENT;goto done;}
    r=e4_load_plan(e,source,&in,&raw);if(r)goto done;
    if(in.mode!=VFS_S_IFREG) {r=-VFS_EOPNOTSUPP;goto done;}
    r=e4_load_plan(e,victim,&in,&raw);if(r)goto done;
    if(in.mode!=VFS_S_IFREG) {r=-VFS_EOPNOTSUPP;goto done;}
    r=e4_orphan_add(e,victim,raw);if(r)goto done;
    e4_p16(raw+26,0);e4_inode_sum(e->fs,victim,raw);
    r=e4_replace_entry(e,&dest,next,victim,source);if(r)goto done;
    r=e4_replace_entry(e,&old,name,source,0);if(r)goto done;
    uint64_t now=time_get_epoch_seconds();if(!now)now=1726500000;
    r=e4_replace_stamp(e,source,false,now);if(r)goto done;
    r=e4_replace_stamp(e,victim,false,now);if(r)goto done;
    r=e4_replace_stamp(e,old_parent,true,now);if(r)goto done;
    if(new_parent!=old_parent) {r=e4_replace_stamp(e,new_parent,true,now);if(r)goto done;}
    r=e4_plan_end(e);if(r)goto done;
    e4_seal(e);if(e->error) {r=e->error;goto done;}
    /* No frees/data images: credits and writer allocations precede the first
     * barrier. Source bytes must be durable before publishing either name. */
    e->revoke_count=0;e->credits=(jbd2_credits_t){e->images,0,0};
    r=jbd2_writer_begin(e->writer,e->credits);if(r)goto done;
    r=e4_transaction_commit_ordered(e,true);
    if(!r) {e->dirty=true;e4_orphan_added(e,victim);}
done:
    e->file_planning=false;
    if(r)ext4_engine_abort(e);
    e4_semantic_invalidate(e);return r;
}
static int e4_jreplace(vfs_node_t *old,const char *name,vfs_node_t *dest,const char *next,const creds_t *actor) {
    if(!e4_namespace_name(name)||!e4_namespace_name(next))return -VFS_EINVAL;
    if(dest->replace!=e4_jreplace)return -VFS_EOPNOTSUPP;
    uint64_t flags=spin_lock_irqsave(&e4_lock);
    ext4_mount_t *fs=((e4_inode_t *)old->fs_private)->fs;
    int r=fs!=((e4_inode_t *)dest->fs_private)->fs ? -VFS_EOPNOTSUPP : e4_state(old);
    if(!r)r=e4_state(dest);
    if(!r && (e4_account_name(old,name)||e4_account_name(dest,next)))r=-VFS_EROFS;
    if(!r && strlen(dest->path)+strlen(next)+1>=VFS_MAX_PATH)r=-VFS_EFBIG;
    uint32_t source=0,victim=0;e4_inode_t src={0},dst={0};
    if(!r) {r=e4_journal_refresh(old);if(!r)r=e4_scan(old->fs_private,name,0,NULL,&source);if(r>=0)r=source ? e4_inode(fs,source,&src) : -VFS_ENOENT;}
    if(!r) {r=e4_journal_refresh(dest);if(!r)r=e4_scan(dest->fs_private,next,0,NULL,&victim);if(r>=0)r=victim ? e4_inode(fs,victim,&dst) : -VFS_ENOENT;}
    if(!r && (e4_account_inode(fs,source)||e4_account_inode(fs,victim)))r=-VFS_EROFS;
    if(!r && (src.mode!=VFS_S_IFREG || dst.mode!=VFS_S_IFREG))r=-VFS_EOPNOTSUPP;
#if defined(FORTRESS_DAC_ENFORCED) || defined(TEST_PERMISSIONS_ENFORCEMENT)
    if(!r && actor) {
        r=e4_access_locked(old,VFS_MAY_WRITE|VFS_MAY_EXEC,actor);
        if(!r)r=e4_access_locked(dest,VFS_MAY_WRITE|VFS_MAY_EXEC,actor);
        if(!r) {vfs_metadata_t d=e4_value(old->fs_private),v=e4_value(&src);r=permission_delete_value(&d,&v,actor);}
        if(!r) {vfs_metadata_t d=e4_value(dest->fs_private),v=e4_value(&dst);r=permission_delete_value(&d,&v,actor);}
    }
#else
    if(actor)r=-VFS_EOPNOTSUPP;
#endif
    if(!r && old==dest && !strcmp(name,next))goto done;
    if(!r && (source==victim || source<11 || victim<11 || source==fs->journal_ino || victim==fs->journal_ino))r=-VFS_EOPNOTSUPP;
    if(!r)for(unsigned i=0;i<fs->nodes;i++)if(fs->cached[i]->inode.ino==source && fs->cached[i]->opens) {r=-VFS_EAGAIN;break;}
    if(!r && e4_orphan_pinned(fs->engine,source))r=-VFS_EAGAIN;
    e4_node_t *published=NULL;
    if(!r && !e4_cache_room(fs))r=-VFS_EFBIG;
    if(!r) {
        published=kcalloc(1,sizeof(*published));if(!published)r=-VFS_ENOMEM;
        else {
            e4_setup(published,&src);published->node.parent=dest;
            memcpy(published->node.name,next,strlen(next)+1);
            size_t len=!strcmp(dest->path,"/") ? 0 : strlen(dest->path);
            memcpy(published->node.path,dest->path,len);published->node.path[len]='/';
            memcpy(published->node.path+len+1,next,strlen(next)+1);
        }
    }
    if(!r) {
        r=e4_replace_activate(fs->engine,((e4_inode_t *)old->fs_private)->ino,name,
                              ((e4_inode_t *)dest->fs_private)->ino,next,source,victim);
        if(r && fs->engine->tainted) {fs->uncertain_victim=dst;fs->namespace_uncertain=true;}
        if(!r) {
            /* Activation is complete. Cleanup is deliberately deferred to
             * final close/sync/freeze; its errors cannot undo this success. */
            vfs_node_t **link=&dest->children;
            while(*link) {
                e4_node_t *n=(e4_node_t *)*link;
                if(n->inode.ino==victim) {*link=n->node.next;n->node.next=NULL;n->removed=true;n->replaced=true;e4_collect_removed(n);break;}
                link=&(*link)->next;
            }
            link=&old->children;
            while(*link) {
                e4_node_t *n=(e4_node_t *)*link;
                if(n->inode.ino==source) {*link=n->node.next;n->node.next=NULL;n->removed=true;e4_collect_removed(n);break;}
                link=&(*link)->next;
            }
            /* An in-flight open may hold an owned source lookup reference.
             * Retire that dentry rather than letting its later open acquire a
             * writer after the source-open check. Only the fresh destination
             * entry admits future opens of the published inode. */
            published->node.next=dest->children;dest->children=&published->node;
            fs->cached[fs->nodes++]=published;published=NULL;
            /* Parent inode sizes/mappings did not change. Timestamp fields
             * are not cached. Publication needs no fallible post-commit read. */
        }
    }
    kfree(published);
done:
    spin_unlock_irqrestore(&e4_lock,flags);return r;
}
