/* Private Ring-3 graphics lifecycle acceptance; excluded from release initramfs. */
#include "common.h"
#include "pthread.h"

static fb_info_v1_t fb;
static char output[128];
static int ready_pipe[2];
static long anon_results[2];
static long call(long nr,uintptr_t a,uintptr_t b,uintptr_t c) { return tool_syscall(nr,a,b,c); }
static void say(const char *s) { call(SYS_WRITE,1,(uintptr_t)s,tool_length(s)); }
static int fail(const char *s) { say("[FB FAIL] "); say(s); say("\n"); return 1; }
#define CHECK(c,s) do {if (!(c)) return fail(s);} while (0)
static void pause_ms(unsigned ms) { call(SYS_POLL,0,0,ms); }
static long spawn(const char *mode,const char *extra) {
    const char *argv[]={"/bin/fb-probe",mode,extra,NULL};
    return call(SYS_SPAWN,(uintptr_t)argv[0],(uintptr_t)argv,0);
}
static bool waited(long pid,uint64_t expected) {
    uint64_t status=999;
    return pid>0 && !call(SYS_WAIT,pid,(uintptr_t)&status,0) && status==expected;
}
static bool acquire_eventually(void) {
    for (unsigned i=0;i<200;i++) {
        long result=call(SYS_FB_ACQUIRE,0,0,0);
        if (!result) return true;
        if (result!=SYSCALL_EBUSY) return false;
        pause_ms(10);
    }
    return false;
}
static bool map(void) { return !call(SYS_FBINFO,(uintptr_t)&fb,0,0); }
static void *shared_thread(void *arg) {
    (void)arg;
    if (call(SYS_FB_ACQUIRE,0,0,0)!=SYSCALL_EBUSY || !map()) return (void *)1;
    if (call(SYS_FB_RELEASE,0,0,0) || call(SYS_FB_ACQUIRE,0,0,0) || !map()) return (void *)2;
    return NULL;
}
static void *anonymous_thread(void *arg) {
    anon_results[(uintptr_t)arg]=call(SYS_MMAP,8192,0,0);
    return NULL;
}
static void mem_line(const char *tag) {
    sysinfo_mem_t mem;
    if (call(SYS_MEMINFO,(uintptr_t)&mem,sizeof(mem),0)) { say("[FB FAIL] meminfo\n"); return; }
    say(tag);
    tool_format_u64(output,mem.pmm_free_frames); say(output); say(" tables=");
    tool_format_u64(output,mem.vmm_table_frames); say(output); say(" deferred=");
    tool_format_u64(output,mem.vmm_deferred_spaces); say(output); say("\n");
}
static long compositor(bool wait_ready) {
    const char *argv[]={"/bin/compositor",NULL};
    if (!wait_ready) return call(SYS_SPAWN,(uintptr_t)argv[0],(uintptr_t)argv,0);
    if (call(SYS_PIPE,(uintptr_t)ready_pipe,0,0)) return -1;
    spawn_fd_action_t action={.type=SPAWN_FD_ACTION_DUP2,.dst_fd=1,.src_fd=ready_pipe[1]};
    spawn_opts_t opts={.size=sizeof(opts),.version=1,.argv=(uintptr_t)argv,
        .fd_actions=(uintptr_t)&action,.action_count=1};
    long pid=call(SYS_SPAWN_EXT,(uintptr_t)argv[0],(uintptr_t)&opts,sizeof(opts));
    call(SYS_CLOSE,ready_pipe[1],0,0);
    size_t len=0;
    while (pid>0 && len<sizeof(output)-1) {
        char c;
        if (call(SYS_READ,ready_pipe[0],(uintptr_t)&c,1)!=1) break;
        if (c=='\n') {
            output[len]=0;
            bool ready=tool_equal(output,"[COMPOSITOR] [OK] Window server ready; polling");
            say(output); say("\n"); len=0;
            if (ready) { call(SYS_CLOSE,ready_pipe[0],0,0); return pid; }
        } else output[len++]=c;
    }
    call(SYS_CLOSE,ready_pipe[0],0,0); return -1;
}
static bool race_owners(void) {
    int start[2], result[2], finish[2];
    if (call(SYS_PIPE,(uintptr_t)start,0,0) || call(SYS_PIPE,(uintptr_t)result,0,0)
        || call(SYS_PIPE,(uintptr_t)finish,0,0)) return false;
    char args[48]; size_t n=tool_format_u64(args,start[0]); args[n++]=',';
    n+=tool_format_u64(args+n,result[1]); args[n++]=',';
    tool_format_u64(args+n,finish[0]);
    long children[8];
    for (unsigned i=0;i<8;i++) if ((children[i]=spawn("race",args))<=0) return false;
    if (call(SYS_WRITE,start[1],(uintptr_t)"SSSSSSSS",8)!=8) return false;
    unsigned winners=0;
    for (unsigned i=0;i<8;i++) {
        char c;
        if (call(SYS_READ,result[0],(uintptr_t)&c,1)!=1 || (c!='W' && c!='B')) return false;
        winners+=c=='W';
    }
    if (call(SYS_WRITE,finish[1],(uintptr_t)"FFFFFFFF",8)!=8) return false;
    for (unsigned i=0;i<8;i++) if (!waited(children[i],0)) return false;
    for (unsigned i=0;i<2;i++) { call(SYS_CLOSE,start[i],0,0); call(SYS_CLOSE,result[i],0,0); call(SYS_CLOSE,finish[i],0,0); }
    return winners==1;
}
int fb_probe_main(int argc,char **argv) {
    CHECK(argc>=2,"mode");
    if (tool_equal(argv[1],"anonexec")) {
        long address=call(SYS_MMAP,4096,0,0);
        CHECK(address>0,"NX arena");
        *(volatile uint8_t *)(uintptr_t)address=0xc3;
        ((void (*)(void))(uintptr_t)address)();
        return fail("anonymous RAM was executable");
    }
    if (tool_equal(argv[1],"anonrace")) {
        pthread_t workers[2];
        CHECK(!pthread_create(&workers[0],NULL,anonymous_thread,(void *)0),"anon thread 0");
        CHECK(!pthread_create(&workers[1],NULL,anonymous_thread,(void *)1),"anon thread 1");
        CHECK(!pthread_join(workers[0],NULL) && !pthread_join(workers[1],NULL),"anon joins");
        CHECK((anon_results[0]>0 && anon_results[1]==SYSCALL_EBUSY)
            || (anon_results[1]>0 && anon_results[0]==SYSCALL_EBUSY),"one anonymous winner");
        return 0;
    }
    if (tool_equal(argv[1],"race")) {
        CHECK(argc==3,"race fds");
        unsigned fds[3]={0}; unsigned i=0;
        for (const char *p=argv[2];*p;p++) {
            if (*p==',') { CHECK(i<2,"race separators"); i++; }
            else { CHECK(*p>='0' && *p<='9',"race fd digit"); fds[i]=fds[i]*10+*p-'0'; }
        }
        char c; CHECK(call(SYS_READ,fds[0],(uintptr_t)&c,1)==1,"race start");
        long result=call(SYS_FB_ACQUIRE,0,0,0);
        CHECK(result==0 || result==SYSCALL_EBUSY,"race acquire");
        c=result==0 ? 'W':'B';
        CHECK(call(SYS_WRITE,fds[1],(uintptr_t)&c,1)==1,"race publish");
        CHECK(call(SYS_READ,fds[2],(uintptr_t)&c,1)==1,"race finish");
        if (!result) CHECK(!call(SYS_FB_RELEASE,0,0,0),"race release");
        return 0;
    }
    if (tool_equal(argv[1],"sample")) { pause_ms(100); mem_line("FB_MEM free="); return 0; }
    if (tool_equal(argv[1],"contend")) {
        CHECK(call(SYS_FB_ACQUIRE,0,0,0)==SYSCALL_EBUSY,"contender acquire");
        CHECK(call(SYS_FB_RELEASE,0,0,0)==SYSCALL_EPERM,"contender release");
        CHECK(call(SYS_FBINFO,(uintptr_t)&fb,0,0)==SYSCALL_EPERM,"contender map");
        say("FB_CONTENDER_PASS\n"); return 0;
    }
    if (tool_equal(argv[1],"normal") || tool_equal(argv[1],"fault") || tool_equal(argv[1],"stale")
        || tool_equal(argv[1],"hold")) {
        CHECK(call(SYS_MMAP,16385,0,0)>0,"child anonymous arena");
        CHECK(acquire_eventually() && map(),"child acquire/map");
        *(volatile uint32_t *)(uintptr_t)fb.fb_user_vaddr=0x00365476;
        if (tool_equal(argv[1],"fault")) { __asm__ volatile("ud2"); }
        if (tool_equal(argv[1],"stale")) {
            CHECK(!call(SYS_FB_RELEASE,0,0,0),"stale release");
            *(volatile uint32_t *)(uintptr_t)fb.fb_user_vaddr=0;
            return fail("stale VRAM remained writable");
        }
        if (tool_equal(argv[1],"hold")) {
            CHECK(argc==3,"pipe argument");
            unsigned fd=0; for (const char *p=argv[2];*p;p++) fd=fd*10+*p-'0';
            call(SYS_WRITE,fd,(uintptr_t)"R",1);
            for (;;) call(SYS_POLL,0,0,(uintptr_t)-1);
        }
        return 0; // deliberately omit release: actual final-space callback
    }
    if (tool_equal(argv[1],"clients")) {
        long first=compositor(true); CHECK(first>0,"first compositor ready");
        CHECK(call(SYS_FB_ACQUIRE,0,0,0)==SYSCALL_EBUSY,"first compositor acquire");
        CHECK(waited(compositor(false),1),"second compositor must reject busy");
        CHECK(!call(SYS_KILL,first,9,0) && waited(first,137),"kill owner compositor");
        CHECK(acquire_eventually(),"compositor death releases ownership");
        CHECK(!call(SYS_FB_RELEASE,0,0,0),"release retry");
        long restarted=compositor(true); CHECK(restarted>0,"restarted compositor ready");
        CHECK(call(SYS_FB_ACQUIRE,0,0,0)==SYSCALL_EBUSY,"restarted compositor acquired");
        CHECK(!call(SYS_KILL,restarted,9,0) && waited(restarted,137),"restart cleanup");
        CHECK(acquire_eventually(),"restart reaped"); call(SYS_FB_RELEASE,0,0,0);
        say("FB_CLIENTS_PASS\n"); return 0;
    }
    CHECK(tool_equal(argv[1],"check"),"unknown mode");
    CHECK(call(SYS_FBINFO,(uintptr_t)&fb,0,0)==SYSCALL_EPERM,"map without lease");
    CHECK(call(SYS_MMAP,0,0,0)==SYSCALL_EINVAL,"zero mmap");
    CHECK(call(SYS_MMAP,UINT64_MAX,0,0)==SYSCALL_EINVAL,"overflow mmap");
    long arena=call(SYS_MMAP,8193,0,0); CHECK(arena>0,"anonymous arena");
    for (unsigned i=0;i<12288;i++) CHECK(((volatile uint8_t *)(uintptr_t)arena)[i]==0,"anonymous zeroing");
    CHECK(call(SYS_MMAP,1,0,0)==SYSCALL_EBUSY,"arena collision");
    CHECK(acquire_eventually(),"acquire");
    CHECK(call(SYS_FBINFO,0,0,0)==SYSCALL_EFAULT,"bad output pointer");
    CHECK(call(SYS_FBINFO,(uintptr_t)&fb,arena,0)==SYSCALL_EEXIST,"mapping collision");
    CHECK(map(),"default mapping");
    CHECK(call(SYS_WRITE,1,fb.fb_user_vaddr,4)==SYSCALL_EFAULT,"MMIO not a syscall copy buffer");
    CHECK(call(SYS_FBINFO,(uintptr_t)&fb,0x7000000000ULL,0)==SYSCALL_EBUSY,"no aliases");
    volatile uint32_t *vram=(volatile uint32_t *)(uintptr_t)fb.fb_user_vaddr;
    for (unsigned y=0;y<fb.height;y++) for (unsigned x=0;x<fb.width;x++) vram[y*(fb.pitch/4)+x]=0x00365476;
    say("FB_CONSOLE_SERIAL_ONLY\n");
    for (unsigned y=0;y<fb.height;y++) for (unsigned x=0;x<fb.width;x++) CHECK(vram[y*(fb.pitch/4)+x]==0x00365476,"console changed VRAM");
    CHECK(waited(spawn("contend",NULL),0),"contender process");
    pthread_t thread; void *result=(void *)99;
    CHECK(!pthread_create(&thread,NULL,shared_thread,NULL),"shared thread create");
    CHECK(!pthread_join(thread,&result) && !result,"shared thread ownership");
    CHECK(map(),"secondary thread exit preserves owner");
    CHECK(!call(SYS_FB_RELEASE,0,0,0),"release");
    CHECK(waited(spawn("normal",NULL),0),"normal owner exit");
    CHECK(waited(spawn("fault",NULL),132),"fault owner exit");
    CHECK(waited(spawn("stale",NULL),139),"revoked address faults");
    CHECK(waited(spawn("anonexec",NULL),139),"anonymous NX fault");
    CHECK(waited(spawn("anonrace",NULL),0),"shared-thread anonymous race");
    CHECK(!call(SYS_PIPE,(uintptr_t)ready_pipe,0,0),"ready pipe");
    tool_format_u64(output,ready_pipe[1]);
    long held=spawn("hold",output); CHECK(held>0,"hold spawn");
    CHECK(call(SYS_READ,ready_pipe[0],(uintptr_t)output,1)==1 && output[0]=='R',"hold ready");
    CHECK(!call(SYS_KILL,held,9,0) && waited(held,137),"forced owner exit");
    call(SYS_CLOSE,ready_pipe[0],0,0); call(SYS_CLOSE,ready_pipe[1],0,0);
    CHECK(acquire_eventually(),"pre-race cleanup"); call(SYS_FB_RELEASE,0,0,0);
    CHECK(race_owners(),"8-process simultaneous acquire");
    say("FB_SMP_RACE_PASS\n");
    for (unsigned i=0;i<3;i++) CHECK(waited(spawn("normal",NULL),0),"warmup");
    pause_ms(100); mem_line("FB_BASE free=");
    for (unsigned i=0;i<20;i++) CHECK(waited(spawn("normal",NULL),0),"repeat owner exit");
    CHECK(acquire_eventually(),"final reacquire"); call(SYS_FB_RELEASE,0,0,0);
    pause_ms(100); mem_line("FB_FINAL free=");
    say("FB_LIFECYCLE_PASS\n"); return 0;
}
