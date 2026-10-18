#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "user/tools/common.h"
#include "user/kill_cli.h"
int kill_main(int,char **);
static unsigned calls;
static long outcome;
static int64_t last_pid;
static unsigned last_sig;
long tool_syscall(long nr,uintptr_t a,uintptr_t b,uintptr_t c) {
    if (nr==SYS_WRITE) {(void)a;(void)b;return (long)c;}
    assert(nr==SYS_KILL && c==0);calls++;
    last_pid=(int64_t)a;last_sig=b;return outcome;
}
static int invoke(const char *a,const char *b,const char *c) {
    char *argv[]={"kill",(char *)a,(char *)b,(char *)c,NULL};
    return kill_main(c ? 4 : b ? 3 : a ? 2 : 1,argv);
}
int main(void) {
    assert(!invoke("123",NULL,NULL) && last_pid==123 && last_sig==SIGTERM);
    assert(!invoke("-9","123",NULL) && last_sig==SIGKILL);
    assert(!invoke("123","SIGTERM",NULL) && last_sig==SIGTERM);
    assert(!invoke("-s","STOP","123") && last_sig==SIGSTOP);
    assert(!invoke("-0","123",NULL) && last_sig==0);
    assert(!invoke("9223372036854775807",NULL,NULL) && last_pid==INT64_MAX);
    unsigned before=calls;
    const char *bad[]={"0","-1","%1","+1","12x","9223372036854775808","999999999999999999999999"};
    for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) assert(invoke(bad[i],NULL,NULL)==2);
    assert(invoke(NULL,NULL,NULL)==2);
    assert(invoke("-999","123",NULL)==2 && invoke("-s","NOPE","123")==2);
    assert(!invoke("--help",NULL,NULL) && calls==before);
    outcome=SYSCALL_EPERM;assert(invoke("123",NULL,NULL)==1);
    outcome=SYSCALL_ESRCH;assert(invoke("123",NULL,NULL)==1);
    puts("PASS actual /bin/kill: PID bounds, signal syntax, permission/missing errors, no unintended signals");
    return 0;
}
