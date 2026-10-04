/* Traditional s_last_orphan / i_dtime chain. Exclusive workbench only.
 * Every intent, right-edge reclamation and chain removal uses JBD2. No direct
 * metadata I/O, no allocation while shrinking, no mounted handle publication. */
static int e4_orphan_index(ext4_engine_t *e,uint32_t ino) {
    for (unsigned i=0;i<e->orphan_count;i++) if (e->orphans[i]==ino) return (int)i;
    return -1;
}
static bool e4_orphan_pinned(ext4_engine_t *e,uint32_t ino) {
    if (e->fs->journal_mounted) for (unsigned i=0;i<e->fs->nodes;i++)
        if (e->fs->cached[i]->inode.ino==ino && e->fs->cached[i]->opens) return true;
    for (unsigned i=0;i<16;i++) if (e->handles[i].refs && e->handles[i].ino==ino) return true;
    return false;
}
static int e4_orphan_begin(ext4_engine_t *e) {
    e4_engine_owner(e);
    if (!e || !e->orphan_enabled) return -VFS_EOPNOTSUPP;
    if (e->tainted) return -VFS_EIO;
    if (e->ready || jbd2_writer_state(e->writer)!=JBD2_WRITE_IDLE) return -VFS_EAGAIN;
    e->file_planning=true;int r=e4_plan_begin(e);
    if (r) e->file_planning=false;
    return r;
}
static int e4_orphan_load(ext4_engine_t *e,uint32_t ino,e4_inode_t *in,uint8_t **raw) {
    e->fs->orphan_access=ino;int r=e4_load_plan(e,ino,in,raw);e->fs->orphan_access=0;
    return !r && e4_u16(*raw+116) ? -VFS_EOPNOTSUPP : r;
}
static int e4_orphan_add(ext4_engine_t *e,uint32_t ino,uint8_t *raw) {
    if (e4_orphan_index(e,ino)>=0) return -VFS_EAGAIN;
    if (e->orphan_count==64) return -VFS_EFBIG;
    if (e4_u32(raw+20)) return -VFS_EIO;
    uint8_t *sb=e4_super(e);if (!sb) return e->error;
    if (e4_u32(sb+232)!=(e->orphan_count ? e->orphans[0] : 0)) return -VFS_EIO;
    e4_p32(raw+20,e4_u32(sb+232));e4_p32(sb+232,ino);return 0;
}
static void e4_orphan_added(ext4_engine_t *e,uint32_t ino) {
    for (unsigned i=e->orphan_count;i;i--) e->orphans[i]=e->orphans[i-1];
    e->orphans[0]=ino;e->orphan_count++;
}
/* Before orphan mutation, validate every normal inode's ownership against the
 * complete allocation bitmap, including orphan extents beyond the target EOF.
 * This rejects aliasing, unlisted deleted inodes, cycles and dangling names.
 * Bounds deliberately narrow only this intermediate workbench. */
static int e4_orphan_validate(ext4_engine_t *e) {
    ext4_mount_t *fs=e->fs;uint8_t sb[1024];
    if (!e4_bytes(fs,1024,sb,sizeof(sb))) return -VFS_EIO;
    uint32_t ino=e4_u32(sb+232);e->orphan_count=0;
    while (ino) {
        if (ino<11 || ino>fs->inodes || ino==fs->journal_ino || e4_orphan_index(e,ino)>=0) return -VFS_EIO;
        if (e->orphan_count==64) return -VFS_EFBIG;
        int r=e4_orphan_begin(e);if (r) return r;
        e4_inode_t in;uint8_t *raw;r=e4_orphan_load(e,ino,&in,&raw);
        uint32_t next=0;
        if (!r) {
            if (e4_u16(raw+26)>1 || (in.mode==0x4000 && e4_u16(raw+26))) r=-VFS_EOPNOTSUPP;
            next=e4_u32(raw+20);
        }
        ext4_engine_abort(e);e->file_planning=false;e4_semantic_invalidate(e);
        if (r) return r;
        e->orphans[e->orphan_count++]=ino;ino=next;
    }
    if (fs->blocks>1048576 || fs->inodes>65536) return -VFS_EFBIG;
    uint8_t *owned=kcalloc((fs->blocks+7)/8,1);if (!owned) return -VFS_ENOMEM;
    struct { uint32_t incoming,subdirs,parent,owner;uint16_t mode,links; } *refs=kcalloc(fs->inodes+1,sizeof(*refs));
    if (!refs) { kfree(owned);return -VFS_ENOMEM; }
    int r=0;unsigned directories=0;
    for (unsigned i=0;i<fs->ranges;i++) for (uint32_t k=0;k<fs->reserved[i].len;k++)
        e4_set_bit(owned,fs->reserved[i].lo+k,true);
    for (unsigned i=0;i<fs->journal_ranges;i++) for (uint32_t k=0;k<fs->journal_reserved[i].len;k++) {
        uint32_t block=fs->journal_reserved[i].lo+k;
        if (e4_bit(owned,block)) { r=-VFS_EIO;goto done; }e4_set_bit(owned,block,true);
    }
    for (uint32_t g=0;g<fs->groups;g++) {
        if (e4_u16(fs->gd[g].raw+18)&1) {
            if (e4_u16(fs->gd[g].raw+14)!=fs->ipg || e4_u16(fs->gd[g].raw+16)) { r=-VFS_EIO;goto done; }
            continue;
        }
        if (!e4_bitmap(fs,g,true)) { r=-VFS_EIO;goto done; }
        /* One heap-owned scratch bitmap; traversal reuses fs->scratch. */
        memcpy(e->write_run,fs->scratch,fs->bs);
        unsigned group_dirs=0;
        for (uint32_t bit=0;bit<fs->ipg;bit++) {
            ino=g*fs->ipg+bit+1;
            if (!e4_bit(e->write_run,bit) || (ino<11 && ino!=2) || ino==fs->journal_ino) continue;
            bool orphan=e4_orphan_index(e,ino)>=0;
            r=e4_orphan_begin(e);if (r) goto done;
            e4_inode_t in;uint8_t *raw;
            r=orphan ? e4_orphan_load(e,ino,&in,&raw) : e4_load_plan(e,ino,&in,&raw);
            bool deleted=!r && !e4_u16(raw+26);
            if (!r) {
                if (e4_u16(raw+116)) r=-VFS_EOPNOTSUPP;
                refs[ino].mode=in.mode;refs[ino].links=e4_u16(raw+26);
                if (in.mode==0x4000) {
                    group_dirs++;
                    if (++directories>E4_NODES) r=-VFS_EFBIG;
                }
            }
            bool scan_dir=!r && in.mode==0x4000;
            if (scan_dir && deleted) {
                /* Supported rmdir has a single dot block. After its reclaim
                 * transaction there is no directory data left to scan. */
                if (in.size!=fs->bs || fs->pending_count>1 ||
                    (fs->pending_count && (fs->pending[0].logical || fs->pending[0].len!=1))) r=-VFS_EOPNOTSUPP;
                scan_dir=fs->pending_count!=0;
            }
            if (!r && !orphan && e4_u32(raw+20)) r=-VFS_EIO;
            if (!r) for (unsigned i=0;i<fs->physical_count && !r;i++) for (uint32_t k=0;k<fs->physical[i].len;k++) {
                uint32_t block=fs->physical[i].lo+k;
                if (e4_bit(owned,block)) { r=-VFS_EIO;break; }e4_set_bit(owned,block,true);
            }
            ext4_engine_abort(e);e->file_planning=false;
            if (!r && scan_dir) {
                uint32_t child;vfs_dirent_t ent;bool dot=false,dotdot=false;
                fs->orphan_access=deleted ? ino : 0;
                for (uint64_t cookie=0;!r;cookie++) {
                    int scan=e4_scan(&in,NULL,cookie,&ent,&child);
                    if (scan<0) { r=scan;break; }if (!scan) break;
                    if (!strcmp(ent.name,".")) { if (dot || child!=ino || ent.type!=VFS_DIRECTORY) r=-VFS_EIO;dot=true; }
                    else if (!strcmp(ent.name,"..")) {
                        if (dotdot || ent.type!=VFS_DIRECTORY) r=-VFS_EIO;
                        dotdot=true;refs[ino].parent=child;
                    }
                    else if (deleted) r=-VFS_EIO;
                    if (!deleted) {
                        e4_inode_t target;
                        if (e4_inode(fs,child,&target) || target.mode!=(ent.type==VFS_DIRECTORY ? 0x4000 : 0x8000)) r=-VFS_EIO;
                        else if (strcmp(ent.name,".") && strcmp(ent.name,"..")) {
                            if (child<11 || child==fs->journal_ino || ++refs[child].incoming>1) r=-VFS_EIO;
                            refs[child].owner=ino;
                            if (target.mode==0x4000) refs[ino].subdirs++;
                        }
                    }
                }
                fs->orphan_access=0;
                if (!dot || !dotdot) r=-VFS_EIO;
            }
            e4_semantic_invalidate(e);if (r) goto done;
        }
        if (group_dirs!=e4_u16(fs->gd[g].raw+16)) { r=-VFS_EIO;goto done; }
    }
    if (refs[2].mode!=0x4000 || refs[2].parent!=2 || refs[2].incoming) { r=-VFS_EIO;goto done; }
    for (ino=2;ino<=fs->inodes;ino++) if (refs[ino].mode) {
        if (!refs[ino].links) {
            if (refs[ino].incoming || e4_orphan_index(e,ino)<0) { r=-VFS_EIO;goto done; }
        } else if (refs[ino].mode==0x8000) {
            if (refs[ino].links!=1 || refs[ino].incoming!=1) { r=-VFS_EIO;goto done; }
        } else {
            if (refs[ino].links!=2+refs[ino].subdirs ||
                (ino!=2 && (refs[ino].incoming!=1 || refs[ino].parent!=refs[ino].owner))) { r=-VFS_EIO;goto done; }
            uint32_t walk=ino;unsigned steps=0;
            while (walk!=2) {
                if (!walk || walk>fs->inodes || refs[walk].mode!=0x4000 || !refs[walk].links || ++steps>directories) {
                    r=-VFS_EIO;goto done;
                }
                walk=refs[walk].parent;
            }
        }
    }
    for (uint32_t g=0;g<fs->groups;g++) {
        uint32_t start=fs->first+g*fs->bpg,count=fs->blocks-start;if (count>fs->bpg) count=fs->bpg;
        /* Uninitialized bitmap helper does not synthesize bits; compare fixed
         * ranges instead, as the allocator's initialization contract does. */
        bool lazy=(e4_u16(fs->gd[g].raw+18)&2)!=0;
        if (!lazy && !e4_bitmap(fs,g,false)) { r=-VFS_EIO;goto done; }
        if (lazy) {
            memset(fs->scratch,0,fs->bs);
            for (unsigned i=0;i<fs->ranges;i++) if (fs->reserved[i].lo>=start && fs->reserved[i].lo<start+count)
                for (uint32_t k=0;k<fs->reserved[i].len;k++) e4_set_bit(fs->scratch,fs->reserved[i].lo-start+k,true);
        }
        unsigned free=0;
        for (uint32_t bit=0;bit<count;bit++) {
            bool allocated=e4_bit(fs->scratch,bit);
            if (allocated!=e4_bit(owned,start+bit)) { r=-VFS_EIO;goto done; }
            free+=!allocated;
        }
        if (free!=e4_u16(fs->gd[g].raw+12)) { r=-VFS_EIO;goto done; }
    }
done:
    fs->orphan_access=0;kfree(refs);kfree(owned);return r;
}
static int e4_open_journal_orphans_owned(block_dev_t *dev,bool admitted,ext4_engine_t **out,bool reserved,bool lifecycle) {
    int r=e4_open_journal_files_owned(dev,admitted,true,out,reserved,lifecycle);if (r) return r;
    ext4_engine_t *e=*out;e->namespace_enabled=e->orphan_enabled=true;
    /* A truncate intent must not strand cleanup on a journal too small for
     * the engine's bounded metadata/revoke class. Reserve and abort without
     * I/O before admitting any semantic mutation. Ordered data uses no log. */
    r=jbd2_writer_begin(e->writer,(jbd2_credits_t){64,0,64});
    if (!r) { jbd2_writer_abort(e->writer);r=e4_orphan_validate(e); }
    if (r) { if (reserved) e4_engine_dispose(e);else ext4_engine_close(e);*out=NULL;return r; }
    e->orphan_recovery_required=e->orphan_count!=0;return 0;
}
int ext4_engine_open_journal_orphans(block_dev_t *dev,bool admitted,ext4_engine_t **out) {
    return e4_open_journal_orphans_owned(dev,admitted,out,false,false);
}
/* Remove from one rightmost leaf; parents lose only their final index. Empty
 * external nodes are revoked without staging a home image of the freed node.
 * A retained node keeps the same left key and depth, so no tree reallocation
 * or disk-space credit is needed even on a completely full filesystem. */
static int e4_orphan_edge(ext4_engine_t *e,e4_inode_t *in,uint8_t *h,unsigned depth,
                           bool root,uint32_t keep,unsigned *freed,bool *changed) {
    unsigned count=e4_u16(h+2);if (!count) return 0;
    uint8_t *last=h+12+(count-1)*12;bool empty=false;
    if (depth) {
        uint32_t block=e4_u32(last+4);uint8_t *b=e->orphan_tree[depth-1];
        if (!e4_bytes(e->fs,(uint64_t)block*e->fs->bs,b,e->fs->bs)) return -VFS_EIO;
        int r=e4_orphan_edge(e,in,b,depth-1,false,keep,freed,changed);if (r || !*changed) return r;
        empty=!e4_u16(b+2);
        if (empty) { if (!e4_release(e,block,1)) return e->error;(*freed)++; }
        else {
            uint8_t *staged=e4_stage(e,block,E4_REFERENCE);if (!staged) return e->error;
            unsigned tail=12+e4_u16(b+4)*12;
            e4_p32(b+tail,e4_crc(e4_inode_seed(e->fs,in->ino,in->generation),b,tail));
            memcpy(staged,b,e->fs->bs);
        }
    } else {
        uint32_t logical=e4_u32(last),encoded=e4_u16(last+4),len=encoded>32768 ? encoded-32768 : encoded;
        uint64_t end=(uint64_t)logical+len;if (end<=keep) return 0;
        unsigned remove=logical>=keep ? len : (unsigned)(end-keep);if (remove>32) remove=32;
        if (!e4_release(e,e4_u32(last+8)+len-remove,remove)) return e->error;
        *freed+=remove;*changed=true;len-=remove;empty=!len;
        if (len) e4_p16(last+4,(uint16_t)(len+(encoded>32768 ? 32768 : 0)));
    }
    if (empty) { memset(last,0,12);e4_p16(h+2,(uint16_t)(count-1)); }
    if (root && !e4_u16(h+2)) { memset(h,0,60);e4_header(h,0,4,0); }
    return 0;
}
static int e4_orphan_detach(ext4_engine_t *e,uint32_t ino,uint8_t *raw) {
    int at=e4_orphan_index(e,ino);if (at<0) return -VFS_EIO;
    uint32_t next=(unsigned)(at+1)<e->orphan_count ? e->orphans[at+1] : 0;
    if (e4_u32(raw+20)!=next) return -VFS_EIO;
    if (!at) {
        uint8_t *sb=e4_super(e);if (!sb) return e->error;
        if (e4_u32(sb+232)!=ino) return -VFS_EIO;
        e4_p32(sb+232,next);
    } else {
        uint32_t prev=e->orphans[at-1];e4_inode_t in;uint8_t *praw;
        int r=e4_orphan_load(e,prev,&in,&praw);if (r) return r;
        if (e4_u32(praw+20)!=ino) return -VFS_EIO;
        e4_p32(praw+20,next);e4_inode_sum(e->fs,prev,praw);
    }
    e4_p32(raw+20,0);return 0;
}
static int e4_orphan_cleanup(ext4_engine_t *e,uint32_t ino,bool trim_pinned) {
    for (;;) {
        int r=e4_orphan_begin(e);if (r) return r;
        e4_inode_t in;uint8_t *raw;r=e4_orphan_load(e,ino,&in,&raw);
        if (r) return e4_namespace_end(e,r);
        bool deleted=!e4_u16(raw+26);
        bool pinned=deleted && e4_orphan_pinned(e,ino);
        if (pinned && !trim_pinned) {
            ext4_engine_abort(e);e->file_planning=false;e4_semantic_invalidate(e);return 0;
        }
        uint32_t keep=deleted && !pinned ? 0 : (uint32_t)((in.size+e->fs->bs-1)/e->fs->bs);
        /* Also seal foreign linked-orphan tails. The initial FortressOS intent
         * already did this; recovery must not rely on in-memory provenance. */
        if ((!deleted || pinned) && in.size%e->fs->bs) for (unsigned i=0;i<e->fs->pending_count;i++) {
            e4_extent_t x=e->fs->pending[i];uint32_t logical=(uint32_t)(in.size/e->fs->bs);
            if (x.unwritten || logical<x.logical || logical-x.logical>=x.len) continue;
            uint32_t block=x.physical+logical-x.logical;
            if (!e4_bytes(e->fs,(uint64_t)block*e->fs->bs,e->fs->scratch,e->fs->bs))
                return e4_namespace_end(e,-VFS_EIO);
            bool zero=true;
            for (unsigned k=(unsigned)(in.size%e->fs->bs);k<e->fs->bs;k++) if (e->fs->scratch[k]) zero=false;
            if (!zero) {
                uint8_t *b=e4_stage(e,block,E4_REFERENCE);if (!b) return e4_namespace_end(e,e->error);
                memset(b+in.size%e->fs->bs,0,e->fs->bs-in.size%e->fs->bs);
            }
            break;
        }
        unsigned freed=0;bool changed=false;
        r=e4_orphan_edge(e,&in,raw+40,e4_u16(raw+46),true,keep,&freed,&changed);
        if (r) return e4_namespace_end(e,r);
        if (changed) {
            uint32_t sectors=freed*(e->fs->bs/512);
            if (e4_u32(raw+28)<sectors) return e4_namespace_end(e,-VFS_EIO);
            e4_p32(raw+28,e4_u32(raw+28)-sectors);e4_inode_sum(e->fs,ino,raw);
            r=e4_namespace_end(e,0);if (r) return r;continue;
        }
        if (pinned) {
            ext4_engine_abort(e);e->file_planning=false;e4_semantic_invalidate(e);return 0;
        }
        r=e4_orphan_detach(e,ino,raw);if (r) return e4_namespace_end(e,r);
        if (deleted) {
            uint32_t g=(ino-1)/e->fs->ipg,bit=(ino-1)%e->fs->ipg;
            uint8_t *map=e4_stage_bitmap(e,g,true);if (!map) return e4_namespace_end(e,e->error);
            e4_set_bit(map,bit,false);if (!e4_account(e,g,true,1)) return e4_namespace_end(e,e->error);
            if (in.mode==0x4000) {
                uint8_t *d=e4_descriptor(e,g);if (!d) return e4_namespace_end(e,e->error);
                if (!e4_u16(d+16)) return e4_namespace_end(e,-VFS_EIO);
                e4_p16(d+16,(uint16_t)(e4_u16(d+16)-1));
            }
            uint32_t generation=in.generation;memset(raw,0,256);e4_p32(raw+100,generation);e4_p16(raw+128,32);
        }
        e4_inode_sum(e->fs,ino,raw);r=e4_namespace_end(e,0);if (r) return r;
        unsigned at=(unsigned)e4_orphan_index(e,ino);
        for (unsigned i=at+1;i<e->orphan_count;i++) e->orphans[i-1]=e->orphans[i];
        e->orphan_count--;return 0;
    }
}
int ext4_engine_orphan_recover(ext4_engine_t *e) {
    e4_engine_owner(e);if (!e || !e->orphan_enabled) return -VFS_EOPNOTSUPP;
    if (e->tainted) return -VFS_EIO;
    /* Runtime retry may coexist with pins: deleted pinned files stay alive. */
    for (unsigned i=0;i<e->orphan_count;) {
        unsigned count=e->orphan_count;int r=e4_orphan_cleanup(e,e->orphans[i],false);if (r) return r;
        if (e->orphan_count==count) i++;
    }
    e->orphan_recovery_required=false;return 0;
}
int ext4_engine_file_truncate(ext4_engine_t *e,uint32_t ino,uint64_t size) {
    int r=e4_orphan_begin(e);if (r) return r;
    if (e->orphan_recovery_required) return e4_namespace_end(e,-VFS_EAGAIN);
    if (ino<11 || ino==e->fs->journal_ino) return e4_namespace_end(e,-VFS_EOPNOTSUPP);
    if (e4_orphan_index(e,ino)>=0) return e4_namespace_end(e,-VFS_EAGAIN);
    e4_inode_t in;uint8_t *raw;r=e4_load_plan(e,ino,&in,&raw);
    if (r) return e4_namespace_end(e,r);
    if (in.mode!=0x8000 || size>in.size) return e4_namespace_end(e,-VFS_EOPNOTSUPP);
    if (size==in.size) { ext4_engine_abort(e);e->file_planning=false;e4_semantic_invalidate(e);return 0; }
    r=e4_orphan_add(e,ino,raw);if (r) return e4_namespace_end(e,r);
    /* Journal the one partial tail with the size intent, rather than ordered
     * overwrite of bytes still visible if the size transaction never commits. */
    if (size%e->fs->bs) for (unsigned i=0;i<e->fs->pending_count;i++) {
        e4_extent_t x=e->fs->pending[i];uint32_t logical=(uint32_t)(size/e->fs->bs);
        if (logical>=x.logical && logical-x.logical<x.len && !x.unwritten) {
            uint8_t *b=e4_stage(e,x.physical+logical-x.logical,E4_REFERENCE);
            if (!b) return e4_namespace_end(e,e->error);
            memset(b+size%e->fs->bs,0,e->fs->bs-size%e->fs->bs);break;
        }
    }
    e4_p32(raw+4,(uint32_t)size);e4_p32(raw+108,(uint32_t)(size>>32));e4_inode_sum(e->fs,ino,raw);
    r=e4_namespace_end(e,0);if (r) return r;e4_orphan_added(e,ino);
    return e4_orphan_cleanup(e,ino,false);
}
int ext4_engine_orphan_unlink(ext4_engine_t *e,uint32_t parent,const char *name,bool directory) {
    if (!e4_namespace_name(name)) return -VFS_EINVAL;
    int r=e4_orphan_begin(e);if (r) return r;
    if (e->orphan_recovery_required) return e4_namespace_end(e,-VFS_EAGAIN);
    e4_inode_t dir,in;uint32_t ino;
    r=e4_namespace_parent(e,parent,&dir);if (r) return e4_namespace_end(e,r);
    r=e4_scan(&dir,name,0,NULL,&ino);if (r<0) return e4_namespace_end(e,r);
    if (!ino) return e4_namespace_end(e,-VFS_ENOENT);
    if (ino<11 || ino==e->fs->journal_ino) return e4_namespace_end(e,-VFS_EOPNOTSUPP);
    uint8_t *raw;r=e4_load_plan(e,ino,&in,&raw);if (r) return e4_namespace_end(e,r);
    if (in.mode!=(directory ? 0x4000 : 0x8000)) return e4_namespace_end(e,-VFS_EOPNOTSUPP);
    if (directory) {
        if (in.size!=e->fs->bs) return e4_namespace_end(e,-VFS_EOPNOTSUPP);
        bool dot=false,dotdot=false;uint32_t child;vfs_dirent_t ent;
        for (uint64_t cookie=0;;cookie++) {
            r=e4_scan(&in,NULL,cookie,&ent,&child);if (r<0) return e4_namespace_end(e,r);if (!r) break;
            if (!strcmp(ent.name,".")) { if (dot || child!=ino || ent.type!=VFS_DIRECTORY) return e4_namespace_end(e,-VFS_EIO);dot=true; }
            else if (!strcmp(ent.name,"..")) { if (dotdot || child!=parent || ent.type!=VFS_DIRECTORY) return e4_namespace_end(e,-VFS_EIO);dotdot=true; }
            else return e4_namespace_end(e,-VFS_ENOTEMPTY);
        }
        if (!dot || !dotdot || e4_u16(raw+26)!=2) return e4_namespace_end(e,-VFS_EIO);
        uint8_t *praw=e4_inode_image(e,parent);if (!praw) return e4_namespace_end(e,e->error);
        if (e4_u16(praw+26)<3) return e4_namespace_end(e,-VFS_EIO);
        e4_p16(praw+26,(uint16_t)(e4_u16(praw+26)-1));e4_inode_sum(e->fs,parent,praw);
    }
    r=e4_orphan_add(e,ino,raw);if (r) return e4_namespace_end(e,r);
    e4_p16(raw+26,0);e4_inode_sum(e->fs,ino,raw);
    r=e4_dir_edit(e,&dir,name,0,0);r=e4_namespace_end(e,r);
    if (r) return r;
    e4_orphan_added(e,ino);return e4_orphan_cleanup(e,ino,false);
}
static int e4_handle_slot(ext4_engine_t *e,uint64_t token) {
    if (!e || !e->orphan_enabled || !token) return -VFS_EINVAL;
    for (unsigned i=0;i<16;i++) if (e->handles[i].refs && e->handles[i].token==token) return (int)i;
    return -VFS_EINVAL;
}
int ext4_engine_handle_open(ext4_engine_t *e,uint32_t ino,uint64_t *handle) {
    spin_debug_assert_unheld();if (!e || !e->orphan_enabled || !handle) return -VFS_EINVAL;
    if (e->tainted) return -VFS_EIO;
    if (e->orphan_recovery_required || e4_orphan_index(e,ino)>=0) return -VFS_EAGAIN;
    if (ino<11 || ino==e->fs->journal_ino) return -VFS_EOPNOTSUPP;
    e4_inode_t in;int r=e4_inode(e->fs,ino,&in);if (r) return r;
    if (in.mode!=0x8000) return -VFS_EOPNOTSUPP;
    if (e->handle_serial==UINT64_MAX) return -VFS_EFBIG;
    for (unsigned i=0;i<16;i++) if (!e->handles[i].refs) {
        e->handles[i].token=++e->handle_serial;e->handles[i].offset=0;
        e->handles[i].ino=ino;e->handles[i].generation=in.generation;e->handles[i].refs=1;
        *handle=e->handles[i].token;return 0;
    }
    return -VFS_EFBIG;
}
int ext4_engine_handle_dup(ext4_engine_t *e,uint64_t token) {
    spin_debug_assert_unheld();int at=e4_handle_slot(e,token);if (at<0) return at;
    if (e->handles[at].refs==UINT32_MAX) return -VFS_EFBIG;
    e->handles[at].refs++;return 0;
}
static int e4_handle_inode(ext4_engine_t *e,unsigned at,e4_inode_t *in) {
    uint32_t ino=e->handles[at].ino;e->fs->orphan_access=e4_orphan_index(e,ino)>=0 ? ino : 0;
    int r=e4_inode(e->fs,ino,in);e->fs->orphan_access=0;
    return r ? r : in->generation!=e->handles[at].generation ? -VFS_EIO : 0;
}
int ext4_engine_handle_close(ext4_engine_t *e,uint64_t token) {
    spin_debug_assert_unheld();int at=e4_handle_slot(e,token);if (at<0) return at;
    uint32_t ino=e->handles[at].ino;if (--e->handles[at].refs) return 0;
    if (e4_orphan_index(e,ino)<0 || e4_orphan_pinned(e,ino)) return 0;
    return e4_orphan_cleanup(e,ino,false);
}
int64_t ext4_engine_handle_read(ext4_engine_t *e,uint64_t token,void *bytes,size_t len) {
    spin_debug_assert_unheld();int at=e4_handle_slot(e,token);if (at<0) return at;
    if (!bytes && len) return -VFS_EINVAL;
    e4_node_t node={0};int r=e4_handle_inode(e,(unsigned)at,&node.inode);if (r) return r;
    node.node.fs_private=&node.inode;
    if (len>32768) len=32768;
    int64_t n=e4_read(&node.node,e->handles[at].offset,bytes,len);
    if (n>0) e->handles[at].offset+=(uint64_t)n;
    e4_semantic_invalidate(e);return n;
}
int64_t ext4_engine_handle_write(ext4_engine_t *e,uint64_t token,bool append,const void *bytes,size_t len) {
    spin_debug_assert_unheld();int at=e4_handle_slot(e,token);if (at<0) return at;
    e4_inode_t in;int r=e4_handle_inode(e,(unsigned)at,&in);if (r) return r;
    uint32_t ino=e->handles[at].ino;
    if (e4_orphan_index(e,ino)>=0) {
        uint8_t raw[256];uint32_t g=(ino-1)/e->fs->ipg,index=(ino-1)%e->fs->ipg;
        if (!e4_bytes(e->fs,(uint64_t)e4_u32(e->fs->gd[g].raw+8)*e->fs->bs+index*256,raw,256)) return -VFS_EIO;
        if (e4_u16(raw+26)) return -VFS_EAGAIN;
        e->fs->orphan_access=ino;
    }
    int64_t n=ext4_engine_file_write(e,ino,&e->handles[at].offset,append,bytes,len);
    e->fs->orphan_access=0;return n;
}
static int e4_orphan_pinned_truncate(ext4_engine_t *e,uint32_t ino,uint64_t size) {
    if (e4_orphan_index(e,ino)<0) return ext4_engine_file_truncate(e,ino,size);
    int r=e4_orphan_begin(e);if (r) return r;
    e4_inode_t in;uint8_t *raw;r=e4_orphan_load(e,ino,&in,&raw);if (r) return e4_namespace_end(e,r);
    if (e4_u16(raw+26)) return e4_namespace_end(e,-VFS_EAGAIN);
    if (size>in.size) return e4_namespace_end(e,-VFS_EOPNOTSUPP);
    /* Existing deleted-orphan registration remains until the final close.
     * Offsets, including shared offsets beyond the new EOF, do not change. */
    if (size%e->fs->bs) for (unsigned i=0;i<e->fs->pending_count;i++) {
        e4_extent_t x=e->fs->pending[i];uint32_t logical=(uint32_t)(size/e->fs->bs);
        if (!x.unwritten && logical>=x.logical && logical-x.logical<x.len) {
            uint8_t *b=e4_stage(e,x.physical+logical-x.logical,E4_REFERENCE);
            if (!b) return e4_namespace_end(e,e->error);
            memset(b+size%e->fs->bs,0,e->fs->bs-size%e->fs->bs);break;
        }
    }
    e4_p32(raw+4,(uint32_t)size);e4_p32(raw+108,(uint32_t)(size>>32));e4_inode_sum(e->fs,ino,raw);
    r=e4_namespace_end(e,0);if (r) return r;
    return e4_orphan_cleanup(e,ino,true);
}
int ext4_engine_handle_truncate(ext4_engine_t *e,uint64_t token,uint64_t size) {
    spin_debug_assert_unheld();int at=e4_handle_slot(e,token);if (at<0) return at;
    e4_inode_t in;int r=e4_handle_inode(e,(unsigned)at,&in);if (r) return r;
    return e4_orphan_pinned_truncate(e,e->handles[at].ino,size);
}
