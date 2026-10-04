/* E4-A synchronous VFS mutations. Included after the bounded engine. */
static int e4_rw_workspace(ext4_mount_t *fs) {
    ext4_engine_t *e=kcalloc(1,sizeof(*e));
    e4_map_t *map=kmalloc(sizeof(*map)+E4_MAP_MAX*sizeof(e4_extent_t));
    if (!e || !map) { kfree(e); kfree(map); return -VFS_ENOMEM; }
    while (fs->maps) { e4_map_t *old=fs->maps; fs->maps=old->next; kfree(old); }
    fs->map_entries=0; fs->engine=e; fs->rw_map=map; e->fs=fs;
    return 0;
}
static int e4_state(vfs_node_t *node) {
    if (!node || !node->fs_private) return -VFS_EINVAL;
    e4_node_t *n=(e4_node_t *)node;
    if (n->removed) return -VFS_ENOENT;
    ext4_mount_t *fs=n->inode.fs;
    if (fs->engine && fs->engine->tainted) return -VFS_EIO;
    return !fs->engine || fs->frozen ? -VFS_EROFS : 0;
}
static int e4_can_write(vfs_node_t *node) {
    uint64_t f=spin_lock_irqsave(&e4_lock); int r=e4_state(node);
    spin_unlock_irqrestore(&e4_lock,f); return r;
}
static int e4_open(vfs_node_t *node) {
    uint64_t f=spin_lock_irqsave(&e4_lock); e4_node_t *n=(e4_node_t *)node;
    int r=n->removed ? -VFS_ENOENT : n->opens==UINT32_MAX ? -VFS_EFBIG : 0;
    if (!r) n->opens++;
    spin_unlock_irqrestore(&e4_lock,f); return r;
}
static void e4_close(vfs_node_t *node) {
    uint64_t f=spin_lock_irqsave(&e4_lock); e4_node_t *n=(e4_node_t *)node;
    if (n->opens) n->opens--;
    spin_unlock_irqrestore(&e4_lock,f);
}
static int e4_load_plan(ext4_engine_t *e,uint32_t ino,e4_inode_t *in,uint8_t **raw) {
    int r=e4_inode(e->fs,ino,in); if (r) { e->error=r; return r; }
    *raw=e4_inode_image(e,ino); if (!*raw) return e->error;
    if (in->mode==0x8000 && e4_u16(*raw+26)!=1) return e->error=-VFS_EOPNOTSUPP;
    ext4_mount_t *fs=e->fs;
    fs->pending_count=fs->physical_count=fs->visits=0; e->old_count=0;
    unsigned depth=e4_u16(in->extent+6);
    if (depth>2) return e->error=-VFS_EOPNOTSUPP;
    r=e4_tree(in,in->extent,depth,true,0,1ULL<<32);
    if (r) return e->error=r;
    if (!e4_old_tree(e,in->extent,depth)) return e->error;
    uint64_t blocks=e->old_count;
    for (unsigned i=0;i<fs->pending_count;i++) blocks+=fs->pending[i].len;
    if (blocks*(fs->bs/512)!=e4_u32(*raw+28)) return e->error=-VFS_EIO;
    return 0;
}
/* Insert a hole or split an unwritten run around one initialized block. */
static uint32_t e4_initialize(ext4_engine_t *e,uint32_t logical,bool *zero) {
    ext4_mount_t *fs=e->fs; unsigned at=0;
    while (at<fs->pending_count && (uint64_t)fs->pending[at].logical+fs->pending[at].len<=logical) at++;
    e4_extent_t x={0}; bool exists=at<fs->pending_count && fs->pending[at].logical<=logical;
    if (exists) x=fs->pending[at];
    if (exists && !x.unwritten) { *zero=false; return x.physical+logical-x.logical; }
    unsigned before=exists ? logical-x.logical : 0, after=exists ? x.len-before-1 : 0;
    unsigned n=1+(before!=0)+(after!=0), added=n-(exists ? 1 : 0);
    if (added>E4_MAP_MAX-fs->pending_count) { e->error=-VFS_EFBIG; return 0; }
    uint32_t block=exists ? x.physical+before : e4_allocate(e);
    if (!block) return 0;
    for (unsigned i=fs->pending_count;i>at+(exists ? 1u : 0u);i--) fs->pending[i+added-1]=fs->pending[i-1];
    unsigned pos=at;
    if (before) { x.len=before; fs->pending[pos++]=x; }
    fs->pending[pos++]=(e4_extent_t){logical,block,1,false};
    if (after) fs->pending[pos]=(e4_extent_t){logical+1,block+1,after,true};
    fs->pending_count+=added; *zero=true; return block;
}
static void e4_merge_pending(ext4_mount_t *fs) {
    unsigned used=0;
    for (unsigned i=0;i<fs->pending_count;i++) {
        e4_extent_t x=fs->pending[i];
        if (used && fs->pending[used-1].unwritten==x.unwritten &&
            (uint64_t)fs->pending[used-1].logical+fs->pending[used-1].len==x.logical &&
            fs->pending[used-1].physical+fs->pending[used-1].len==x.physical &&
            fs->pending[used-1].len+x.len<=(x.unwritten ? 32767u : 32768u)) fs->pending[used-1].len+=x.len;
        else fs->pending[used++]=x;
    }
    fs->pending_count=used;
}
static void e4_invalidate(ext4_mount_t *fs) {
    for (unsigned i=0;i<fs->nodes;i++) fs->cached[i]->inode.map=NULL;
    fs->validation_valid=false; fs->rw_ino=0;
}
static int e4_refresh(vfs_node_t *node) {
    e4_node_t *n=(e4_node_t *)node; e4_inode_t in;
    int r=e4_inode(n->inode.fs,n->inode.ino,&in);
    if (r) { n->inode.fs->engine->tainted=true; return r; }
    n->inode=in; node->size=in.size; return 0;
}
static int e4_publish(ext4_engine_t *e) {
    int r=e4_plan_end(e); if (!r) r=e4_commit_unlocked(e);
    e4_invalidate(e->fs); e->ready=false; return r;
}
static int64_t e4_write(vfs_node_t *node,uint64_t *offset,bool append,const void *data,size_t len) {
    if (!offset || (!data && len)) return -VFS_EINVAL;
    spin_debug_assert_unheld();uint64_t lock_start=e4_tsc();
    uint64_t f=spin_lock_irqsave(&e4_lock);
    uint64_t profile_start=e4_tsc();e4_write_calls++;
    e4_elapsed(&e4_write_lock_wait_cycles,lock_start,profile_start);
    int r=e4_state(node); if (r) goto done;
    ext4_mount_t *fs=((e4_inode_t *)node->fs_private)->fs; ext4_engine_t *e=fs->engine;
    r=e4_plan_begin(e); if (r) goto done;
    e4_inode_t in; uint8_t *raw;
    r=e4_load_plan(e,((e4_inode_t *)node->fs_private)->ino,&in,&raw); if (r) goto abort;
    uint64_t off=append ? in.size : *offset;
    if (len>32768) len=32768;
    if (off>8ULL*1024*1024*1024 || len>8ULL*1024*1024*1024-off) { r=-VFS_EFBIG; goto abort; }
    if (!len) { r=0; goto abort; }
    /* Clear any initialized allocations between old EOF and a distant write.
     * Holes and unwritten extents need no I/O. Credits reject large gaps. */
    if (off>in.size) for (unsigned i=0;i<fs->pending_count;i++) {
        e4_extent_t x=fs->pending[i]; if (x.unwritten) continue;
        uint64_t lo=(uint64_t)x.logical*fs->bs,hi=lo+(uint64_t)x.len*fs->bs;
        if (lo<in.size) lo=in.size;
        if (hi>off) hi=off;
        while (lo<hi) {
            unsigned skip=lo%fs->bs; size_t n=fs->bs-skip; if (n>hi-lo) n=(size_t)(hi-lo);
            uint32_t block=x.physical+(uint32_t)(lo/fs->bs)-x.logical;
            uint8_t *b=e4_stage(e,block,E4_NEW); if (!b) { r=e->error; goto abort; }
            memset(b+skip,0,n); lo+=n;
        }
    }
    for (size_t at=0;at<len;) {
        uint64_t pos=off+at; unsigned skip=pos%fs->bs; size_t n=fs->bs-skip; if (n>len-at) n=len-at;
        bool zero; uint32_t block=e4_initialize(e,(uint32_t)(pos/fs->bs),&zero);
        if (!block) { r=e->error; goto abort; }
        bool full=skip==0 && n==fs->bs;
        uint8_t *b=(zero || full) ? e4_stage_zero(e,block) : e4_stage(e,block,E4_NEW); if (!b) { r=e->error; goto abort; }
        if (zero) memset(b,0,fs->bs);
        memcpy(b+skip,(const uint8_t *)data+at,n); at+=n;
    }
    if (in.size<off+len) in.size=off+len;
    e4_merge_pending(fs); e4_rebuild(e,&in,raw);
    uint64_t plan_end=e4_tsc();e4_elapsed(&e4_plan_cycles,profile_start,plan_end);
    r=e4_publish(e);uint64_t commit_end=e4_tsc();e4_elapsed(&e4_commit_cycles,plan_end,commit_end);
    if (r) goto done;
    r=e4_refresh(node);uint64_t refresh_end=e4_tsc();e4_elapsed(&e4_refresh_cycles,commit_end,refresh_end);
    if (!r) { *offset=off+len;e4_write_bytes_total+=len;
        e4_write_elapsed(profile_start,refresh_end);
        spin_unlock_irqrestore(&e4_lock,f); return (int64_t)len; }
    goto done;
abort:
    e->ready=false;
done:
    e4_write_failures+=r<0;e4_write_elapsed(profile_start,e4_tsc());
    spin_unlock_irqrestore(&e4_lock,f); return r;
}
static int e4_truncate(vfs_node_t *node,uint64_t size) {
    if (size) return -VFS_EINVAL;
    spin_debug_assert_unheld(); uint64_t f=spin_lock_irqsave(&e4_lock);
    int r=e4_state(node); if (r) goto done;
    ext4_engine_t *e=((e4_inode_t *)node->fs_private)->fs->engine;
    r=e4_change(e,((e4_inode_t *)node->fs_private)->ino,0,0,true);
    if (!r) r=e4_commit_unlocked(e);
    e->ready=false; e4_invalidate(e->fs);
    if (!r) r=e4_refresh(node);
done:
    spin_unlock_irqrestore(&e4_lock,f); return r;
}
static bool e4_name(const char *name) {
    if (!name || !*name || strlen(name)>=VFS_MAX_NAME || !strcmp(name,".") || !strcmp(name,"..")) return false;
    for (const char *p=name;*p;p++) if (*p=='/') return false;
    return true;
}
static void e4_dir_sum(e4_inode_t *in,uint8_t *b) {
    uint8_t *tail=b+in->fs->bs-12;
    memset(tail,0,12); e4_p16(tail+4,12); tail[7]=0xde;
    e4_p32(tail+8,e4_crc(e4_inode_seed(in->fs,in->ino,in->generation),b,in->fs->bs-12));
}
static void e4_dir_entry(uint8_t *b,uint32_t ino,unsigned len,const char *name,unsigned type) {
    memset(b,0,len); e4_p32(b,ino); e4_p16(b+4,(uint16_t)len);
    if (ino) { b[6]=(uint8_t)strlen(name); b[7]=(uint8_t)type; memcpy(b+8,name,b[6]); }
}
/* Caller validates complete directory before staging. Growth edits its root in
 * the same plan as the initialized directory block. */
static int e4_dir_edit(ext4_engine_t *e,e4_inode_t *in,const char *name,uint32_t ino,unsigned type) {
    ext4_mount_t *fs=e->fs; in->map=NULL;
    uint32_t found; int r=e4_scan(in,name,0,NULL,&found); if (r<0) return e->error=r;
    if (ino && found) return e->error=-VFS_EEXIST;
    if (!ino && !found) return e->error=-VFS_ENOENT;
    unsigned need=(8+(unsigned)strlen(name)+3)&~3u;
    for (uint32_t l=0;l<in->size/fs->bs;l++) {
        uint32_t physical; r=e4_dir_block(in,l,&physical); if (r) return e->error=r;
        if (!e4_bytes_kind(fs,(uint64_t)physical*fs->bs,fs->scratch,fs->bs,E4_PR_DIRECTORY)) return e->error=-VFS_EIO;
        for (unsigned at=0;at<fs->bs-12;) {
            uint8_t *d=fs->scratch+at; unsigned rec=e4_u16(d+4),used=e4_u32(d) ? (8+d[6]+3)&~3u : 0;
            bool match=e4_u32(d) && d[6]==strlen(name) && !memcmp(d+8,name,d[6]);
            if ((!ino && match) || (ino && rec-used>=need)) {
                uint8_t *b=e4_stage(e,physical,E4_REFERENCE); if (!b) return e->error;
                if (!ino) e4_p32(b+at,0);
                else {
                    if (used) e4_p16(b+at+4,(uint16_t)used);
                    e4_dir_entry(b+at+used,ino,rec-used,name,type);
                }
                e4_dir_sum(in,b); return 0;
            }
            at+=rec;
        }
    }
    if (!ino) return e->error=-VFS_EIO;
    if (in->size>=E4_DIR_MAX) return e->error=-VFS_EFBIG;
    e4_inode_t loaded; uint8_t *raw;
    r=e4_load_plan(e,in->ino,&loaded,&raw); if (r) return r;
    bool zero; uint32_t block=e4_initialize(e,(uint32_t)(in->size/fs->bs),&zero);
    if (!block || !zero) return e->error ? e->error : (e->error=-VFS_EIO);
    uint8_t *b=e4_stage_zero(e,block); if (!b) return e->error;
    memset(b,0,fs->bs); e4_dir_entry(b,ino,fs->bs-12,name,type); e4_dir_sum(in,b);
    loaded.size+=fs->bs; e4_merge_pending(fs); e4_rebuild(e,&loaded,raw);
    return e->error;
}
static vfs_node_t *e4_create(vfs_node_t *parent,const char *name,vfs_node_type_t type) {
    vfs_set_last_create_error(-VFS_EINVAL);
    if (!e4_name(name) || (type!=VFS_FILE && type!=VFS_DIRECTORY)) return NULL;
    /* Reserve the node before any media mutation. */
    e4_node_t *n=kcalloc(1,sizeof(*n));
    if (!n) { vfs_set_last_create_error(-VFS_ENOMEM); return NULL; }
    uint64_t f=spin_lock_irqsave(&e4_lock); int r=e4_state(parent);
    e4_inode_t *dir=parent->fs_private; ext4_mount_t *fs=dir->fs; ext4_engine_t *e=fs->engine;
    if (r) goto fail;
    if (fs->nodes==E4_NODES || strlen(parent->path)+1+strlen(name)>=VFS_MAX_PATH) { r=-VFS_EFBIG; goto fail; }
    dir->map=NULL; uint32_t found; r=e4_scan(dir,name,0,NULL,&found);
    if (r<0) goto fail;
    if (found) { r=-VFS_EEXIST; goto fail; }
    uint32_t ino; r=e4_reserve_inode_unlocked(e,type==VFS_DIRECTORY,&ino); if (r) goto fail;
    uint8_t *raw=e4_inode_image(e,ino); if (!raw) { r=e->error; goto fail; }
    e4_p16(raw+26,type==VFS_DIRECTORY ? 2 : 1);
    e4_inode_t in={.fs=fs,.ino=ino,.generation=e4_u32(raw+100),.mode=type==VFS_DIRECTORY ? 0x4000 : 0x8000};
    if (type==VFS_DIRECTORY) {
        uint32_t block=e4_allocate(e); uint8_t *b=block ? e4_stage_zero(e,block) : NULL;
        if (!b) { r=e->error; goto fail; }
        memset(b,0,fs->bs); e4_dir_entry(b,ino,12,".",2);
        e4_dir_entry(b+12,dir->ino,fs->bs-24,"..",2); e4_dir_sum(&in,b);
        e4_header(raw+40,1,4,0); e4_extent_t x={0,block,1,false}; e4_leaf(raw+52,&x);
        e4_p32(raw+4,fs->bs); e4_p32(raw+28,fs->bs/512);
        uint8_t *praw=e4_inode_image(e,dir->ino);
        if (!praw) { r=e->error; goto fail; }
        if (e4_u16(praw+26)==UINT16_MAX) { r=-VFS_EFBIG; goto fail; }
        e4_p16(praw+26,e4_u16(praw+26)+1); e4_inode_sum(fs,dir->ino,praw);
    }
    e4_inode_sum(fs,ino,raw);
    r=e4_dir_edit(e,dir,name,ino,type==VFS_DIRECTORY ? 2 : 1); if (r) goto fail;
    /* Namespace-owned inodes are never provisional after commit. */
    e->inode_delta=0; e->reserve_ino=0;
    r=e4_publish(e); if (r) goto fail;
    r=e4_refresh(parent); if (r) goto fail;
    r=e4_inode(fs,ino,&in); if (r) { e->tainted=true; goto fail; }
    e4_setup(n,&in); memcpy(n->node.name,name,strlen(name)+1); memcpy(n->node.path,parent->path,strlen(parent->path)+1);
    size_t plen=strlen(n->node.path); n->node.path[plen]='/'; memcpy(n->node.path+plen+1,name,strlen(name)+1);
    n->node.parent=parent; n->node.next=parent->children; parent->children=&n->node;
    fs->cached[fs->nodes++]=n; spin_unlock_irqrestore(&e4_lock,f); return &n->node;
fail:
    if (e) e->ready=false;
    spin_unlock_irqrestore(&e4_lock,f); kfree(n); vfs_set_last_create_error(r); return NULL;
}
static int e4_unlink(vfs_node_t *parent,const char *name) {
    if (!e4_name(name)) return -VFS_EINVAL;
    uint64_t f=spin_lock_irqsave(&e4_lock); int r=e4_state(parent); if (r) goto done;
    e4_inode_t *dir=parent->fs_private; ext4_mount_t *fs=dir->fs; ext4_engine_t *e=fs->engine;
    dir->map=NULL; uint32_t ino; r=e4_scan(dir,name,0,NULL,&ino); if (r<0) goto done;
    if (!ino || ino<11) { r=-VFS_ENOENT; goto done; }
    e4_node_t *target=NULL;
    for (unsigned i=0;i<fs->nodes;i++) if (!fs->cached[i]->removed && fs->cached[i]->inode.ino==ino) {
        if (fs->cached[i]->opens) { r=-VFS_EOPNOTSUPP; goto done; }
        target=fs->cached[i];
    }
    r=e4_plan_begin(e); if (r) goto done;
    e4_inode_t in; uint8_t *raw; r=e4_load_plan(e,ino,&in,&raw); if (r) goto abort;
    bool directory=in.mode==0x4000;
    if (directory) {
        uint32_t child; vfs_dirent_t ent; bool dot=false,dotdot=false;
        for (uint64_t cookie=0;;cookie++) {
            r=e4_scan(&in,NULL,cookie,&ent,&child); if (r<0) goto abort;
            if (!r) break;
            if (!strcmp(ent.name,".")) {
                if (dot || child!=ino || ent.type!=VFS_DIRECTORY) { r=-VFS_EIO; goto abort; }
                dot=true;
            } else if (!strcmp(ent.name,"..")) {
                if (dotdot || child!=dir->ino || ent.type!=VFS_DIRECTORY) { r=-VFS_EIO; goto abort; }
                dotdot=true;
            } else { r=-VFS_ENOTEMPTY; goto abort; }
        }
        if (!dot || !dotdot) { r=-VFS_EIO; goto abort; }
        if (e4_u16(raw+26)!=2) { r=-VFS_EOPNOTSUPP; goto abort; }
    }
    /* Capture all reclamation before directory helpers reuse traversal state. */
    for (unsigned i=0;i<fs->pending_count;i++) if (!e4_release(e,fs->pending[i].physical,fs->pending[i].len)) { r=e->error; goto abort; }
    for (unsigned i=0;i<e->old_count;i++) if (!e4_release(e,e->old_nodes[i],1)) { r=e->error; goto abort; }
    uint32_t g=(ino-1)/fs->ipg, bit=(ino-1)%fs->ipg;
    uint8_t *map=e4_stage_bitmap(e,g,true); if (!map) { r=e->error; goto abort; }
    e4_set_bit(map,bit,false); if (!e4_account(e,g,true,1)) { r=e->error; goto abort; }
    e->release_ino=ino; e->release_dir=directory;
    if (directory) {
        uint8_t *d=e4_descriptor(e,g),*praw=e4_inode_image(e,dir->ino);
        if (!d || !praw || !e4_u16(d+16) || e4_u16(praw+26)<3) { r=-VFS_EIO; goto abort; }
        e4_p16(d+16,e4_u16(d+16)-1); e4_p16(praw+26,e4_u16(praw+26)-1); e4_inode_sum(fs,dir->ino,praw);
    }
    uint32_t generation=in.generation; memset(raw,0,256); e4_p32(raw+100,generation); e4_p16(raw+128,32); e4_inode_sum(fs,ino,raw);
    r=e4_dir_edit(e,dir,name,0,0); if (r) goto abort;
    r=e4_publish(e); if (!r) {
        if (target) {
            target->removed=true;
            vfs_node_t **link=&parent->children;
            while (*link && *link!=&target->node) link=&(*link)->next;
            if (*link) *link=target->node.next;
            target->node.next=NULL;
        }
        r=e4_refresh(parent);
    }
    goto done;
abort:
    e->ready=false;
done:
    spin_unlock_irqrestore(&e4_lock,f); return r;
}
static int e4_rename(vfs_node_t *old_dir,const char *old_name,vfs_node_t *new_dir,const char *new_name) {
    if (!e4_name(old_name) || !e4_name(new_name)) return -VFS_EINVAL;
    uint64_t f=spin_lock_irqsave(&e4_lock); int r=e4_state(old_dir); if (r) goto done;
    r=e4_state(new_dir); if (r) goto done;
    e4_inode_t *old=old_dir->fs_private,*dest=new_dir->fs_private;
    if (old->fs!=dest->fs) { r=-VFS_EOPNOTSUPP; goto done; }
    ext4_engine_t *e=old->fs->engine; old->map=NULL; dest->map=NULL;
    uint32_t ino,existing; r=e4_scan(old,old_name,0,NULL,&ino); if (r<0) goto done;
    if (!ino) { r=-VFS_ENOENT; goto done; }
    if (old_dir==new_dir && !strcmp(old_name,new_name)) { r=0; goto done; }
    e4_inode_t in; r=e4_inode(old->fs,ino,&in); if (r) goto done;
    /* Directory moves need descendant-path and parent-link publication; defer
     * them rather than publishing an incomplete VFS subtree move. */
    if (in.mode!=0x8000) { r=-VFS_EOPNOTSUPP; goto done; }
    if (strlen(new_dir->path)+1+strlen(new_name)>=VFS_MAX_PATH) { r=-VFS_EFBIG; goto done; }
    dest->map=NULL; r=e4_scan(dest,new_name,0,NULL,&existing); if (r<0) goto done;
    if (existing) { r=-VFS_EEXIST; goto done; }
    r=e4_plan_begin(e); if (r) goto done;
    uint8_t *inode_raw; r=e4_load_plan(e,ino,&in,&inode_raw);
    if (r) { e->ready=false; goto done; }
    r=e4_dir_edit(e,dest,new_name,ino,1); if (!r) r=e4_dir_edit(e,old,old_name,0,0);
    if (!r) r=e4_publish(e); else e->ready=false;
    if (!r) r=e4_refresh(old_dir);
    if (!r && old_dir!=new_dir) r=e4_refresh(new_dir);
    if (!r) for (unsigned i=0;i<old->fs->nodes;i++) {
        e4_node_t *n=old->fs->cached[i];
        if (n->removed || n->inode.ino!=ino) continue;
        vfs_node_t **link=&old_dir->children;
        while (*link && *link!=&n->node) link=&(*link)->next;
        if (*link) *link=n->node.next;
        n->node.next=new_dir->children; new_dir->children=&n->node; n->node.parent=new_dir;
        memcpy(n->node.name,new_name,strlen(new_name)+1);
        size_t plen=strlen(new_dir->path);
        memcpy(n->node.path,new_dir->path,plen); n->node.path[plen]='/';
        memcpy(n->node.path+plen+1,new_name,strlen(new_name)+1);
        break;
    }
done:
    spin_unlock_irqrestore(&e4_lock,f); return r;
}
int ext4_sync(ext4_mount_t *fs) {
    spin_debug_assert_unheld(); uint64_t f=spin_lock_irqsave(&e4_lock);
    int r=!fs || fs!=e4_active ? -VFS_EINVAL : 0;
    if (!r && fs->engine) {
        if (fs->engine->tainted) r=-VFS_EIO;
        else if (!block_flush(fs->dev)) { fs->engine->tainted=true; r=-VFS_EIO; }
    }
    spin_unlock_irqrestore(&e4_lock,f); return r;
}
int ext4_freeze_and_sync(ext4_mount_t *fs) {
    spin_debug_assert_unheld();
    uint64_t f=spin_lock_irqsave(&e4_lock);
    if (!fs || fs!=e4_active) { spin_unlock_irqrestore(&e4_lock,f); return -VFS_EINVAL; }
    fs->frozen=true; ext4_engine_t *e=fs->engine;
    spin_unlock_irqrestore(&e4_lock,f);
    return e ? ext4_engine_finish(e) : 0;
}
