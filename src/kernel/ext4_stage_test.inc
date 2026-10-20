/* Fixture-only private syscall transport. No production ABI number. */
static uint64_t stage_test_owners[4];
static int64_t sys_stage_test_write(uint64_t fd,uintptr_t ptr,size_t count,uint64_t expected,uint64_t flags) {
    tcb_t *self=thread_current();file_t *file=(self && fd<MAX_PROCESS_FDS)?fd_get(self,(int)fd):NULL;
    if (!file || !file->node || (file->flags&VFS_O_ACCMODE)==VFS_O_RDONLY) return SYSCALL_EBADF;
    if (flags || !count || count>EXT4_STAGE_TEST_MAX || expected>8ULL*1024*1024*1024 ||
        count>8ULL*1024*1024*1024-expected) return SYSCALL_EINVAL;
    if (!vmm_validate_user_range(vmm_get_active_pml4_virt(),ptr,count,false)) return SYSCALL_EFAULT;
    if (!vfs_file_get(file)) return SYSCALL_EIO;
    int slot=-1;int64_t result=SYSCALL_EAGAIN;void *copy=NULL;
    for(unsigned i=0;i<4;i++) if(__atomic_load_n(&stage_test_owners[i],__ATOMIC_ACQUIRE)==self->tid) goto done;
    for(unsigned i=0;i<4;i++) {uint64_t empty=0;
        if(__atomic_compare_exchange_n(&stage_test_owners[i],&empty,self->tid,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {slot=(int)i;break;}}
    if(slot<0)goto done;
    copy=kmalloc(count);if(!copy){result=SYSCALL_ENOMEM;goto done;}
    memcpy(copy,(const void *)ptr,count);creds_t actor;
    if(!syscall_actor(&actor))result=SYSCALL_ESRCH;
    else {int64_t n=ext4_stage_test_write(file,copy,count,expected,&actor);result=n<0?syscall_from_vfs_error(n):n;}
done:
    kfree(copy);vfs_close(file);
    if(slot>=0)__atomic_store_n(&stage_test_owners[slot],0,__ATOMIC_RELEASE);
    return result;
}
static int64_t sys_stage_test_sync(uint64_t fd) {
    tcb_t *self=thread_current();file_t *file=(self && fd<MAX_PROCESS_FDS)?fd_get(self,(int)fd):NULL;
    if(!file)return SYSCALL_EBADF;
    if(!vfs_file_get(file))return SYSCALL_EIO;
    int r=ext4_stage_test_sync(file);vfs_close(file);return syscall_from_vfs_error(r);
}
