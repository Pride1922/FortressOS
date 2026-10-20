#include "common.h"
/* Raw guest TSC deltas: no conversion using interrupt-driven guest uptime. */
static uint8_t bytes[16384];
static uint64_t durations[640];
static stat_ext_v1_t snapshot;
static void say(const char *s) {tool_write("latency-probe",s,tool_length(s));}
#define CHECK(x) do {if(!(x)){say("LATENCY FAIL " #x "\n");return 1;}}while(0)
static void number(const char *key,uint64_t n) {char b[32];say(key);tool_format_u64(b,n);say(b);say("\n");}
int latency_probe_main(int argc,char **argv) {
    if(argc>1 && tool_equal(argv[1],"peer")) {
        long log=tool_syscall(SYS_OPEN,(uintptr_t)"/mnt/latency-log",VFS_O_CREAT|VFS_O_WRONLY|VFS_O_APPEND,0);
        long reader=tool_syscall(SYS_OPEN,(uintptr_t)"/mnt/latency-stage",VFS_O_RDONLY,0);CHECK(log>=0&&reader>=0);
        uint64_t maximum=0,total=0,overlap=0;
        for(unsigned i=0;i<64;i++) {
            uint64_t t=tool_rdtsc();CHECK(!tool_syscall(SYS_FSTAT,reader,(uintptr_t)&snapshot,0));
            if(snapshot.file_size>0&&snapshot.file_size<10485760)overlap++;
            long n=tool_syscall(SYS_READ,reader,(uintptr_t)bytes,32);CHECK(n>=0);
            for(long k=0;k<n;k++)CHECK(bytes[k]=='L');
            CHECK(tool_syscall(SYS_WRITE,log,(uintptr_t)"LOG\n",4)==4);
            uint64_t elapsed=tool_rdtsc()-t;total+=elapsed;if(elapsed>maximum)maximum=elapsed;
        }
        CHECK(!tool_syscall(SYS_CLOSE,log,0,0)&&!tool_syscall(SYS_CLOSE,reader,0,0));
        number("LATENCY peer_total_tsc=",total);number("LATENCY peer_max_tsc=",maximum);number("LATENCY peer_overlap_samples=",overlap);
        say("LATENCY peer PASS\n");return 0;
    }
    for(unsigned i=0;i<sizeof(bytes);i++)bytes[i]='L';
    long fd=tool_syscall(SYS_OPEN,(uintptr_t)"/mnt/latency-stage",VFS_O_CREAT|VFS_O_RDWR|VFS_O_EXCL,0);CHECK(fd>=0);
    uint64_t begin=tool_rdtsc(),maximum=0,total=0;long child=-1;
    for(unsigned i=0;i<640;i++) {
        uint64_t t=tool_rdtsc();long n=tool_syscall(SYS_WRITE,fd,(uintptr_t)bytes,sizeof(bytes));
        durations[i]=tool_rdtsc()-t;CHECK(n==sizeof(bytes));total+=durations[i];if(durations[i]>maximum)maximum=durations[i];
        if(!i) {const char *args[]={"latency-probe","peer",0};child=tool_syscall(SYS_SPAWN,(uintptr_t)"/bin/latency-probe",(uintptr_t)args,0);CHECK(child>0);}
    }
    uint64_t stage=tool_rdtsc()-begin;CHECK(!tool_syscall(SYS_CLOSE,fd,0,0));
    begin=tool_rdtsc();CHECK(!tool_syscall(SYS_SYNC,0,0,0));uint64_t sync=tool_rdtsc()-begin;
    int64_t status=-1;CHECK(!tool_syscall(SYS_WAIT,child,(uintptr_t)&status,0)&&status==0);
    /* Sort only after all timed I/O. */
    for(unsigned i=1;i<640;i++) {uint64_t x=durations[i];unsigned j=i;while(j&&durations[j-1]>x){durations[j]=durations[j-1];j--;}durations[j]=x;}
    number("LATENCY bytes=",10485760);number("LATENCY calls=",640);
    number("LATENCY stage_tsc=",stage);number("LATENCY writes_total_tsc=",total);
    number("LATENCY write_p50_tsc=",durations[320]);number("LATENCY write_p95_tsc=",durations[607]);
    number("LATENCY write_max_tsc=",maximum);number("LATENCY sync_tsc=",sync);
    say("LATENCY Ring3 PASS\n");return 0;
}
