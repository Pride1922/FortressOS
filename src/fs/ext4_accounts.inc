/* Fixed paths belong to the kernel, never a user request or recovery record. */
static const char *e4_ac_paths[ACCOUNT_ROLE_COUNT]={
    "/etc/passwd","/etc/group","/etc/shadow",
    "/var/lib/fortress/accounts/passwd.new","/var/lib/fortress/accounts/group.new","/var/lib/fortress/accounts/shadow.new",
    "/etc/passwd.publish","/etc/group.publish","/etc/shadow.publish",
    "/var/lib/fortress/accounts/intent.tmp","/var/lib/fortress/accounts/pending"
};
static vfs_node_t *e4_ac_lookup(ext4_mount_t *fs,const char *path,int *error) {
    creds_t root={.umask=0022,.cap_effective=CAP_ALL};
    return vfs_lookup_at_creds(&fs->cached[0]->node,path,&root,error);
}
static vfs_node_t *e4_ac_parent(ext4_mount_t *fs,unsigned role,const char **name,int *error) {
    const char *path=e4_ac_paths[role];size_t split=strlen(path);
    while (split && path[split-1]!='/') split--;
    char parent[VFS_MAX_PATH];memcpy(parent,path,split-1);parent[split-1]=0;*name=path+split;
    return e4_ac_lookup(fs,parent,error);
}
static unsigned e4_ac_mode(unsigned role) {
    return role==ACCOUNT_PASSWD || role==ACCOUNT_GROUP || role==ACCOUNT_TEMP_PASSWD || role==ACCOUNT_TEMP_GROUP ? 0644 : 0600;
}
static int e4_ac_read(void *context,unsigned role,void *bytes,size_t capacity,size_t *length) {
    if (role>=ACCOUNT_ROLE_COUNT) return -VFS_EINVAL;
    int r;vfs_node_t *n=e4_ac_lookup(context,e4_ac_paths[role],&r);if (!n) return r;
    vfs_metadata_t m;r=vfs_metadata(n,&m);
    if (!r && (m.type!=VFS_FILE || m.uid || m.gid || (m.mode & 07777)!=e4_ac_mode(role) || m.size>capacity)) r=-VFS_EINVAL;
    size_t done=0;
    while (!r && done<m.size) {
        int64_t count=n->read(n,done,(uint8_t *)bytes+done,m.size-done);
        if (count<=0 || (uint64_t)count>m.size-done) r=count<0 ? (int)count : -VFS_EIO;
        else done+=(size_t)count;
    }
    if (!r) *length=done;
    vfs_node_put(n);return r;
}
static int e4_ac_remove(void *context,unsigned role) {
    if (role>=ACCOUNT_ROLE_COUNT) return -VFS_EINVAL;
    const char *name;int r;vfs_node_t *p=e4_ac_parent(context,role,&name,&r);if (!p) return r;
    r=e4_junlink_account(p,name,false,NULL,true);vfs_node_put(p);return r==-VFS_ENOENT ? 0 : r;
}
static int e4_ac_write(void *context,unsigned role,const void *bytes,size_t length) {
    if (role>=ACCOUNT_ROLE_COUNT || !length || length>ACCOUNT_FILE_MAX) return -VFS_EINVAL;
    const char *name;int r;vfs_node_t *p=e4_ac_parent(context,role,&name,&r);if (!p) return r;
    vfs_create_attrs_t a={.mode=e4_ac_mode(role),.uid=0,.gid=0};
    vfs_node_t *n=e4_jcreate_account(p,name,VFS_FILE,&r,true,&a,NULL,true);vfs_node_put(p);
    if (!n) return r;
    uint64_t flags=spin_lock_irqsave(&e4_lock);
    r=e4_state_account(n,true);e4_node_t *file=(e4_node_t *)n;uint64_t offset=0;
    if (!r) {
        file->inode.fs->account_allocation=true;
        int64_t count=e4_file_write_actor(file->inode.fs->engine,file->inode.ino,&offset,false,bytes,length,NULL);
        file->inode.fs->account_allocation=false;
        r=count<0 ? (int)count : (size_t)count!=length ? -VFS_EIO : 0;
        if (!r && e4_journal_refresh(n)) {e4_jtaint(file->inode.fs);r=-VFS_EIO;}
    }
    spin_unlock_irqrestore(&e4_lock,flags);vfs_node_put(n);return r;
}
static int e4_ac_rename(void *context,unsigned from,unsigned to) {
    if (from>=ACCOUNT_ROLE_COUNT || to>=ACCOUNT_ROLE_COUNT) return -VFS_EINVAL;
    const char *a,*b;int r;vfs_node_t *p=e4_ac_parent(context,from,&a,&r);if (!p) return r;
    vfs_node_t *q=e4_ac_parent(context,to,&b,&r);
    if (q) r=e4_jrename_account(p,a,q,b,false,NULL,true);
    vfs_node_put(p);vfs_node_put(q);return r;
}
static int e4_ac_sync(void *context) {return ext4_sync(context);}
int ext4_root_account_io(ext4_mount_t *fs,account_io_t *io) {
    if (!fs || !io || !fs->trusted_root || !fs->journal_mounted) return -VFS_EINVAL;
    const char *paths[]={"/var/lib","/var/lib/fortress","/var/lib/fortress/accounts"};
    for (unsigned i=0;i<3;i++) {
        int r;vfs_node_t *n=e4_ac_lookup(fs,paths[i],&r);
        if (!n && r==-VFS_ENOENT) {
            char parent[VFS_MAX_PATH];size_t split=strlen(paths[i]);while (paths[i][split-1]!='/') split--;
            memcpy(parent,paths[i],split-1);parent[split-1]=0;
            vfs_node_t *p=e4_ac_lookup(fs,parent,&r);if (!p) return r;
            vfs_create_attrs_t attrs={.mode=i==2 ? 0700 : 0755,.uid=0,.gid=0};
            n=e4_jcreate_account(p,paths[i]+split,VFS_DIRECTORY,&r,true,&attrs,NULL,true);vfs_node_put(p);
        }
        if (!n) return r;
        vfs_metadata_t m;r=vfs_metadata(n,&m);
        if (!r && (m.type!=VFS_DIRECTORY || m.uid || m.gid || (m.mode & 07777)!=(i==2 ? 0700u : 0755u))) r=-VFS_EINVAL;
        vfs_node_put(n);if (r) return r;
    }
    uint64_t flags=spin_lock_irqsave(&e4_lock);fs->accounts_mutable=true;spin_unlock_irqrestore(&e4_lock,flags);
    *io=(account_io_t){.context=fs,.read=e4_ac_read,.write=e4_ac_write,.remove=e4_ac_remove,.rename=e4_ac_rename,.sync=e4_ac_sync};return 0;
}
int ext4_root_account_headroom(ext4_mount_t *fs) {
    if (!fs || !fs->trusted_root) return -VFS_EINVAL;
    uint64_t flags=spin_lock_irqsave(&e4_lock);uint8_t value[8];
    int r=fs->engine->tainted || fs->quiesced ? -VFS_EIO : fs->frozen ? -VFS_EROFS :
        !e4_bytes(fs,1036,value,sizeof(value)) ? -VFS_EIO : ((uint64_t)e4_u32(value)*fs->bs<EXT4_ACCOUNT_RESERVE_BYTES || e4_u32(value+4)<EXT4_ACCOUNT_RESERVE_INODES) ? -VFS_ENOSPC : 0;
    spin_unlock_irqrestore(&e4_lock,flags);return r;
}
