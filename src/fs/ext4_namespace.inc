/* Phase 8.3: exclusive namespace planner. No mounted VFS state is published.
 * Existing E4-A directory/allocator/checksum helpers remain shared; publication
 * always uses the semantic JBD2 boundary, never e4_publish/direct home I/O. */
int ext4_engine_open_journal_namespace(block_dev_t *dev,bool admitted,ext4_engine_t **out) {
    int r=ext4_engine_open_journal_files(dev,admitted,out);
    if (!r) (*out)->namespace_enabled=true;
    return r;
}
static bool e4_namespace_name(const char *name) {
    if (!name) return false;
    unsigned n=0;
    while (n<VFS_MAX_NAME && name[n]) { if (name[n]=='/') return false;n++; }
    return n && n<VFS_MAX_NAME && strcmp(name,".") && strcmp(name,"..");
}
static int e4_namespace_begin(ext4_engine_t *e) {
    e4_engine_owner(e);
    if (!e || !e->namespace_enabled) return -VFS_EOPNOTSUPP;
    if (e->tainted) return -VFS_EIO;
    if (e->orphan_recovery_required) return -VFS_EAGAIN;
    if (e->ready || jbd2_writer_state(e->writer)!=JBD2_WRITE_IDLE) return -VFS_EAGAIN;
    e->file_planning=true;
    int r=e4_plan_begin(e);
    if (r) e->file_planning=false;
    return r;
}
static int e4_namespace_end(ext4_engine_t *e,int r) {
    e->file_planning=false;
    if (!r) r=e4_semantic_commit(e);
    if (r) ext4_engine_abort(e);
    e4_semantic_invalidate(e);return r;
}
/* Validate the whole directory and its dot ownership before any edit. */
static int e4_namespace_parent(ext4_engine_t *e,uint32_t ino,e4_inode_t *in) {
    if (ino==e->fs->journal_ino) return -VFS_EOPNOTSUPP;
    int r=e4_inode(e->fs,ino,in);if (r) return r;
    if (in->mode!=0x4000) return -VFS_EINVAL;
    uint32_t self,parent;vfs_dirent_t entry;
    r=e4_scan(in,".",0,&entry,&self);if (r<0) return r;
    if (self!=ino || entry.type!=VFS_DIRECTORY) return -VFS_EIO;
    r=e4_scan(in,"..",0,&entry,&parent);if (r<0) return r;
    if (!parent || entry.type!=VFS_DIRECTORY || (ino==2 && parent!=2)) return -VFS_EIO;
    e4_inode_t p;r=e4_inode(e->fs,parent,&p);
    if (r) return r;
    if (p.mode!=0x4000) return -VFS_EIO;
    uint8_t *raw=e4_inode_image(e,ino);
    if (!raw) return e->error;
    return e4_u16(raw+26)>=2 ? 0 : -VFS_EIO;
}
int ext4_engine_namespace_create(ext4_engine_t *e,uint32_t parent,const char *name,bool directory,uint32_t *out) {
    if (!out || !e4_namespace_name(name)) return -VFS_EINVAL;
    int r=e4_namespace_begin(e);if (r) return r;
    e4_inode_t dir;uint32_t found=0,ino=0;
    r=e4_namespace_parent(e,parent,&dir);if (r) goto done;
    r=e4_scan(&dir,name,0,NULL,&found);if (r<0) goto done;
    if (found) { r=-VFS_EEXIST;goto done; }
    /* Reserve uses plan_begin: parent validation images above are read-only.
     * The fresh plan owns every allocation and the namespace link together. */
    r=e4_reserve_inode_unlocked(e,directory,&ino);if (r) goto done;
    uint8_t *raw=e4_inode_image(e,ino);if (!raw) { r=e->error;goto done; }
    e4_p16(raw+26,directory ? 2 : 1);
    e4_inode_t in={.fs=e->fs,.ino=ino,.generation=e4_u32(raw+100),.mode=directory ? 0x4000 : 0x8000};
    if (directory) {
        uint32_t block=e4_allocate(e);uint8_t *b=block ? e4_stage_zero(e,block) : NULL;
        if (!b) { r=e->error;goto done; }
        e4_dir_entry(b,ino,12,".",2);e4_dir_entry(b+12,parent,e->fs->bs-24,"..",2);e4_dir_sum(&in,b);
        e4_header(raw+40,1,4,0);e4_extent_t x={0,block,1,false};e4_leaf(raw+52,&x);
        e4_p32(raw+4,e->fs->bs);e4_p32(raw+28,e->fs->bs/512);
        uint8_t *praw=e4_inode_image(e,parent);if (!praw) { r=e->error;goto done; }
        if (e4_u16(praw+26)==UINT16_MAX) { r=-VFS_EFBIG;goto done; }
        e4_p16(praw+26,(uint16_t)(e4_u16(praw+26)+1));e4_inode_sum(e->fs,parent,praw);
    }
    e4_inode_sum(e->fs,ino,raw);
    r=e4_dir_edit(e,&dir,name,ino,directory ? 2 : 1);
    e->inode_delta=0;e->reserve_ino=0;
done:
    r=e4_namespace_end(e,r);if (!r) *out=ino;return r;
}
int ext4_engine_namespace_remove(ext4_engine_t *e,uint32_t parent,const char *name,bool directory) {
    if (e && e->orphan_enabled) return ext4_engine_orphan_unlink(e,parent,name,directory);
    if (!e4_namespace_name(name)) return -VFS_EINVAL;
    int r=e4_namespace_begin(e);if (r) return r;
    e4_inode_t dir,in;uint32_t ino=0;
    r=e4_namespace_parent(e,parent,&dir);if (r) goto done;
    r=e4_scan(&dir,name,0,NULL,&ino);if (r<0) goto done;
    if (!ino) { r=-VFS_ENOENT;goto done; }
    if (ino<11 || ino==e->fs->journal_ino) { r=-VFS_EOPNOTSUPP;goto done; }
    for (unsigned i=0;i<e->fs->nodes;i++) if (e->fs->cached[i]->inode.ino==ino && e->fs->cached[i]->opens) {
        r=-VFS_EOPNOTSUPP;goto done;
    }
    uint8_t *raw;r=e4_load_plan(e,ino,&in,&raw);if (r) goto done;
    if (in.mode!=(directory ? 0x4000 : 0x8000)) { r=-VFS_EOPNOTSUPP;goto done; }
    if (directory) {
        bool dot=false,dotdot=false;uint32_t child;vfs_dirent_t ent;
        for (uint64_t cookie=0;;cookie++) {
            r=e4_scan(&in,NULL,cookie,&ent,&child);if (r<0) goto done;
            if (!r) break;
            if (!strcmp(ent.name,".")) {
                if (dot || child!=ino || ent.type!=VFS_DIRECTORY) { r=-VFS_EIO;goto done; }dot=true;
            } else if (!strcmp(ent.name,"..")) {
                if (dotdot || child!=parent || ent.type!=VFS_DIRECTORY) { r=-VFS_EIO;goto done; }dotdot=true;
            } else { r=-VFS_ENOTEMPTY;goto done; }
        }
        if (!dot || !dotdot || e4_u16(raw+26)!=2) { r=-VFS_EIO;goto done; }
        /* Directory scans may reuse traversal arrays; reload before capture. */
        r=e4_load_plan(e,ino,&in,&raw);if (r) goto done;
    }
    /* Snapshot the entire bounded free set before parent edits reuse traversal
     * state. Large cleanup requires Phase 8.4, never a partially freed name. */
    uint64_t freed=e->old_count;
    for (unsigned i=0;i<e->fs->pending_count;i++) freed+=e->fs->pending[i].len;
    if (freed>64) { r=-VFS_EFBIG;goto done; }
    for (unsigned i=0;i<e->fs->pending_count;i++)
        if (!e4_release(e,e->fs->pending[i].physical,e->fs->pending[i].len)) { r=e->error;goto done; }
    for (unsigned i=0;i<e->old_count;i++) if (!e4_release(e,e->old_nodes[i],1)) { r=e->error;goto done; }
    uint32_t g=(ino-1)/e->fs->ipg,bit=(ino-1)%e->fs->ipg;
    uint8_t *map=e4_stage_bitmap(e,g,true);if (!map) { r=e->error;goto done; }
    e4_set_bit(map,bit,false);if (!e4_account(e,g,true,1)) { r=e->error;goto done; }
    e->release_ino=ino;e->release_dir=directory;
    if (directory) {
        uint8_t *d=e4_descriptor(e,g),*praw=e4_inode_image(e,parent);
        if (!d || !praw) { r=e->error;goto done; }
        if (!e4_u16(d+16) || e4_u16(praw+26)<3) { r=-VFS_EIO;goto done; }
        e4_p16(d+16,(uint16_t)(e4_u16(d+16)-1));e4_p16(praw+26,(uint16_t)(e4_u16(praw+26)-1));
        e4_inode_sum(e->fs,parent,praw);
    }
    uint32_t generation=in.generation;memset(raw,0,256);e4_p32(raw+100,generation);e4_p16(raw+128,32);
    e4_inode_sum(e->fs,ino,raw);
    r=e4_dir_edit(e,&dir,name,0,0);
done:
    return e4_namespace_end(e,r);
}
int ext4_engine_namespace_rename(ext4_engine_t *e,uint32_t old_parent,const char *old_name,uint32_t new_parent,const char *new_name) {
    if (!e4_namespace_name(old_name) || !e4_namespace_name(new_name)) return -VFS_EINVAL;
    int r=e4_namespace_begin(e);if (r) return r;
    e4_inode_t old,dest,in;uint32_t ino=0,existing=0;
    r=e4_namespace_parent(e,old_parent,&old);if (r) goto done;
    r=e4_namespace_parent(e,new_parent,&dest);if (r) goto done;
    r=e4_scan(&old,old_name,0,NULL,&ino);if (r<0) goto done;
    if (!ino) { r=-VFS_ENOENT;goto done; }
    if (ino<11 || ino==e->fs->journal_ino) { r=-VFS_EOPNOTSUPP;goto done; }
    r=e4_inode(e->fs,ino,&in);if (r) goto done;
    if (in.mode!=0x8000) { r=-VFS_EOPNOTSUPP;goto done; }
    if (old_parent==new_parent && !strcmp(old_name,new_name)) {
        ext4_engine_abort(e);e->file_planning=false;e4_semantic_invalidate(e);return 0;
    }
    r=e4_scan(&dest,new_name,0,NULL,&existing);if (r<0) goto done;
    if (existing) { r=-VFS_EEXIST;goto done; }
    uint8_t *raw;r=e4_load_plan(e,ino,&in,&raw);if (r) goto done;
    r=e4_dir_edit(e,&dest,new_name,ino,1);
    if (!r) r=e4_dir_edit(e,&old,old_name,0,0);
done:
    return e4_namespace_end(e,r);
}
