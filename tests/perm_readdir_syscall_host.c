/* Actual SYS_READDIR and VFS descriptor adapter; no scheduler/IRQ claim. */
#define TEST_PERMISSIONS_READDIR
#define main pipe_fixture_main
#include "pipe_host.c"
#undef main
static int result;
static int directory_read(vfs_node_t *node,uint64_t offset,void *output) {
    (void)node; assert(offset==7);
    if (result==1) {
        vfs_dirent_t *dent=output; memset(dent,0,sizeof(*dent));
        memcpy(dent->name,"entry",6); dent->type=VFS_FILE; dent->size=123;
    }
    return result;
}
int main(void) {
    vfs_node_t node={.type=VFS_DIRECTORY,.mode=VFS_S_IFDIR,.readdir=directory_read};
    file_t file={.node=&node,.flags=VFS_O_RDONLY,.offset=7,.ref_count=1};
    current.fd_table[4]=&file;
    /* No published credential binding: descriptor rights alone are sufficient. */
    vfs_dirent_t output,before; memset(&output,0xa5,sizeof(output));
    memcpy(&before,&output,sizeof(output));
    result=-VFS_EIO;
    assert(sys_readdir(4,(uintptr_t)&output)==SYSCALL_EIO && file.offset==7);
    assert(!memcmp(&before,&output,sizeof(output)));
    result=-VFS_EACCES;
    assert(sys_readdir(4,(uintptr_t)&output)==SYSCALL_EACCES && file.offset==7);
    result=0; assert(!sys_readdir(4,(uintptr_t)&output) && file.offset==7);
    assert(!memcmp(&before,&output,sizeof(output)));
    file.flags=VFS_O_WRONLY; result=1;
    assert(sys_readdir(4,(uintptr_t)&output)==SYSCALL_EBADF && file.offset==7);
    file.flags=VFS_O_RDONLY; valid_range=false;
    assert(sys_readdir(4,(uintptr_t)&output)==SYSCALL_EFAULT && file.offset==7);
    valid_range=true;
    assert(sys_readdir(4,(uintptr_t)&output)==1 && file.offset==8);
    assert(!strcmp(output.name,"entry") && output.size==123 && output.type==VFS_FILE);
    current.fd_table[4]=NULL;
    assert(!heap_live && !pages_live);
    puts("PASS actual SYS_READDIR: admitted descriptor, errno propagation, output fault/no publication, success-only offset");
    return 0;
}
