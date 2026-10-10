/* Actual syscall/registry/VFS adapters; bounded user-range mock, no CPU claim. */
#define TEST_PERMISSIONS_SYSCALLS
#define main pipe_fixture_main
#include "pipe_host.c"
#undef main
static unsigned changes;
static int change(vfs_node_t *n,bool chown,uint32_t mode,creds_id_change_t uid,
                   creds_id_change_t gid,const creds_t *actor) {
    (void)n;assert(actor->euid==0 && actor->ngroups==2);
    if (chown) {assert(uid.value==UINT32_MAX && !uid.keep && gid.keep);}
    else assert(mode==06777);
    changes++;return 0;
}
int main(void) {
    current.tid=100;current.is_user=true;creds_init_root(&current.creds);
    current.creds.ngroups=2;current.creds.groups[0]=42;current.creds.groups[1]=43;
    assert(!process_record_begin(100,0,false,0,0));
    assert(process_record_bind_creds(100,&current.creds));process_record_commit(100);
    uint32_t r=0xa5a5a5a5,e=r,s=r;reject_range=(uintptr_t)&s;
    assert(sys_getres(false,(uintptr_t)&r,(uintptr_t)&e,(uintptr_t)&s)==SYSCALL_EFAULT);
    assert(r==0xa5a5a5a5 && e==r && s==r && validation_calls==3);
    reject_range=0;assert(!sys_getres(false,(uintptr_t)&r,(uintptr_t)&e,(uintptr_t)&s) && !r && !e && !s);
    assert(!sys_getres(true,(uintptr_t)&r,(uintptr_t)&e,(uintptr_t)&s) && !r && !e && !s);
    uint32_t groups[3]={0xa5a5a5a5,0xa5a5a5a5,0xa5a5a5a5};
    assert(sys_getgroups(0,0)==2 && sys_getgroups(1,(uintptr_t)groups)==SYSCALL_EINVAL && groups[0]==0xa5a5a5a5);
    reject_range=(uintptr_t)groups;assert(sys_getgroups(2,(uintptr_t)groups)==SYSCALL_EFAULT && groups[0]==0xa5a5a5a5);
    reject_range=0;assert(sys_getgroups(3,(uintptr_t)groups)==2 && groups[0]==42 && groups[1]==43 && groups[2]==0xa5a5a5a5);
    assert(sys_umask(0xffffffff)==0022 && current.creds.umask==0777);
    assert(sys_umask(0027)==0777 && current.creds.umask==0027 && !current.creds.reserved && current.creds.groups[0]==42);
    assert(sys_umask(UMASK_QUERY)==0027 && current.creds.umask==0027);
    assert(sys_umask(0x100000000ull)==SYSCALL_EINVAL && current.creds.umask==0027);
    vfs_node_t root={.type=VFS_DIRECTORY,.mode=VFS_S_IFDIR|0755};
    vfs_node_t n={.name="file",.type=VFS_FILE,.mode=VFS_S_IFREG|0644,.setattr_actor=change};root.children=&n;n.parent=&root;g_vfs_root=&root;
    file_t file={.node=&n,.flags=VFS_O_RDONLY,.ref_count=1};current.fd_table[4]=&file;
    assert(sys_fchmod(32,06777)==SYSCALL_EBADF && !changes);
    assert(!sys_fchmod(4,06777) && changes==1);
    char path[]="/file";reject_range=(uintptr_t)path;
    assert(sys_chmod((uintptr_t)path,06777)==SYSCALL_EFAULT && changes==1);
    reject_range=0;assert(!sys_chmod((uintptr_t)path,06777) && changes==2);
    assert(sys_chown((uintptr_t)path,0,0,4)==SYSCALL_EINVAL && changes==2);
    assert(!sys_chown((uintptr_t)path,UINT32_MAX,0,CHOWN_KEEP_GID) && changes==3);
    assert(sys_chown((uintptr_t)path,0x100000000ull,0,0)==SYSCALL_EINVAL && changes==3);
    creds_t before=current.creds;
    assert(sys_setres(false,0x100000000ull,0,0,0)==SYSCALL_EINVAL && !memcmp(&before,&current.creds,sizeof(before)));
    assert(sys_setres(true,0,0,0,8)==SYSCALL_EINVAL && !memcmp(&before,&current.creds,sizeof(before)));
    assert(sys_setgroups(17,0)==SYSCALL_EINVAL);
    reject_range=(uintptr_t)groups;assert(sys_setgroups(2,(uintptr_t)groups)==SYSCALL_EFAULT && !memcmp(&before,&current.creds,sizeof(before)));reject_range=0;
    assert(sys_capset(1ull<<63)==SYSCALL_EINVAL && sys_capget()==CAP_ALL);
    assert(!sys_setgroups(0,0) && current.creds.ngroups==0 && !current.creds.groups[0]);
    groups[0]=1000;groups[1]=10;assert(!sys_setgroups(2,(uintptr_t)groups));
    assert(!sys_setres(true,UINT32_MAX,UINT32_MAX,UINT32_MAX,0) && current.creds.gid==UINT32_MAX);
    assert(!sys_setres(false,1000,1000,1000,0) && !sys_capget());before=current.creds;
    assert(sys_setres(false,0,0,0,0)==SYSCALL_EPERM && !memcmp(&before,&current.creds,sizeof(before)));
    assert(sys_setgroups(0,0)==SYSCALL_EPERM && sys_setres(true,0,0,0,0)==SYSCALL_EPERM);
    assert(!sys_capset(CAP_ALL) && !sys_capget());
    assert(!sys_setres(false,0,0,0,7) && current.creds.uid==1000);
    before=current.creds;creds_t proposed=before;proposed.umask=0077;
    assert(sys_umask(0022)==0027);
    assert(credential_commit(&before,&proposed,0)==SYSCALL_EAGAIN && current.creds.umask==0022);
    process_record_exit(100,0);process_record_forget(100);g_vfs_root=NULL;
    puts("PASS actual metadata/credential syscalls: validate-all-before-output, group bounds/canaries, full-ID keep flags, canonical umask publication, fd owner adapter");
    return 0;
}
