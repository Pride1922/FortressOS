#include "common.h"
#include "ext4.h"
static uint8_t bytes[EXT4_STAGE_MAX];
static uint64_t times[640];
static stat_ext_v1_t meta;
static void say(const char *s){tool_write("stage-probe",s,tool_length(s));}
#define CHECK(x) do{if(!(x)){say("STAGE FAIL " #x "\n");return 1;}}while(0)
static long call5(long nr,uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e){
    register uintptr_t r10 __asm__("r10")=d;register uintptr_t r8 __asm__("r8")=e;long result;
    __asm__ volatile("syscall":"=a"(result):"a"(nr),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8):"rcx","r11","memory");return result;
}
static void number(const char *key,uint64_t n){char s[32];say("STAGE ");say(key);say("=");tool_format_u64(s,n);say(s);say("\n");}
static long open_file(const char *p,unsigned flags){return tool_syscall(SYS_OPEN,(uintptr_t)p,flags,0);}
static long batch(long fd,uint64_t at,size_t n){return call5(SYS_STAGE_WRITE,fd,(uintptr_t)bytes,n,at,0);}
int stage_probe_main(int argc,char **argv){
    if(argc>1&&tool_equal(argv[1],"peer")){
        long reader=open_file(argv[2],VFS_O_RDONLY),log=open_file("/mnt/stage-log",VFS_O_CREAT|VFS_O_WRONLY|VFS_O_APPEND);CHECK(reader>=0&&log>=0);
        uint64_t overlap=0,maximum=0;
        for(unsigned i=0;i<64;i++){
            uint64_t t=tool_rdtsc();CHECK(!tool_syscall(SYS_FSTAT,reader,(uintptr_t)&meta,0));
            if(meta.file_size>0&&meta.file_size<10485760)overlap++;
            long n=tool_syscall(SYS_READ,reader,(uintptr_t)bytes,32);CHECK(n>=0);
            for(long j=0;j<n;j++)CHECK(bytes[j]=='B');
            CHECK(tool_syscall(SYS_WRITE,log,(uintptr_t)"LOG\n",4)==4);
            uint64_t elapsed=tool_rdtsc()-t;if(elapsed>maximum)maximum=elapsed;
        }
        CHECK(!tool_syscall(SYS_CLOSE,reader,0,0)&&!tool_syscall(SYS_CLOSE,log,0,0));
        number(tool_equal(argv[2],"/mnt/stage-base")?"base_peer_overlap":"batch_peer_overlap",overlap);
        number(tool_equal(argv[2],"/mnt/stage-base")?"base_peer_max_tsc":"batch_peer_max_tsc",maximum);
        return 0;
    }
    for(unsigned i=0;i<sizeof(bytes);i++)bytes[i]='B';
    long f=open_file("/mnt/stage-check",VFS_O_CREAT|VFS_O_RDWR|VFS_O_EXCL);CHECK(f>=0);
    CHECK(call5(SYS_STAGE_WRITE,99,0,0,0,0)==SYSCALL_EBADF);
    CHECK(batch(f,0,0)==SYSCALL_EINVAL);
    CHECK(call5(SYS_STAGE_WRITE,f,0,1,0,0)==SYSCALL_EFAULT);
    CHECK(call5(SYS_STAGE_WRITE,f,(uintptr_t)bytes,1,0,1)==SYSCALL_EINVAL);
    CHECK(batch(f,1,1)==SYSCALL_EAGAIN);CHECK(batch(f,0,1)==1);
    CHECK(batch(f,1,sizeof(bytes))==131071);CHECK(!tool_syscall(SYS_FSYNC,f,0,0));
    CHECK(!tool_syscall(SYS_CLOSE,f,0,0));
    /* Keep ordinary write behavior and its syscall limit unchanged. */
    f=open_file("/mnt/stage-base",VFS_O_CREAT|VFS_O_RDWR|VFS_O_EXCL);CHECK(f>=0);
    CHECK(tool_syscall(SYS_WRITE,f,(uintptr_t)bytes,sizeof(bytes))==SYSCALL_EINVAL);
    for(unsigned mode=0;mode<3;mode++){
        const char *path=mode==0?"/mnt/stage-base":mode==1?"/mnt/stage-batch":"/mnt/stage-100";
        if(mode) {f=open_file(path,VFS_O_CREAT|VFS_O_RDWR|VFS_O_EXCL);CHECK(f>=0);}
        uint64_t length=mode==2?104857600:10485760,off=0,begin=tool_rdtsc(),maximum=0;unsigned calls=0;long child=-1;
        while(off<length){
            size_t n=mode?sizeof(bytes):16384;
            if(n>length-off)n=(size_t)(length-off);
            uint64_t t=tool_rdtsc();long got=mode?batch(f,off,n):tool_syscall(SYS_WRITE,f,(uintptr_t)bytes,n);
            uint64_t elapsed=tool_rdtsc()-t;CHECK(got>0&&got<=(long)n);off+=(uint64_t)got;
            if(elapsed>maximum)maximum=elapsed;
            if(calls<640)times[calls]=elapsed;
            calls++;
            if(calls==1&&mode<2){const char *args[]={"stage-probe","peer",path,0};child=tool_syscall(SYS_SPAWN,(uintptr_t)"/bin/stage-probe",(uintptr_t)args,0);CHECK(child>0);}
        }
        CHECK(!tool_syscall(SYS_FSYNC,f,0,0));uint64_t elapsed=tool_rdtsc()-begin;
        CHECK(!tool_syscall(SYS_CLOSE,f,0,0));
        if(child>0){int64_t status=-1;CHECK(!tool_syscall(SYS_WAIT,child,(uintptr_t)&status,0)&&status==0);}
        number(mode==0?"base_calls":mode==1?"batch_calls":"large_calls",calls);
        number(mode==0?"base_tsc":mode==1?"batch_tsc":"large_tsc",elapsed);
        number(mode==0?"base_max_tsc":mode==1?"batch_max_tsc":"large_max_tsc",maximum);
        if(mode<2){
            for(unsigned i=1;i<calls;i++){uint64_t x=times[i];unsigned j=i;while(j&&times[j-1]>x){times[j]=times[j-1];j--;}times[j]=x;}
            number(mode?"batch_p95_tsc":"base_p95_tsc",times[(calls-1)*95/100]);
        }
    }
    say("STAGE Ring3 PASS\n");return 0;
}
