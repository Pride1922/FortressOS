/* Included after resolve_path and syscall_actor. No G across VFS entry. */
/* Derive from a coherent old value; expected-old publication revalidates it
 * under G. A stale decision returns EAGAIN without effects. User copies and
 * all value-only work happen outside G; no credential pointer escapes. */
static int64_t credential_commit(const creds_t *old,const creds_t *next,int error) {
    if (error) return error==CREDS_EPERM ? SYSCALL_EPERM : SYSCALL_EINVAL;
    tcb_t *self=thread_current();
    return self && process_record_publish_creds(self->tid,old,next) ? 0 : SYSCALL_EAGAIN;
}
static int64_t sys_setres(bool gid,uint64_t real,uint64_t effective,uint64_t saved,uint64_t flags) {
    if (real>UINT32_MAX || effective>UINT32_MAX || saved>UINT32_MAX || (flags & ~7ull)) return SYSCALL_EINVAL;
    creds_t old,next;if (!syscall_actor(&old)) return SYSCALL_ESRCH;
    creds_id_change_t r={(uint32_t)real,(flags & CREDS_KEEP_REAL)!=0},
        e={(uint32_t)effective,(flags & CREDS_KEEP_EFFECTIVE)!=0},
        s={(uint32_t)saved,(flags & CREDS_KEEP_SAVED)!=0};
    int error=gid ? creds_setresgid(&old,r,e,s,&next) : creds_setresuid(&old,r,e,s,&next);
    return credential_commit(&old,&next,error);
}
static int64_t sys_setgroups(uint64_t count,uintptr_t input) {
    if (count>CREDS_MAX_GROUPS) return SYSCALL_EINVAL;
    uint32_t groups[CREDS_MAX_GROUPS]={0};size_t bytes=count*sizeof(uint32_t);
    if (bytes && !vmm_validate_user_range(vmm_get_active_pml4_virt(),input,bytes,false)) return SYSCALL_EFAULT;
    if (bytes) memcpy(groups,(void *)input,bytes);
    creds_t old,next;if (!syscall_actor(&old)) return SYSCALL_ESRCH;
    int error=creds_setgroups(&old,groups,count,&next);
    return credential_commit(&old,&next,error);
}
static int64_t sys_capset(uint64_t mask) {
    creds_t old,next;if (!syscall_actor(&old)) return SYSCALL_ESRCH;
    int error=creds_capset(&old,mask,&next);
    return credential_commit(&old,&next,error);
}
static int64_t sys_capget(void) {
    creds_t value;return syscall_actor(&value) ? (int64_t)value.cap_effective : SYSCALL_ESRCH;
}
static int64_t sys_umask(uint64_t mask) {
    tcb_t *self=thread_current();creds_t old,next;
    if (!self || !syscall_actor(&old)) return SYSCALL_ESRCH;
    if (mask==UMASK_QUERY) return old.umask;
    if (mask>UINT32_MAX) return SYSCALL_EINVAL;
    if (creds_umask(&old,(uint32_t)mask,&next)) return SYSCALL_EINVAL;
    if (!process_record_publish_creds(self->tid,&old,&next)) return SYSCALL_EAGAIN;
    return old.umask;
}
static int64_t sys_getres(bool groups,uintptr_t real,uintptr_t effective,uintptr_t saved) {
    uint64_t *pml4=vmm_get_active_pml4_virt();
    if (!vmm_validate_user_range(pml4,real,4,true) ||
        !vmm_validate_user_range(pml4,effective,4,true) ||
        !vmm_validate_user_range(pml4,saved,4,true)) return SYSCALL_EFAULT;
    creds_t value;if (!syscall_actor(&value)) return SYSCALL_ESRCH;
    uint32_t r=groups ? value.gid : value.uid,e=groups ? value.egid : value.euid,
        s=groups ? value.sgid : value.suid;
    memcpy((void *)real,&r,4);memcpy((void *)effective,&e,4);memcpy((void *)saved,&s,4);
    return 0;
}
static int64_t sys_getgroups(uint64_t capacity,uintptr_t output) {
    if (capacity>CREDS_MAX_GROUPS) return SYSCALL_EINVAL;
    creds_t value;if (!syscall_actor(&value)) return SYSCALL_ESRCH;
    if (!capacity) return value.ngroups;
    if (capacity<value.ngroups) return SYSCALL_EINVAL;
    size_t bytes=value.ngroups*sizeof(uint32_t);
    if (bytes && !vmm_validate_user_range(vmm_get_active_pml4_virt(),output,bytes,true)) return SYSCALL_EFAULT;
    if (bytes) memcpy((void *)output,value.groups,bytes);
    return value.ngroups;
}
static int64_t sys_chmod(uintptr_t path,uint64_t mode) {
    if (mode>UINT32_MAX) return SYSCALL_EINVAL;
    char raw[VFS_MAX_PATH],resolved[VFS_MAX_PATH];
    int r=copy_user_string(vmm_get_active_pml4_virt(),path,raw,sizeof(raw));if (r) return r;
    r=resolve_path(thread_current(),raw,resolved,sizeof(resolved));if (r) return r;
    creds_t actor;if (!syscall_actor(&actor)) return SYSCALL_ESRCH;
    return syscall_from_vfs_error(vfs_chmod_creds(resolved,(uint32_t)mode,&actor));
}
static int64_t sys_fchmod(uint64_t fd,uint64_t mode) {
    if (fd>=MAX_PROCESS_FDS) return SYSCALL_EBADF;
    if (mode>UINT32_MAX) return SYSCALL_EINVAL;
    tcb_t *self=thread_current();file_t *file=self ? fd_get(self,(int)fd) : NULL;
    if (!file) return SYSCALL_EBADF;
    creds_t actor;if (!syscall_actor(&actor)) return SYSCALL_ESRCH;
    return syscall_from_vfs_error(vfs_fchmod_creds(file,(uint32_t)mode,&actor));
}
static int64_t sys_chown(uintptr_t path,uint64_t uid,uint64_t gid,uint64_t flags) {
    if (uid>UINT32_MAX || gid>UINT32_MAX || (flags & ~(CHOWN_KEEP_UID|CHOWN_KEEP_GID))) return SYSCALL_EINVAL;
    char raw[VFS_MAX_PATH],resolved[VFS_MAX_PATH];
    int r=copy_user_string(vmm_get_active_pml4_virt(),path,raw,sizeof(raw));if (r) return r;
    r=resolve_path(thread_current(),raw,resolved,sizeof(resolved));if (r) return r;
    creds_t actor;if (!syscall_actor(&actor)) return SYSCALL_ESRCH;
    creds_id_change_t u={.value=(uint32_t)uid,.keep=(flags & CHOWN_KEEP_UID)!=0},
        g={.value=(uint32_t)gid,.keep=(flags & CHOWN_KEEP_GID)!=0};
    return syscall_from_vfs_error(vfs_chown_creds(resolved,u,g,&actor));
}
#ifdef TEST_PERMISSIONS_ENFORCEMENT
static int64_t sys_test_setcreds(void) {
    tcb_t *self=thread_current();creds_t old;
    if (!self || !syscall_actor(&old)) return SYSCALL_ESRCH;
    if (!(old.cap_effective & CAP_SETUID) || !(old.cap_effective & CAP_SETGID)) return SYSCALL_EPERM;
    creds_t next={.uid=1001,.euid=1001,.suid=1001,.gid=1001,.egid=1001,.sgid=1001,.umask=old.umask};
    return process_record_publish_creds(self->tid,&old,&next) ? 0 : SYSCALL_EAGAIN;
}
#endif
