/* Fixed namespace, retained nodes, synchronous one-call publication. Mounted
 * exclusion remains local to each backend operation. This operation gate is
 * not a spinlock and contention returns immediately; no scheduler lock nests. */
#include "hostkey_abi.h"
static unsigned e4_hostkey_busy;
static void e4_hostkey_wipe(void *p,size_t n) {
    volatile uint8_t *v=p;while(n--) *v++=0;
}
ext4_mount_t *ext4_hostkey_installed(void) {
    spin_debug_assert_unheld();uint64_t irq=spin_lock_irqsave(&e4_lock);
    ext4_mount_t *fs=e4_active && e4_active->trusted_root && !e4_active->installer_owned ? e4_active : NULL;
    spin_unlock_irqrestore(&e4_lock,irq);return fs;
}
static int e4_hostkey_policy(ext4_mount_t *fs,vfs_node_t *node,bool directory) {
    if(!node || node->metadata!=e4_metadata || !node->fs_private ||
       ((e4_inode_t *)node->fs_private)->fs!=fs) return -VFS_EACCES;
    vfs_metadata_t m;int r=vfs_metadata(node,&m);if(r) return r;
    if(m.uid || m.gid || m.type!=(directory ? VFS_DIRECTORY : VFS_FILE) ||
       (m.mode&VFS_S_IFMT)!=(directory ? VFS_S_IFDIR : VFS_S_IFREG) ||
       (directory ? (m.mode&07022u)!=0 : (m.mode&07777u)!=0600)) return -VFS_EACCES;
    return 0;
}
static vfs_node_t *e4_hostkey_lookup(vfs_node_t *parent,const char *name,int *r) {
    /* Single basename callback: no path parser, symlink or mount substitution. */
    return parent->lookup_ref ? parent->lookup_ref(parent,name,r) : (*r=-VFS_EOPNOTSUPP,(vfs_node_t *)NULL);
}
static int e4_hostkey_parents(ext4_mount_t *fs,bool create,vfs_node_t *nodes[3]) {
    if(!fs || !fs->trusted_root || !fs->journal_mounted || !fs->engine || !fs->nodes) return -VFS_EOPNOTSUPP;
    nodes[0]=&fs->cached[0]->node;
    if(!nodes[0]->get || nodes[0]->get(nodes[0])) {nodes[0]=NULL;return -VFS_EIO;}
    int r=e4_hostkey_policy(fs,nodes[0],true);if(r) return r;
    nodes[1]=e4_hostkey_lookup(nodes[0],"etc",&r);if(!nodes[1]) return r;
    r=e4_hostkey_policy(fs,nodes[1],true);if(r) return r;
    nodes[2]=e4_hostkey_lookup(nodes[1],"ssh",&r);
    if(!nodes[2] && r==-VFS_ENOENT && create) {
        vfs_create_attrs_t attrs={.mode=0700};
        nodes[2]=nodes[1]->create_attrs_ref ? nodes[1]->create_attrs_ref(nodes[1],"ssh",VFS_DIRECTORY,&attrs,&r) : NULL;
        if(!nodes[2] && r==-VFS_EEXIST) nodes[2]=e4_hostkey_lookup(nodes[1],"ssh",&r);
    }
    return nodes[2] ? e4_hostkey_policy(fs,nodes[2],true) : r ? r : -VFS_EOPNOTSUPP;
}
static int e4_hostkey_bytes(ext4_mount_t *fs,vfs_node_t *node,char *out,size_t cap,size_t *size) {
    int r=e4_hostkey_policy(fs,node,false);if(r) return r;
    vfs_metadata_t m;r=vfs_metadata(node,&m);if(r) return r;
    if(!m.size || m.size>=HOSTKEY_FILE_MAX || m.size>cap || !node->read) return -VFS_EINVAL;
    size_t at=0;
    while(at<m.size) {
        int64_t n=node->read(node,at,out+at,(size_t)m.size-at);
        if(n<=0 || (uint64_t)n>m.size-at) return n<0 ? (int)n : -VFS_EIO;
        at+=(size_t)n;
    }
    char extra;int64_t tail=node->read(node,at,&extra,1);
    if(tail) return tail<0 ? (int)tail : -VFS_EIO;
    r=e4_hostkey_policy(fs,node,false);if(!r) *size=at;return r;
}
static void e4_hostkey_release(vfs_node_t *nodes[3]) {
    for(unsigned i=3;i;i--) vfs_node_put(nodes[i-1]);
}
int ext4_hostkey_read(ext4_mount_t *fs,char *out,size_t cap,size_t *size) {
    if(!out || cap!=HOSTKEY_FILE_MAX || !size) return -VFS_EINVAL;
    if(__atomic_exchange_n(&e4_hostkey_busy,1,__ATOMIC_ACQUIRE)) return -VFS_EAGAIN;
    char *scratch=kcalloc(1,HOSTKEY_FILE_MAX);vfs_node_t *nodes[3]={0},*key=NULL;size_t n=0;
    int r=scratch ? e4_hostkey_parents(fs,false,nodes) : -VFS_ENOMEM;
    if(!r) {key=e4_hostkey_lookup(nodes[2],HOSTKEY_NAME,&r);if(!key && !r) r=-VFS_EIO;}
    if(!r) r=e4_hostkey_bytes(fs,key,scratch,HOSTKEY_FILE_MAX,&n);
    /* Successful load re-establishes a barrier after recovery/uncertain prior
     * publication; there is no persistent boolean which bypasses validation. */
    if(!r) r=ext4_sync(fs);
    if(!r) {memcpy(out,scratch,n);*size=n;}
    vfs_node_put(key);e4_hostkey_release(nodes);
    if(scratch) {e4_hostkey_wipe(scratch,HOSTKEY_FILE_MAX);kfree(scratch);}
    __atomic_store_n(&e4_hostkey_busy,0,__ATOMIC_RELEASE);return r;
}
int ext4_hostkey_publish(ext4_mount_t *fs,const char *bytes,size_t size) {
    if(!bytes || !size || size>=HOSTKEY_FILE_MAX) return -VFS_EINVAL;
    if(__atomic_exchange_n(&e4_hostkey_busy,1,__ATOMIC_ACQUIRE)) return -VFS_EAGAIN;
    char *verify=kcalloc(1,HOSTKEY_FILE_MAX);vfs_node_t *nodes[3]={0},*stage=NULL,*existing=NULL;size_t n=0;
    int r=verify ? e4_hostkey_parents(fs,true,nodes) : -VFS_ENOMEM;
    if(!r) {
        existing=e4_hostkey_lookup(nodes[2],HOSTKEY_NAME,&r);
        if(existing) r=-VFS_EEXIST;else if(r==-VFS_ENOENT) r=0;
    }
    if(!r) {
        vfs_create_attrs_t attrs={.mode=0600};
        stage=nodes[2]->create_attrs_ref ? nodes[2]->create_attrs_ref(nodes[2],HOSTKEY_STAGE,VFS_FILE,&attrs,&r) : NULL;
        if(!stage && !r) r=-VFS_EOPNOTSUPP;
    }
    if(!r) r=e4_hostkey_policy(fs,stage,false);
    uint64_t offset=0;
    while(!r && offset<size) {
        uint64_t before=offset;
        int64_t k=stage->write ? stage->write(stage,&offset,false,bytes+before,size-(size_t)before) : -VFS_EOPNOTSUPP;
        if(k<=0 || (uint64_t)k>size-before || offset!=before+(uint64_t)k) r=k<0 ? (int)k : -VFS_EIO;
    }
    if(!r) r=e4_hostkey_bytes(fs,stage,verify,HOSTKEY_FILE_MAX,&n);
    if(!r && (n!=size || memcmp(bytes,verify,n))) r=-VFS_EIO;
    if(!r) r=ext4_sync(fs);
    for(unsigned i=0;!r && i<3;i++) r=e4_hostkey_policy(fs,nodes[i],true);
    if(!r) {
        /* Namespace replacement by a privileged writer is outside the threat
         * boundary. Unprivileged actors cannot modify these retained parents.
         * Backend rename atomically refuses an existing destination. */
        r=nodes[2]->rename && nodes[2]->rename_no_replace ? nodes[2]->rename(nodes[2],HOSTKEY_STAGE,nodes[2],HOSTKEY_NAME) : -VFS_EOPNOTSUPP;
    }
    if(!r) r=ext4_sync(fs);
    vfs_node_put(stage);stage=NULL;
    if(!r) {
        vfs_node_put(existing);existing=e4_hostkey_lookup(nodes[2],HOSTKEY_NAME,&r);
        if(!existing && !r) r=-VFS_EIO;
    }
    if(!r) r=e4_hostkey_bytes(fs,existing,verify,HOSTKEY_FILE_MAX,&n);
    if(!r && (n!=size || memcmp(bytes,verify,n))) r=-VFS_EIO;
    /* On any uncertain failure leave residue for explicit diagnosis. Never
     * unlink by name in rollback, and never remove a published canonical key. */
    vfs_node_put(existing);e4_hostkey_release(nodes);
    if(verify) {e4_hostkey_wipe(verify,HOSTKEY_FILE_MAX);kfree(verify);}
    __atomic_store_n(&e4_hostkey_busy,0,__ATOMIC_RELEASE);return r;
}
