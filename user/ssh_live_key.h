#ifndef FORTRESS_SSH_LIVE_KEY_H
#define FORTRESS_SSH_LIVE_KEY_H
#include "syscall_abi.h"
#include "hostkey_abi.h"
#include "vfs.h"

#define SSH_LIVE_DIR "/run/ssh"
#define SSH_LIVE_KEY SSH_LIVE_DIR "/ssh_host_ed25519_key"
#define SSH_LIVE_STAGE SSH_LIVE_DIR "/.hostkey.pending"
typedef long (*ssh_live_call_t)(long,uintptr_t,uintptr_t,uintptr_t);

static inline bool ssh_live_metadata(ssh_live_call_t call,long fd,unsigned type,unsigned mode,
                                      stat_ext_v1_t *st) {
    return call(SYS_FSTAT,(uintptr_t)fd,(uintptr_t)st,0)==0 &&
           st->size==sizeof(*st) && st->version==1 && st->type==type &&
           st->uid==0 && st->gid==0 && (st->mode & 07777u)==mode;
}
/* ENOENT means no runtime directory. A present but incomplete/unsafe runtime
 * identity returns EIO: callers must not fall back to another host identity. */
static inline long ssh_live_read(ssh_live_call_t call,const char *path,char out[HOSTKEY_FILE_MAX]) {
    long dir=call(SYS_OPEN,(uintptr_t)SSH_LIVE_DIR,VFS_O_RDONLY|VFS_O_CLOEXEC,0);
    if (dir<0) return dir;
    stat_ext_v1_t st={0};
    bool safe=ssh_live_metadata(call,dir,VFS_DIRECTORY,0700,&st);
    (void)call(SYS_CLOSE,dir,0,0);
    if (!safe) return SYSCALL_EIO;
    long fd=call(SYS_OPEN,(uintptr_t)path,VFS_O_RDONLY|VFS_O_CLOEXEC,0);
    if (fd<0) return SYSCALL_EIO;
    long result=SYSCALL_EIO;
    if (!ssh_live_metadata(call,fd,VFS_FILE,0600,&st) || !st.file_size || st.file_size>=HOSTKEY_FILE_MAX) goto done;
    size_t used=0;
    while (used<st.file_size) {
        long n=call(SYS_READ,fd,(uintptr_t)(out+used),st.file_size-used);
        if (n<=0 || (uint64_t)n>st.file_size-used) goto done;
        used+=(size_t)n;
    }
    char extra;
    if (call(SYS_READ,fd,(uintptr_t)&extra,1)!=0) goto done;
    out[used]=0;result=(long)used;
done:
    (void)call(SYS_CLOSE,fd,0,0);
    return result;
}
#endif
