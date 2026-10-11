#include "account_tool.h"
static bool append_text(char *out,size_t cap,size_t *n,const char *text) {
    while (*text) {if (*n+1>=cap) return false;out[(*n)++]=*text++;}out[*n]=0;return true;
}
int useradd_main(int argc,char **argv) {
    uint32_t uid,euid,suid,id=0;char hash[78]={0};int result=1;
    if (argc!=3 || tool_syscall(SYS_GETRESUID,(uintptr_t)&uid,(uintptr_t)&euid,(uintptr_t)&suid) || uid || euid || suid) goto done;
    size_t name=tool_length(argv[1]);if (!name || name>=DB_NAME_MAX) goto done;
    for (size_t i=0;i<name;i++) if (!((argv[1][i]>='a' && argv[1][i]<='z') || (i && argv[1][i]>='0' && argv[1][i]<='9') || argv[1][i]=='_')) goto done;
    if (!*argv[2]) goto done;
    for (const char *p=argv[2];*p;p++) {if (*p<'0' || *p>'9' || id>(UINT32_MAX-(unsigned)(*p-'0'))/10) goto done;id=id*10+(*p-'0');}
    if (id<1000 || id==UINT32_MAX || !account_snapshot() || db_user_name(&account_db,argv[1]) || db_user_id(&account_db,id) || db_group_id(&account_db,id) ||
        account_db.nusers==DB_MAX_RECORDS || account_db.ngroups==DB_MAX_RECORDS || !account_password(hash) || !account_begin()) goto done;
    char home[128]="/home/",number[12],line[512];size_t n=6;
    if (!append_text(home,sizeof(home),&n,argv[1])) goto done;
    tool_format_u64(number,id);
    if (tool_syscall(SYS_MKDIR,(uintptr_t)home,0700,0) || permission_call4(SYS_CHOWN,(uintptr_t)home,id,id,0)) goto done;
    n=0;
    if (!append_text(line,sizeof(line),&n,argv[1]) || !append_text(line,sizeof(line),&n,":x:") ||
        !append_text(line,sizeof(line),&n,number) || !append_text(line,sizeof(line),&n,":") || !append_text(line,sizeof(line),&n,number) ||
        !append_text(line,sizeof(line),&n,"::") || !append_text(line,sizeof(line),&n,home) || !append_text(line,sizeof(line),&n,":/bin/shell\n") || !account_append(0,line)) goto done;
    n=0;if (!append_text(line,sizeof(line),&n,argv[1]) || !append_text(line,sizeof(line),&n,":x:") || !append_text(line,sizeof(line),&n,number) ||
        !append_text(line,sizeof(line),&n,":\n") || !account_append(1,line)) goto done;
    n=0;if (!append_text(line,sizeof(line),&n,argv[1]) || !append_text(line,sizeof(line),&n,":") || !append_text(line,sizeof(line),&n,hash) ||
        !append_text(line,sizeof(line),&n,":::::::\n") || !account_append(2,line) || !account_commit()) goto done;
    tool_write("useradd","Account created\n",16);result=0;
done:
    db_wipe(hash,sizeof(hash));account_clear();
    return result ? tool_error("useradd","usage: useradd NAME UID; persistent privileged installation and confirmed password required",0) : 0;
}
