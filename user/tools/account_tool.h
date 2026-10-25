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
static inline bool account_snapshot(void) {
    account_request=(account_request_t){.size=sizeof(account_request),.version=1,
        .passwd=(uintptr_t)account_files[0],.group=(uintptr_t)account_files[1],.shadow=(uintptr_t)account_files[2],
        .passwd_bytes=DB_FILE_MAX,.group_bytes=DB_FILE_MAX,.shadow_bytes=DB_FILE_MAX,.flags=ACCOUNT_WITH_SHADOW};
    if (tool_syscall(SYS_ACCOUNTCTL,ACCOUNT_SNAPSHOT,(uintptr_t)&account_request,sizeof(account_request))) return false;
    account_db=(userdb_t){0};
    return db_passwd(&account_db,account_files[0],account_request.passwd_bytes) &&
        db_group(&account_db,account_files[1],account_request.group_bytes) &&
        db_shadow(&account_db,account_files[2],account_request.shadow_bytes) && db_validate(&account_db);
}
static inline bool account_line(char out[129]) {
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
static inline bool account_password(char hash[78]) {
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
static inline bool account_begin(void) {
    account_request_t lease={.size=sizeof(lease),.version=1,.generation=account_request.generation};
    if (tool_syscall(SYS_ACCOUNTCTL,ACCOUNT_BEGIN,(uintptr_t)&lease,sizeof(lease))) return false;
    account_request.token=lease.token;account_request.flags=0;return true;
}
static inline bool account_commit(void) {
    if (tool_syscall(SYS_ACCOUNTCTL,ACCOUNT_UPDATE,(uintptr_t)&account_request,sizeof(account_request))) return false;
    account_request_t finish={.size=sizeof(finish),.version=1,.generation=account_request.generation,.token=account_request.token};
    return !tool_syscall(SYS_ACCOUNTCTL,ACCOUNT_FINISH,(uintptr_t)&finish,sizeof(finish));
}
static inline void account_clear(void) {
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
static inline bool account_remove_entry(unsigned i,const char *name) {
    if (i>2 || !name || !*name) return false;
    uint32_t *size=i==0 ? &account_request.passwd_bytes : i==1 ? &account_request.group_bytes : &account_request.shadow_bytes;
    char temp[DB_FILE_MAX];size_t out=0,in=0,name_len=tool_length(name);
    bool found=false;
    while (in<*size) {
        size_t start=in;
        while (in<*size && account_files[i][in]!='\n') in++;
        size_t line_len=in-start;
        if (in<*size && account_files[i][in]=='\n') in++;
        bool match=(line_len>name_len && account_files[i][start+name_len]==':');
        if (match) {
            for (size_t k=0;k<name_len;k++) if (account_files[i][start+k]!=name[k]) {match=false;break;}
        }
        if (match) {
            found=true;
        } else {
            if (out+line_len+1>DB_FILE_MAX) return false;
            for (size_t k=start;k<in;k++) temp[out++]=account_files[i][k];
            if (out && temp[out-1]!='\n') temp[out++]='\n';
        }
    }
    if (!found) return false;
    for (size_t k=0;k<out;k++) account_files[i][k]=temp[k];
    *size=(uint32_t)out;
    return true;
}
static inline bool account_replace_entry(unsigned i,const char *name,const char *new_line) {
    if (i>2 || !name || !*name || !new_line) return false;
    uint32_t *size=i==0 ? &account_request.passwd_bytes : i==1 ? &account_request.group_bytes : &account_request.shadow_bytes;
    char temp[DB_FILE_MAX];size_t out=0,in=0,name_len=tool_length(name);
    size_t new_len=tool_length(new_line);
    bool found=false;
    while (in<*size) {
        size_t start=in;
        while (in<*size && account_files[i][in]!='\n') in++;
        size_t line_len=in-start;
        if (in<*size && account_files[i][in]=='\n') in++;
        bool match=(line_len>name_len && account_files[i][start+name_len]==':');
        if (match) {
            for (size_t k=0;k<name_len;k++) if (account_files[i][start+k]!=name[k]) {match=false;break;}
        }
        if (match) {
            found=true;
            if (out+new_len+1>DB_FILE_MAX) return false;
            for (size_t k=0;k<new_len;k++) temp[out++]=new_line[k];
            if (out && temp[out-1]!='\n') temp[out++]='\n';
        } else {
            if (out+line_len+1>DB_FILE_MAX) return false;
            for (size_t k=start;k<in;k++) temp[out++]=account_files[i][k];
            if (out && temp[out-1]!='\n') temp[out++]='\n';
        }
    }
    if (!found) return false;
    for (size_t k=0;k<out;k++) account_files[i][k]=temp[k];
    *size=(uint32_t)out;
    return true;
}
static inline uint32_t account_next_uid(void) {
    uint32_t candidate=1000;
    for (;;) {
        bool used=false;
        for (unsigned i=0;i<account_db.nusers;i++) {
            if (account_db.users[i].uid==candidate) {used=true;break;}
        }
        if (!used) return candidate;
        candidate++;
        if (candidate==UINT32_MAX) return 0;
    }
}
static inline uint32_t account_next_gid(void) {
    uint32_t candidate=1000;
    for (;;) {
        bool used=false;
        for (unsigned i=0;i<account_db.ngroups;i++) {
            if (account_db.groups[i].gid==candidate) {used=true;break;}
        }
        if (!used) return candidate;
        candidate++;
        if (candidate==UINT32_MAX) return 0;
    }
}
#endif
