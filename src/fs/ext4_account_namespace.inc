/* Explicit opt-in only. No current boot/account/package caller enables this.
 * e4_lock covers raw identity/geometry validation and fence publication together. */
typedef struct {
    uint8_t block[4096];
    uint32_t anchors[5],roles[ACCOUNT_ROLE_COUNT];
    ext4_account_namespace_report_t report;
} e4_ns_workspace_t;
static int e4_ns_child(ext4_mount_t *fs,uint32_t parent,const char *name,
                       unsigned mode,uint32_t *out) {
    e4_inode_t dir,in;uint32_t found=0;int r=e4_inode(fs,parent,&dir);
    if (!r) {r=e4_scan(&dir,name,0,NULL,&found);if (r>=0) r=found ? 0 : -VFS_ENOENT;}
    if (!r) r=e4_inode(fs,found,&in);
    if (!r && (in.mode!=VFS_S_IFDIR || in.uid || in.gid ||
        (in.full_mode & 07777)!=mode)) r=-VFS_EACCES;
    if (!r) *out=found;
    return r;
}
static int e4_ns_role(const uint8_t *d,unsigned parent) {
    static const char *names[ACCOUNT_ROLE_COUNT]={"passwd","group","shadow",
        "passwd.new","group.new","shadow.new","passwd.publish","group.publish",
        "shadow.publish","intent.tmp","pending"};
    for (unsigned i=0;i<ACCOUNT_ROLE_COUNT;i++) {
        bool private_role=(i>=3 && i<=5) || i>=9;
        if (private_role==(parent!=0) && d[6]==strlen(names[i]) && !memcmp(d+8,names[i],d[6])) return (int)i;
    }
    return -1;
}
static int e4_ns_file(ext4_mount_t *fs,e4_ns_workspace_t *w,unsigned role,uint32_t ino) {
    if (w->roles[role]) return -VFS_EINVAL;
    for (unsigned i=0;i<ACCOUNT_ROLE_COUNT;i++) if (w->roles[i]==ino) return -VFS_EINVAL;
    e4_inode_t in;int r=e4_inode(fs,ino,&in);if (r) return r;
    if (in.mode!=VFS_S_IFREG || in.uid || in.gid ||
        (in.full_mode & 07777)!=e4_ac_mode(role) || in.size>ACCOUNT_FILE_MAX ||
        (role<3 && !in.size) || (role==ACCOUNT_PENDING && in.size!=ACCOUNT_INTENT_SIZE)) return -VFS_EACCES;
    unsigned group=(ino-1)/fs->ipg,index=(ino-1)%fs->ipg;uint8_t links[2];
    uint64_t where=(uint64_t)e4_u32(fs->gd[group].raw+8)*fs->bs+(uint64_t)index*256+26;
    if (!e4_bytes(fs,where,links,2)) return -VFS_EIO;
    if (e4_u16(links)!=1) return -VFS_EINVAL;
    w->roles[role]=ino;w->report.present_roles|=1u<<role;return 0;
}
static int e4_ns_directory(ext4_mount_t *fs,e4_ns_workspace_t *w,unsigned which,uint32_t parent) {
    e4_inode_t in;int r=e4_inode(fs,w->anchors[which ? 4 : 0],&in);if (r) return r;
    uint32_t found;r=e4_scan(&in,".",0,NULL,&found);if (r<0) return r;
    unsigned dots=0;
    for (uint32_t l=0;l<in.size/fs->bs;l++) {
        uint32_t physical;r=e4_dir_block(&in,l,&physical);if (r) return r;
        if (!e4_bytes_kind(fs,(uint64_t)physical*fs->bs,w->block,fs->bs,E4_PR_DIRECTORY)) return -VFS_EIO;
        /* e4_scan validated the entire directory including names/checksums. */
        for (unsigned at=0;at<fs->bs-12;) {
            const uint8_t *d=w->block+at;uint32_t ino=e4_u32(d);
            unsigned rec=e4_u16(d+4),used=ino ? (8+d[6]+3)&~3u : 0;
            if (rec<12 || rec%4 || rec>fs->bs-12-at || used>rec) return -VFS_EINVAL;
            bool dot=ino && d[6]==1 && d[8]=='.';
            bool up=ino && d[6]==2 && d[8]=='.' && d[9]=='.';
            int role=ino && !dot && !up ? e4_ns_role(d,which) : -1;
            if (dot || up) {
                unsigned bit=dot ? 1 : 2;
                if ((dots & bit) || d[7]!=2 || ino!=(dot ? in.ino : parent) || rec-used>=16) return -VFS_EINVAL;
                dots|=bit;
            } else if (!ino) {
                if (rec!=12 && rec!=16 && rec!=24) return -VFS_EINVAL;
                if (rec==24) w->report.slots[which]++;
            } else if (role>=0) {
                if (d[7]!=1 || (rec!=24 && !(used==16 && rec==16))) return -VFS_EINVAL;
                r=e4_ns_file(fs,w,(unsigned)role,ino);if (r) return r;
                if (rec==24) w->report.slots[which]++;
            } else if (which || rec-used>=16) return -VFS_EINVAL;
            at+=rec;
        }
    }
    return dots==3 && w->report.slots[which]>=(which ? 5u : 6u) ? 0 : -VFS_ENOSPC;
}
int ext4_root_account_namespace_admit(ext4_mount_t *fs,ext4_account_namespace_report_t *report) {
    spin_debug_assert_unheld();if (report) memset(report,0,sizeof(*report));
    if (!fs || !report) return -VFS_EINVAL;
    e4_ns_workspace_t *w=kcalloc(1,sizeof(*w));if (!w) return -VFS_ENOMEM;
    uint64_t flags=spin_lock_irqsave(&e4_lock);
    int r=!fs->trusted_root || !fs->detached || !fs->journal_mounted || !fs->engine ||
        fs->installer_owned || fs->frozen || fs->quiesced ? -VFS_EROFS :
        fs->engine->tainted || fs->namespace_uncertain ? -VFS_EIO :
        fs->engine->ready || fs->engine->orphan_recovery_required ? -VFS_EAGAIN : 0;
    e4_inode_t root;
    if (!r) r=e4_inode(fs,2,&root);
    if (!r && (root.mode!=VFS_S_IFDIR || root.uid || root.gid ||
        (root.full_mode & 07777)!=0755)) r=-VFS_EACCES;
    if (!r) r=e4_ns_child(fs,2,"etc",0755,&w->anchors[0]);
    if (!r) r=e4_ns_child(fs,2,"var",0755,&w->anchors[1]);
    if (!r) r=e4_ns_child(fs,w->anchors[1],"lib",0755,&w->anchors[2]);
    if (!r) r=e4_ns_child(fs,w->anchors[2],"fortress",0755,&w->anchors[3]);
    if (!r) r=e4_ns_child(fs,w->anchors[3],"accounts",0700,&w->anchors[4]);
    if (!r) r=e4_ns_directory(fs,w,0,2);
    if (!r) r=e4_ns_directory(fs,w,1,w->anchors[3]);
    if (!r) {
        memcpy(fs->account_namespace_anchors,w->anchors,sizeof(w->anchors));
        fs->account_namespace_fenced=true;*report=w->report;
    }
    spin_unlock_irqrestore(&e4_lock,flags);kfree(w);return r;
}
