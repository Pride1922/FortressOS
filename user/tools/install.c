#include "common.h"
#include "userdb.h"
#include "terminal.h"
#include "install_abi.h"
#include "account_abi.h"

static install_identity_v1_t identity;
static install_identity_v1_t disks[8];
static install_deploy_v1_t request;
static install_deploy_v3_t named_request;
static install_status_v1_t status;
static void say(const char *s) {(void)tool_write("install",s,tool_length(s));}
static void clear_screen(void) {say("\033[2J\033[H");}
static unsigned screen_step=1;
static const char *screen_title="Keyboard layout";
static const char *password_label="Password: ";
static void draw_header(void) {
    clear_screen();say("FortressOS installer - Step ");
    char digit[2]={(char)('0'+screen_step),0};say(digit);
    say(" of 5\n");say(screen_title);say("\n\n");
}
static void screen(unsigned step,const char *title) {
    screen_step=step;screen_title=title;draw_header();
}
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
    if (op==INSTALL_ENUM) return 0;
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
static bool go_back;
static void navigation(void) {say("Escape: previous step | Ctrl-C: cancel installation\nType CANCEL at any non-password prompt to cancel.\n\n");}
/* After the first reveal, redraw locally so editing wrapped passwords also
 * removes old visible characters. Clear before restoring mirrored output. */
static void password_frame(const char *text,size_t n,bool visible) {
    draw_header();
    say(visible ? "Visible - Tab hides password\n" : "Hidden - Tab shows password\n");
    say("Escape: previous step | Ctrl-C: cancel\n\n");say(password_label);
    if (visible) (void)tool_write("install",text,n);
    else for (size_t i=0;i<n;i++) say("*");
}
static bool line(char out[129],bool secret) {
    go_back=false;
    size_t n=0;bool invalid=false,local=false,visible=false,accepted=false;
    terminal_info_t saved={0};
    for (;;) {
        unsigned char c;
        if (tool_syscall(SYS_INPUT_READ,(uintptr_t)&c,1,(uintptr_t)-1)!=1 || c==3) break;
        if (c==27) {go_back=true;break;}
        if (c=='\n' || c=='\r') {
            out[n]=0;accepted=true;break;
        }
        if (secret && c=='\t') {
            if (!local) {
                if (tool_syscall(SYS_TERMCTL,TERM_GET,(uintptr_t)&saved,sizeof(saved))) break;
                terminal_info_t route=saved;route.mode=TERM_LOCAL;
                if (tool_syscall(SYS_TERMCTL,TERM_SET,(uintptr_t)&route,sizeof(route))) {
                    say("\nShowing passwords requires a local screen; input remains hidden.\n");continue;
                }
                local=true;
            }
            visible=!visible;password_frame(out,n,visible);continue;
        }
        if (c==8 || c==127) {
            if (n) {out[--n]=0;if (local) password_frame(out,n,visible);else say("\b \b");}
            continue;
        }
        if (c<32 || c>126 || n==128) {invalid=true;continue;}
        out[n++]=(char)c;
        if (local) password_frame(out,n,visible);
        else {char display[2]={secret ? '*' : (char)c,0};say(display);}
    }
    if (local) {
        draw_header();
        if (tool_syscall(SYS_TERMCTL,TERM_SET,(uintptr_t)&saved,sizeof(saved))) accepted=false;
    }
    if (!accepted) return false;
    say("\n");
    if (invalid) {out[0]=0;say("Use at most 128 printable characters. Please try again.\n");}
    if (!secret && (tool_equal(out,"CANCEL") || tool_equal(out,"cancel"))) return false;
    return true;
}
/* Small local blocklist, not a breach database. Compare without keeping another
 * plaintext copy. Case variants and numeric/punctuation suffixes
 * are rejected too; all other weak passwords remain usable. */
static bool common_password(const char *text) {
    static const char *const common[]={"password","passw0rd","p@ssword","p@ssw0rd","qwerty","qwertyuiop",
        "12345678","123456789","1234567890","87654321","11111111","00000000","abcdefgh",
        "abcd1234","abc123","letmein","welcome","iloveyou","admin","administrator","changeme",
        "default","monkey","dragon","football","baseball","sunshine","princess","trustno1",
        "azerty","azertyuiop","asdfghjk","zxcvbnm","operator","fortress","fortressos"};
    for (unsigned i=0;i<sizeof(common)/sizeof(common[0]);i++) {
        size_t at=0;
        while (common[i][at] && text[at]) {
            char c=text[at];if (c>='A' && c<='Z') c=(char)(c-'A'+'a');
            if (c!=common[i][at]) break;
            at++;
        }
        if (common[i][at]) continue;
        while (text[at] && ((text[at]>='0' && text[at]<='9') || text[at]=='!' || text[at]=='?')) at++;
        if (!text[at]) return true;
    }
    return false;
}
/* Advisory length/diversity hint, not an entropy estimate or composition rule.
 * Long lowercase passphrases can be Strong; repeated short patterns stay Weak. */
static unsigned password_strength(const char *text) {
    size_t n=tool_length(text);unsigned distinct=0;
    for (size_t i=0;i<n;i++) {
        bool seen=false;for (size_t j=0;j<i;j++) if (text[j]==text[i]) {seen=true;break;}
        if (!seen) distinct++;
    }
    if (n<12 || distinct<6 || common_password(text)) return 0;
    for (size_t period=1;period<=4 && period<n;period++) {
        bool repeated=true;for (size_t i=period;i<n;i++) if (text[i]!=text[i%period]) {repeated=false;break;}
        if (repeated) return 0;
    }
    return n<16 ? 1 : 2;
}
/* Plaintext is displayed only on the local screen, never the mirrored UART.
 * Clear it before restoring output routing; keep the secret in password()'s
 * existing short-lived, wiped buffers. */
static bool recovery_saved(const char *secret) {
    char answer[129]={0};bool ok=false;
    for (;;) {
        say("Store your recovery password somewhere safe. Losing it prevents recovery.\n");
        say("Have you stored it safely? YES to continue, SHOW to view it: ");
        if (!line(answer,false)) break;
        if (tool_equal(answer,"YES") || tool_equal(answer,"yes")) {ok=true;break;}
        if (tool_equal(answer,"SHOW") || tool_equal(answer,"show")) {
            terminal_info_t saved={0};
            if (tool_syscall(SYS_TERMCTL,TERM_GET,(uintptr_t)&saved,sizeof(saved))) break;
            terminal_info_t local=saved;local.mode=TERM_LOCAL;
            if (tool_syscall(SYS_TERMCTL,TERM_SET,(uintptr_t)&local,sizeof(local))) {
                say("Showing the password requires a local screen. It remains hidden.\n");continue;
            }
            screen(4,"Recovery password");say("Recovery password: ");say(secret);
            say("\n\nStore it safely, then press Enter to hide it.\n");
            password_label="Press Enter to hide password: ";
            bool accepted=line(answer,true);
            draw_header();
            long restored=tool_syscall(SYS_TERMCTL,TERM_SET,(uintptr_t)&saved,sizeof(saved));
            if (!accepted || restored) break;
            draw_header();
        } else say("Type YES when it is stored, or SHOW to view it. Escape goes back; Ctrl-C cancels.\n");
    }
    db_wipe(answer,sizeof(answer));return ok;
}
static bool password(const char *prompt,char out[78],const char *different_hash) {
    go_back=false;
    char first[129]={0},second[129]={0},salt[17]={0};uint8_t seed[16]={0};bool ok=false;
    for (;;) {
    db_wipe(first,sizeof(first));db_wipe(second,sizeof(second));
    say("Passwords start hidden. Tab shows or hides them on this screen.\n");
    password_label=prompt;say(prompt);if (!line(first,true)) goto done;
    if (tool_length(first)<8) {say("Use at least 8 characters. Please try again.\n");continue;}
    unsigned strength=password_strength(first);
    say(strength==2 ? "Strength: Strong\n" : strength==1 ? "Strength: OK\n" : "Strength: Weak - consider a longer passphrase.\n");
    if (common_password(first)) {say("That password is too common. Choose a less predictable password.\n");continue;}
    password_label="Confirm password: ";say(password_label);if (!line(second,true)) goto done;
    if (!tool_equal(first,second)) {say("Passwords did not match. Please try again.\n");continue;}
    if (different_hash && db_verify(first,different_hash)) {
        say("Recovery password must differ from your login password. Please try again.\n");continue;
    }
    break;
    }
    if (different_hash && !recovery_saved(first)) goto done;
    if (tool_syscall(SYS_INSTALLSEED,(uintptr_t)seed,16,0)) goto done;
    const char *alphabet="./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    for (unsigned i=0;i<16;i++) salt[i]=alphabet[seed[i]&63];
    draw_header();say("Securing password... please wait.\n");ok=db_crypt(first,salt,out);
done:
    db_wipe(first,sizeof(first));db_wipe(second,sizeof(second));db_wipe(salt,sizeof(salt));db_wipe(seed,sizeof(seed));return ok;
}
static void fixed_text(const char *text,size_t n) {
    char copy[65];if (n>64) n=64;
    while (n && (text[n-1]==' ' || !text[n-1])) n--;
    for (size_t i=0;i<n;i++) copy[i]=text[i]>=32 && text[i]<=126 ? text[i] : '?';
    copy[n]=0;say(copy);
}
static void number(uint64_t value) {char text[32];tool_format_u64(text,value);say(text);}
/* Summary bytes come from the kernel's read-only boot inventory (see INSTALL_SUMMARY_*). */
static void contents_warning(const install_identity_v1_t *id) {
    const uint8_t *s=id->reserved2;bool any=false;
    if (s[0]!=INSTALL_SUMMARY_VALID || (s[3]&INSTALL_SUMMARY_UNREADABLE)) {
        say("Contents unknown - this disk may contain data.\n");return;
    }
    if (!s[2]) {say("No existing partitions detected. This disk may still contain data.\n");return;}
    say("Existing partitions: ");
    if (s[3]&INSTALL_SUMMARY_WINDOWS) {say("Windows-type");any=true;}
    if (s[3]&INSTALL_SUMMARY_LINUX) {say(any ? ", " : "");say("Linux-type");any=true;}
    if (s[3]&INSTALL_SUMMARY_EFI) {say(any ? ", " : "");say("EFI boot");any=true;}
    if (s[3]&INSTALL_SUMMARY_OTHER) {say(any ? ", " : "");say("other");any=true;}
    if (!any) say("contents unknown");
    say("\n");
}
static void disk_label(const install_identity_v1_t *id) {
    fixed_text(id->model,40);say(" - ");
    uint64_t bytes=id->sector_count*id->sector_size;
    uint64_t gb=bytes/1000000000u;
    if (gb) {number(gb);say(" GB NVMe");}
    else {number(bytes/(1024*1024));say(" MiB NVMe");}
}
/* The call result uses syscall errors; status.error retains raw VFS errors.
 * Keep the namespaces separate, and never guess a failed physical component. */
static void failure_reason(long call,unsigned stage) {
    switch (call) {
    case SYSCALL_ENOMEM:
        say("The installer ran out of memory. Restart and try again.\n");return;
    case SYSCALL_ENOSPC:
        say("The installation ran out of space. Recreate the installer USB; if this repeats, report the details below.\n");return;
    case SYSCALL_EAGAIN:
        say("The selected disk could not be locked safely. It may be busy or its identity may have changed.\nRestart the installer and choose the disk again.\n");return;
    case SYSCALL_EACCES: case SYSCALL_EPERM: case SYSCALL_EROFS:
        say("The installer cannot safely use this disk or configuration. Choose another supported disk, or report the details below.\n");return;
    case SYSCALL_EOPNOTSUPP:
        say("This disk or installer configuration is not supported. Use a supported NVMe disk and the latest installer image.\n");return;
    case SYSCALL_EINVAL:
        say("The installer files or installation settings could not be validated. Recreate the USB using the latest installer image.\n");return;
    case SYSCALL_EIO: case SYSCALL_ETIMEDOUT:
        if (stage==0) say("The installer files could not be checked. Recreate the installer USB and try again.\n");
        else if (stage==1) say("The selected disk could not be checked safely. Restart and select it again.\n");
        else if (stage==2 || stage==3) say("The selected disk could not be prepared for installation.\n");
        else if (stage==4) say("FortressOS files could not be copied to the selected disk.\n");
        else if (stage==5) say("Your account and recovery information could not be saved.\n");
        else if (stage==6) say("The files on the selected disk could not be verified.\n");
        else say("The installation could not be completed safely.\n");
        if (stage>=2) say("Keep the power and USB connected. Restart before trying again; if it repeats, try another disk or USB.\n");
        return;
    default:
        say("The installer could not complete this step. Restart and try again; if it repeats, report the details below.\n");return;
    }
}
static bool log_failed,failure_reported;
static void log_event(unsigned event,unsigned stage,int error,long call) {
    install_log_event_v1_t entry={.size=sizeof(entry),.version=1,.event=event,.stage=stage,.error=error,.call=call};
    if (control(INSTALL_LOG,&entry,sizeof(entry),NULL,0)) log_failed=true;
}
static void log_notice(uint32_t warnings) {
    if (log_failed || (warnings&INSTALL_STATUS_USB_LOG_WARNING)) say("Warning: the USB log could not be saved. Installation can continue.\n");
    else say("USB log: /fortress-install.log on the FORTRESSLOG volume.\n");
    if (warnings&INSTALL_STATUS_ROOT_LOG_WARNING) say("Warning: the laptop log could not be saved.\n");
}
static void failure_screen(unsigned step,const char *action,long call,unsigned stage,int error,bool may_have_written) {
    failure_reported=true;log_event(INSTALL_LOG_FAILED,stage,error,call);
    screen(step,"Installation result");say("Installation failed.\n\n");say(action);say("\n\n");
    long reason=call;
    if (!reason) {
        if (error==-VFS_ENOMEM) reason=SYSCALL_ENOMEM;
        else if (error==-VFS_ENOSPC) reason=SYSCALL_ENOSPC;
        else if (error==-VFS_EINVAL) reason=SYSCALL_EINVAL;
        else if (error==-VFS_EIO) reason=SYSCALL_EIO;
    }
    failure_reason(reason,stage);
    say(may_have_written ? "\nThe disk may be partly erased. Do not try to boot it yet.\nNothing was reported as installed.\n" : "\nInstallation has not started. No installation data was written to the disk.\n");
    say("\nDetails: stage ");number(stage);say(", error ");char code[32];tool_format_i64(code,error);say(code);
    say(", call ");tool_format_i64(code,call);say(code);
    say("\n");log_notice(0);
    say("\nPress Enter to return to the installer.\n");
    for (;;) {unsigned char key=0;if (tool_syscall(SYS_INPUT_READ,(uintptr_t)&key,1,(uintptr_t)-1)!=1 || key=='\n' || key=='\r') break;}
}
int install_main(int argc,char **argv) {
    (void)argc;(void)argv;
    terminal_attrs_t saved,attrs;bool changed=false;int result=1;int32_t fd=-1;char choice[129]={0};
    request=(install_deploy_v1_t){.size=sizeof(request),.version=1,.fd=-1,.flags=INSTALL_DRY_RUN};
    named_request=(install_deploy_v3_t){0};
    status=(install_status_v1_t){0};uint32_t index=0,count=0,layout=0;bool selected=false;go_back=false;
#ifdef INSTALLER_PREVIEW
    say("FortressOS installer interface PREVIEW\nInstallation is disabled in this image.\nSynthetic target only; use example passphrases.\n\n");
#elif defined(INSTALLER_PHYSICAL)
    say("FortressOS installer — identified spare NVMe only\n");
#elif defined(INSTALLER_MULTI)
    say("FortressOS installer — choose an NVMe disk\n");
#else
    say("FortressOS text installer — disposable workbench\n");
#endif
    if (tool_syscall(SYS_TERMCTL,TERM_ISATTY,0,0)!=1) goto done;
    /* Echo non-secret input and mask passwords. Disable ISIG while input is read
     * so Ctrl-C follows the explicit wipe/release path. Restore on every exit. */
    if (terminal(TERM_GET,&saved)) goto done;
    attrs=saved;attrs.input_flags&=~TERM_ISIG;
    if (terminal(TERM_SET,&attrs)) goto done;
    changed=true;log_failed=false;failure_reported=false;log_event(INSTALL_LOG_START,0,0,0);screen(1,"Checking disks");
    for (index=0;index<=8;index++) {
        long entry=control(INSTALL_ENUM,&index,4,&identity,sizeof(identity));
        if (!entry) break;
        if (entry!=1 || index==8 || !identity.sector_size || identity.sector_count>UINT64_MAX/identity.sector_size) {
            failure_screen(1,"Available disks could not be listed safely.",entry,1,0,false);goto done;
        }
        disks[count++]=identity;
    }
    if (!count) {
#ifdef INSTALLER_PHYSICAL
        failure_screen(1,"The identified spare NVMe is not available. Use live rescue or power off.",SYSCALL_EOPNOTSUPP,1,0,false);goto done;
#else
        failure_screen(1,"No supported NVMe disk is available. Use live rescue or power off.",SYSCALL_EOPNOTSUPP,1,0,false);goto done;
#endif
    }
layout_step:;
    screen(1,"Keyboard layout");navigation();log_notice(0);say("1. US QWERTY\n2. Belgian AZERTY\n\n");
    for (;;) {
        say("Press 1 or 2, then Enter (Enter keeps the current layout): ");
        if (!line(choice,false)) {if (go_back) goto layout_step;goto done;}
        if (!choice[0]) {(void)tool_syscall(SYS_KBD_LAYOUT,layout,0,0);break;}
        if (tool_equal(choice,"1") || tool_equal(choice,"2")) {layout=(uint32_t)(choice[0]-'1');(void)tool_syscall(SYS_KBD_LAYOUT,layout,0,0);break;}
        say("Type 1 or 2.\n");
    }
disk_step:;
    screen(2,"Choose your disk");navigation();
    for (unsigned i=0;i<count;i++) {
        number(i+1);say(". ");disk_label(&disks[i]);
        if (disks[i].flags & INSTALL_ID_SOURCE) say(" (installer media - unavailable)");
        else if (disks[i].flags!=INSTALL_ID_ELIGIBLE) say(" (unavailable)");
        say("\n");
    }
    say("\nThe installer USB is excluded.\n");
    if (selected) {say("Current disk: ");number(index+1);say(". Enter keeps this choice.\n");}
    else say("No disk is preselected.\n");
    for (;;) {
        say(count==1 ? "Choose disk 1, Escape goes back: " : "Choose disk number, Escape goes back: ");
        if (!line(choice,false)) {if (go_back) goto layout_step;goto done;}
        if (!choice[0] && selected) break;
        if (choice[0]>='1' && choice[0]<='8' && !choice[1] && (unsigned)(choice[0]-'0')<=count) {
            unsigned candidate=(unsigned)(choice[0]-'1');
            if (disks[candidate].flags==INSTALL_ID_ELIGIBLE) {index=candidate;identity=disks[index];selected=true;break;}
            say("That disk is unavailable for installation. Choose another disk.\n");continue;
        }
        say("Type a disk number from the list and press Enter.\n");
    }
    screen(2,"Checking installer files");say("Selected disk ");number(index+1);say(": ");disk_label(&identity);say("\n\nChecking installer files... please wait.\n");
    long checked=control(INSTALL_DEPLOY,&request,sizeof(request),&status,sizeof(status));
    if (checked) {
        failure_screen(2,"Checking the installer files failed.",checked,status.stage,status.error,false);goto done;
    }
#ifdef INSTALLER_PREVIEW
    say("Preview layout selected. Validation/install actions are disabled.\n");
#else
    say("Installer files checked. Your disk has not been changed.\n");
#endif
account_step:;
    screen(3,"Create your account");navigation();
    for (;;) {
        say("Computer name [");say(named_request.hostname[0] ? named_request.hostname : "fortress");say("]: ");
        if (!line(choice,false)) {if (go_back) goto disk_step;goto done;}
        if (!choice[0]) {
            if (!named_request.hostname[0]) {const char *name="fortress";for (unsigned i=0;name[i];i++) named_request.hostname[i]=name[i];}
            break;
        }
        if (tool_length(choice)<64 && install_hostname_valid(choice)) {
            db_wipe(named_request.hostname,sizeof(named_request.hostname));
            for (unsigned i=0;choice[i];i++) named_request.hostname[i]=choice[i];
            break;
        }
        say("Use 1-63 letters, digits or hyphens. Start and end with a letter or digit.\n");
    }
    for (;;) {
        if (named_request.username[0]) {say("Current username: ");say(named_request.username);say(" (Enter keeps it)\n");}
        say("Choose username: ");if (!line(choice,false)) {if (go_back) goto disk_step;goto done;}
        if (!choice[0] && named_request.username[0]) break;
        if (tool_length(choice)<32 && install_username_valid(choice)) break;
        say("Use 1-31 lowercase letters, digits, _ or -. Start with a letter or _. Root and wheel are reserved.\n");
    }
    if (choice[0]) {db_wipe(named_request.username,sizeof(named_request.username));for (unsigned i=0;choice[i];i++) named_request.username[i]=choice[i];}
    if (request.operator_hash[0]) {
        say("Login password saved. Enter keeps it; CHANGE chooses a new one: ");
        if (!line(choice,false)) {if (go_back) goto disk_step;goto done;}
        if (choice[0] && !tool_equal(choice,"CHANGE") && !tool_equal(choice,"change")) {say("Type CHANGE or press Enter.\n");goto account_step;}
    } else choice[0]='x';
    if (choice[0]) {
        db_wipe(request.operator_hash,sizeof(request.operator_hash));
        db_wipe(request.recovery_hash,sizeof(request.recovery_hash));
        if (!password("Choose login password: ",request.operator_hash,NULL)) {if (go_back) goto disk_step;goto done;}
    }
recovery_step:;
    screen(4,"Recovery password");navigation();
    if (request.recovery_hash[0]) {
        say("Recovery password saved. Enter keeps it; CHANGE chooses a new one: ");
        if (!line(choice,false)) {if (go_back) goto account_step;goto done;}
        if (choice[0] && !tool_equal(choice,"CHANGE") && !tool_equal(choice,"change")) {say("Type CHANGE or press Enter.\n");goto recovery_step;}
    } else choice[0]='x';
    if (choice[0]) {
        db_wipe(request.recovery_hash,sizeof(request.recovery_hash));
        if (!password("Choose recovery password: ",request.recovery_hash,request.operator_hash)) {if (go_back) goto account_step;goto done;}
    }
    /* Reminder is shown on the confirmation screen below. */
    if (tool_syscall(SYS_INSTALLSEED,(uintptr_t)request.disk_guid,80,0)) goto done;
    for (unsigned i=0;i<4;i++) {uint8_t *g=i ? request.partition_guid[i-1] : request.disk_guid;g[7]=(g[7]&15)|64;g[8]=(g[8]&63)|128;}
    screen(5,"Confirm installation");navigation();say("Disk ");number(index+1);say(": ");disk_label(&identity);
    say("\nUsername: ");say(named_request.username);say("\nComputer name: ");say(named_request.hostname);say("\nRemember/store the separate recovery password; losing it prevents authenticated recovery.");
    say("\n\n");contents_warning(&identity);say("\nFortressOS uses a 64 MiB boot partition.\nThe root filesystem uses the remaining disk space.\n\n");
    for (;;) {
        say("Installing will erase this disk.\nType ");number(index+1);say(" to confirm, or NO to cancel: ");
        if (!line(choice,false)) {if (go_back) goto recovery_step;goto done;}
        if (tool_equal(choice,"NO") || tool_equal(choice,"no")) goto done;
        if (choice[0]==(char)('1'+index) && !choice[1]) break;
        say("Not confirmed. Your answers are saved and the disk is unchanged.\n");
    }
#ifdef INSTALLER_PREVIEW
    say("Preview complete. Installation remains disabled.\n");result=0;goto done;
#endif
    install_target_v1_t target={.size=sizeof(target),.version=1,.index=index,.generation=identity.generation,.flags=INSTALL_CONFIRM_WHOLE_DISK};
    for (unsigned i=0;i<32;i++) target.digest[i]=identity.digest[i];
    long acquired=control(INSTALL_ACQUIRE,&target,sizeof(target),&fd,sizeof(fd));
    if (acquired) {failure_screen(5,"The selected disk could not be opened safely.",acquired,1,0,false);goto done;}
    request.fd=fd;request.flags=0;
    request.size=sizeof(named_request);request.version=3;named_request.base=request;
    screen(5,"Installing FortressOS");say("Installing; keep power and installer media connected.\n\n");
    long deployed=control(INSTALL_DEPLOY,&named_request,sizeof(named_request),&status,sizeof(status));
    if (deployed || !status.complete || status.error) {
        /* A zero call with incomplete status is still failure, never success. */
        failure_screen(5,"Installing FortressOS failed.",deployed,status.stage,status.error,true);
        goto done;
    }
    screen(5,"Installation result");say("Installation complete.\nLaptop log: /var/log/fortress-install.log\n");log_notice(status.reserved);say("\nRemove the USB installer media, then press Enter to restart.\n");result=0;
    /* Stay here silently: exiting would make the kernel restart the installer. */
    for (;;) {
        unsigned char key=0;
        if (tool_syscall(SYS_INPUT_READ,(uintptr_t)&key,1,(uintptr_t)-1)!=1 || (key!='\n' && key!='\r')) continue;
        if (fd>=0) {(void)control(INSTALL_RELEASE,&fd,4,NULL,0);fd=-1;}
        db_wipe(choice,sizeof(choice));db_wipe(&request,sizeof(request));db_wipe(&named_request,sizeof(named_request));
        if (changed) {(void)terminal(TERM_SET,&saved);changed=false;}
        (void)tool_syscall(SYS_REBOOT,1,0,0);
    }
done:
    if (changed && result && !failure_reported) log_event(INSTALL_LOG_CANCELLED,screen_step ? screen_step-1 : 0,0,0);
    if (fd>=0) (void)control(INSTALL_RELEASE,&fd,4,NULL,0);
    db_wipe(choice,sizeof(choice));db_wipe(&request,sizeof(request));db_wipe(&named_request,sizeof(named_request));
    if (changed) (void)terminal(TERM_SET,&saved);
    if (result) say("Installer stopped; no success reported.\n");
    return result;
}
