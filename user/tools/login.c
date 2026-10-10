#include "common.h"
#include "userdb.h"
#include "terminal.h"
#include "../permissions_cli.h"
static userdb_t database;
static int say(const char *s) {return tool_write("login",s,tool_length(s));}
/* Input is already raw and never kernel-echoed. Only username bytes are
 * echoed here; overflow drains to newline and rejects the whole attempt. */
static bool line(char *out,size_t cap,bool echo) {
    size_t n=0;bool overflow=false;
    for (;;) {unsigned char c;long r=tool_syscall(SYS_INPUT_READ,(uintptr_t)&c,1,(uintptr_t)-1);
        if (r<=0) {db_wipe(out,cap);return false;}
        if (c=='\n' || c=='\r') {out[n]=0;say("\n");return !overflow;}
        if (c==8 || c==127) {if (n) {out[--n]=0;if (echo) say("\b \b");}continue;}
        if (c<32 || c>126) {overflow=true;continue;}
        if (n+1>=cap) {overflow=true;continue;}out[n++]=(char)c;
        if (echo) tool_write("login",&c,1);
    }
}
static void delay(void) {
    /* The input wait is predicate-based and incoming bytes can wake it early.
     * Measure BSP time rather than counting calls, so input cannot skip delay. */
    sysinfo_t info;if (tool_syscall(SYS_SYSINFO,(uintptr_t)&info,0,0)<0) return;
    uint64_t start=info.uptime_ticks;uint64_t ticks=2ull*info.tick_hz;
    do {unsigned char discard[64];tool_syscall(SYS_INPUT_READ,(uintptr_t)discard,sizeof(discard),1000);
        if (tool_syscall(SYS_SYSINFO,(uintptr_t)&info,0,0)<0) break;
    } while (info.uptime_ticks-start<ticks);
}
static bool runtime(const db_user_t *u,char path[128]) {
    char id[12];tool_format_u64(id,u->uid);size_t n=0;
    const char *prefix="/run/user/";while (*prefix) path[n++]=*prefix++;
    for (size_t i=0;id[i];i++) path[n++]=id[i];
    path[n]=0;
    if (!tool_equal(u->home,path)) return false;
    stat_ext_v1_t st;long r=permission_call4(SYS_STAT_EXT,(uintptr_t)path,(uintptr_t)&st,sizeof(st),1);
    if (r==SYSCALL_ENOENT) {
        if (tool_syscall(SYS_MKDIR,(uintptr_t)path,0700,0)<0 ||
            permission_call4(SYS_CHOWN,(uintptr_t)path,u->uid,u->gid,0)<0 ||
            tool_syscall(SYS_CHMOD,(uintptr_t)path,0700,0)<0) return false;
        r=permission_call4(SYS_STAT_EXT,(uintptr_t)path,(uintptr_t)&st,sizeof(st),1);
    }
    return r==0 && st.type==VFS_DIRECTORY && st.mode==(VFS_S_IFDIR|0700) && st.uid==u->uid && st.gid==u->gid && !st.mnt_flags;
}
static void env(char *out,const char *key,const char *value) {while (*key) *out++=*key++;while (*value) *out++=*value++;*out=0;}
int login_main(int argc,char **argv) {
    (void)argc;(void)argv;
    uint32_t uid,euid,suid;
    if (tool_syscall(SYS_GETRESUID,(uintptr_t)&uid,(uintptr_t)&euid,(uintptr_t)&suid)<0 || uid || euid || suid ||
        tool_syscall(SYS_TERMCTL,TERM_ISATTY,0,0)!=1 || !db_load(&database,true)) return tool_error("login","invalid privileged terminal/database",0);
    const db_shadow_t *op=db_shadow_name(&database,"operator");
    if (op && !*op->hash) say("WARNING: live-media operator login is passwordless; home is temporary.\n");
    for (;;) {
        char username[32]={0},password[DB_PASSWORD_MAX+1]={0};
        say("FortressOS login: ");bool valid=line(username,sizeof(username),true);
        const db_user_t *u=valid ? db_user_name(&database,username) : 0;
        const db_shadow_t *s=valid ? db_shadow_name(&database,username) : 0;
        /* Never let configured UID zero log in on this live-media policy. */
        bool empty=u && s && !*s->hash && u->uid!=0 && !s->restricted;
        if (!empty) {say("Password: ");valid=line(password,sizeof(password),false) && valid;}
        bool ok=valid && u && s && u->uid!=0 && !s->restricted && db_verify(password,s->hash);
        db_wipe(password,sizeof(password));
        if (!ok) {delay();say("Login incorrect\n");continue;}
        uint32_t groups[16];unsigned count;char home[128];
        if (!db_groups(&database,u,groups,&count) || !runtime(u,home)) return tool_error("login","unsafe runtime directory",0);
        /* Input never echoes and normal ISIG remains intact. Killing login
         * causes the kernel to restart a fresh privileged login process. */
        if (tool_syscall(SYS_SETGROUPS,count,(uintptr_t)groups,0)<0 ||
            permission_call4(SYS_SETRESGID,u->gid,u->gid,u->gid,0)<0 ||
            permission_call4(SYS_SETRESUID,u->uid,u->uid,u->uid,0)<0 ||
            tool_syscall(SYS_CAPGET,0,0,0)!=0) return tool_error("login","credential drop failed",0);
        /* Login waits in the shell's foreground group. Prompt control signals
         * must not kill/stop the session parent; the shell sets its own prompt
         * dispositions, and its job launcher restores command dispositions. */
        signal_action_t ignore={.handler=SIG_IGN};
        if (tool_syscall(SYS_SIGACTION,SIGINT,(uintptr_t)&ignore,0)<0 ||
            tool_syscall(SYS_SIGACTION,SIGTSTP,(uintptr_t)&ignore,0)<0)
            return tool_error("login","cannot protect session parent",0);
        char h[144],user[48],logname[48],shell[144];
        env(h,"HOME=",home);env(user,"USER=",u->name);env(logname,"LOGNAME=",u->name);env(shell,"SHELL=",u->shell);
        const char *environment[]={h,user,logname,shell,"PATH=/bin","TERM=fortress",0};
        const char *args[]={u->shell,0};spawn_opts_t opts={.size=sizeof(opts),.version=1,
            .argv=(uintptr_t)args,.envp=(uintptr_t)environment,.cwd=(uintptr_t)home};
        /* Group is inherited from the boot login and remains foreground. */
        long pid=tool_syscall(SYS_SPAWN_EXT,(uintptr_t)u->shell,(uintptr_t)&opts,sizeof(opts));
        db_wipe(&database,sizeof(database));
        if (pid<0) return tool_error("login","cannot start shell",0);
        uint64_t status=0;long r=tool_syscall(SYS_WAIT,pid,(uintptr_t)&status,0);
        return r<0 ? 1 : (int)status;
    }
}
