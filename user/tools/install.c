#include "common.h"
#include "userdb.h"
#include "terminal.h"
#include "install_abi.h"
#include "account_abi.h"

static install_identity_v1_t identity;
static install_deploy_v1_t request;
static install_deploy_v2_t named_request;
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
static bool line(char out[129],bool secret) {
    size_t n=0;bool invalid=false;
    for (;;) {
        unsigned char c;
        if (tool_syscall(SYS_INPUT_READ,(uintptr_t)&c,1,(uintptr_t)-1)!=1 || c==3 || c==27) return false;
        if (c=='\n' || c=='\r') {
            out[n]=0;say("\n");
            if (invalid) {out[0]=0;say("Use at most 128 printable characters. Please try again.\n");}
            return true;
        }
        if (c==8 || c==127) {if (n) {out[--n]=0;say("\b \b");}continue;}
        if (c<32 || c>126 || n==128) {invalid=true;continue;}
        out[n++]=(char)c;
        char visible[2]={secret ? '*' : (char)c,0};say(visible);
    }
}
static bool password(const char *prompt,char out[78],const char *different_hash) {
    char first[129]={0},second[129]={0},salt[17]={0};uint8_t seed[16]={0};bool ok=false;
    for (;;) {
    db_wipe(first,sizeof(first));db_wipe(second,sizeof(second));
    say(prompt);if (!line(first,true)) goto done;
    if (tool_length(first)<8) {say("Use at least 8 characters. Please try again.\n");continue;}
    say("Confirm passphrase: ");if (!line(second,true)) goto done;
    if (!tool_equal(first,second)) {say("Passwords did not match. Please try again.\n");continue;}
    if (different_hash && db_verify(first,different_hash)) {
        say("Recovery passphrase must differ from your login password. Please try again.\n");continue;
    }
    break;
    }
    if (tool_syscall(SYS_INSTALLSEED,(uintptr_t)seed,16,0)) goto done;
    const char *alphabet="./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    for (unsigned i=0;i<16;i++) salt[i]=alphabet[seed[i]&63];
    say("Securing password... please wait.\n");ok=db_crypt(first,salt,out);
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
    named_request=(install_deploy_v2_t){0};
    status=(install_status_v1_t){0};uint32_t index=0;
#ifdef INSTALLER_PREVIEW
    say("FortressOS installer interface PREVIEW\nInstallation is disabled in this image.\nSynthetic target only; use example passphrases.\n\n");
#elif defined(INSTALLER_PHYSICAL)
    say("FortressOS installer — identified spare NVMe only\n");
#else
    say("FortressOS text installer — disposable workbench\n");
#endif
    if (tool_syscall(SYS_TERMCTL,TERM_ISATTY,0,0)!=1) goto done;
    /* Echo non-secret input and mask passwords. Disable ISIG while input is read
     * so Ctrl-C follows the explicit wipe/release path. Restore on every exit. */
    if (terminal(TERM_GET,&saved)) goto done;
    attrs=saved;attrs.input_flags&=~TERM_ISIG;
    if (terminal(TERM_SET,&attrs)) goto done;
    changed=true;
    if (control(INSTALL_ENUM,&index,4,&identity,sizeof(identity))!=1) {
#ifdef INSTALLER_PHYSICAL
        say("No admitted bound NVMe target. Use live rescue or power off.\n");goto done;
#else
        say("No admitted disposable NVMe target. Use live rescue or power off.\n");goto done;
#endif
    }
    say("\nStep 1 of 4 - Choose your disk\n\n1. ");fixed_text(identity.model,40);
    say("\n   Size: ");
    uint64_t gigabytes=identity.sector_count/(1000000000u/512);
    if (gigabytes) {number(gigabytes);say(" GB");}
    else {number(identity.sector_count/2048);say(" MiB");}
    say("\n   Serial: ");fixed_text(identity.serial,20);
    say("\n\nOnly this approved disk is available. The installer USB is excluded.\n");
    for (;;) {
        say("Choose disk 1, or cancel with Escape: ");
        if (!line(choice,false)) goto done;
        if (tool_equal(choice,"1")) break;
        say("Type 1 and press Enter to select the disk shown above.\n");
    }
    say("Checking installer files... please wait.\n");
    long checked=control(INSTALL_DEPLOY,&request,sizeof(request),&status,sizeof(status));
    if (checked) {
        char error[32];tool_format_i64(error,checked);
        say("Payload validation failed before writes (error ");say(error);say(").\n");goto done;
    }
#ifdef INSTALLER_PREVIEW
    say("Preview layout selected. Validation/install actions are disabled.\n");
#else
    say("Installer files checked. Your disk has not been changed.\n");
#endif
    say("\nStep 2 of 4 - Create your account\n");
    for (;;) {
        say("Choose username: ");if (!line(choice,false)) goto done;
        if (tool_length(choice)<32 && install_username_valid(choice)) break;
        say("Use 1-31 lowercase letters, digits, _ or -. Start with a letter or _. Root and wheel are reserved.\n");
    }
    for (unsigned i=0;choice[i];i++) named_request.username[i]=choice[i];
    if (!password("Choose login password (8..128 characters): ",request.operator_hash,NULL)) goto done;
    say("\nStep 3 of 4 - Recovery password\n");
    if (!password("Choose independent recovery passphrase (8..128 characters): ",request.recovery_hash,request.operator_hash)) goto done;
    say("Remember/store the separate recovery passphrase; losing it prevents authenticated recovery.\n");
    if (tool_syscall(SYS_INSTALLSEED,(uintptr_t)request.disk_guid,80,0)) goto done;
    for (unsigned i=0;i<4;i++) {uint8_t *g=i ? request.partition_guid[i-1] : request.disk_guid;g[7]=(g[7]&15)|64;g[8]=(g[8]&63)|128;}
    say("\nStep 4 of 4 - Confirm installation\nDisk: ");fixed_text(identity.model,40);
    say("\nUsername: ");say(named_request.username);
    say("\nAll existing partitions on this disk will be erased.\nFortressOS uses a 64 MiB boot partition and a 256 MiB root partition.\nThe remaining space will be unallocated.\n");
    for (;;) {
        say("Type YES to erase this disk and install, or NO to cancel: ");
        if (!line(choice,false) || tool_equal(choice,"NO") || tool_equal(choice,"no")) goto done;
        if (tool_equal(choice,"YES")) break;
        say("Not confirmed. Your answers are saved and the disk is unchanged.\n");
    }
#ifdef INSTALLER_PREVIEW
    say("Preview complete. Installation remains disabled.\n");result=0;goto done;
#endif
    install_target_v1_t target={.size=sizeof(target),.version=1,.generation=identity.generation,.flags=INSTALL_CONFIRM_WHOLE_DISK};
    for (unsigned i=0;i<32;i++) target.digest[i]=identity.digest[i];
    if (control(INSTALL_ACQUIRE,&target,sizeof(target),&fd,sizeof(fd))) goto done;
    request.fd=fd;request.flags=0;
    request.size=sizeof(named_request);request.version=2;named_request.base=request;
    say("Installing; keep power and installer media connected.\n");
    long deployed=control(INSTALL_DEPLOY,&named_request,sizeof(named_request),&status,sizeof(status));
    if (deployed || !status.complete || status.error) {
        say("Installation failed at stage ");number(status.stage);say(" error ");number((unsigned long)(status.error<0 ? -status.error : status.error));say(" call ");number((unsigned long)(deployed<0 ? -deployed : deployed));say(". Target may be partially erased; no rollback or success is claimed.\n");goto done;
    }
    say("Installation complete. Power off, remove installer media, then boot the target.\nFor Dell 5590: F2 Add Boot Option -> target EFI/BOOT/BOOTX64.EFI; select it with F12.\nRecovery menu requires your separate recovery passphrase.\n");result=0;
done:
    if (fd>=0) (void)control(INSTALL_RELEASE,&fd,4,NULL,0);
    db_wipe(choice,sizeof(choice));db_wipe(&request,sizeof(request));db_wipe(&named_request,sizeof(named_request));
    if (changed) (void)terminal(TERM_SET,&saved);
    if (result) say("Installer stopped; no success reported.\n");
    return result;
}
