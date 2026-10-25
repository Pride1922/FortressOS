/* Actual private CLI/parser, mocked syscalls. No kernel acceptance claim. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "tests/pkg_exec_cli.h"
#include "pkg_abi.h"
#include "user/tools/common.h"
static unsigned acquire_calls,release_calls,mutation_calls,recovery_calls,status_calls;
static bool held,fail_output,fail_mutation,fail_acquire,fail_release,bad_result,fail_recovery;
static char expected[33]="field-kit";
static void result(pkg_result_v1 *r,uint64_t session) {
    *r=(pkg_result_v1){.size=80,.version=1,.session=session,.sequence=17,
        .account_sequence=2,.credential_generation=3};
    if(bad_result)r->reserved=1;
}
long tool_syscall(long nr,uintptr_t a,uintptr_t b,uintptr_t c) {
    if(nr==SYS_WRITE){assert(a==1||a==2);return fail_output?SYSCALL_EIO:(long)c;}
    assert(nr==SYS_PKGCTL&&c==64);
    const pkg_request_v1 *request=(void *)a;
    if(request->operation<=PKG_RELEASE) {
        const pkg_request_v1 *q=(void *)a;pkg_result_v1 *r=(void *)b;
        assert(c==64&&q->size==64&&q->version==1&&!q->reserved&&!q->reserved64);
        if(q->operation==PKG_STATUS){assert(!q->session);status_calls++;result(r,0);return 0;}
        if(q->operation==PKG_ACQUIRE){
            acquire_calls++;assert(!q->session&&!held);if(fail_acquire)return SYSCALL_EACCES;
            held=true;result(r,42);return 0;
        }
        assert(held&&q->session==42);
        if(q->operation==PKG_RELEASE){release_calls++;held=false;result(r,0);return fail_release?SYSCALL_EIO:0;}
        assert(q->operation==PKG_RECOVER);recovery_calls++;result(r,42);return fail_recovery?SYSCALL_EFAULT:0;
    }
    assert(nr==SYS_PKGCTL&&c==64);const pkg_request_v1 *q=(void *)a;
    assert(held&&q->size==64&&q->version==1&&!q->reserved&&!q->reserved64&&q->session==42);
    assert(q->operation==PKG_INSTALL||q->operation==PKG_REMOVE);
    assert(!memcmp(q->package,expected,strlen(expected)));
    for(size_t i=strlen(expected);i<32;i++)assert(!q->package[i]);
    mutation_calls++;result((void *)b,42);return fail_mutation?SYSCALL_EFAULT:0;
}
static int command(const char *verb,const char *name) {
    char *args[]={"pkg",(char *)verb,(char *)name};
    return pkg_private_cli(name?3:2,args);
}
int main(void) {
    assert(!command("--help",NULL)&&!acquire_calls);
    assert(command("install",NULL)&&!acquire_calls);
    assert(command("install","../field-kit")&&!acquire_calls);
    assert(command("install","--key")&&!acquire_calls);
    assert(command("update","field-kit")&&!acquire_calls);
    assert(!command("status",NULL)&&status_calls==1&&!acquire_calls);
    assert(!command("install",expected)&&mutation_calls==1&&acquire_calls==1&&release_calls==1&&!held);
    fail_output=true;assert(command("remove",expected)&&mutation_calls==2&&release_calls==2&&!held);fail_output=false;
    fail_mutation=true;assert(command("install",expected)&&mutation_calls==3&&release_calls==3&&!held);fail_mutation=false;
    fail_acquire=true;assert(command("install",expected)&&mutation_calls==3&&release_calls==3&&!held);fail_acquire=false;
    assert(!command("recover",NULL)&&recovery_calls==1&&!held);
    fail_recovery=true;assert(command("recover",NULL)&&recovery_calls==2&&!held);fail_recovery=false;
    bad_result=true;unsigned before=mutation_calls;assert(command("install",expected)&&mutation_calls==before&&!held);bad_result=false;
    memcpy(expected,"abcdefghijklmnopqrstuvwxyz012345",33);assert(strlen(expected)==32);
    assert(!command("install",expected)&&mutation_calls==before+1&&!held);
    fail_release=true;assert(command("remove",expected)&&mutation_calls==before+2&&!held);fail_release=false;
    puts("PASS private CLI lazy acquire, status, recovery, full-width names, output/copyout failures, cleanup, no retries");
    return 0;
}
