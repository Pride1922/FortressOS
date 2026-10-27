/* Actual control service and extracted production descriptor/fcntl functions.
 * User-range, allocator, scheduler/current-process and device callbacks are host
 * adapters. No hardware, PTE, scheduler or native filesystem claim. */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "vfs.h"
#include "thread.h"
#include "syscall.h"
#include "install_control.h"
#include "install_log.h"
static tcb_t process={.tid=10,.is_user=true},child={.tid=11,.is_user=true};
static tcb_t *current=&process;
static bool oom,deny_input,deny_output;
static unsigned allocations,writes,reads,flushes,identities;
static bool fail_write,fail_read,fail_flush,replace,source,mounted,reenter;
static uint8_t sector[4096];
tcb_t *thread_current(void) {return current;}
void spin_debug_assert_unheld(void) {}
void accounts_process_exit(uint64_t pid) {(void)pid;}
/* Extracted fd_close_cloexec also releases package exec ownership. Package
 * authority is outside this descriptor/installer harness. */
void pkg_owner_exec(struct tcb *proc) {(void)proc;}
void *kcalloc(size_t n,size_t s) {if (oom) return NULL;void *p=calloc(n,s);if (p) allocations++;return p;}
void kfree(void *p) {if (p) {assert(allocations);allocations--;free(p);}}
uint64_t *vmm_get_active_pml4_virt(void) {static uint64_t table[512];return table;}
bool vmm_validate_user_range(uint64_t *pml4,uintptr_t p,size_t n,bool out) {
    (void)pml4;return p>=4096 && n && p<=UINTPTR_MAX-n && !(out ? deny_output : deny_input);
}
int vfs_close(file_t *f) {
    assert(f && f->ref_count>0);if (--f->ref_count) return 0;
    if (f->node && f->node->close) f->node->close(f->node);
    kfree(f);return 0;
}
file_t *vfs_open_terminal(int flags) {
    file_t *f=kcalloc(1,sizeof(*f));if (f) {f->flags=flags;f->ref_count=1;}return f;
}
/* Generated verbatim by runner, not a mirrored descriptor implementation. */
#include "install_fds.inc"
#include "install_fcntl.inc"
#include "install_errors.inc"
bool block_read_sector(block_dev_t *d,uint64_t l,void *p) {return d && l<d->sector_count && d->read_sector && d->read_sector(d,l,p);}
bool block_write_sector(block_dev_t *d,uint64_t l,const void *p) {return d && l<d->sector_count && d->write_sector && d->write_sector(d,l,p);}
bool block_flush(block_dev_t *d) {return d && d->flush && d->flush(d);}
#include "../src/kernel/install_control.c"
static install_identity_v1_t hardware;
static creds_t root={.cap_effective=CAP_ALL};
static int identity(void *ctx,install_identity_v1_t *id) {
    (void)ctx;identities++;*id=hardware;
    if (replace) id->generation++;
    if (source) id->flags|=INSTALL_ID_SOURCE;
    if (mounted) id->flags|=INSTALL_ID_MOUNTED;
    install_identity_digest(id);
    if (reenter) {
        uint32_t index=0;install_identity_v1_t out;
        assert(install_control(INSTALL_ENUM,(uintptr_t)&index,4,(uintptr_t)&out,sizeof(out),&root)==SYSCALL_EAGAIN);
    }
    return 0;
}
static int device_read(void *ctx,uint64_t lba,uint32_t n,void *data) {
    (void)ctx;(void)lba;reads++;if (fail_read) return -VFS_EIO;memcpy(data,sector,n*512);return 0;
}
static int device_write(void *ctx,uint64_t lba,uint32_t n,const void *data) {
    (void)ctx;(void)lba;writes++;if (fail_write) return 1;memcpy(sector,data,n*512);return 0;
}
static int device_flush(void *ctx) {(void)ctx;flushes++;return fail_flush ? -VFS_EIO : 0;}
static install_backend_t b={.identity=identity,.read=device_read,.write=device_write,.flush=device_flush};
static unsigned deployments;
static int deploy_v4(void *ctx,const install_deploy_v1_t *req,install_status_v1_t *status) {
    (void)ctx;const install_deploy_v4_t *v=(const void *)req;
    assert(req->size==sizeof(*v) && req->version==4 && !req->flags);
    assert(!strcmp(v->username,"operator") && !strcmp(v->hostname,"ssh-test"));
    assert(v->hostkey_bytes==4 && !memcmp((const void *)(uintptr_t)v->hostkey,"key\n",4));
    deployments++;status->complete=1;return 0;
}
static install_identity_v1_t indexed[2];
static unsigned selected_index;
static int indexed_enum(void *ctx,unsigned i,install_identity_v1_t *id) {
    (void)ctx;if (i>=2) return 0;*id=indexed[i];return 1;
}
static int indexed_select(void *ctx,unsigned i) {
    (void)ctx;if (i>=2) return -VFS_EINVAL;selected_index=i;hardware=indexed[i];return 0;
}
static int acquire(void) {
    install_target_v1_t t={.size=64,.version=1,.generation=hardware.generation,.flags=1};
    memcpy(t.digest,hardware.digest,32);int32_t fd=-1;
    assert(!install_control(INSTALL_ACQUIRE,(uintptr_t)&t,sizeof(t),(uintptr_t)&fd,sizeof(fd),&root));return fd;
}
static int release(int32_t fd) {return (int)install_control(INSTALL_RELEASE,(uintptr_t)&fd,4,0,0,&root);}
static void unchanged(unsigned old) {assert(writes==old);}
int main(void) {
    install_log_event_v1_t event={.size=32,.version=1};
    install_control_boot(NULL,true);
    assert(install_control(INSTALL_LOG,(uintptr_t)&event,32,0,0,&root)==SYSCALL_EIO);
    event.reserved=1;assert(install_control(INSTALL_LOG,(uintptr_t)&event,32,0,0,&root)==SYSCALL_EINVAL);event.reserved=0;
    assert(install_control(INSTALL_LOG,(uintptr_t)&event,31,0,0,&root)==SYSCALL_EINVAL);
    assert(install_control(INSTALL_LOG,0,32,0,0,&root)==SYSCALL_EFAULT);
    process.current_cpu=1;assert(install_control(INSTALL_LOG,(uintptr_t)&event,32,0,0,&root)==SYSCALL_EPERM);process.current_cpu=0;
    install_control_boot(NULL,false);assert(install_control(INSTALL_LOG,(uintptr_t)&event,32,0,0,&root)==SYSCALL_EOPNOTSUPP);
    hardware=(install_identity_v1_t){.size=192,.version=1,.generation=1,.sector_size=512,
        .sector_count=384*2048,.nsid=1,.flags=INSTALL_ID_ELIGIBLE};
    memcpy(hardware.serial,"fixture-only",12);install_identity_digest(&hardware);
    uint32_t index=0;install_identity_v1_t out;install_target_v1_t target={.size=64,.version=1,.generation=1,.flags=1};
    memcpy(target.digest,hardware.digest,32);int32_t fd;
    assert(install_control(INSTALL_ENUM,(uintptr_t)&index,4,(uintptr_t)&out,sizeof(out),&root)==SYSCALL_EOPNOTSUPP);
    install_control_boot(&b,true);
    install_deploy_v2_t deploy2={.base={.size=288,.version=2,.fd=-1,.flags=INSTALL_DRY_RUN}};
    install_status_v1_t deploy_status;
    const char *bad_names[]={"","root","wheel","../root","a:b","1user","Upper","-user"};
    for (unsigned i=0;i<sizeof(bad_names)/sizeof(bad_names[0]);i++) {
        memset(deploy2.username,0,32);memcpy(deploy2.username,bad_names[i],strlen(bad_names[i])+1);
        assert(install_control(INSTALL_DEPLOY,(uintptr_t)&deploy2,288,(uintptr_t)&deploy_status,32,&root)==SYSCALL_EINVAL);
    }
    memset(deploy2.username,'a',32);
    assert(install_control(INSTALL_DEPLOY,(uintptr_t)&deploy2,288,(uintptr_t)&deploy_status,32,&root)==SYSCALL_EINVAL);
    memcpy(deploy2.username,"fabio",6);
    assert(install_control(INSTALL_DEPLOY,(uintptr_t)&deploy2,288,(uintptr_t)&deploy_status,32,&root)==SYSCALL_EOPNOTSUPP);
    install_deploy_v3_t deploy3={.base={.size=352,.version=3,.fd=-1,.flags=INSTALL_DRY_RUN},.username="fabio",.hostname="dell-5590"};
    assert(install_control(INSTALL_DEPLOY,(uintptr_t)&deploy3,352,(uintptr_t)&deploy_status,32,&root)==SYSCALL_EOPNOTSUPP);
    const char *bad_hosts[]={"","-bad","bad-","bad/name","bad name","bad\033host"};
    for (unsigned i=0;i<sizeof(bad_hosts)/sizeof(bad_hosts[0]);i++) {
        memset(deploy3.hostname,0,64);memcpy(deploy3.hostname,bad_hosts[i],strlen(bad_hosts[i])+1);
        assert(install_control(INSTALL_DEPLOY,(uintptr_t)&deploy3,352,(uintptr_t)&deploy_status,32,&root)==SYSCALL_EINVAL);
    }
    memset(deploy3.hostname,'a',64);
    assert(install_control(INSTALL_DEPLOY,(uintptr_t)&deploy3,352,(uintptr_t)&deploy_status,32,&root)==SYSCALL_EINVAL);
    creds_t user={.euid=1000};
    assert(install_control(INSTALL_ENUM,(uintptr_t)&index,4,(uintptr_t)&out,sizeof(out),&user)==SYSCALL_EPERM);
    deny_output=true;assert(install_control(INSTALL_ACQUIRE,(uintptr_t)&target,64,(uintptr_t)&fd,4,&root)==SYSCALL_EFAULT);deny_output=false;
    assert(!identities && !flushes && !writes);
    assert(install_control(INSTALL_ENUM,0,4,(uintptr_t)&out,sizeof(out),&root)==SYSCALL_EFAULT);
    assert(install_control(INSTALL_ENUM,(uintptr_t)&index,5,(uintptr_t)&out,sizeof(out),&root)==SYSCALL_EINVAL);
    assert(install_control(INSTALL_ENUM,(uintptr_t)&index,4,(uintptr_t)&out,sizeof(out),&root)==1);
    assert(!memcmp(&out,&hardware,sizeof(out)));index=1;
    assert(!install_control(INSTALL_ENUM,(uintptr_t)&index,4,(uintptr_t)&out,sizeof(out),&root));index=0;
    for (unsigned i=0;i<8;i++) {
        target.reserved[i]=1;assert(install_control(INSTALL_ACQUIRE,(uintptr_t)&target,64,(uintptr_t)&fd,4,&root)==SYSCALL_EINVAL);target.reserved[i]=0;
    }
    target.generation++;assert(install_control(INSTALL_ACQUIRE,(uintptr_t)&target,64,(uintptr_t)&fd,4,&root)==SYSCALL_EAGAIN);target.generation--;
    source=true;assert(install_control(INSTALL_ACQUIRE,(uintptr_t)&target,64,(uintptr_t)&fd,4,&root)==SYSCALL_EAGAIN);source=false;
    mounted=true;assert(install_control(INSTALL_ACQUIRE,(uintptr_t)&target,64,(uintptr_t)&fd,4,&root)==SYSCALL_EAGAIN);mounted=false;
    /* Correctly enumerated excluded source still cannot acquire. */
    hardware.flags|=INSTALL_ID_SOURCE;install_identity_digest(&hardware);memcpy(target.digest,hardware.digest,32);
    assert(install_control(INSTALL_ACQUIRE,(uintptr_t)&target,64,(uintptr_t)&fd,4,&root)==SYSCALL_EACCES);
    hardware.flags=INSTALL_ID_ELIGIBLE;install_identity_digest(&hardware);memcpy(target.digest,hardware.digest,32);
    oom=true;assert(install_control(INSTALL_ACQUIRE,(uintptr_t)&target,64,(uintptr_t)&fd,4,&root)==SYSCALL_ENOMEM);oom=false;
    fail_flush=true;assert(install_control(INSTALL_ACQUIRE,(uintptr_t)&target,64,(uintptr_t)&fd,4,&root)==SYSCALL_EIO);fail_flush=false;
    assert(!allocations && !install_control_owned());
    reenter=true;fd=acquire();reenter=false;
    assert(install_control_owned() && process.fd_table[fd]->exclusive_owner && process.fd_flags[fd]==FD_FLAG_CLOEXEC);
    assert(fd_dup(&process,fd)==SYSCALL_EBADF && fd_dup2(&process,fd,fd)==SYSCALL_EBADF);
    assert(sys_fcntl(fd,F_DUPFD,0)==SYSCALL_EBADF && sys_fcntl(fd,F_DUPFD_CLOEXEC,0)==SYSCALL_EBADF);
    assert(sys_fcntl(fd,F_SETFD,0)==SYSCALL_EPERM && !sys_fcntl(fd,F_SETFD,FD_CLOEXEC));
    fd_clone_table(&process,&child);for (unsigned i=0;i<MAX_PROCESS_FDS;i++) assert(!child.fd_table[i]);
    uint8_t bytes[4096];memset(bytes,0x93,sizeof(bytes));
    install_io_v1_t io={.size=40,.version=1,.fd=fd,.count=8,.buffer=(uintptr_t)bytes};
    current=&child;assert(install_control(INSTALL_WRITE,(uintptr_t)&io,40,0,0,&root)==SYSCALL_EBADF);current=&process;
    unsigned old=writes;io.count=9;assert(install_control(INSTALL_WRITE,(uintptr_t)&io,40,0,0,&root)==SYSCALL_EINVAL);unchanged(old);io.count=8;
    io.lba=UINT64_MAX;assert(install_control(INSTALL_WRITE,(uintptr_t)&io,40,0,0,&root)==SYSCALL_EINVAL);unchanged(old);io.lba=0;
    deny_input=true;assert(install_control(INSTALL_WRITE,(uintptr_t)&io,40,0,0,&root)==SYSCALL_EFAULT);deny_input=false;unchanged(old);
    assert(!install_control(INSTALL_WRITE,(uintptr_t)&io,40,0,0,&root) && !memcmp(sector,bytes,sizeof(bytes)));
    memset(bytes,0,sizeof(bytes));assert(!install_control(INSTALL_READ,(uintptr_t)&io,40,0,0,&root) && !memcmp(sector,bytes,sizeof(bytes)));
    deny_output=true;assert(install_control(INSTALL_READ,(uintptr_t)&io,40,0,0,&root)==SYSCALL_EFAULT);deny_output=false;
    replace=true;old=writes;assert(install_control(INSTALL_WRITE,(uintptr_t)&io,40,0,0,&root)==SYSCALL_EIO);replace=false;
    assert(install_control(INSTALL_WRITE,(uintptr_t)&io,40,0,0,&root)==SYSCALL_EIO);unchanged(old);assert(!release(fd));
    fd=acquire();io.fd=fd;fail_write=true;assert(install_control(INSTALL_WRITE,(uintptr_t)&io,40,0,0,&root)==SYSCALL_EIO);fail_write=false;
    old=writes;assert(install_control(INSTALL_WRITE,(uintptr_t)&io,40,0,0,&root)==SYSCALL_EIO);unchanged(old);assert(!release(fd));
    fd=acquire();io.fd=fd;fail_read=true;memset(bytes,0xaa,sizeof(bytes));assert(install_control(INSTALL_READ,(uintptr_t)&io,40,0,0,&root)==SYSCALL_EIO);fail_read=false;
    for (unsigned i=0;i<4096;i++) assert(bytes[i]==0xaa);
    assert(!release(fd));
    fd=acquire();fail_flush=true;assert(install_control(INSTALL_FLUSH,(uintptr_t)&fd,4,0,0,&root)==SYSCALL_EIO);fail_flush=false;assert(!release(fd));
    fd=acquire();fd_close_cloexec(&process);assert(!install_control_owned() && !process.fd_table[fd]);
    fd=acquire();fd_close_all(&process);assert(!install_control_owned() && !allocations);
    assert(release(fd)==SYSCALL_EBADF);puts("PASS installer control: ABI, exclusions, stale identity, failures, non-inherited/dup/CLOEXEC/exit leases");
    /* Actual v4 dispatcher handoff and bounds; backend crypto/EXT4 work is
     * independently exercised by the runtime and host-key mounted tests. */
    b.deploy=deploy_v4;install_control_boot(&b,true);fd=acquire();
    install_deploy_v4_t v4={.base={.size=sizeof(v4),.version=4,.fd=fd},.username="operator",.hostname="ssh-test",
        .hostkey=(uintptr_t)"key\n",.hostkey_bytes=4};
    install_status_v1_t status={.size=sizeof(status),.version=1};
    assert(!install_control(INSTALL_DEPLOY,(uintptr_t)&v4,sizeof(v4),(uintptr_t)&status,sizeof(status),&root));
    assert(deployments==1 && status.complete);
    v4.hostkey=1;assert(install_control(INSTALL_DEPLOY,(uintptr_t)&v4,sizeof(v4),(uintptr_t)&status,sizeof(status),&root)==SYSCALL_EFAULT);
    v4.hostkey=(uintptr_t)"key\n";v4.hostkey_bytes=1024;
    assert(install_control(INSTALL_DEPLOY,(uintptr_t)&v4,sizeof(v4),(uintptr_t)&status,sizeof(status),&root)==SYSCALL_EINVAL);
    v4.hostkey_bytes=4;v4.reserved=1;
    assert(install_control(INSTALL_DEPLOY,(uintptr_t)&v4,sizeof(v4),(uintptr_t)&status,sizeof(status),&root)==SYSCALL_EINVAL);
    v4.reserved=0;v4.hostkey=0;
    assert(install_control(INSTALL_DEPLOY,(uintptr_t)&v4,sizeof(v4),(uintptr_t)&status,sizeof(status),&root)==SYSCALL_EINVAL);
    v4.hostkey=(uintptr_t)"key\n";v4.base.flags=INSTALL_DRY_RUN;v4.base.fd=-1;
    assert(install_control(INSTALL_DEPLOY,(uintptr_t)&v4,sizeof(v4),(uintptr_t)&status,sizeof(status),&root)==SYSCALL_EINVAL);
    assert(deployments==1 && !release(fd));b.deploy=NULL;
    puts("PASS installer v4 control: exact host-key handoff, pointer/size/reserved/pair/dry-run rejection");
    indexed[0]=hardware;indexed[1]=hardware;indexed[1].serial[0]='2';install_identity_digest(&indexed[1]);
    b.enumerate=indexed_enum;b.select=indexed_select;install_control_boot(&b,true);
    index=1;assert(install_control(INSTALL_ENUM,(uintptr_t)&index,4,(uintptr_t)&out,sizeof(out),&root)==1);
    target.index=1;memcpy(target.digest,out.digest,32);
    target.digest[0]^=1;old=writes;
    assert(install_control(INSTALL_ACQUIRE,(uintptr_t)&target,64,(uintptr_t)&fd,4,&root)==SYSCALL_EAGAIN);
    unchanged(old);target.digest[0]^=1;
    assert(!install_control(INSTALL_ACQUIRE,(uintptr_t)&target,64,(uintptr_t)&fd,4,&root));
    assert(selected_index==1);
    index=0;assert(install_control(INSTALL_ENUM,(uintptr_t)&index,4,(uintptr_t)&out,sizeof(out),&root)==SYSCALL_EAGAIN);
    io.fd=fd;assert(!install_control(INSTALL_WRITE,(uintptr_t)&io,40,0,0,&root));assert(selected_index==1);
    assert(!release(fd));target.index=2;
    assert(install_control(INSTALL_ACQUIRE,(uintptr_t)&target,64,(uintptr_t)&fd,4,&root)==SYSCALL_EINVAL);
    puts("PASS indexed inventory: second-drive lease, stale selection, invalid index and enumeration excluded while leased");
    return 0;
}
