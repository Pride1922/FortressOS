#ifndef FORTRESS_ACCOUNT_TOOL_H
#define FORTRESS_ACCOUNT_TOOL_H
#include "common.h"
#include "userdb.h"
#include "account_abi.h"
#include "terminal.h"
#include "../permissions_cli.h"
static char account_files[3][DB_FILE_MAX];
static userdb_t account_db;
static account_request_t account_request;
static bool account_snapshot(void) {
    account_request=(account_request_t){.size=sizeof(account_request),.version=1,
        .passwd=(uintptr_t)account_files[0],.group=(uintptr_t)account_files[1],.shadow=(uintptr_t)account_files[2],
        .passwd_bytes=DB_FILE_MAX,.group_bytes=DB_FILE_MAX,.shadow_bytes=DB_FILE_MAX,.flags=ACCOUNT_WITH_SHADOW};
    if (tool_syscall(SYS_ACCOUNTCTL,ACCOUNT_SNAPSHOT,(uintptr_t)&account_request,sizeof(account_request))) return false;
    account_db=(userdb_t){0};
    return db_passwd(&account_db,account_files[0],account_request.passwd_bytes) &&
        db_group(&account_db,account_files[1],account_request.group_bytes) &&
        db_shadow(&account_db,account_files[2],account_request.shadow_bytes) && db_validate(&account_db);
}
static bool account_line(char out[129]) {
    size_t n=0;bool bad=false;
    for (;;) {
        unsigned char c;long r=tool_syscall(SYS_INPUT_READ,(uintptr_t)&c,1,(uintptr_t)-1);
        if (r!=1 || c==3 || c==27) {db_wipe(out,129);return false;}
        if (c=='\n' || c=='\r') {out[n]=0;tool_write("accounts","\n",1);return !bad;}
        if (c==8 || c==127) {if (n) out[--n]=0;continue;}
        if (c<32 || c>126 || n==128) {bad=true;continue;}
        out[n++]=(char)c;
    }
}
static bool account_password(char hash[78]) {
    char first[129]={0},second[129]={0},salt[17]={0};uint8_t seed[16]={0};bool ok=false;
    if (tool_syscall(SYS_TERMCTL,TERM_ISATTY,0,0)!=1) goto done;
    const char prompt[]="New password (8..128 characters): ";
    tool_write("accounts",prompt,sizeof(prompt)-1);
    if (!account_line(first) || tool_length(first)<8) goto done;
    tool_write("accounts","Confirm new password: ",22);
    if (!account_line(second) || !tool_equal(first,second)) goto done;
    if (tool_syscall(SYS_INSTALLSEED,(uintptr_t)seed,sizeof(seed),0)) goto done;
    const char alphabet[]="./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    for (unsigned i=0;i<16;i++) salt[i]=alphabet[seed[i]&63];
    ok=db_crypt(first,salt,hash);
done:
    db_wipe(first,sizeof(first));db_wipe(second,sizeof(second));db_wipe(salt,sizeof(salt));db_wipe(seed,sizeof(seed));return ok;
}
static bool account_begin(void) {
    account_request_t lease={.size=sizeof(lease),.version=1,.generation=account_request.generation};
    if (tool_syscall(SYS_ACCOUNTCTL,ACCOUNT_BEGIN,(uintptr_t)&lease,sizeof(lease))) return false;
    account_request.token=lease.token;account_request.flags=0;return true;
}
static bool account_commit(void) {
    if (tool_syscall(SYS_ACCOUNTCTL,ACCOUNT_UPDATE,(uintptr_t)&account_request,sizeof(account_request))) return false;
    account_request_t finish={.size=sizeof(finish),.version=1,.generation=account_request.generation,.token=account_request.token};
    return !tool_syscall(SYS_ACCOUNTCTL,ACCOUNT_FINISH,(uintptr_t)&finish,sizeof(finish));
}
static void account_clear(void) {
    db_wipe(account_files,sizeof(account_files));db_wipe(&account_db,sizeof(account_db));db_wipe(&account_request,sizeof(account_request));
}
static inline bool account_append(unsigned i,const char *text) {
    uint32_t *size=i==0 ? &account_request.passwd_bytes : i==1 ? &account_request.group_bytes : &account_request.shadow_bytes;
    size_t n=tool_length(text);if (n>DB_FILE_MAX-*size) return false;
    if (*size && account_files[i][*size-1]!='\n') {
        if (*size==DB_FILE_MAX || n==DB_FILE_MAX-*size) return false;
        account_files[i][(*size)++]='\n';
    }
    for (size_t j=0;j<n;j++) account_files[i][(*size)++]=text[j];
    return true;
}
#endif
