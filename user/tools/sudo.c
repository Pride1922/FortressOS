#include "common.h"
#include "userdb.h"
#include "terminal.h"
#include "../permissions_cli.h"
#include "../entry_security.h"
static userdb_t database;
static bool wheel(const db_user_t *u) {
    uint32_t groups[16];unsigned n;
    const db_group_t *w=NULL;
    for (unsigned i=0;i<database.ngroups;i++)
        if (tool_equal(database.groups[i].name,"wheel")) w=&database.groups[i];
    if (!w || !db_groups(&database,u,groups,&n)) return false;
    for (unsigned i=0;i<n;i++) if (groups[i]==w->gid) return true;
    return false;
}
static bool password_line(int tty,char out[DB_PASSWORD_MAX+1]) {
    unsigned n=0;bool bad=false;
    for (;;) {
        unsigned char c;long r=tool_syscall(SYS_READ,tty,(uintptr_t)&c,1);
        if (r!=1) {db_wipe(out,DB_PASSWORD_MAX+1);return false;}
        if (c=='\n' || c=='\r') {out[n]=0;return !bad;}
        if (c==8 || c==127) {if (n) out[--n]=0;continue;}
        if (c<32 || c>126 || n==DB_PASSWORD_MAX) {bad=true;continue;}
        out[n++]=(char)c;
    }
}
int sudo_main(int argc,char **argv,const char *const *envp) {
    uint32_t uid,euid,suid;
    if (argc<2) return tool_error("sudo","usage: sudo command [args]",0);
    if (tool_syscall(SYS_GETRESUID,(uintptr_t)&uid,(uintptr_t)&euid,(uintptr_t)&suid)<0 ||
        euid!=0 || (uid && !user_entry_secure(envp)) || !db_load(&database,true))
        return tool_error("sudo","privileged installation required",0);
    const db_user_t *u=db_user_id(&database,uid),*root=db_user_id(&database,0);
    const db_shadow_t *shadow=u ? db_shadow_name(&database,u->name) : NULL;
    if (!u || !root || (uid && (!wheel(u) || !shadow || shadow->restricted)))
        return tool_error("sudo","user is not authorized",0);
    if (uid) {
        int tty=tool_syscall(SYS_OPEN,(uintptr_t)"/dev/tty",VFS_O_RDWR,0);
        if (tty<0 || tool_syscall(SYS_TERMCTL,TERM_ISATTY,tty,0)!=1) {
            if (tty>=0) tool_syscall(SYS_CLOSE,tty,0,0);
            return tool_error("sudo","controlling terminal required",0);
        }
        char password[DB_PASSWORD_MAX+1]={0};bool ok;
        if (!*shadow->hash) {
            const char *warning="WARNING: live-media sudo authentication is passwordless.\n";
            tool_syscall(SYS_WRITE,tty,(uintptr_t)warning,tool_length(warning));ok=true;
        } else {
            const char *prompt="sudo password: ";tool_syscall(SYS_WRITE,tty,(uintptr_t)prompt,tool_length(prompt));
            ok=password_line(tty,password) && db_verify(password,shadow->hash);
            tool_syscall(SYS_WRITE,tty,(uintptr_t)"\n",1);
        }
        db_wipe(password,sizeof(password));tool_syscall(SYS_CLOSE,tty,0,0);
        if (!ok) {db_wipe(&database,sizeof(database));return tool_error("sudo","authentication failed",0);}
    }
    uint32_t groups[16];unsigned count;
    if (!db_groups(&database,root,groups,&count) ||
        permission_call4(SYS_SETRESGID,0,0,0,0)<0 ||
        tool_syscall(SYS_SETGROUPS,count,(uintptr_t)groups,0)<0 ||
        permission_call4(SYS_SETRESUID,0,0,0,0)<0)
        return tool_error("sudo","credential transition failed",0);
    char command[VFS_MAX_PATH];size_t n=tool_length(argv[1]);
    if (!n || n+6>sizeof(command)) return tool_error("sudo","invalid command",0);
    unsigned off=0;
    if (argv[1][0]!='/') {
        for (size_t i=0;i<n;i++) if (argv[1][i]=='/') return tool_error("sudo","absolute command path required",0);
        const char *prefix="/bin/";while (*prefix) command[off++]=*prefix++;
    }
    for (size_t i=0;i<=n;i++) command[off+i]=argv[1][i];
    char term[64]="TERM=fortress";
    if (envp) for (unsigned i=0;i<MAX_SPAWN_ENVP && envp[i];i++) {
        const char *p=envp[i];
        if (p[0]!='T' || p[1]!='E' || p[2]!='R' || p[3]!='M' || p[4]!='=') continue;
        size_t len=tool_length(p);bool safe=len>5 && len<sizeof(term);
        for (size_t j=5;j<len;j++) if (p[j]<32 || p[j]>126) safe=false;
        if (safe) for (size_t j=0;j<=len;j++) term[j]=p[j];
    }
    const char *environment[]={"PATH=/bin","HOME=/root","USER=root","LOGNAME=root","SHELL=/bin/shell",term,NULL};
    spawn_opts_t opts={.size=sizeof(opts),.version=1,.argv=(uintptr_t)(argv+1),.envp=(uintptr_t)environment};
    db_wipe(&database,sizeof(database));
    long pid=tool_syscall(SYS_SPAWN_EXT,(uintptr_t)command,(uintptr_t)&opts,sizeof(opts));
    if (pid<0) return tool_sys_error("sudo","cannot start command",argv[1],pid);
    uint64_t status=0;
    return tool_syscall(SYS_WAIT,pid,(uintptr_t)&status,0)<0 ? 1 : (int)status;
}
