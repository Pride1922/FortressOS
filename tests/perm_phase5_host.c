#define TEST_PERMISSIONS_ENFORCEMENT
#define TEST_PERMISSIONS_SYSCALLS
#define main pipe_fixture_main
#include "pipe_host.c"
#undef main
cpu_local_t cpu_locals[MAX_DETECTED_CPUS];
volatile bool g_cpu_installed[MAX_DETECTED_CPUS];
static unsigned profile_calls,log_calls,read_calls,config_calls,spawn_calls;
size_t net_poll_profile_format(char *p,size_t n) {(void)p;(void)n;profile_calls++;return 0;}
void dmesg_append_str(const char *p,size_t n) {(void)p;(void)n;log_calls++;}
size_t dmesg_read(char *p,size_t n) {read_calls++;if(n) p[0]='x';return n ? 1:0;}
int net_validate_ifset(const netctl_ifset_t *p,net_config_t *out) {
    (void)p;memset(out,0,sizeof(*out));return 0;
}
void net_set_config(const net_config_t *p) {(void)p;config_calls++;}
/* Adapter ends at construction: real set-ID/descriptor/publication is QEMU. */
int64_t process_spawn_from_vfs_group(const char *p,int argc,const char *const argv[],
 int envc,const char *const envp[],const char *cwd,int count,const spawn_kaction_t *a,
 uint32_t flags,uint64_t group,int64_t *pid) {
    (void)argc;(void)argv;(void)envc;(void)envp;(void)cwd;(void)count;(void)a;(void)flags;(void)group;(void)pid;
    assert(!strcmp(p,"/bin/sudo"));spawn_calls++;return SYSCALL_ENOEXEC;
}
int main(void) {
    current.tid=41;current.is_user=true;memcpy(current.cwd,"/",2);creds_init_root(&current.creds);
    current.creds.uid=current.creds.euid=current.creds.suid=1000;current.creds.cap_effective=0;
    assert(!process_record_begin(41,0,false,0,0));assert(process_record_bind_creds(41,&current.creds));process_record_commit(41);
    char buf[16];memset(buf,0xa5,sizeof(buf));netctl_ifset_t cfg={0};
    assert(sys_dmesg((uintptr_t)buf,sizeof(buf))==SYSCALL_EPERM);
    assert(sys_dmesg(0,0)==SYSCALL_EPERM && !profile_calls && !log_calls && !read_calls);
    for(unsigned i=0;i<sizeof(buf);i++)assert((unsigned char)buf[i]==0xa5);
    assert(sys_net_ifset((uintptr_t)&cfg,sizeof(cfg))==SYSCALL_EPERM && !config_calls);
    creds_t before=current.creds,next=before;next.cap_effective=CAP_SYS_ADMIN;
    assert(process_record_publish_creds(41,&before,&next));
    assert(sys_dmesg((uintptr_t)buf,sizeof(buf))==SYSCALL_EPERM);
    valid_range=false;assert(sys_net_ifset((uintptr_t)&cfg,sizeof(cfg))==SYSCALL_EFAULT && !config_calls);
    valid_range=true;assert(!sys_net_ifset((uintptr_t)&cfg,sizeof(cfg)) && config_calls==1);
    before=current.creds;next=before;next.euid=0;next.cap_effective=0;
    assert(process_record_publish_creds(41,&before,&next));
    assert(sys_dmesg((uintptr_t)buf,sizeof(buf))==1 && buf[0]=='x');
    assert(sys_net_ifset((uintptr_t)&cfg,sizeof(cfg))==SYSCALL_EPERM && config_calls==1);
    spawn_opts_t seed={.size=sizeof(seed),.version=1},opts;
    unsigned rng=70089;
    for(unsigned i=0;i<10000;i++) {
        opts=seed;rng=rng*1664525+1013904223;
        switch(i%6) {
        case 0:opts.size=rng|1;break;case 1:opts.version=3+(rng%100);break;
        case 2:opts.reserved0=rng|1;break;case 3:opts.reserved1=rng|1;break;
        case 4:opts.flags=rng|4;break;case 5:opts.action_count=MAX_SPAWN_ACTIONS+1+(rng%100);break;
        }
        assert(sys_spawn_ext((uintptr_t)"/bin/sudo",(uintptr_t)&opts,sizeof(opts))==SYSCALL_EINVAL);
        assert(!heap_live && !spawn_calls && !memcmp(&current.creds,&next,sizeof(next)));
    }
    for(int failure=0;failure<3;failure++) {
        spawn_fd_action_t a={.type=SPAWN_FD_ACTION_OPEN,.dst_fd=9,.path=(uintptr_t)"/etc/shadow",.flags=VFS_O_RDONLY};
        const char *env[]={"PATH=/evil",NULL};opts=seed;opts.action_count=1;opts.fd_actions=(uintptr_t)&a;opts.envp=(uintptr_t)env;
        fail_after=failure;
        assert(sys_spawn_ext((uintptr_t)"/bin/sudo",(uintptr_t)&opts,sizeof(opts))==SYSCALL_ENOMEM && !heap_live);
    }
    fail_after=-1;assert(sys_spawn_ext((uintptr_t)"/bin/sudo",(uintptr_t)&seed,sizeof(seed))==SYSCALL_ENOEXEC && !heap_live && spawn_calls==1);
    process_record_exit(41,0);process_record_forget(41);
    puts("PASS actual root dmesg/admin IFSET denial invariance; 10000 malformed set-ID spawn options and allocation cleanup");
}
