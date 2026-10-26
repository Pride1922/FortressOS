/* Private CPU-pinned graphics campaign, no release binary/ABI dependencies. */
#include "common.h"
#include "pthread.h"
#include "signal_abi.h"
#include "fb_campaign.h"

static fb_info_v1_t fb;
static unsigned char baseline[1024*1024], snapshot[1024*1024];
static uint8_t stacks[3][16384] __attribute__((aligned(16)));
static int command_fd, report_fd, scenario;
static uint64_t leader_tid, worker_tid, primed_preempts;
static unsigned primed, go;
static bool inject_leak;
static char number[64];
static long sc(long nr,uintptr_t a,uintptr_t b,uintptr_t c) { return tool_syscall(nr,a,b,c); }
static long observer(unsigned op,uintptr_t a,uintptr_t b) { return sc(FB_TEST_CALL,op,a,b); }
static void say(const char *s) { sc(SYS_WRITE,1,(uintptr_t)s,tool_length(s)); }
static int fail(const char *s) { say("[FB CAMPAIGN FAIL] ");say(s);say("\n");return 1; }
#define REQUIRE(c,s) do {if (!(c)) return fail(s);} while (0)
static void delay(unsigned ms) { sc(SYS_POLL,0,0,ms); }
static uint64_t now(void) { timespec_t t; if (sc(SYS_CLOCK_GETTIME,1,(uintptr_t)&t,0)) return 0; return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec; }
static bool read_exact(int fd,void *buffer,size_t bytes,unsigned timeout_ms) {
    size_t done=0; uint64_t end=now()+(uint64_t)timeout_ms*1000000;
    while(done<bytes && now()<end) {
        struct pollfd p={.fd=fd,.events=POLLIN};
        long r=sc(SYS_POLL,(uintptr_t)&p,1,50);
        if (r<0 || (p.revents&(POLLERR|POLLNVAL))) return false;
        if (!(p.revents&POLLIN)) continue;
        long n=sc(SYS_READ,fd,(uintptr_t)((char *)buffer+done),bytes-done);
        if(n<=0) return false;
        done+=(size_t)n;
    }
    return done==bytes;
}
static bool write_exact(int fd,const void *p,size_t n) { return sc(SYS_WRITE,fd,(uintptr_t)p,n)==(long)n; }
static bool acquire(void) {
    uint64_t end=now()+3000000000ULL;
    while(now()<end) {long r=sc(SYS_FB_ACQUIRE,0,0,0);if(!r)return true;if(r!=SYSCALL_EBUSY)return false;delay(1);}return false;
}
static bool mapped(void) {return !sc(SYS_FBINFO,(uintptr_t)&fb,0,0);}
static bool report(unsigned kind,unsigned id,uint64_t wins,uint64_t busy,uint64_t attempts) {
    fb_report_t r={kind,id,observer(FB_TEST_CPU,0,0),observer(FB_TEST_ROOT,0,0),wins,busy,attempts,now()};
    return write_exact(report_fd,&r,sizeof(r));
}
static bool wait_child(long pid,unsigned expected) {
    uint64_t status=0,end=now()+5000000000ULL;
    while(now()<end) {
        long r=sc(SYS_WAITPID,pid,(uintptr_t)&status,WNOHANG);
        if (r==pid) return WIFEXITED(status) ? WEXITSTATUS(status)==expected : 128+WTERMSIG(status)==expected;
        if(r<0)return false;
        delay(1);
    }
    return false;
}
static long spawn(unsigned cpu,const char *mode,int readfd,int writefd) {
    char core[8],fds[32]; tool_format_u64(core,cpu);
    size_t n=tool_format_u64(fds,readfd);fds[n++]=',';tool_format_u64(fds+n,writefd);
    const char *argv[]={"/bin/fb-smp-probe","--worker",core,mode,fds,NULL};
    return sc(SYS_SPAWN,(uintptr_t)argv[0],(uintptr_t)argv,0);
}
static void fill(uint32_t color) {
    volatile uint32_t *p=(volatile uint32_t *)(uintptr_t)fb.fb_user_vaddr;
    for(unsigned y=0;y<fb.height;y++)for(unsigned x=0;x<fb.width;x++)p[y*(fb.pitch/4)+x]=color;
}
static bool intact(uint32_t color) {
    volatile uint32_t *p=(volatile uint32_t *)(uintptr_t)fb.fb_user_vaddr;
    for(unsigned y=0;y<fb.height;y++)for(unsigned x=0;x<fb.width;x++)if(p[y*(fb.pitch/4)+x]!=color)return false;
    return true;
}
static _Noreturn void worker_exit(unsigned value) {sc(SYS_THREAD_EXIT,value,0,0);for(;;)__asm__ volatile("pause");}
void fb_worker_main(uintptr_t cpu);
__asm__(".text\n.global fb_worker_entry\nfb_worker_entry:\ncall fb_worker_main\nud2\n");
extern void fb_worker_entry(void);
static bool create_worker(unsigned cpu) {
    return pthread_sys_4(SYS_THREAD_CREATE,(uintptr_t)fb_worker_entry,
        (uintptr_t)&stacks[cpu-1][16384],cpu,(uintptr_t)&worker_tid)>0;
}

void fb_worker_main(uintptr_t cpu) {
    if(observer(FB_TEST_CPU,0,0)!=(long)cpu) {fail("worker CPU");worker_exit(1);}
    if(scenario==5) {
        __atomic_fetch_or(&primed,1U<<cpu,__ATOMIC_RELEASE);
        worker_exit(0);
    }
    primed_preempts=observer(FB_TEST_PREEMPTS,0,0);
    volatile uint32_t *pixel=(volatile uint32_t *)(uintptr_t)fb.fb_user_vaddr;
    *pixel=0x00123456; // prime the AP's real VRAM translation
    __atomic_store_n(&primed,1,__ATOMIC_RELEASE);
    if(scenario!=0) {
        uint64_t end=now()+3000000000ULL;
        while(observer(FB_TEST_THREAD_LIVE,leader_tid,0)>0 && now()<end) __asm__ volatile("pause");
        if(observer(FB_TEST_THREAD_LIVE,leader_tid,0)!=0 || !report(FB_LEADER_GONE,scenario,0,0,0)) {
            fail("leader did not thread-exit");worker_exit(1);
        }
        char c;
        if(!read_exact(command_fd,&c,1,5000)) {fail("worker go timeout");worker_exit(1);}
        if(scenario==2) { // final thread still owns display, no explicit release
            *pixel=0x0056789a;
            worker_exit(0);
        }
    } else {
        while(!__atomic_load_n(&go,__ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    }
    *pixel=0x00badbad; // must take a real #PF/SIGSEGV after release
    fail("stale sibling mapping remained writable");worker_exit(1);
}

static int victim(const char *mode) {
    scenario=tool_equal(mode,"live")?0:tool_equal(mode,"leader-exit")?1:tool_equal(mode,"final-thread")?2:tool_equal(mode,"process-exit")?3:4;
    leader_tid=sc(SYS_THREAD_SELF,0,0,0);
    REQUIRE(observer(FB_TEST_CPU,0,0)==0,"victim leader CPU");
    REQUIRE(acquire() && mapped() && create_worker(1),"victim setup");
    uint64_t end=now()+3000000000ULL;
    while(!__atomic_load_n(&primed,__ATOMIC_ACQUIRE) && now()<end)__asm__ volatile("pause");
    REQUIRE(primed,"prime timeout");
    if(scenario==3) {
        REQUIRE(report(FB_READY,scenario,0,0,worker_tid),"process exit report");
        sc(SYS_EXIT,0,0,0);
        return fail("SYS_EXIT returned");
    }
    if(scenario<2) {
        REQUIRE(!sc(SYS_FB_RELEASE,0,0,0),"leader release");
        uint64_t mask=observer(FB_TEST_REMOTE_MASK,0,0), seen=observer(FB_TEST_REMOTE_PREEMPTS,1,0);
        if(scenario==0) REQUIRE((mask&2) && seen==primed_preempts,"AP not resident without preemption at revocation");
        REQUIRE(report(FB_RELEASED,scenario,mask,seen,primed_preempts),"release report");
    }
    if(scenario) worker_exit(0); // leader-thread exit, not SYS_EXIT
    char c; REQUIRE(read_exact(command_fd,&c,1,5000),"leader go timeout");
    __atomic_store_n(&go,1,__ATOMIC_RELEASE);
    uint64_t status=0;
    sc(SYS_THREAD_JOIN,worker_tid,(uintptr_t)&status,0);
    return fail("fault did not terminate shared process");
}

static int sibling_case(const char *mode) {
    int in[2],out[2];REQUIRE(!sc(SYS_PIPE,(uintptr_t)in,0,0)&&!sc(SYS_PIPE,(uintptr_t)out,0,0),"sibling pipes");
    long pid=spawn(0,mode,in[0],out[1]);REQUIRE(pid>0,"sibling spawn");
    fb_report_t r;REQUIRE(read_exact(out[0],&r,sizeof(r),5000),"sibling first report");
    uint64_t root=r.root,expected_preempts=r.attempts;
    bool final=tool_equal(mode,"final-thread") || tool_equal(mode,"process-exit") || tool_equal(mode,"killed-survivor");
    bool process_exit=tool_equal(mode,"process-exit"), killed=tool_equal(mode,"killed-survivor");
    if(!final && tool_equal(mode,"leader-exit")) REQUIRE(read_exact(out[0],&r,sizeof(r),5000)&&r.kind==FB_LEADER_GONE,"leader gone report");
    if(final) {
        REQUIRE(process_exit ? r.kind==FB_READY : r.kind==FB_LEADER_GONE && r.cpu==1,"final survivor CPU");
        if(!process_exit) REQUIRE(sc(SYS_FB_ACQUIRE,0,0,0)==SYSCALL_EBUSY,"last live thread must retain lease");
    } else {
        REQUIRE(acquire()&&mapped(),"replacement owner");fill(0x00c0ffee);
    }
    if(killed) REQUIRE(!sc(SYS_KILL,pid,SIGKILL,0),"kill leaderless process");
    else if(!process_exit) REQUIRE(write_exact(in[1],"G",1),"sibling go");
    REQUIRE(wait_child(pid,killed?137:final?0:139),"shared process wait/exit status");
    if(tool_equal(mode,"live")) {
        long fault_preempts=observer(FB_TEST_FAULT_PREEMPTS,root,0);
        REQUIRE(fault_preempts==(long)expected_preempts,"AP preempted between priming and stale fault");
        say("FB_REVOCATION cpu=1 primed_preempts=");tool_format_u64(number,expected_preempts);say(number);
        say(" ipi_preempts=");tool_format_u64(number,r.busy);say(number);
        say(" fault_preempts=");tool_format_u64(number,fault_preempts);say(number);say("\n");
    }
    uint64_t end=now()+3000000000ULL;
    while(observer(FB_TEST_ROOT_LIVE,root,0)>0&&now()<end)delay(1);
    REQUIRE(observer(FB_TEST_ROOT_LIVE,root,0)==0,"old address space not reclaimed");
    if(!final) {REQUIRE(intact(0x00c0ffee),"new owner VRAM corrupted");REQUIRE(!sc(SYS_FB_RELEASE,0,0,0),"replacement release");}
    else {REQUIRE(acquire(),"final-thread auto-release");REQUIRE(!sc(SYS_FB_RELEASE,0,0,0),"final release");}
    for(unsigned i=0;i<2;i++){sc(SYS_CLOSE,in[i],0,0);sc(SYS_CLOSE,out[i],0,0);}
    say("FB_SIBLING_PASS ");say(mode);say("\n");return 0;
}

static int account_child(void) {
    REQUIRE(acquire()&&mapped(),"account map");
    scenario=5;
    REQUIRE(create_worker(1)&&create_worker(2),"account unjoined workers");
    uint64_t end=now()+3000000000ULL;
    while(__atomic_load_n(&primed,__ATOMIC_ACQUIRE)!=6 && now()<end)delay(1);
    REQUIRE(primed==6,"account workers incomplete");
    REQUIRE(sc(SYS_MMAP,16385,0,0)>0,"account anonymous");
    uint64_t root=observer(FB_TEST_ROOT,0,0);
    REQUIRE(write_exact(report_fd,&root,8),"account identity");
    if(inject_leak) REQUIRE(observer(FB_TEST_LEAK,0,0)>0,"negative-control leak");
    return 0;
}
static int accounting(void) {
    REQUIRE(!observer(FB_TEST_WARM_HEAP,0,0),"heap warmup");
    int p[2];REQUIRE(!sc(SYS_PIPE,(uintptr_t)p,0,0),"account pipe");
    unsigned char *base=baseline;
    sysinfo_mem_t initial={0},current;
    long bytes=observer(FB_TEST_BITMAP,(uintptr_t)base,sizeof(baseline));
    REQUIRE(bytes>0,"bitmap query");
    for(unsigned i=0;i<105;i++) {
        long pid=spawn(0,inject_leak?"leak-child":"account-child",0,p[1]);
        uint64_t root;REQUIRE(pid>0&&read_exact(p[0],&root,8,5000)&&wait_child(pid,0),"account child");
        uint64_t end=now()+3000000000ULL;
        while(observer(FB_TEST_ROOT_LIVE,root,0)>0&&now()<end)delay(1);
        REQUIRE(!observer(FB_TEST_ROOT_LIVE,root,0),"account child root remains");
        if(i==4 || (i>=5 && (i-4)%25==0)) {
            delay(30);
            REQUIRE(!sc(SYS_MEMINFO,(uintptr_t)&current,sizeof(current),0),"account meminfo");
            REQUIRE(observer(FB_TEST_BITMAP,(uintptr_t)snapshot,sizeof(snapshot))==bytes,"bitmap size stable");
            if(i==4) {for(long b=0;b<bytes;b++)base[b]=snapshot[b];initial=current;}
            else {
                unsigned changed=0;for(long b=0;b<bytes;b++)if(base[b]!=snapshot[b])changed++;
                if(current.pmm_free_frames!=initial.pmm_free_frames || current.vmm_table_frames!=initial.vmm_table_frames
                    || current.vmm_deferred_spaces || changed
                    || current.heap_used_bytes!=initial.heap_used_bytes || current.heap_committed_bytes!=initial.heap_committed_bytes) {
                    say("FB_ACCOUNT_DRIFT cycles=");tool_format_u64(number,i-4);say(number);
                    say(" base_free=");tool_format_u64(number,initial.pmm_free_frames);say(number);
                    say(" current_free=");tool_format_u64(number,current.pmm_free_frames);say(number);
                    say(" base_tables=");tool_format_u64(number,initial.vmm_table_frames);say(number);
                    say(" current_tables=");tool_format_u64(number,current.vmm_table_frames);say(number);
                    say(" base_heap_used=");tool_format_u64(number,initial.heap_used_bytes);say(number);
                    say(" current_heap_used=");tool_format_u64(number,current.heap_used_bytes);say(number);
                    say(" base_heap_committed=");tool_format_u64(number,initial.heap_committed_bytes);say(number);
                    say(" current_heap_committed=");tool_format_u64(number,current.heap_committed_bytes);say(number);
                    say(" changed_bitmap_bytes=");tool_format_u64(number,changed);say(number);say("\n");
                    return fail("allocation-set drift");
                }
                say("FB_ACCOUNT_CHECKPOINT cycles=");tool_format_u64(number,i-4);say(number);say(" bitmap_exact=1\n");
            }
        }
    }
    sc(SYS_CLOSE,p[0],0,0);sc(SYS_CLOSE,p[1],0,0);
    say("FB_ACCOUNT_100_PASS\n");return 0;
}

static int stress_worker(unsigned id) {
    REQUIRE(observer(FB_TEST_CPU,0,0)==(long)id,"stress CPU placement");
    REQUIRE(report(FB_READY,id,0,0,0),"stress ready");
    uint64_t start;REQUIRE(read_exact(command_fd,&start,8,5000),"stress start");
    while(now()<start)__asm__ volatile("pause");
    uint64_t begun=now();
    REQUIRE(report(FB_STARTED,id,0,0,0),"stress started");
    uint64_t wins=0,busy=0,attempts=0,next=begun+1000000000ULL;
    while(now()<begun+10000000000ULL) {
        REQUIRE(observer(FB_TEST_CPU,0,0)==(long)id,"stress migrated");
        long r=sc(SYS_FB_ACQUIRE,0,0,0);attempts++;
        if(r==SYSCALL_EBUSY)busy++;
        else {
            REQUIRE(!r&&mapped(),"stress acquire/map");
            volatile uint32_t *pixel=(volatile uint32_t *)(uintptr_t)fb.fb_user_vaddr;
            *pixel=0x550000+id;REQUIRE(*pixel==0x550000+id,"exclusive write");
            REQUIRE(!sc(SYS_FB_RELEASE,0,0,0),"stress release");wins++;
            sc(SYS_THREAD_YIELD,0,0,0);
        }
        if(now()>=next){REQUIRE(report(FB_PROGRESS,id,wins,busy,attempts),"stress heartbeat");next+=1000000000ULL;}
    }
    REQUIRE(wins>0 && busy>0,"stress path coverage");
    REQUIRE(report(FB_DONE,id,wins,busy,attempts),"stress done");return 0;
}
static int stress(void) {
    int start[4][2],results[2];long pids[4];unsigned done=0;
    REQUIRE(!sc(SYS_PIPE,(uintptr_t)results,0,0),"stress reports");
    for(unsigned i=0;i<4;i++){REQUIRE(!sc(SYS_PIPE,(uintptr_t)start[i],0,0),"stress barrier pipe");pids[i]=spawn(i,"stress-worker",start[i][0],results[1]);REQUIRE(pids[i]>0,"stress spawn");}
    fb_report_t r;unsigned ready=0;
    while(ready!=15){REQUIRE(read_exact(results[0],&r,sizeof(r),5000)&&r.kind==FB_READY&&r.id<4&&r.cpu==r.id,"stress barrier placement");REQUIRE(!(ready&(1u<<r.id)),"duplicate ready");ready|=1u<<r.id;}
    uint64_t start_ns=now()+250000000ULL;
    for(unsigned i=0;i<4;i++)REQUIRE(write_exact(start[i][1],&start_ns,8),"stress release barrier");
    uint64_t end=start_ns+20000000000ULL, last[4]={start_ns,start_ns,start_ns,start_ns}, started[4]={0};
    while(done!=15 && now()<end) {
        REQUIRE(read_exact(results[0],&r,sizeof(r),2500)&&r.id<4&&r.cpu==r.id,"stress watchdog/read");
        REQUIRE(r.kind==FB_PROGRESS||r.kind==FB_DONE||r.kind==FB_STARTED,"stress report kind");
        if(r.kind==FB_STARTED){REQUIRE(!started[r.id]&&r.ns>=start_ns&&r.ns-start_ns<100000000ULL,"stress start skew");started[r.id]=r.ns;
            say("FB_STRESS_START cpu=");tool_format_u64(number,r.cpu);say(number);say(" ns=");tool_format_u64(number,r.ns);say(number);say("\n");}
        REQUIRE(r.attempts==r.wins+r.busy&&r.ns>=last[r.id],"stress counters");last[r.id]=r.ns;
        for(unsigned i=0;i<4;i++)REQUIRE((done&(1u<<i))||now()-last[i]<3000000000ULL,"worker heartbeat stalled");
        if(r.kind==FB_DONE){REQUIRE(r.wins&&r.busy&&started[r.id]&&r.ns>=started[r.id]+10000000000ULL,"stress duration/coverage");done|=1u<<r.id;}
        say("FB_STRESS cpu=");tool_format_u64(number,r.cpu);say(number);say(" wins=");tool_format_u64(number,r.wins);say(number);say(" busy=");tool_format_u64(number,r.busy);say(number);say(" done=");say(r.kind==FB_DONE?"1\n":"0\n");
    }
    REQUIRE(done==15,"stress final watchdog");
    for(unsigned i=0;i<4;i++){REQUIRE(wait_child(pids[i],0),"stress worker exit");sc(SYS_CLOSE,start[i][0],0,0);sc(SYS_CLOSE,start[i][1],0,0);}
    sc(SYS_CLOSE,results[0],0,0);sc(SYS_CLOSE,results[1],0,0);
    say("FB_STRESS_10S_PASS\n");return 0;
}
int fb_smp_probe_main(int argc,char **argv) {
    REQUIRE(argc>=4&&tool_equal(argv[1],"--worker"),"arguments");
    unsigned id=0;for(const char *p=argv[2];*p;p++)id=id*10+*p-'0';
    if(argc==5){const char *p=argv[4];while(*p&&*p!=',')command_fd=command_fd*10+*p++-'0';if(*p)p++;while(*p)report_fd=report_fd*10+*p++-'0';}
    REQUIRE(observer(FB_TEST_CPU,0,0)==(long)id,"initial CPU");
    const char *mode=argv[3];
    if(tool_equal(mode,"live")||tool_equal(mode,"leader-exit")||tool_equal(mode,"final-thread")||tool_equal(mode,"process-exit")||tool_equal(mode,"killed-survivor"))return victim(mode);
    if(tool_equal(mode,"stress-worker"))return stress_worker(id);
    inject_leak=tool_equal(mode,"leak-child")||tool_equal(mode,"negative");
    if(tool_equal(mode,"account-child")||tool_equal(mode,"leak-child"))return account_child();
    if(tool_equal(mode,"account")||inject_leak)return accounting();
    if(tool_equal(mode,"stress"))return stress();
    REQUIRE(tool_equal(mode,"siblings"),"unknown mode");
    REQUIRE(!sibling_case("live"),"live sibling case");
    REQUIRE(!sibling_case("leader-exit"),"leader-thread exit case");
    REQUIRE(!sibling_case("final-thread"),"final-thread case");
    REQUIRE(!sibling_case("process-exit"),"process-wide exit case");
    REQUIRE(!sibling_case("killed-survivor"),"leaderless SIGKILL case");
    say("FB_SIBLINGS_PASS\n");return 0;
}
