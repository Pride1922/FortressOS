#ifndef FORTRESS_LIVE_PASSWORD_H
#define FORTRESS_LIVE_PASSWORD_H
#include "common.h"
#include "userdb.h"
#include "account_abi.h"
#include "creds.h"
#define LIVE_PASSWORD_DIR "/run/live-password"
#define LIVE_PASSWORD_FILE LIVE_PASSWORD_DIR "/operator"
#define LIVE_PASSWORD_STAGE LIVE_PASSWORD_DIR "/.pending"
/* Single explicit setup per boot. mkdir owns publication; incomplete setup
 * fails closed until reboot, without replacing any existing credential. */
static inline bool live_password_metadata(long fd,unsigned type,unsigned mode) {
    stat_ext_v1_t s={0};
    return !tool_syscall(SYS_FSTAT,fd,(uintptr_t)&s,0) && s.size==sizeof(s) && s.version==1 &&
        s.type==type && !s.uid && !s.gid && (s.mode & 07777u)==mode;
}
static inline long live_password_read(char hash[78]) {
    long fd=tool_syscall(SYS_OPEN,(uintptr_t)LIVE_PASSWORD_DIR,VFS_O_RDONLY|VFS_O_CLOEXEC,0);
    if (fd<0) return fd;
    bool safe=live_password_metadata(fd,VFS_DIRECTORY,0700);
    tool_syscall(SYS_CLOSE,fd,0,0);
    if (!safe) return SYSCALL_EIO;
    fd=tool_syscall(SYS_OPEN,(uintptr_t)LIVE_PASSWORD_FILE,VFS_O_RDONLY|VFS_O_CLOEXEC,0);
    if (fd<0) return SYSCALL_EIO;
    long result=SYSCALL_EIO;size_t used=0;
    if (!live_password_metadata(fd,VFS_FILE,0600)) goto done;
    while (used<78) {
        long n=tool_syscall(SYS_READ,fd,(uintptr_t)(hash+used),78-used);
        if (n<0 || (size_t)n>78-used) goto done;
        if (!n) break;
        used+=(size_t)n;
    }
    if (!used || used>=78) goto done;
    hash[used]=0;
    if (tool_length(hash)!=used || hash[0]!='$' || !db_hash_valid(hash)) goto done;
    result=0;
done:
    tool_syscall(SYS_CLOSE,fd,0,0);return result;
}
static inline bool live_password_apply(userdb_t *db) {
    char hash[78]={0};long r=live_password_read(hash);bool ok=r==SYSCALL_ENOENT;
    if (!r) for (unsigned i=0;i<db->nshadows;i++) {
        if (!tool_equal(db->shadows[i].name,"operator")) continue;
        const db_user_t *u=db_user_name(db,"operator");
        if (!u || u->uid!=1000) break;
        db_wipe(db->shadows[i].hash,sizeof(db->shadows[i].hash));
        for (size_t j=0;j<=tool_length(hash);j++) db->shadows[i].hash[j]=hash[j];
        db->shadows[i].restricted=false;ok=true;break;
    }
    db_wipe(hash,sizeof(hash));return ok;
}
static inline bool live_password_publish(const char *hash) {
    uint32_t r,e,s;account_request_t req={.size=sizeof(req),.version=1};
    long caps=tool_syscall(SYS_CAPGET,0,0,0);
    if (tool_syscall(SYS_GETRESUID,(uintptr_t)&r,(uintptr_t)&e,(uintptr_t)&s) || e ||
        caps<0 || !((uint64_t)caps & CAP_SYS_ADMIN) ||
        tool_syscall(SYS_ACCOUNTCTL,ACCOUNT_STATUS,(uintptr_t)&req,sizeof(req))!=SYSCALL_EOPNOTSUPP ||
        hash[0]!='$' || !db_hash_valid(hash)) return false;
    long mask=tool_syscall(SYS_UMASK,077,0,0);if (mask<0) return false;
    long fd=-1;bool ok=false;
    if (tool_syscall(SYS_MKDIR,(uintptr_t)LIVE_PASSWORD_DIR,0700,0)) goto done;
    fd=tool_syscall(SYS_OPEN,(uintptr_t)LIVE_PASSWORD_STAGE,VFS_O_WRONLY|VFS_O_CREAT|VFS_O_CLOEXEC,0);
    if (fd<0 || !live_password_metadata(fd,VFS_FILE,0600)) goto done;
    size_t length=tool_length(hash),used=0;
    while (used<length) {
        long n=tool_syscall(SYS_WRITE,fd,(uintptr_t)(hash+used),length-used);
        if (n<=0 || (size_t)n>length-used) goto done;
        used+=(size_t)n;
    }
    if (tool_syscall(SYS_CLOSE,fd,0,0)) {fd=-1;goto done;}
    fd=-1;
    if (tool_syscall(SYS_RENAME,(uintptr_t)LIVE_PASSWORD_STAGE,(uintptr_t)LIVE_PASSWORD_FILE,0)) goto done;
    char verify[78]={0};ok=!live_password_read(verify) && tool_equal(hash,verify);db_wipe(verify,sizeof(verify));
done:
    if (fd>=0) tool_syscall(SYS_CLOSE,fd,0,0);
    if (tool_syscall(SYS_UMASK,mask,0,0)<0) ok=false;
    return ok;
}
#endif
