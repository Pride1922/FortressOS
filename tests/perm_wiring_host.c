/* Actual VFS/runfs/devfs/TarFS; pthread filesystem adapters, not IRQ proof. */
#define FORTRESS_PERMISSIONS_TRACE
#define TEST_PERMISSIONS_WIRING
#define main phase0_main
#include "perm_fs_host.c"
#undef main
static creds_t expected;
static unsigned calls, searches, reads, writes, executes;
void vfs_permission_trace(const vfs_node_t *node,unsigned mask,const creds_t *actor) {
    assert(node && actor && !memcmp(actor,&expected,sizeof(expected)));
    assert(mask && !(mask & ~7u));
    calls++;
    if (mask & VFS_MAY_READ) reads++;
    if (mask & VFS_MAY_WRITE) writes++;
    if (mask & VFS_MAY_EXEC) { executes++;if (node->type==VFS_DIRECTORY) searches++; }
}
int main(void) {
    expected=(creds_t){.uid=1001,.euid=1234,.suid=1001,.gid=1002,.egid=5678,.sgid=1002,
        .groups={44,104},.ngroups=2,.umask=0027};
    vfs_init();int error=0;
    assert(!vfs_lookup_creds("/run",NULL,&error) && error==-VFS_EINVAL);
    assert(!vfs_open_creds("/run/a",VFS_O_CREAT|VFS_O_RDWR,NULL,&error));
    assert(vfs_mkdir_creds("/run/nest",0700,&expected)==0);
    vfs_node_t *dir=vfs_lookup_creds("/run/nest",&expected,&error);assert(dir && !error);
    vfs_metadata_t meta;assert(!vfs_metadata(dir,&meta));
    assert(meta.mode==(VFS_S_IFDIR|0700) && meta.uid==1234 && meta.gid==5678);
    vfs_node_put(dir);
    file_t *file=vfs_open_creds("/run/nest/a",VFS_O_CREAT|VFS_O_RDWR,&expected,&error);assert(file);
    assert(!vfs_metadata(file->node,&meta) && meta.uid==1234 && meta.gid==5678 && meta.mode==(VFS_S_IFREG|0644));
    assert(vfs_write(file,"one",3)==3);assert(!vfs_close(file));
    file=vfs_open_mode_creds("/run/nest/mode",VFS_O_CREAT|VFS_O_WRONLY,06754,&expected,&error);assert(file);
    assert(!vfs_metadata(file->node,&meta) && meta.mode==(VFS_S_IFREG|06754) && meta.uid==1234 && meta.gid==5678);
    assert(!vfs_close(file));assert(!vfs_unlink_creds("/run/nest/mode",&expected));
    unsigned before=calls;
    file=vfs_open_exec_creds("/run/nest/a",&expected,&error);assert(file && calls>before);
    assert(!vfs_close(file)); /* 0644 execution remains permissive. */
    file=vfs_open_creds("/run/nest/a",VFS_O_WRONLY|VFS_O_TRUNC,&expected,&error);assert(file);assert(!vfs_close(file));
    dir=vfs_lookup_creds("/run/nest",&expected,&error);vfs_dirent_t dent;
    assert(vfs_readdir_creds(dir,0,&dent,&expected)==1);vfs_node_put(dir);
    /* runfs rename is intentionally unsupported, but admission is still wired. */
    before=writes;assert(vfs_rename_creds("/run/nest/a","/run/nest/b",&expected)==-VFS_EROFS);assert(writes>=before+2);
    assert(!vfs_unlink_creds("/run/nest/a",&expected));assert(!vfs_unlink_creds("/run/nest",&expected));
    assert(!vfs_open_creds("/bin/absent",VFS_O_RDONLY,&expected,&error) && error==-VFS_ENOENT);
    before=calls;file=vfs_open_ext_kernel("/dev/null",VFS_O_WRONLY,&error);assert(file && calls==before);assert(!vfs_close(file));
    assert(searches>=12 && reads && writes && executes);
    kfree(g_vfs_root);g_vfs_root=NULL;assert(!live);
    puts("PASS Phase1 actual VFS: actor value/masks, owned walk, open/exec/truncate/create/readdir/unlink/rename, trusted bypass, missing actor");
    return 0;
}
