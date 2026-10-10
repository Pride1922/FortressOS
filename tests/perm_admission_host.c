/* Actual VFS descriptor/no-op paths, pthread filesystem shims. */
#define FORTRESS_PERMISSIONS_TRACE
#define TEST_PERMISSIONS_WIRING
#define main phase0_main
#include "perm_fs_host.c"
#undef main
static creds_t expected;
static unsigned calls, writes;
void vfs_permission_trace(const vfs_node_t *node,unsigned mask,const creds_t *actor) {
    assert(node && actor && !memcmp(actor,&expected,sizeof(expected)));
    calls++;
    if (mask & VFS_MAY_WRITE) writes++;
}
static int enumeration_error(vfs_node_t *n, uint64_t i, void *out) {
    (void)n; (void)i; (void)out; return -VFS_EIO;
}
int main(void) {
    expected=(creds_t){.uid=1001,.euid=1001,.suid=1001,
        .gid=1002,.egid=1002,.sgid=1002,.umask=0027};
    vfs_init(); int error;
    file_t *file=vfs_open_creds("/run/a",VFS_O_CREAT|VFS_O_RDWR,&expected,&error);
    assert(file && !error && vfs_write(file,"keep",4)==4);
    unsigned before=calls; size_t allocated=live;
    assert(!vfs_open_creds("/run/a",VFS_O_RDONLY|VFS_O_TRUNC,&expected,&error));
    assert(error==-VFS_EINVAL && calls==before && live==allocated && file->node->size==4);
    assert(!vfs_open_creds("/run/absent",VFS_O_RDONLY|VFS_O_TRUNC|VFS_O_CREAT,&expected,&error));
    assert(error==-VFS_EINVAL && calls==before && live==allocated);
    assert(!vfs_lookup_ref("/run/absent",&error) && error==-VFS_ENOENT);
    file->flags=VFS_O_RDONLY; uint64_t offset=file->offset;
    assert(vfs_write(file,"bad",3)==-VFS_EBADF && file->offset==offset && file->node->size==4);
    file->flags=VFS_O_WRONLY; char bytes[4]={0};
    assert(vfs_read(file,bytes,4)==-VFS_EBADF && file->offset==offset);
    file->flags=VFS_O_RDWR; file->offset=0;
    assert(vfs_read(file,bytes,4)==4 && !memcmp(bytes,"keep",4));
    assert(!vfs_close(file));
    before=writes; assert(!vfs_rename_creds("/run/a","/run/a",&expected));
    assert(writes>=before+2);
    assert(vfs_rename_creds("/run/no","/run/no",&expected)==-VFS_ENOENT);
    assert(vfs_rename_creds("/run/a/","/run/a",&expected)==-VFS_ENOTDIR);
    assert(vfs_rename_creds("/run/a","/run/a/",&expected)==-VFS_ENOTDIR);
    assert(vfs_rename_kernel("/run/no","/run/no")==-VFS_ENOENT);

    file=vfs_open_creds("/run",VFS_O_RDONLY,&expected,&error); assert(file);
    vfs_dirent_t dent; before=calls;
    /* Coherent mode replacement under runfs exclusion; no descriptor revoke. */
    uint64_t flags=spin_lock_irqsave(&run_lock); file->node->mode=VFS_S_IFDIR;
    spin_unlock_irqrestore(&run_lock,flags);
    assert(vfs_readdir_file(file,&dent)==1 && file->offset==1 && calls==before);
    assert(vfs_readdir_file(file,&dent)==1 && file->offset==2);
    assert(vfs_readdir_file(file,&dent)==0 && file->offset==2);
    file->flags=VFS_O_WRONLY;
    assert(vfs_readdir_file(file,&dent)==-VFS_EBADF && file->offset==2);
    file->flags=VFS_O_RDONLY;
    int (*original)(vfs_node_t *,uint64_t,void *)=file->node->readdir;
    file->node->readdir=enumeration_error;
    assert(vfs_readdir_file(file,&dent)==-VFS_EIO && file->offset==2);
    file->node->readdir=original; assert(!vfs_close(file));
    assert(!vfs_unlink_kernel("/run/a"));
    kfree(g_vfs_root);g_vfs_root=NULL;assert(!live);
    puts("PASS descriptor admission: invalid truncate zero effects, access modes, readdir rights/error/offset, same-path source/slash validation and hooks");
    return 0;
}
