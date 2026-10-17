/* Actual text installer with terminal/control adapters; no guest I/O claim. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#define INSTALLER_CLI_HOST_TEST
#include "../user/tools/install.c"
/* Codec/randomness is independently tested using actual crypto; this adapter
 * exercises installation continuation without a provisionable entropy source. */
int ssh_hostkey_generate(char *p,size_t cap,size_t *n,ssh_hostkey_workspace *w) {
    (void)p;(void)cap;(void)n;db_wipe(w,sizeof(*w));return SYSCALL_EIO;
}
static const char *input;
static jmp_buf rebooted;
static bool keyboard_pending,finish_pending;
static char output[16384];
static size_t output_size;
static size_t hostname_prompt_seen;
static unsigned acquisitions,deployments,releases,restores,seeds;
static bool fail_deploy,fail_log;
static unsigned fail_stage=7;
static long fail_validation,fail_acquire;
static unsigned disk_count=1,expected_index;
static const char *expected_username="operator",*expected_password="operator-passphrase";
static terminal_attrs_t current;
static unsigned output_mode, reveals;
static bool local_unavailable;
static char local_output[131072];
static size_t local_size;
size_t tool_length(const char *s) {return strlen(s);}
bool tool_equal(const char *a,const char *b) {return !strcmp(a,b);}
int tool_write(const char *name,const void *bytes,size_t n) {
    (void)name;
    if (output_mode==TERM_LOCAL) {assert(local_size+n<sizeof(local_output));memcpy(local_output+local_size,bytes,n);local_size+=n;local_output[local_size]=0;if (n==strlen("independent-recovery") && !memcmp(bytes,"independent-recovery",n)) reveals++;return 0;}
    assert(output_size+n<sizeof(output));
    memcpy(output+output_size,bytes,n);output_size+=n;output[output_size]=0;return 0;
}
long tool_syscall(long nr,uintptr_t a,uintptr_t b,uintptr_t c) {
    (void)c;
    if (nr==SYS_TERMCTL) {
        if (a==TERM_ISATTY) return 1;
        terminal_info_t *info=(void *)b;assert(c==sizeof(*info));
        if (a==TERM_GET) *info=(terminal_info_t){.version=1,.mode=output_mode};
        else {assert(a==TERM_SET && info->version==1);if (info->mode==TERM_LOCAL && local_unavailable) return SYSCALL_EOPNOTSUPP;output_mode=info->mode;}
        return 0;
    }
    if (nr==SYS_KBD_LAYOUT) return 0;
    if (nr==SYS_REBOOT) {assert(releases==1 && restores==1);longjmp(rebooted,1);}
    if (nr==SYS_INPUT_READ) {
        if (keyboard_pending) {keyboard_pending=false;*(char *)a='\n';return 1;}
        /* Existing navigation/password scenarios keep the default hostname. */
        const char *hp=strstr(output+hostname_prompt_seen,"Computer name [");
        if (hp) {hostname_prompt_seen=output_size;*(char *)a='\n';return 1;}
        if (!*input) {
            if (finish_pending && (deployments || strstr(output,"Press Enter to return to the installer"))) {finish_pending=false;*(char *)a='\n';return 1;}
            return SYSCALL_EINTR;
        }
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
        unsigned i=*(const unsigned *)in;if (i>=disk_count) return 0;
        install_identity_v1_t *id=out;*id=(install_identity_v1_t){.sector_count=384*2048,.sector_size=512,.generation=1,.flags=INSTALL_ID_ELIGIBLE};
        memcpy(id->model,i ? "Second NVMe" : "First NVMe",i ? 11 : 10);memcpy(id->serial,"SECRET-SERIAL",13);return 1;
    }
    if (op==INSTALL_ACQUIRE) {if (fail_acquire) return fail_acquire;assert(((const install_target_v1_t *)in)->index==expected_index);acquisitions++;*(int32_t *)out=3;return 0;}
    if (op==INSTALL_RELEASE) {releases++;return 0;}
    if (op==INSTALL_LOG) {
        const install_log_event_v1_t *event=in;
        assert(event->size==32 && event->version==1 && event->event<=INSTALL_LOG_FAILED && !event->reserved);return fail_log ? SYSCALL_EIO : 0;
    }
    assert(op==INSTALL_DEPLOY);
    const install_deploy_v1_t *req=in;
    if (req->flags==INSTALL_DRY_RUN) {assert(!acquisitions);return fail_validation;}
    assert(req->version==4 && req->size==368 && !strcmp(((const install_deploy_v3_t *)req)->username,expected_username));
    assert(!((const install_deploy_v4_t *)req)->hostkey && !((const install_deploy_v4_t *)req)->hostkey_bytes);
    assert(!strcmp(((const install_deploy_v3_t *)req)->hostname,"fortress"));
    deployments++;
    assert(db_verify(expected_password,req->operator_hash));
    assert(db_verify("independent-recovery",req->recovery_hash));
    *(install_status_v1_t *)out=(install_status_v1_t){.stage=fail_stage,.complete=!fail_deploy,.error=fail_deploy ? -5 : 0};
    return fail_deploy ? SYSCALL_EIO : 0;
}
static void run(const char *bytes,bool success,bool destructive) {
    input=bytes;output_size=hostname_prompt_seen=0;acquisitions=deployments=releases=restores=seeds=0;
    keyboard_pending=finish_pending=true;output_mode=TERM_MIRROR;reveals=0;local_size=0;
    current=(terminal_attrs_t){.input_flags=TERM_ISIG};
    if (!setjmp(rebooted)) assert(install_main(0,NULL)==(success ? 0 : 1));
    else assert(success);
    assert(restores==1 && (current.input_flags&TERM_ISIG) && output_mode==TERM_MIRROR);
    assert(strstr(output,"FortressOS installer - Step 1 of 5"));
    if (deployments) {assert(strstr(output,"FortressOS installer - Step 5 of 5\nInstalling FortressOS"));assert(strstr(output,"FortressOS installer - Step 5 of 5\nInstallation result"));}
    assert(strstr(output,"384 MiB NVMe"));assert(!strstr(output,"SECRET-SERIAL") && !strstr(output,"Serial:"));
    if (deployments) {
        const char *host=strstr(output,"Computer name [");
        const char *user=strstr(output,"Choose username: ");
        const char *pass=strstr(output,"Choose login password: ");
        assert(host && user && pass && host<user && user<pass);
    }
    assert(acquisitions==destructive && deployments==destructive && releases==destructive);
    const unsigned char *named=(void *)&named_request;for (size_t i=0;i<sizeof(named_request);i++) assert(!named[i]);
    const unsigned char *p=(void *)&request;for (size_t i=0;i<sizeof(request);i++) assert(!p[i]);
    assert(!strstr(output,"operator-passphrase") && !strstr(output,"independent-recovery"));
    assert(!strstr(output,expected_password));
}
int main(void) {
    assert(install_hostname_valid("dell-5590") && install_hostname_valid("a") && install_hostname_valid("HOST1"));
    assert(!install_hostname_valid("") && !install_hostname_valid("-bad") && !install_hostname_valid("bad-") && !install_hostname_valid("bad name") && !install_hostname_valid("bad\033name"));
    assert(common_password("Password1") && common_password("P@ssw0rd!") && common_password("12345678"));
    assert(common_password("Password123456789!") && common_password("AZERTYUIOP") && common_password("FortressOS"));
    assert(!common_password("eight888") && !common_password("passwords can be longer"));
    assert(password_strength("eight888")==0);
    assert(password_strength("cobalt meadow")==1);
    assert(password_strength("cobalt meadow lantern")==2);
    assert(password_strength("aaaaaaaaaaaaaaaaaaaaaaaa")==0 && password_strength("abcdabcdabcdabcdabcd")==0);
    assert(password_strength("Password123!")==0);
    install_identity_v1_t summary={0};output_size=0;contents_warning(&summary);
    assert(strstr(output,"Contents unknown - this disk may contain data."));
    output_size=0;summary.reserved2[0]=INSTALL_SUMMARY_VALID;contents_warning(&summary);
    assert(strstr(output,"No existing partitions detected. This disk may still contain data."));
    output_size=0;summary.reserved2[2]=2;summary.reserved2[3]=INSTALL_SUMMARY_WINDOWS|INSTALL_SUMMARY_EFI;contents_warning(&summary);
    assert(strstr(output,"Existing partitions: Windows-type, EFI boot\n"));
    output_size=0;summary.reserved2[3]=INSTALL_SUMMARY_UNREADABLE;contents_warning(&summary);
    assert(strstr(output,"Contents unknown"));
    run("\033",false,false);run("\003",false,false);run("\n",false,false);run("2\n",false,false);
    run("1\noperator\n\n",false,false);run("1\noperator\nshort\n",false,false);
    run("1\noperator\noperator-passphrase\ndifferent\n",false,false);
    run("1\noperator\noperator-passphrase\noperator-passphrase\n\033",false,false);
    run("1\noperator\noperator-passphrase\noperator-passphrase\noperator-passphrase\noperator-passphrase\n",false,false);
    char long_input[512]="1\noperator\n";memset(long_input+11,'x',129);memcpy(long_input+140,"\n",2);run(long_input,false,false);
    const char *prefix="1\noperator\noperator-passphrase\noperator-passphrase\nindependent-recovery\nindependent-recovery\nYES\n";
    char flow[512];snprintf(flow,sizeof(flow),"%swrong-identity\n",prefix);run(flow,false,false);
    snprintf(flow,sizeof(flow),"%s1\n",prefix);run(flow,true,true);
    snprintf(flow,sizeof(flow),"%swrong\n\nYES\n2\n1\n",prefix);run(flow,true,true);
    assert(strstr(output,"Your answers are saved") && strstr(output,"Type 1 to confirm"));
    run("1\nroot\noperator\nshort\noperator-passphrase\nwrong\noperator-passphrase\noperator-passphrase\noperator-passphrase\noperator-passphrase\nindependent-recovery\nindependent-recovery\nYES\n1\n",true,true);
    assert(strstr(output,"at least 8") && strstr(output,"did not match") && strstr(output,"Please try again"));
    fail_deploy=true;run(flow,false,true);fail_deploy=false;
    expected_username="fabio";expected_password="eight888";
    run("1\nfabio\neight888\neight888\nindependent-recovery\nindependent-recovery\nYES\n1\n",true,true);
    assert(strstr(output,"Username: fabio") && strstr(output,"********\n"));
    run("1\nfabio\nPassword1\n12345678\neight888\neight888\nWELCOME1\nindependent-recovery\nindependent-recovery\nYES\n1\n",true,true);
    assert(strstr(output,"too common") && strstr(output,"Strength: Weak") && strstr(output,"Strength: Strong"));
    assert(!strstr(output,"Password1") && !strstr(output,"12345678") && !strstr(output,"WELCOME1"));
    run("1\nfabio\neight888\neight888\nindependent-recovery\nindependent-recovery\nYES\nNO\n",false,false);
    disk_count=2;expected_index=1;
    run("0\n3\n2\nfabio\neight888\neight888\nindependent-recovery\nindependent-recovery\nYES\n1\n\nYES\n2\n",true,true);
    assert(strstr(output,"1. First NVMe - 384 MiB NVMe") && strstr(output,"2. Second NVMe - 384 MiB NVMe") && strstr(output,"Disk 2: Second NVMe"));
    run("2\nfabio\neight888\neight888\nindependent-recovery\nindependent-recovery\nNO\nSHOW\n\nSHOW\n\nYES\n2\n",true,true);
    assert(reveals==2 && strstr(output,"Type YES when it is stored"));
    run("2\nfabio\neight888\neight888\nindependent-recovery\nindependent-recovery\nSHOW\n\033",false,false);
    assert(reveals==1);
    run("2\nfabio\neight888\neight888\nindependent-recovery\nindependent-recovery\n\003",false,false);
    local_unavailable=true;
    run("2\nfabio\neight888\neight888\nindependent-recovery\nindependent-recovery\nSHOW\nYES\n2\n",true,true);
    assert(!reveals && strstr(output,"requires a local screen"));local_unavailable=false;
    // Traverse all prior screens and retain both hashes: no seed/hash rerun.
    run("2\nfabio\neight888\neight888\nindependent-recovery\nindependent-recovery\nYES\n\033\033\033\033\n\n\n\n\n2\n",true,true);
    assert(seeds==4 && strstr(output,"Current disk: 2") && strstr(output,"Current username: fabio"));
    // Shortening a username clears its previous tail; changing login invalidates recovery.
    expected_username="al";expected_password="new-login-passphrase";
    run("2\nfabio\neight888\neight888\nindependent-recovery\nindependent-recovery\nYES\n\033\033al\nCHANGE\nnew-login-passphrase\nnew-login-passphrase\nindependent-recovery\nindependent-recovery\nYES\n2\n",true,true);
    // Partial secret input is discarded when going back; no deployment on cancel.
    run("2\nal\npartial\033CANCEL\n",false,false);
    expected_username="fabio";expected_password="eight888";
    expected_index=0;
    run("2\nfabio\neight888\neight888\nindependent-recovery\nindependent-recovery\nYES\n\033\033\0331\n\n\n\n1\n",true,true);
    assert(strstr(output,"Disk 1: First NVMe"));
    expected_index=1;
    run("2\nfabio\neight888\neight888\nindependent-recovery\nindependent-recovery\nYES\n\033CHANGE\nindependent-recovery\nindependent-recovery\nYES\n2\n",true,true);
    assert(seeds==5);
    run("2\nfabio\neight888\neight888\n\033\n\nCANCEL\n",false,false);
    run("2\nfabio\n\teight888\t\neig\tht888\n\tindependent-recovery\nindependent-\trecovery\t\nYES\n2\n",true,true);
    assert(strstr(local_output,"Step 3 of 5\nCreate your account") && strstr(local_output,"Step 4 of 5\nRecovery password") && strstr(local_output,"Confirm password: "));
    assert(strstr(local_output,"eight888") && strstr(local_output,"independent-recovery") && strstr(local_output,"Hidden - Tab"));
    run("2\nfabio\n\tpartial\b\033CANCEL\n",false,false);
    assert(strstr(local_output,"Choose login password: partia") && !strstr(output,"partial"));
    local_unavailable=true;
    run("2\nfabio\n\teight888\neight888\nindependent-recovery\nindependent-recovery\nYES\n2\n",true,true);
    assert(strstr(output,"input remains hidden"));local_unavailable=false;
    char full_entry[134],answer[129]={0};full_entry[0]='\t';
    memset(full_entry+1,'z',128);memcpy(full_entry+129,"\t\n",3);
    input=full_entry;keyboard_pending=finish_pending=false;output_size=local_size=0;output_mode=TERM_MIRROR;
    assert(line(answer,true) && strlen(answer)==128 && output_mode==TERM_MIRROR);
    assert(!strstr(output,"zzzz") && strstr(local_output,"Hidden - Tab"));db_wipe(answer,sizeof(answer));
    keyboard_pending=false;expected_index=1;expected_username="fabio";expected_password="eight888";
    fail_validation=SYSCALL_EINVAL;run("2\n",false,false);fail_validation=0;
    assert(strstr(output,"could not be validated") && strstr(output,"No installation data was written"));
    fail_acquire=SYSCALL_EAGAIN;
    run("2\nfabio\neight888\neight888\nindependent-recovery\nindependent-recovery\nYES\n2\n",false,false);fail_acquire=0;
    assert(strstr(output,"could not be locked safely") && strstr(output,"No installation data was written"));
    fail_deploy=true;fail_stage=6;
    run("2\nfabio\neight888\neight888\nindependent-recovery\nindependent-recovery\nYES\n2\n",false,true);
    assert(strstr(output,"could not be verified") && strstr(output,"Do not try to boot it yet") && strstr(output,"error -5, call -9"));
    fail_deploy=false;fail_stage=7;fail_log=true;
    run("2\nfabio\neight888\neight888\nindependent-recovery\nindependent-recovery\nYES\n2\n",true,true);
    assert(strstr(output,"USB log could not be saved") && strstr(output,"Installation complete."));fail_log=false;
    input="\n";keyboard_pending=finish_pending=false;output_size=0;
    failure_screen(5,"Test incomplete status",0,5,-VFS_ENOMEM,true);
    assert(strstr(output,"out of memory") && strstr(output,"error -12, call 0"));
    output_size=0;failure_reason(SYSCALL_ENOMEM,0);assert(strstr(output,"out of memory"));
    output_size=0;failure_reason(SYSCALL_ENOSPC,5);assert(strstr(output,"out of space"));
    output_size=0;failure_reason(SYSCALL_EIO,4);assert(strstr(output,"could not be copied"));
    output_size=0;failure_reason(SYSCALL_EIO,6);assert(strstr(output,"could not be verified"));
    output_size=0;failure_reason(SYSCALL_EAGAIN,1);assert(strstr(output,"identity may have changed"));
    output_size=0;failure_reason(-999,99);assert(strstr(output,"report the details"));
    puts("PASS actual installer CLI: visible choices/masked secrets, 8-character password, chosen username, retained-step retries, cancellation before writes, wiped request, terminal restoration and lease cleanup");
}
