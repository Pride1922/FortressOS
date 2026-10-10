#ifndef FORTRESS_SPAWN_SECURITY_H
#define FORTRESS_SPAWN_SECURITY_H
#include "vfs.h"
#include "syscall_abi.h"
/* Values must describe the same admitted image bytes. No pathname relookup. */
static inline bool spawn_credentials(const creds_t *actor, const vfs_metadata_t *image,
                                     creds_t *child) {
    *child=*actor;
    if (image->mnt_flags & VFS_MNT_NOSUID) return false;
    bool uid=(image->mode & 04000)!=0;
    bool gid=(image->mode & 02000) && (image->mode & 0010);
    if (uid) {
        child->euid=child->suid=image->uid;
        if (child->euid!=actor->euid)
            child->cap_effective=child->euid==0 ? CAP_ALL : 0;
    }
    if (gid) child->egid=child->sgid=image->gid;
    return uid || gid;
}
/* Final destinations, after all actions. A later close removes admission. */
static inline uint32_t spawn_mapped_fds(int count,const spawn_kaction_t *actions) {
    uint32_t mapped=7;
    for (int i=0;i<count;i++) {
        unsigned fd=(unsigned)actions[i].dst_fd;
        if (fd>=MAX_PROCESS_FDS) continue;
        if (actions[i].type==SPAWN_FD_ACTION_CLOSE) mapped &= ~(1u<<fd);
        else mapped |= 1u<<fd;
    }
    return mapped;
}
#endif
