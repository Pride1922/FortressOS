#include "syscall_abi.h"
#include "tests/pkg_backend_candidate.h"
static long raw(long n,uintptr_t a,uintptr_t b,uintptr_t c){
 register uintptr_t fourth __asm__("r10")=0;
 __asm__ volatile("syscall":"+a"(n):"D"(a),"S"(b),"d"(c),"r"(fourth):"rcx","r11","memory","cc");return n;
}
static void check(bool ok,unsigned line){
 if(ok)return;
 const char text[]="GATE1 RING3 FAIL ";char n[]={'0'+line/100%10,'0'+line/10%10,'0'+line%10,'\n'};
 raw(SYS_WRITE,2,(uintptr_t)text,sizeof text-1);raw(SYS_WRITE,2,(uintptr_t)n,sizeof n);raw(SYS_EXIT,1,0,0);__builtin_trap();
}
#define require(x) check((x),__LINE__)
static long call(unsigned op,uint64_t session,pb_result *r){pb_request q={.size=32,.version=1,.operation=op,.session=session};return raw(200,(uintptr_t)&q,(uintptr_t)r,sizeof q);}
static volatile unsigned handled;
static void handler(int sig){if(sig==SIGINT)handled++;}
static uint64_t number(const char *s){uint64_t n=0;while(*s){require(*s>='0'&&*s<='9');n=n*10+(unsigned)(*s++-'0');}return n;}
static long spawn(const char *mode,uint64_t session){
 char token[24];unsigned n=0;do{token[n++]='0'+session%10;session/=10;}while(session);for(unsigned i=0;i<n/2;i++){char c=token[i];token[i]=token[n-1-i];token[n-1-i]=c;}token[n]=0;
 const char *args[]={"/bin/pkg-gate1",mode,token,0};spawn_opts_t opts={.size=64,.version=2,.argv=(uintptr_t)args};
 return raw(SYS_SPAWN_EXT,(uintptr_t)args[0],(uintptr_t)&opts,64);
}
static void reap(long p,unsigned sig){uint64_t s=~0ULL;require(raw(SYS_WAITPID,p,(uintptr_t)&s,0)==p);require(sig?(WIFSIGNALED(s)&&WTERMSIG(s)==sig):(WIFEXITED(s)&&!WEXITSTATUS(s)));}
static void pause(void){timespec_t delay={.tv_sec=0,.tv_nsec=10000000};require(!raw(SYS_NANOSLEEP,(uintptr_t)&delay,0,0));}
static void ready(unsigned n){for(unsigned i=0;i<3000;i++){if(raw(201,12,0,0)==n)return;pause();}require(false);}
int pkg_call_main(int argc,char **argv){
 pb_result r;
 if(argc>1){
  if(argv[1][0]=='r'){
   require(call(PB_STATUS,0,&r)==SYSCALL_EOPNOTSUPP);require(call(PB_ACQUIRE,0,&r)==SYSCALL_EOPNOTSUPP);
   require(!raw(201,13,0,0));const char pass[]="GATE1 REFUSAL PASS writes=0\n";raw(SYS_WRITE,1,(uintptr_t)pass,sizeof pass-1);return 0;
  }
  if(argv[1][0]=='a'){
   require(!call(PB_STATUS,0,&r)&&r.phase==PKG_TX_IDLE&&r.sequence==12);
   require(!call(PB_ACQUIRE,0,&r));uint64_t session=r.session;require(!call(PB_RECOVER,session,&r)&&r.phase==PKG_TX_IDLE&&r.sequence==12);
   require(!call(PB_RELEASE,session,&r)&&!raw(201,14,0,0));require(!raw(201,15,0,0));
   const char pass[]="GATE1 REPEAT PASS commits=0 IDLE=12\n";raw(SYS_WRITE,1,(uintptr_t)pass,sizeof pass-1);
   return 0;
  }
  char mode=argv[1][0];uint64_t inherited=number(argv[2]);
  if(mode=='m'){require(call(PB_RECOVER,inherited,&r)==SYSCALL_EACCES);require(call(PB_ACQUIRE,0,&r)==SYSCALL_EACCES);return 0;}
  require(!call(PB_ACQUIRE,0,&r));uint64_t session=r.session;
  if(mode=='n')return 0; /* normal exit must cancel the unreleased session */
  if(mode=='d'){require(!raw(SYS_CAPSET,0,0,0));require(call(PB_RECOVER,session,&r)==SYSCALL_EACCES);require(!call(PB_RELEASE,session,&r));return 0;}
  if(mode=='u'){require(!raw(SYS_SETRESUID,1000,1000,1000));require(call(PB_RECOVER,session,&r)==SYSCALL_EACCES);require(!call(PB_RELEASE,session,&r));return 0;}
  if(mode=='h'){signal_action_t a={.handler=(uintptr_t)handler};require(!raw(SYS_SIGACTION,SIGINT,(uintptr_t)&a,0));}
  require(!raw(201,mode=='k'?11:10,0,0));
  long result=call(PB_RECOVER,session,&r);
  if(mode=='h'){require(result==SYSCALL_EINTR&&handled==1);require(!call(PB_RELEASE,session,&r));return 0;}
  require(false);return 1;
 }
 require(!call(PB_STATUS,0,&r)&&r.phase==PKG_TX_RETIRING&&r.sequence==1);
 long before=raw(201,13,0,0);pb_request q={.size=32,.version=1,.operation=PB_ACQUIRE};
 require(raw(200,0,(uintptr_t)&r,32)==SYSCALL_EFAULT);
 require(raw(200,(uintptr_t)&q,0,32)==SYSCALL_EFAULT);
 require(raw(200,(uintptr_t)&q,(uintptr_t)pkg_call_main,32)==SYSCALL_EFAULT);
 require(raw(200,(uintptr_t)&q,0xffffffff80000000ULL,32)==SYSCALL_EFAULT);
 require(raw(200,0xffffffff80000000ULL,(uintptr_t)&r,32)==SYSCALL_EFAULT);
 require(raw(200,(uintptr_t)&q,0x7fffffffffe0ULL,32)==SYSCALL_EFAULT);
 require(raw(200,4090,(uintptr_t)&r,32)==SYSCALL_EFAULT);
 require(raw(200,(uintptr_t)&q,(uintptr_t)&r,31)==SYSCALL_EINVAL);
 q.reserved=1;require(raw(200,(uintptr_t)&q,(uintptr_t)&r,32)==SYSCALL_EINVAL);q.reserved=0;
 q.operation=5;require(raw(200,(uintptr_t)&q,(uintptr_t)&r,32)==SYSCALL_EINVAL);
 require(raw(201,13,0,0)==before);
 require(!call(PB_ACQUIRE,0,&r));uint64_t old=r.session;
 long p=spawn("misuse",old);require(p>0);reap(p,0);require(!call(PB_RELEASE,old,&r));
 p=spawn("drop",0);require(p>0);reap(p,0);
 p=spawn("uid",0);require(p>0);reap(p,0);
 p=spawn("normal",0);require(p>0);reap(p,0);
 unsigned ready_count=0;
 const char *modes[]={"kill-mid","term","handler"};unsigned signals[]={SIGKILL,SIGTERM,SIGINT};
 for(unsigned i=0;i<3;i++){
  p=spawn(modes[i],0);require(p>0);ready(++ready_count);
  require(call(PB_ACQUIRE,0,&r)==SYSCALL_EAGAIN); /* occupied execution credit */
  if(i==1){uint64_t s;require(!raw(SYS_KILL,p,SIGSTOP,0));require(raw(SYS_WAITPID,p,(uintptr_t)&s,WUNTRACED)==p&&WIFSTOPPED(s));require(!raw(SYS_KILL,p,SIGCONT,0));}
  require(!raw(SYS_KILL,p,signals[i],0));reap(p,i<2?signals[i]:0);
  require(!call(PB_ACQUIRE,0,&r));uint64_t fresh=r.session;require(fresh!=old);
  require(call(PB_RECOVER,old,&r)==SYSCALL_EACCES);require(!call(PB_RELEASE,fresh,&r));
 }
 require(raw(201,14,0,0)>0&&raw(201,14,0,0)<11);
 require(!call(PB_ACQUIRE,0,&r));uint64_t session=r.session;
 require(!raw(201,4,0,0));require(call(PB_RECOVER,session,&r)==SYSCALL_EFAULT);
 require(!call(PB_STATUS,0,&r)&&r.phase==PKG_TX_IDLE&&r.sequence==12);
 long commits=raw(201,14,0,0);require(commits==11);require(!call(PB_RECOVER,session,&r)&&r.phase==PKG_TX_IDLE&&raw(201,14,0,0)==commits);
 for(unsigned i=0;i<3000&&raw(201,2,0,0)<7;i++)pause();
 require(raw(201,2,0,0)==7);require(!raw(201,15,0,0));
 before=raw(201,13,0,0);require(!raw(201,16,0,0));
 require(call(PB_STATUS,0,&r)==SYSCALL_EIO&&call(PB_RECOVER,session,&r)==SYSCALL_EIO&&call(PB_ACQUIRE,0,&r)==SYSCALL_EIO);
 require(raw(201,13,0,0)==before);require(!call(PB_RELEASE,session,&r));
 const char pass[]="GATE1 RING3 PASS children=7 reaped=7 signals=3 commits=11 IDLE=12\n";raw(SYS_WRITE,1,(uintptr_t)pass,sizeof pass-1);
 long mask=raw(201,3,0,0);char line[]="GATE1 CPU_MASK=000\n";line[15]='0'+mask/100%10;line[16]='0'+mask/10%10;line[17]='0'+mask%10;raw(SYS_WRITE,1,(uintptr_t)line,sizeof line-1);return 0;
}
