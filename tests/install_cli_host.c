/* Actual text installer with terminal/control adapters; no guest I/O claim. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define INSTALLER_CLI_HOST_TEST
#include "../user/tools/install.c"
static const char *input;
static char output[16384];
static size_t output_size;
static unsigned acquisitions,deployments,releases,restores,seeds;
static bool fail_deploy;
static const char *expected_username="operator",*expected_password="operator-passphrase";
static terminal_attrs_t current;
size_t tool_length(const char *s) {return strlen(s);}
bool tool_equal(const char *a,const char *b) {return !strcmp(a,b);}
int tool_write(const char *name,const void *bytes,size_t n) {
    (void)name;assert(output_size+n<sizeof(output));
    memcpy(output+output_size,bytes,n);output_size+=n;output[output_size]=0;return 0;
}
long tool_syscall(long nr,uintptr_t a,uintptr_t b,uintptr_t c) {
    (void)c;
    if (nr==SYS_TERMCTL) return 1;
    if (nr==SYS_INPUT_READ) {
        if (!*input) return SYSCALL_EINTR;
        *(char *)a=*input++;return 1;
    }
    if (nr==SYS_INSTALLSEED) {
        memset((void *)a,++seeds,b);return 0;
    }
    assert(!"unexpected syscall");return -1;
}
static long terminal(unsigned op,terminal_attrs_t *attrs) {
    if (op==TERM_GET) *attrs=current;
    else {current=*attrs;if (attrs->input_flags&TERM_ISIG) restores++;}
    return 0;
}
static long control(unsigned op,const void *in,size_t n,void *out,size_t m) {
    (void)n;(void)m;
    if (op==INSTALL_ENUM) {
        *(install_identity_v1_t *)out=(install_identity_v1_t){.sector_count=384*2048,.sector_size=512,.generation=1};return 1;
    }
    if (op==INSTALL_ACQUIRE) {acquisitions++;*(int32_t *)out=3;return 0;}
    if (op==INSTALL_RELEASE) {releases++;return 0;}
    assert(op==INSTALL_DEPLOY);
    const install_deploy_v1_t *req=in;
    if (req->flags==INSTALL_DRY_RUN) {assert(!acquisitions);return 0;}
    assert(req->version==2 && req->size==288 && !strcmp(((const install_deploy_v2_t *)req)->username,expected_username));
    deployments++;
    assert(db_verify(expected_password,req->operator_hash));
    assert(db_verify("independent-recovery",req->recovery_hash));
    *(install_status_v1_t *)out=(install_status_v1_t){.stage=7,.complete=!fail_deploy,.error=fail_deploy ? -5 : 0};
    return fail_deploy ? -5 : 0;
}
static void run(const char *bytes,bool success,bool destructive) {
    input=bytes;output_size=0;acquisitions=deployments=releases=restores=seeds=0;
    current=(terminal_attrs_t){.input_flags=TERM_ISIG};
    assert(install_main(0,NULL)==(success ? 0 : 1));
    assert(restores==1 && (current.input_flags&TERM_ISIG));
    assert(strstr(output,"Size: 384 MiB"));
    assert(acquisitions==destructive && deployments==destructive && releases==destructive);
    const unsigned char *named=(void *)&named_request;for (size_t i=0;i<sizeof(named_request);i++) assert(!named[i]);
    const unsigned char *p=(void *)&request;for (size_t i=0;i<sizeof(request);i++) assert(!p[i]);
    assert(!strstr(output,"operator-passphrase") && !strstr(output,"independent-recovery"));
    assert(!strstr(output,expected_password));
}
int main(void) {
    run("\033",false,false);run("\003",false,false);run("\n",false,false);run("2\n",false,false);
    run("1\noperator\n\n",false,false);run("1\noperator\nshort\n",false,false);
    run("1\noperator\noperator-passphrase\ndifferent\n",false,false);
    run("1\noperator\noperator-passphrase\noperator-passphrase\n\033",false,false);
    run("1\noperator\noperator-passphrase\noperator-passphrase\noperator-passphrase\noperator-passphrase\n",false,false);
    char long_input[512]="1\noperator\n";memset(long_input+11,'x',129);memcpy(long_input+140,"\n",2);run(long_input,false,false);
    const char *prefix="1\noperator\noperator-passphrase\noperator-passphrase\nindependent-recovery\nindependent-recovery\n";
    char flow[512];snprintf(flow,sizeof(flow),"%swrong-identity\n",prefix);run(flow,false,false);
    snprintf(flow,sizeof(flow),"%sYES\n",prefix);run(flow,true,true);
    snprintf(flow,sizeof(flow),"%swrong\nYES\n",prefix);run(flow,true,true);
    assert(strstr(output,"Your answers are saved") && strstr(output,"YES\n"));
    run("1\nroot\noperator\nshort\noperator-passphrase\nwrong\noperator-passphrase\noperator-passphrase\noperator-passphrase\noperator-passphrase\nindependent-recovery\nindependent-recovery\nYES\n",true,true);
    assert(strstr(output,"at least 8") && strstr(output,"did not match") && strstr(output,"Please try again"));
    fail_deploy=true;run(flow,false,true);fail_deploy=false;
    expected_username="fabio";expected_password="eight888";
    run("1\nfabio\neight888\neight888\nindependent-recovery\nindependent-recovery\nYES\n",true,true);
    assert(strstr(output,"Username: fabio") && strstr(output,"********\n"));
    run("1\nfabio\neight888\neight888\nindependent-recovery\nindependent-recovery\nNO\n",false,false);
    puts("PASS actual installer CLI: visible choices/masked secrets, 8-character password, chosen username, retained-step retries, cancellation before writes, wiped request, terminal restoration and lease cleanup");
}
