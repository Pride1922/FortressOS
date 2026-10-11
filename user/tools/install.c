#include "common.h"
#include "userdb.h"
#include "terminal.h"
#include "install_abi.h"
#include "account_abi.h"

static install_identity_v1_t identity;
static install_deploy_v1_t request;
static install_status_v1_t status;
static void say(const char *s) {(void)tool_write("install",s,tool_length(s));}
#if defined(INSTALLER_PREVIEW)
/* Interface-only build: never calls INSTALLCTL or opens a storage device. */
static long control(unsigned op,const void *in,size_t n,void *out,size_t m) {
    (void)n;(void)m;
    if (op==INSTALL_ENUM && *(const uint32_t *)in==0) {
        install_identity_v1_t sample={.size=sizeof(sample),.version=1,.generation=1,
            .sector_count=512u*2048,.sector_size=512,.nsid=1,.flags=INSTALL_ID_ELIGIBLE};
        const char *model="Synthetic preview target",*serial="DEMO-NOT-A-DISK";
        for (unsigned i=0;i<sizeof(sample.model);i++) sample.model[i]=' ';
        for (unsigned i=0;i<sizeof(sample.serial);i++) sample.serial[i]=' ';
        for (unsigned i=0;model[i];i++) sample.model[i]=model[i];
        for (unsigned i=0;serial[i];i++) sample.serial[i]=serial[i];
        for (unsigned i=0;i<32;i++) sample.digest[i]=(uint8_t)(i+1);
        *(install_identity_v1_t *)out=sample;return 1;
    }
    if (op==INSTALL_DEPLOY && ((const install_deploy_v1_t *)in)->flags==INSTALL_DRY_RUN) return 0;
    return SYSCALL_EOPNOTSUPP;
}
#elif defined(INSTALLER_CLI_HOST_TEST)
static long control(unsigned op,const void *in,size_t n,void *out,size_t m);
static long terminal(unsigned op,terminal_attrs_t *attrs);
#else
static long control(unsigned op,const void *in,size_t n,void *out,size_t m) {
    long nr=SYS_INSTALLCTL;register uintptr_t a4 __asm__("r10")=(uintptr_t)out;
    register uintptr_t a5 __asm__("r8")=m;
    __asm__ volatile("syscall":"+a"(nr):"D"((uintptr_t)op),"S"((uintptr_t)in),"d"(n),"r"(a4),"r"(a5):"rcx","r11","cc","memory");
    return nr;
}
#endif
#ifndef INSTALLER_CLI_HOST_TEST
static long terminal(unsigned op,terminal_attrs_t *attrs) {
    long nr=SYS_TERMATTR;register uintptr_t a4 __asm__("r10")=sizeof(*attrs);
    __asm__ volatile("syscall":"+a"(nr):"D"((uintptr_t)0),"S"((uintptr_t)op),"d"((uintptr_t)attrs),"r"(a4):"rcx","r11","cc","memory");
    return nr;
}
#endif
static bool line(char out[129]) {
    size_t n=0;bool invalid=false;
    for (;;) {
        unsigned char c;
        if (tool_syscall(SYS_INPUT_READ,(uintptr_t)&c,1,(uintptr_t)-1)!=1 || c==3 || c==27) return false;
        if (c=='\n' || c=='\r') {out[n]=0;say("\n");return !invalid;}
        if (c==8 || c==127) {if (n) out[--n]=0;continue;}
        if (c<32 || c>126 || n==128) {invalid=true;continue;}
        out[n++]=(char)c;
    }
}
static bool password(const char *prompt,char out[78],const char *different_hash) {
    char first[129]={0},second[129]={0},salt[17]={0};uint8_t seed[16]={0};bool ok=false;
    say(prompt);if (!line(first) || tool_length(first)<12) goto done;
    say("Confirm passphrase: ");if (!line(second) || !tool_equal(first,second)) goto done;
    if (different_hash && db_verify(first,different_hash)) {
        say("Recovery passphrase must differ from the operator password.\n");goto done;
    }
    if (tool_syscall(SYS_INSTALLSEED,(uintptr_t)seed,16,0)) goto done;
    const char *alphabet="./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    for (unsigned i=0;i<16;i++) salt[i]=alphabet[seed[i]&63];
    ok=db_crypt(first,salt,out);
done:
    db_wipe(first,sizeof(first));db_wipe(second,sizeof(second));db_wipe(salt,sizeof(salt));db_wipe(seed,sizeof(seed));return ok;
}
static void fixed_text(const char *text,size_t n) {
    char copy[65];if (n>64) n=64;
    for (size_t i=0;i<n;i++) copy[i]=text[i]>=32 && text[i]<=126 ? text[i] : '?';
    copy[n]=0;say(copy);
}
static void number(uint64_t value) {char text[32];tool_format_u64(text,value);say(text);}
int install_main(int argc,char **argv) {
    (void)argc;(void)argv;
    terminal_attrs_t saved,attrs;bool changed=false;int result=1;int32_t fd=-1;char choice[129]={0};
    request=(install_deploy_v1_t){.size=sizeof(request),.version=1,.fd=-1,.flags=INSTALL_DRY_RUN};
    status=(install_status_v1_t){0};uint32_t index=0;
#ifdef INSTALLER_PREVIEW
    say("FortressOS installer interface PREVIEW\nInstallation is disabled in this image.\nSynthetic target only; use example passphrases.\n\n");
#else
    say("FortressOS text installer — disposable workbench\n");
#endif
    if (tool_syscall(SYS_TERMCTL,TERM_ISATTY,0,0)!=1) goto done;
    /* Raw input never echoes. Disable ISIG while secrets/confirmations are read
     * so Ctrl-C follows the explicit wipe/release path. Restore on every exit. */
    if (terminal(TERM_GET,&saved)) goto done;
    attrs=saved;attrs.input_flags&=~TERM_ISIG;
    if (terminal(TERM_SET,&attrs)) goto done;
    changed=true;
    if (control(INSTALL_ENUM,&index,4,&identity,sizeof(identity))!=1) {
        say("No admitted disposable NVMe target. Use live rescue or power off.\n");goto done;
    }
    say("Target 0: ");fixed_text(identity.model,40);say("\nSerial: ");fixed_text(identity.serial,20);
    say("\nController ");number(identity.segment);say(":");number(identity.bus);say(":");number(identity.device);say(".");number(identity.function);
    say(" namespace ");number(identity.nsid);say("\nCapacity MiB: ");number(identity.sector_count/2048);
    say("; logical sector bytes: ");number(identity.sector_size);
    say("\nSource: installer boot media (excluded). Layout: 1 MiB BIOS, 64 MiB ESP, 256 MiB root.\nUnused MiB: ");
    number(identity.sector_count>659489 ? (identity.sector_count-659489)/2048 : 0);
    say("\nInstallation erases the whole selected disk. No target is preselected.\nEnter target index 0, or cancel with Escape: ");
    if (!line(choice) || !tool_equal(choice,"0")) goto done;
    long checked=control(INSTALL_DEPLOY,&request,sizeof(request),&status,sizeof(status));
    if (checked) {
        char error[32];tool_format_i64(error,checked);
        say("Payload validation failed before writes (error ");say(error);say(").\n");goto done;
    }
#ifdef INSTALLER_PREVIEW
    say("Preview layout selected. Validation/install actions are disabled.\n");
#else
    say("Payload valid; dry-run performed no target I/O.\n");
#endif
    if (!password("Choose operator password (12..128 printable ASCII): ",request.operator_hash,NULL) ||
        !password("Choose independent recovery passphrase (12..128 printable ASCII): ",request.recovery_hash,request.operator_hash)) goto done;
    say("Remember/store the separate recovery passphrase; losing it prevents authenticated recovery.\n");
    if (tool_syscall(SYS_INSTALLSEED,(uintptr_t)request.disk_guid,80,0)) goto done;
    for (unsigned i=0;i<4;i++) {uint8_t *g=i ? request.partition_guid[i-1] : request.disk_guid;g[7]=(g[7]&15)|64;g[8]=(g[8]&63)|128;}
    char confirmation[71]="ERASE ";const char *hex="0123456789abcdef";
    for (unsigned i=0;i<32;i++) {confirmation[6+i*2]=hex[identity.digest[i]>>4];confirmation[7+i*2]=hex[identity.digest[i]&15];}confirmation[70]=0;
    say("To confirm this exact whole-disk identity, type:\n");say(confirmation);say("\nConfirmation: ");
    if (!line(choice) || !tool_equal(choice,confirmation)) goto done;
#ifdef INSTALLER_PREVIEW
    say("Preview complete. Installation remains disabled.\n");result=0;goto done;
#endif
    install_target_v1_t target={.size=sizeof(target),.version=1,.generation=identity.generation,.flags=INSTALL_CONFIRM_WHOLE_DISK};
    for (unsigned i=0;i<32;i++) target.digest[i]=identity.digest[i];
    if (control(INSTALL_ACQUIRE,&target,sizeof(target),&fd,sizeof(fd))) goto done;
    request.fd=fd;request.flags=0;
    say("Installing; keep power and installer media connected.\n");
    long deployed=control(INSTALL_DEPLOY,&request,sizeof(request),&status,sizeof(status));
    if (deployed || !status.complete || status.error) {
        say("Installation failed at stage ");number(status.stage);say(" error ");number((unsigned long)(status.error<0 ? -status.error : status.error));say(" call ");number((unsigned long)(deployed<0 ? -deployed : deployed));say(". Target may be partially erased; no rollback or success is claimed.\n");goto done;
    }
    say("Installation complete. Power off, remove installer media, then boot the target.\nFor Dell 5590: F2 Add Boot Option -> target EFI/BOOT/BOOTX64.EFI; select it with F12.\nRecovery menu requires your separate recovery passphrase.\n");result=0;
done:
    if (fd>=0) (void)control(INSTALL_RELEASE,&fd,4,NULL,0);
    db_wipe(choice,sizeof(choice));db_wipe(&request,sizeof(request));
    if (changed) (void)terminal(TERM_SET,&saved);
    if (result) say("Installer stopped; no success reported.\n");
    return result;
}
