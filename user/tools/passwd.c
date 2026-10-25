#include "account_tool.h"
#include "../entry_security.h"
#include "live_password.h"
int passwd_main(int argc,char **argv,const char *const *envp) {
    uint32_t uid,euid,suid;int result=1;char hash[78]={0};static char replacement[DB_FILE_MAX];
    if (argc==3 && tool_equal(argv[1],"--live") && (tool_equal(argv[2],"fortress") || tool_equal(argv[2],"operator"))) {
        account_request_t req={.size=sizeof(req),.version=1};
        if (tool_syscall(SYS_GETRESUID,(uintptr_t)&uid,(uintptr_t)&euid,(uintptr_t)&suid) || euid || (uid && uid!=1000 && uid!=1001) ||
            (uid && !user_entry_secure(envp)) ||
            tool_syscall(SYS_ACCOUNTCTL,ACCOUNT_STATUS,(uintptr_t)&req,sizeof(req))!=SYSCALL_EOPNOTSUPP ||
            !db_load(&account_db,true) || !db_user_name(&account_db,argv[2])) goto done;
        terminal_attrs_t saved,hidden;
        if (permission_call4(SYS_TERMATTR,0,TERM_GET,(uintptr_t)&saved,sizeof(saved))) goto done;
        hidden=saved;hidden.input_flags &= ~TERM_ECHO;
        if (permission_call4(SYS_TERMATTR,0,TERM_SET,(uintptr_t)&hidden,sizeof(hidden))) goto done;
        bool ok=account_password(hash);
        if (permission_call4(SYS_TERMATTR,0,TERM_SET,(uintptr_t)&saved,sizeof(saved))) ok=false;
        if (!ok || !live_password_publish(hash)) goto done;
        const char message[]="Temporary password set; discarded on reboot\n";
        tool_write("passwd",message,sizeof(message)-1);result=0;goto done;
    }
    if (argc>2 || tool_syscall(SYS_GETRESUID,(uintptr_t)&uid,(uintptr_t)&euid,(uintptr_t)&suid) ||
        euid || (uid && !user_entry_secure(envp)) || !account_snapshot()) goto done;
    if (argc==2 && tool_equal(argv[1],"root")) {
        return tool_error("passwd","cannot modify password for root",0);
    }
    const db_user_t *u=argc==2 ? db_user_name(&account_db,argv[1]) : db_user_id(&account_db,uid);
    if (!u || !u->uid || (uid && uid!=u->uid)) goto done;
    if (uid) {
        char old[129]={0};const db_shadow_t *s=db_shadow_name(&account_db,u->name);
        tool_write("passwd","Current password: ",18);
        bool ok=account_line(old) && s && !s->restricted && db_verify(old,s->hash);db_wipe(old,sizeof(old));
        if (!ok) goto done;
    }
    if (!account_password(hash)) goto done;
    size_t in=0,out=0,name=tool_length(u->name);bool changed=false;
    while (in<account_request.shadow_bytes) {
        size_t start=in;while (in<account_request.shadow_bytes && account_files[2][in]!='\n') in++;
        size_t end=in;if (in<account_request.shadow_bytes) in++;
        bool match=end-start>name && account_files[2][start+name]==':';
        for (size_t j=0;j<name && match;j++) match=account_files[2][start+j]==u->name[j];
        if (match) {
            size_t rest=start+name+1;while (rest<end && account_files[2][rest]!=':') rest++;
            size_t need=name+1+tool_length(hash)+(end-rest)+1;if (need>DB_FILE_MAX-out) goto done;
            for (size_t j=start;j<start+name+1;j++) replacement[out++]=account_files[2][j];
            for (size_t j=0;hash[j];j++) replacement[out++]=hash[j];
            for (size_t j=rest;j<end;j++) replacement[out++]=account_files[2][j];
            replacement[out++]='\n';changed=true;
        } else {
            if (in-start>DB_FILE_MAX-out) goto done;
            for (size_t j=start;j<in;j++) replacement[out++]=account_files[2][j];
        }
    }
    if (!changed || !account_begin()) goto done;
    for (size_t j=0;j<out;j++) account_files[2][j]=replacement[j];
    account_request.shadow_bytes=out;
    if (!account_commit()) goto done;
    tool_write("passwd","Password updated\n",17);result=0;
done:
    db_wipe(hash,sizeof(hash));db_wipe(replacement,sizeof(replacement));account_clear();
    return result ? tool_error("passwd","update failed; no success reported",0) : 0;
}
