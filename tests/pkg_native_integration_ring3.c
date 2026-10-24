#include "syscall_abi.h"
#include "account_abi.h"
#include "tests/pkg_backend_candidate.h"
static long raw(long n,uintptr_t a,uintptr_t b,uintptr_t c){register uintptr_t fourth __asm__("r10")=0;__asm__ volatile("syscall":"+a"(n):"D"(a),"S"(b),"d"(c),"r"(fourth):"rcx","r11","memory","cc");return n;}
static void check(bool ok,unsigned line){if(ok)return;const char s[]="PKGBOOT RING3 FAIL ";char b[4]={'0'+line/100%10,'0'+line/10%10,'0'+line%10,'\n'};raw(SYS_WRITE,2,(uintptr_t)s,sizeof s-1);raw(SYS_WRITE,2,(uintptr_t)b,4);raw(SYS_EXIT,1,0,0);__builtin_trap();}
#define need(x) check((x),__LINE__)
static long call(unsigned op,uint64_t session,pb_result *r){pb_request q={.size=32,.version=1,.operation=op,.session=session};return raw(200,(uintptr_t)&q,(uintptr_t)r,32);}
static void number(uint64_t n){char b[24];unsigned i=24;do{b[--i]='0'+n%10;n/=10;}while(n);raw(SYS_WRITE,1,(uintptr_t)(b+i),24-i);}
static volatile unsigned handled;
static void handler(int sig){if(sig==SIGINT)handled++;}
static void pause(void){timespec_t delay={.tv_nsec=10000000};need(!raw(SYS_NANOSLEEP,(uintptr_t)&delay,0,0));}
static long spawn(const char *mode,uint64_t session){char token[24];unsigned n=0;do{token[n++]='0'+session%10;session/=10;}while(session);for(unsigned i=0;i<n/2;i++){char c=token[i];token[i]=token[n-1-i];token[n-1-i]=c;}token[n]=0;const char *args[]={"/bin/pkg-boot",mode,token,0};spawn_opts_t opts={.size=64,.version=2,.argv=(uintptr_t)args};return raw(SYS_SPAWN_EXT,(uintptr_t)args[0],(uintptr_t)&opts,64);}
static void reap(long p,unsigned sig){uint64_t s;need(raw(SYS_WAITPID,p,(uintptr_t)&s,0)==p);need(sig?(WIFSIGNALED(s)&&WTERMSIG(s)==sig):(WIFEXITED(s)&&!WEXITSTATUS(s)));}
static void ready(unsigned count){for(unsigned i=0;i<3000;i++){if(raw(201,12,0,0)==count)return;pause();}need(false);}
static char passwd[8192],group[8192],shadow[8192];
static void accounts(void){
    account_request_t q={.size=64,.version=1};need(!raw(202,ACCOUNT_STATUS,(uintptr_t)&q,64)&&q.generation==1);
    q=(account_request_t){.size=64,.version=1,.flags=ACCOUNT_WITH_SHADOW,.passwd=(uintptr_t)passwd,.group=(uintptr_t)group,.shadow=(uintptr_t)shadow,.passwd_bytes=8192,.group_bytes=8192,.shadow_bytes=8192};
    need(!raw(202,ACCOUNT_SNAPSHOT,(uintptr_t)&q,64)&&q.generation==1&&q.passwd_bytes==33&&q.group_bytes==10&&q.shadow_bytes==14);
    need(passwd[0]=='r'&&shadow[5]=='!');q=(account_request_t){.size=64,.version=1};need(raw(202,ACCOUNT_BEGIN,(uintptr_t)&q,64)==SYSCALL_EROFS);
    need(raw(202,ACCOUNT_UPDATE,(uintptr_t)&q,64)==SYSCALL_EROFS);
    q=(account_request_t){.size=64,.version=1,.flags=ACCOUNT_WITH_SHADOW,.group=(uintptr_t)group,.shadow=(uintptr_t)shadow,.passwd_bytes=8192,.group_bytes=8192,.shadow_bytes=8192};need(raw(202,ACCOUNT_SNAPSHOT,(uintptr_t)&q,64)==SYSCALL_EFAULT);
    q=(account_request_t){.size=64,.version=1,.generation=1};need(!raw(202,ACCOUNT_CHECK,(uintptr_t)&q,64));
}
int pkg_call_main(int argc,char **argv){
    pb_result r;
    if(argc>2){
        char mode=argv[1][0];if(mode=='m'){uint64_t inherited=0;for(const char *p=argv[2];*p;p++){need(*p>='0'&&*p<='9');inherited=inherited*10+(unsigned)(*p-'0');}need(call(PB_RECOVER,inherited,&r)==SYSCALL_EACCES);need(call(PB_ACQUIRE,0,&r)==SYSCALL_EACCES);return 0;}
        if(mode=='s'){need(!raw(SYS_SETRESUID,1000,1000,1000));account_request_t q={.size=64,.version=1,.flags=ACCOUNT_WITH_SHADOW,.passwd=(uintptr_t)passwd,.group=(uintptr_t)group,.shadow=(uintptr_t)shadow,.passwd_bytes=8192,.group_bytes=8192,.shadow_bytes=8192};need(raw(202,ACCOUNT_SNAPSHOT,(uintptr_t)&q,64)==SYSCALL_EACCES);return 0;}
        need(!call(PB_ACQUIRE,0,&r));uint64_t session=r.session;
        if(mode=='n')return 0;
        if(mode=='d'||mode=='u'){need(!raw(mode=='d'?SYS_CAPSET:SYS_SETRESUID,mode=='d'?0:1000,mode=='d'?0:1000,mode=='d'?0:1000));need(call(PB_RECOVER,session,&r)==SYSCALL_EACCES);need(!call(PB_RELEASE,session,&r));return 0;}
        if(mode=='h'){signal_action_t a={.handler=(uintptr_t)handler};need(!raw(SYS_SIGACTION,SIGINT,(uintptr_t)&a,0));}
        need(!raw(201,mode=='k'?11:10,0,0));long result=call(PB_RECOVER,session,&r);
        if(mode=='c'){need(result==SYSCALL_EACCES);need(!call(PB_RELEASE,session,&r));return 0;}
        if(mode=='h'){need(result==SYSCALL_EINTR&&handled==1);need(!call(PB_RELEASE,session,&r));return 0;}
        need(false);
    }
    need(!call(PB_STATUS,0,&r));bool initial=r.phase!=PKG_TX_IDLE;
    pb_request request={.size=32,.version=1,.operation=PB_ACQUIRE};long unchanged=raw(201,13,0,0);
    need(raw(200,0,(uintptr_t)&r,32)==SYSCALL_EFAULT&&raw(200,(uintptr_t)&request,0,32)==SYSCALL_EFAULT);
    need(raw(200,(uintptr_t)&request,(uintptr_t)&r,31)==SYSCALL_EINVAL);request.reserved=1;need(raw(200,(uintptr_t)&request,(uintptr_t)&r,32)==SYSCALL_EINVAL);need(raw(201,13,0,0)==unchanged);
    if(argc==2 && argv[1][0]=='s'){
        need(!call(PB_ACQUIRE,0,&r));uint64_t session=r.session;need(!call(PB_RECOVER,session,&r)&&r.phase==PKG_TX_IDLE);need(!call(PB_RELEASE,session,&r));
        need(!raw(201,18,0,0));need(raw(201,20,0,0)==-5&&raw(201,22,0,0)==-5);
        account_request_t q={.size=64,.version=1};need(raw(202,ACCOUNT_STATUS,(uintptr_t)&q,64)==SYSCALL_EIO);need(call(PB_STATUS,0,&r)==SYSCALL_EIO);
    }else if(argc==2 && argv[1][0]=='b'){
        need(!call(PB_ACQUIRE,0,&r));uint64_t session=r.session;need(!call(PB_RECOVER,session,&r)&&r.phase==PKG_TX_IDLE);need(!call(PB_RELEASE,session,&r));
        long writes=raw(201,13,0,0),flushes=raw(201,14,0,0);need(!raw(201,21,0,0));need(raw(201,20,0,0)==-22&&raw(201,22,0,0)==-11);
        account_request_t q={.size=64,.version=1};need(raw(202,ACCOUNT_STATUS,(uintptr_t)&q,64)==SYSCALL_EAGAIN);need(raw(201,13,0,0)==writes&&raw(201,14,0,0)==flushes);
    }else if(argc==2){
        need(initial);need(!call(PB_ACQUIRE,0,&r));uint64_t session=r.session;need(!raw(201,17,0,0));need(call(PB_RECOVER,session,&r)==SYSCALL_EIO);
        long writes=raw(201,13,0,0),flushes=raw(201,14,0,0);need(call(PB_STATUS,0,&r)==SYSCALL_EIO&&call(PB_RECOVER,session,&r)==SYSCALL_EIO&&call(PB_ACQUIRE,0,&r)==SYSCALL_EIO);
        need(raw(201,20,0,0)<0&&raw(201,22,0,0)<0);need(raw(201,13,0,0)==writes&&raw(201,14,0,0)==flushes);need(!call(PB_RELEASE,session,&r));
    }else{
        if(initial){
            long before=raw(201,13,0,0);need(raw(201,20,0,0)==-11&&raw(201,22,0,0)==-11);account_request_t q={.size=64,.version=1};need(raw(202,ACCOUNT_STATUS,(uintptr_t)&q,64)==SYSCALL_EAGAIN);need(raw(201,13,0,0)==before);
            need(!call(PB_ACQUIRE,0,&r));uint64_t old=r.session;long p=spawn("misuse",old);need(p>0);reap(p,0);need(!call(PB_RELEASE,old,&r));
            for(unsigned i=0;i<3;i++){p=spawn(i==0?"drop":i==1?"uid":"normal",0);need(p>0);reap(p,0);}
            const char *modes[]={"kill","term","handler"};unsigned signals[]={SIGKILL,SIGTERM,SIGINT};
            for(unsigned i=0;i<3;i++){
                p=spawn(modes[i],0);need(p>0);ready(i+1);need(call(PB_ACQUIRE,0,&r)==SYSCALL_EAGAIN);
                if(i==1){uint64_t s;need(!raw(SYS_KILL,p,SIGSTOP,0));need(raw(SYS_WAITPID,p,(uintptr_t)&s,WUNTRACED)==p&&WIFSTOPPED(s));need(!raw(SYS_KILL,p,SIGCONT,0));}
                need(!raw(SYS_KILL,p,signals[i],0));reap(p,i<2?signals[i]:0);need(!call(PB_ACQUIRE,0,&r));uint64_t fresh=r.session;need(fresh!=old);need(call(PB_RECOVER,old,&r)==SYSCALL_EACCES);need(!call(PB_RELEASE,fresh,&r));
            }
            need(raw(201,15,0,0)==1);p=spawn("credential",0);need(p>0);ready(4);need(!raw(201,24,0,0));reap(p,0);
            need(!call(PB_ACQUIRE,0,&r));uint64_t session=r.session;need(!raw(201,4,0,0));need(call(PB_RECOVER,session,&r)==SYSCALL_EFAULT);
            need(!call(PB_STATUS,0,&r)&&r.phase==PKG_TX_IDLE);long writes=raw(201,13,0,0),steps=raw(201,15,0,0);need(steps==5);need(!call(PB_RECOVER,session,&r));need(raw(201,13,0,0)==writes&&raw(201,15,0,0)==steps);need(!call(PB_RELEASE,session,&r));
            for(unsigned i=0;i<3000&&raw(201,2,0,0)<8;i++)pause();
            need(raw(201,2,0,0)==8);
        }
        need(!raw(201,21,0,0)&&raw(201,22,0,0)==-11);need(!raw(201,20,0,0));need(!raw(201,22,0,0));accounts();
        long flushes=raw(201,14,0,0);need(!raw(201,20,0,0)&&raw(201,14,0,0)==flushes);
        long p=spawn("shadow",0);need(p>0);reap(p,0);
        need(!call(PB_STATUS,0,&r)&&r.phase==PKG_TX_IDLE);
    }
    const char counter[]="PKGBOOT COUNTERS writes=",flush[]=" flushes=",pass[]="\nPKGBOOT RING3 PASS\n";
    raw(SYS_WRITE,1,(uintptr_t)counter,sizeof counter-1);number(raw(201,13,0,0));raw(SYS_WRITE,1,(uintptr_t)flush,sizeof flush-1);number(raw(201,14,0,0));raw(SYS_WRITE,1,(uintptr_t)pass,sizeof pass-1);return 0;
}
