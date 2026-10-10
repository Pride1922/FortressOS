#include "permission_values.h"

bool permission_group_member(const creds_t *actor, uint32_t gid) {
    if (!actor || actor->ngroups > CREDS_MAX_GROUPS) return false;
    if (actor->egid == gid) return true;
    for (unsigned i = 0; i < actor->ngroups; ++i)
        if (actor->groups[i] == gid) return true;
    return false;
}

int permission_access_value(const vfs_metadata_t *value, unsigned mask,
                            const creds_t *actor) {
    if (!value || !actor || !creds_valid(actor) || (mask & ~7u))
        return -VFS_EINVAL;
    unsigned type = value->mode & VFS_S_IFMT;
    bool directory = type == VFS_S_IFDIR;
    bool regular = type == VFS_S_IFREG;
    /* Device writes do not inherit regular-file read-only policy. */
    if ((mask & VFS_MAY_WRITE) && (regular || directory || type == 0120000u) &&
        (value->mnt_flags & VFS_MNT_RDONLY)) return -VFS_EROFS;
    if (regular && (mask & VFS_MAY_EXEC) && !(value->mode & 0111u))
        return -VFS_EACCES;
    unsigned shift = actor->euid == value->uid ? 6 :
        permission_group_member(actor, value->gid) ? 3 : 0;
    if ((((value->mode >> shift) & 7u) & mask) == mask) return 0;
    if (actor->cap_effective & CAP_DAC_OVERRIDE) return 0;
    unsigned read_search = VFS_MAY_READ | (directory ? VFS_MAY_EXEC : 0u);
    if ((actor->cap_effective & CAP_DAC_READ_SEARCH) && !(mask & ~read_search))
        return 0;
    return -VFS_EACCES;
}

int permission_delete_value(const vfs_metadata_t *dir,
                            const vfs_metadata_t *victim, const creds_t *actor) {
    if (!dir || !victim || !actor || !creds_valid(actor)) return -VFS_EINVAL;
    if ((dir->mode & VFS_S_IFMT) != VFS_S_IFDIR) return -VFS_ENOTDIR;
    int error = permission_access_value(dir, VFS_MAY_WRITE | VFS_MAY_EXEC, actor);
    if (error) return error;
    if ((dir->mode & 01000u) && actor->euid != dir->uid &&
        actor->euid != victim->uid && !(actor->cap_effective & CAP_FOWNER))
        return -VFS_EPERM;
    return 0;
}

int permission_chmod_value(const vfs_metadata_t *old, uint32_t requested,
                           const creds_t *actor, vfs_metadata_t *out) {
    if (!old || !actor || !out || !creds_valid(actor)) return -VFS_EINVAL;
    if (old->mnt_flags & VFS_MNT_RDONLY) return -VFS_EROFS;
    if (actor->euid != old->uid && !(actor->cap_effective & CAP_FOWNER))
        return -VFS_EPERM;
    vfs_metadata_t next = *old;
    next.mode = (old->mode & VFS_S_IFMT) | (requested & 07777u);
    if (!permission_group_member(actor, old->gid) &&
        !(actor->cap_effective & CAP_FSETID)) next.mode &= ~02000u;
    *out = next;
    return 0;
}

int permission_chown_value(const vfs_metadata_t *old, creds_id_change_t uid,
                           creds_id_change_t gid, const creds_t *actor,
                           vfs_metadata_t *out) {
    if (!old || !actor || !out || !creds_valid(actor)) return -VFS_EINVAL;
    if (old->mnt_flags & VFS_MNT_RDONLY) return -VFS_EROFS;
    uint32_t new_uid = uid.keep ? old->uid : uid.value;
    uint32_t new_gid = gid.keep ? old->gid : gid.value;
    if (!(actor->cap_effective & CAP_CHOWN) &&
        (actor->euid != old->uid || new_uid != old->uid ||
         (new_gid != old->gid && !permission_group_member(actor, new_gid))))
        return -VFS_EPERM;
    vfs_metadata_t next = *old;
    next.uid = new_uid; next.gid = new_gid;
    /* Every admitted chown clears setuid on non-directories, even when IDs
     * are unchanged. Executable setgid always clears; non-executable setgid
     * also clears outside the inode group without FSETID. */
    if ((old->mode & VFS_S_IFMT) != VFS_S_IFDIR) {
        next.mode &= ~04000u;
        if ((old->mode & 0010u) || (!permission_group_member(actor,old->gid) &&
            !(actor->cap_effective & CAP_FSETID))) next.mode &= ~02000u;
    }
    *out = next;
    return 0;
}

uint16_t permission_content_mode(const vfs_metadata_t *old, const creds_t *actor) {
    /* Called for positive writes or admitted truncation, even unchanged size.
     * Failed/denied/zero-byte writes never publish this.
     * An invalid actor conservatively receives no preservation privilege. */
    if (!old) return 0;
    uint16_t mode = old->mode;
    if ((mode & VFS_S_IFMT) != VFS_S_IFREG) return mode;
    bool valid = actor && creds_valid(actor);
    if (valid && (actor->cap_effective & CAP_FSETID)) return mode;
    mode &= ~04000u;
    if ((mode & 0010u) || !valid || !permission_group_member(actor,old->gid)) mode &= ~02000u;
    return mode;
}

int permission_create_value(const vfs_metadata_t *dir, vfs_node_type_t type,
                            uint32_t requested, const creds_t *actor,
                            vfs_create_attrs_t *out) {
    if (!dir || !out || !actor || !creds_valid(actor) ||
        (type != VFS_FILE && type != VFS_DIRECTORY)) return -VFS_EINVAL;
    if ((dir->mode & VFS_S_IFMT) != VFS_S_IFDIR) return -VFS_ENOTDIR;
    vfs_create_attrs_t result = { .mode = requested & ~actor->umask & 07777u,
        .uid = actor->euid, .gid = actor->egid };
    if (dir->mode & 02000u) {
        result.gid = dir->gid;
        if (type == VFS_DIRECTORY) result.mode |= 02000u;
    }
    /* FSETID allows explicit setgid creation even outside the chosen group.
     * Inherited setgid on directories is retained regardless of membership. */
    if (type == VFS_FILE && !permission_group_member(actor, result.gid) &&
        !(actor->cap_effective & CAP_FSETID)) result.mode &= ~02000u;
    *out = result;
    return 0;
}
