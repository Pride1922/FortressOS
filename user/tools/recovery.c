#include "common.h"
#include "userdb.h"
#include "terminal.h"
static void say(const char *s) {tool_write("recovery",s,tool_length(s));}
static void delay(void) {
    sysinfo_t info;
    if (tool_syscall(SYS_SYSINFO,(uintptr_t)&info,0,0)<0) return;
    uint64_t start=info.uptime_ticks,ticks=2ull*info.tick_hz;
    do {
        unsigned char discard[64];
        tool_syscall(SYS_INPUT_READ,(uintptr_t)discard,sizeof(discard),1000);
        if (tool_syscall(SYS_SYSINFO,(uintptr_t)&info,0,0)<0) break;
    } while (info.uptime_ticks-start<ticks);
}
/* This bootstrap program authenticates only. Kernel supervisor selects its
 * immutable image and validated hash, then launches bootstrap shell on success. */
int recovery_main(int argc,char **argv) {
    uint32_t uid,euid,suid;
    if (argc!=2 || !db_hash_valid(argv[1]) ||
        tool_syscall(SYS_GETRESUID,(uintptr_t)&uid,(uintptr_t)&euid,(uintptr_t)&suid)<0 || uid || euid || suid ||
        tool_syscall(SYS_TERMCTL,TERM_ISATTY,0,0)!=1) return 1;
    say("FortressOS authenticated recovery (bootstrap)\n");
    for (;;) {
        char password[DB_PASSWORD_MAX+1]={0};size_t n=0;bool invalid=false;
        say("Recovery passphrase: ");
        for (;;) {
            unsigned char c;
            if (tool_syscall(SYS_INPUT_READ,(uintptr_t)&c,1,(uintptr_t)-1)<=0) {db_wipe(password,sizeof(password));return 1;}
            if (c=='\r' || c=='\n') break;
            if (c==8 || c==127) {if (n) password[--n]=0;continue;}
            if (c<32 || c>126 || n==DB_PASSWORD_MAX) {invalid=true;continue;}
            password[n++]=(char)c;
        }
        say("\n");bool ok=!invalid && n>=12 && db_verify(password,argv[1]);
        db_wipe(password,sizeof(password));
        if (ok) {say("Recovery authenticated\n");return 0;}
        delay();say("Recovery denied\n");
    }
}
