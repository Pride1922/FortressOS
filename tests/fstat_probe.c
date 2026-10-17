#include "common.h"
#include "../user/permissions_cli.h"
#define CHECK(x) do {if(!(x)) {say("FSTAT FAIL: " #x "\n");return 1;}}while(0)
static void say(const char *s) {tool_write("fstat-probe",s,tool_length(s));}
static bool equal(const void *a,const void *b,size_t n) {
    const uint8_t *x=a,*y=b;for(size_t i=0;i<n;i++) if(x[i]!=y[i]) return false;return true;
}
static long statfd(long fd,void *out) {return tool_syscall(SYS_FSTAT,(uintptr_t)fd,(uintptr_t)out,0);}
static uint32_t cpu(void) {
    uint32_t a,b,c,d;__asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1),"c"(0));return b>>24;
}
static size_t number(char *out,uint64_t n,unsigned base) {
    char tmp[32];size_t k=0;do {tmp[k++]=(char)('0'+n%base);n/=base;}while(n);
    for(size_t i=0;i<k;i++) out[i]=tmp[k-1-i];
    return k;
}
static void print(const stat_ext_v1_t *s) {
    char line[200];size_t n=0;
    const char *labels[]={"FSTAT type="," size="," mode=0"," uid="," gid="," cpu="};
    uint64_t values[]={s->type,s->file_size,s->mode,s->uid,s->gid,cpu()};
    for(unsigned i=0;i<6;i++) {
        const char *p=labels[i];while(*p) line[n++]=*p++;
        n+=number(line+n,values[i],i==2 ? 8 : 10);
    }
    line[n++]='\n';tool_write("fstat-probe",line,n);
}
static int errors(long fd) {
    stat_ext_v1_t s,canary;
    for(size_t i=0;i<sizeof(s);i++) ((uint8_t *)&s)[i]=0xa5;
    canary=s;
    CHECK(statfd(-1,0)==SYSCALL_EBADF);
    CHECK(statfd(32,0)==SYSCALL_EBADF);
    CHECK(statfd((1ULL<<32)|(uint64_t)fd,0)==SYSCALL_EBADF);
    CHECK(statfd(fd,NULL)==SYSCALL_EFAULT);
    CHECK(statfd(fd,(void *)0xffffffff80000000ULL)==SYSCALL_EFAULT);
    CHECK(statfd(fd,(void *)(UINTPTR_MAX-8))==SYSCALL_EFAULT);
    CHECK(statfd(fd,(void *)errors)==SYSCALL_EFAULT);
    CHECK(statfd(-1,&s)==SYSCALL_EBADF && equal(&s,&canary,sizeof(s)));
    return 0;
}
static int selftest(void) {
    proc_info_t self;CHECK(tool_syscall(SYS_PROCINFO,PROC_INFO_SELF,(uintptr_t)&self,0)==1);
    char old[64]="/tmp/fstat-",next[64];size_t n=tool_length(old);
    n+=number(old+n,(uint64_t)self.pid,10);old[n]=0;
    for(size_t i=0;i<=n;i++) next[i]=old[i];
    next[n++]='-';next[n++]='n';next[n]=0;
    long mask=tool_syscall(SYS_UMASK,0077,0,0);CHECK(mask>=0);
    long fd=tool_syscall(SYS_OPEN,(uintptr_t)old,VFS_O_CREAT|VFS_O_RDWR,0);
    CHECK(tool_syscall(SYS_UMASK,(uintptr_t)mask,0,0)>=0 && fd>=0);
    CHECK(tool_syscall(SYS_WRITE,fd,(uintptr_t)"original",8)==8);
    stat_ext_v1_t before,after,path;
    CHECK(!statfd(fd,&before));CHECK(before.size==40 && before.version==1 && !before.reserved);
    CHECK(before.file_size==8 && before.type==VFS_FILE && before.mode==(VFS_S_IFREG|0600));
    CHECK(!permission_call4(SYS_STAT_EXT,(uintptr_t)old,(uintptr_t)&path,sizeof(path),1));
    CHECK(equal(&before,&path,sizeof(before)));
    long dup=tool_syscall(SYS_DUP,fd,0,0);CHECK(dup>=0);
    CHECK(!tool_syscall(SYS_RENAME,(uintptr_t)old,(uintptr_t)next,0));
    vfs_stat_t legacy;
    CHECK(tool_syscall(SYS_STAT,(uintptr_t)old,(uintptr_t)&legacy,0)==SYSCALL_ENOENT);
    CHECK(!statfd(fd,&after) && equal(&before,&after,sizeof(before)));
    long replacement=tool_syscall(SYS_OPEN,(uintptr_t)old,VFS_O_CREAT|VFS_O_RDWR,0);CHECK(replacement>=0);
    CHECK(tool_syscall(SYS_WRITE,replacement,(uintptr_t)"other",5)==5);
    CHECK(!tool_syscall(SYS_STAT,(uintptr_t)old,(uintptr_t)&legacy,0) && legacy.size==5);
    CHECK(!statfd(fd,&after) && after.file_size==8);
    CHECK(!tool_syscall(SYS_UNLINK,(uintptr_t)next,0,0));
    CHECK(!statfd(dup,&after) && equal(&before,&after,sizeof(before)));
    CHECK(!tool_syscall(SYS_CLOSE,dup,0,0));
    for(unsigned i=0;i<32;i++) {
        CHECK(!statfd(fd,&after) && equal(&before,&after,sizeof(before)));
        for(volatile unsigned j=0;j<1000000;j++) {}
    }
    CHECK(!errors(fd));print(&after);
    CHECK(!tool_syscall(SYS_CLOSE,fd,0,0));
    CHECK(statfd(fd,&after)==SYSCALL_EBADF);
    CHECK(!tool_syscall(SYS_CLOSE,replacement,0,0));
    CHECK(!tool_syscall(SYS_UNLINK,(uintptr_t)old,0,0));
    int pipes[2];CHECK(!tool_syscall(SYS_PIPE,(uintptr_t)pipes,0,0));
    CHECK(!statfd(pipes[0],&after) && after.type==VFS_STREAM && (after.mode&VFS_S_IFMT)==VFS_S_IFIFO);
    CHECK(!tool_syscall(SYS_CLOSE,pipes[0],0,0) && !tool_syscall(SYS_CLOSE,pipes[1],0,0));
    say("FSTAT selftest PASS\n");return 0;
}
static int workers(void) {
    long pids[8];int readers[8];unsigned count=0;int result=0;
    const char *argv[]={"fstat-probe","--selftest",NULL};
    for(unsigned i=0;i<8;i++) {
        int fds[2];if(tool_syscall(SYS_PIPE,(uintptr_t)fds,0,0)) {result=1;break;}
        spawn_fd_action_t actions[]={
            {.type=SPAWN_FD_ACTION_DUP2,.dst_fd=1,.src_fd=fds[1]},
            {.type=SPAWN_FD_ACTION_CLOSE,.dst_fd=fds[0]},
            {.type=SPAWN_FD_ACTION_CLOSE,.dst_fd=fds[1]}};
        spawn_opts_t opts={.size=sizeof(opts),.version=1,.argv=(uintptr_t)argv,
            .fd_actions=(uintptr_t)actions,.action_count=3};
        pids[i]=tool_syscall(SYS_SPAWN_EXT,(uintptr_t)"/bin/fstat-probe",(uintptr_t)&opts,sizeof(opts));
        tool_syscall(SYS_CLOSE,fds[1],0,0);
        if(pids[i]<0) {tool_syscall(SYS_CLOSE,fds[0],0,0);result=1;break;}
        readers[i]=fds[0];count++;
    }
    for(unsigned i=0;i<count;i++) {
        char bytes[512];long n;
        while((n=tool_syscall(SYS_READ,readers[i],(uintptr_t)bytes,sizeof(bytes)))>0)
            if(tool_write("fstat-probe",bytes,(size_t)n)) result=1;
        if(n<0) result=1;
        tool_syscall(SYS_CLOSE,readers[i],0,0);
        int64_t status=-1;if(tool_syscall(SYS_WAIT,pids[i],(uintptr_t)&status,0) || status) result=1;
    }
    if(!result) say("FSTAT workers PASS\n");
    return result;
}
int fstat_probe_main(int argc,char **argv) {
    if(argc!=2) {say("usage: fstat-probe <file>|--selftest|--workers\n");return 2;}
    if(tool_equal(argv[1],"--selftest")) return selftest();
    if(tool_equal(argv[1],"--workers")) return workers();
    long fd=tool_syscall(SYS_OPEN,(uintptr_t)argv[1],VFS_O_RDONLY,0);
    if(fd<0) {say("FSTAT open failed\n");return 1;}
    stat_ext_v1_t value;long r=statfd(fd,&value);
    if(!r) print(&value);
    int failed=errors(fd);
    long closed=tool_syscall(SYS_CLOSE,fd,0,0);
    if(r || failed || closed) {say("FSTAT FAIL\n");return 1;}
    say("FSTAT probe PASS\n");return 0;
}
