/* Actual VFS/runfs namespace operations with the existing hosted fd, page,
 * allocator and scheduler adapters. No physical-storage/IRQ/SMP claim. */
#define TEST_PERMISSIONS_SYSCALLS
#define main inherited_pipe_fixture_main
#include "pipe_host.c"
#undef main
#include "../src/fs/runfs.c"

static unsigned metadata_calls;
static bool fail_metadata;
static file_t *callback_file;
static int snapshot(vfs_node_t *node,vfs_metadata_t *out) {
    metadata_calls++;
    assert(callback_file->ref_count==2); /* retained operation reference */
    *out=(vfs_metadata_t){.size=node->size,.type=node->type,.mode=node->mode,
        .uid=node->uid,.gid=node->gid,.mnt_flags=node->mnt_flags};
    return fail_metadata ? -VFS_EIO : 0;
}
static void syscall_cases(void) {
    _Static_assert(SYS_FSTAT==78,"allocated syscall number");
    _Static_assert(sizeof(vfs_stat_t)==16,"legacy layout unchanged");
    _Static_assert(sizeof(stat_ext_v1_t)==40,"existing extended layout unchanged");
    struct {uint64_t left;stat_ext_v1_t value;uint64_t right;} out,canary;
    memset(&out,0xa5,sizeof(out));canary=out;
    vfs_node_t node={.size=1234567890123ULL,.type=VFS_FILE,.mode=VFS_S_IFREG|0600,
        .uid=UINT32_MAX,.gid=4000000000u,.mnt_flags=VFS_MNT_RDONLY|VFS_MNT_NOSUID,.metadata=snapshot};
    file_t file={.node=&node,.ref_count=1,.offset=123};
    current.fd_table[3]=&file;callback_file=&file;
    size_t alloc_before=alloc_calls;
    assert(!sys_fstat(3,(uintptr_t)&out.value));
    assert(out.value.size==40 && out.value.version==1 && !out.value.reserved);
    assert(out.value.file_size==node.size && out.value.type==node.type && out.value.mode==node.mode);
    assert(out.value.uid==node.uid && out.value.gid==node.gid && out.value.mnt_flags==node.mnt_flags);
    assert(out.left==canary.left && out.right==canary.right && file.ref_count==1 && file.offset==123);
    node.type=VFS_DIRECTORY;node.mode=VFS_S_IFDIR|0755;
    assert(!sys_fstat(3,(uintptr_t)&out.value) && out.value.type==VFS_DIRECTORY);
    node.type=VFS_STREAM;node.mode=VFS_S_IFIFO|0600;
    assert(!sys_fstat(3,(uintptr_t)&out.value) && out.value.type==VFS_STREAM && out.value.mode==node.mode);
    /* Closed/out-of-range/full-register fd rejection precedes user validation. */
    unsigned calls=validation_calls;
    for(unsigned i=0;i<4;i++) {
        uint64_t fd=i==0 ? 4 : i==1 ? 32 : i==2 ? UINT64_MAX : (1ULL<<32)|3;
        assert(sys_fstat(fd,0)==SYSCALL_EBADF);
    }
    assert(validation_calls==calls);
    out=canary;calls=metadata_calls;
    assert(sys_fstat(3,0)==SYSCALL_EFAULT);
    valid_range=false; /* inaccessible/kernel/readonly page adapter */
    assert(sys_fstat(3,0xffffffff80000000ULL)==SYSCALL_EFAULT);
    assert(sys_fstat(3,(uintptr_t)&out.value)==SYSCALL_EFAULT);
    valid_range=true;
    assert(sys_fstat(3,UINTPTR_MAX-8)==SYSCALL_EFAULT);
    assert(metadata_calls==calls && !memcmp(&out,&canary,sizeof(out)));
    fail_metadata=true;
    assert(sys_fstat(3,(uintptr_t)&out.value)==SYSCALL_EIO);
    reject_range=(uintptr_t)&out.value;
    assert(sys_fstat(3,(uintptr_t)&out.value)==SYSCALL_EFAULT);
    reject_range=0;fail_metadata=false;
    file.node=NULL;assert(sys_fstat(3,(uintptr_t)&out.value)==SYSCALL_EIO);file.node=&node;
    file.ref_count=2147483647;assert(sys_fstat(3,(uintptr_t)&out.value)==SYSCALL_EIO);file.ref_count=1;
    assert(!memcmp(&out,&canary,sizeof(out)) && file.ref_count==1 && alloc_calls==alloc_before);
    /* Snapshot errors cannot publish a callback's partially filled result. */
    current.fd_table[3]=NULL;
    assert(sys_fstat(3,(uintptr_t)&out.value)==SYSCALL_EBADF);
    callback_file=NULL;
    puts("PASS actual SYS_FSTAT: all types, full-width fields, EBADF/EFAULT/EIO precedence, retention/canaries/no allocation");
}

static void namespace_lifetime(void) {
    vfs_node_t root={.type=VFS_DIRECTORY,.mode=VFS_S_IFDIR|0755,.path="/"};
    g_vfs_root=&root;runfs_init(&root);
    creds_t actor;creds_init_root(&actor);
    int err=0;
    file_t *held=vfs_open_mode_creds("/tmp/fstat-old",VFS_O_CREAT|VFS_O_RDWR,0600,&actor,&err);
    assert(held && !err);
    assert(vfs_write_creds(held,"original",8,&actor)==8);
    current.fd_table[3]=held;
    stat_ext_v1_t before,after,path_value;
    assert(!sys_fstat(3,(uintptr_t)&before));
    assert(!sys_stat_ext((uintptr_t)"/tmp/fstat-old",(uintptr_t)&path_value,sizeof(path_value),1));
    assert(!memcmp(&before,&path_value,sizeof(before)));
    assert(vfs_file_get(held));current.fd_table[4]=held; /* dup-owned reference */
    assert(!sys_fstat(4,(uintptr_t)&after) && !memcmp(&before,&after,sizeof(before)));
    vfs_node_t *original=held->node;
    assert(!vfs_rename_creds("/tmp/fstat-old","/tmp/fstat-new",&actor));
    vfs_node_t *absent=vfs_lookup_creds("/tmp/fstat-old",&actor,&err);
    assert(!absent && err==-VFS_ENOENT);
    file_t *replacement=vfs_open_mode_creds("/tmp/fstat-old",VFS_O_CREAT|VFS_O_RDWR,0644,&actor,&err);
    assert(replacement && replacement->node!=original);
    assert(vfs_write_creds(replacement,"other",5,&actor)==5);
    assert(!sys_fstat(3,(uintptr_t)&after) && !memcmp(&before,&after,sizeof(before)));
    vfs_stat_t legacy;
    assert(!sys_stat((uintptr_t)"/tmp/fstat-old",(uintptr_t)&legacy) && legacy.size==5);
    assert(sys_stat((uintptr_t)"/tmp/missing",(uintptr_t)&legacy)==SYSCALL_ENOENT);
    vfs_metadata_t snap;
    assert(!vfs_metadata(held->node,&snap) && snap.size==8 && snap.mode==(VFS_S_IFREG|0600));
    assert(!vfs_unlink_creds("/tmp/fstat-new",&actor));
    assert(!sys_fstat(3,(uintptr_t)&after) && !memcmp(&before,&after,sizeof(before)));
    assert(!sys_fstat(4,(uintptr_t)&after) && after.file_size==8);
    current.fd_table[4]=NULL;assert(!vfs_close(held));
    assert(!vfs_metadata(held->node,&snap) && snap.size==8 && held->node==original);
    char bytes[8];held->offset=0;assert(vfs_read(held,bytes,8)==8 && !memcmp(bytes,"original",8));
    current.fd_table[3]=NULL;assert(!vfs_close(held));
    assert(!vfs_close(replacement));
    assert(!vfs_unlink_creds("/tmp/fstat-old",&actor));
    g_vfs_root=NULL;
    clean();
}
int main(void) {
    current.tid=100;current.is_user=true;creds_init_root(&current.creds);
    assert(!process_record_begin(100,0,false,0,0));
    assert(process_record_bind_creds(100,&current.creds));process_record_commit(100);
    syscall_cases();
    namespace_lifetime();
    process_record_exit(100,0);process_record_forget(100);
    puts("PASS actual runfs rename/replacement/unlink preserves the retained opened object");
    return 0;
}
