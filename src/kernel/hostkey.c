#include "hostkey.h"
#include "hostkey_abi.h"
#include "syscall.h"
#include "vmm.h"
#include "ext4.h"
#include "heap.h"
#include "string.h"
int64_t hostkey_control(uint64_t op,uintptr_t buffer,uint64_t size,const creds_t *actor) {
    if(op>HOSTKEY_PUBLISH || (op==HOSTKEY_READ ? size!=HOSTKEY_FILE_MAX : !size || size>=HOSTKEY_FILE_MAX)) return SYSCALL_EINVAL;
    if(!actor || actor->euid || !(actor->cap_effective&CAP_SYS_ADMIN)) return SYSCALL_EPERM;
    if(!vmm_validate_user_range(vmm_get_active_pml4_virt(),buffer,size,op==HOSTKEY_READ)) return SYSCALL_EFAULT;
    ext4_mount_t *fs=ext4_hostkey_installed();if(!fs) return SYSCALL_EOPNOTSUPP;
    char *scratch=kcalloc(1,HOSTKEY_FILE_MAX);if(!scratch) return SYSCALL_ENOMEM;
    int r;size_t n=0;
    if(op==HOSTKEY_READ) {
        r=ext4_hostkey_read(fs,scratch,HOSTKEY_FILE_MAX,&n);
        if(!r) memcpy((void *)buffer,scratch,n);
    } else {
        memcpy(scratch,(const void *)buffer,size);
        r=ext4_hostkey_publish(fs,scratch,size);
    }
    volatile uint8_t *v=(void *)scratch;for(size_t i=0;i<HOSTKEY_FILE_MAX;i++) v[i]=0;
    kfree(scratch);return r ? syscall_from_vfs_error(r) : op==HOSTKEY_READ ? (int64_t)n : 0;
}
