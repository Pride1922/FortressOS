/* Actual mounted EXT4/JBD2, redo engine and account syscall service. Memory,
 * VMM range admission and PCI/device timing remain labelled host adapters. */
#define TEST_PERMISSIONS_ENFORCEMENT
#define main phase0_main
#include "ext4_mount_host.c"
#undef main
#include <unistd.h>
#include "../src/kernel/creds.c"
#include "../src/fs/permission_values.c"
#include "../src/kernel/accounts.c"
uint64_t *vmm_get_active_pml4_virt(void) {return (void *)1;}
bool vmm_validate_user_range(uint64_t *p,uintptr_t a,size_t n,bool output) {
    (void)p;(void)output;return a>=4096 && a<0x800000000000ull && n<=0x800000000000ull-a;
}
int64_t syscall_from_vfs_error(int64_t e) {
    switch (-e) {
        case VFS_ENOMEM:return SYSCALL_ENOMEM;
        case VFS_ENOSPC:return SYSCALL_ENOSPC;
        case VFS_EINVAL:return SYSCALL_EINVAL;
        case VFS_EAGAIN:return SYSCALL_EAGAIN;
        case VFS_EROFS:return SYSCALL_EROFS;
        default:return e ? SYSCALL_EIO : 0;
    }
}
static long flush_cut=-1,write_cut=-1;
static unsigned flush_count,write_count;
static bool offline;
static bool no_headroom;
static bool image_read(block_dev_t *d,uint64_t l,void *p) {
    bool ok=!offline && !fseeko(d->priv,l*d->sector_size,SEEK_SET) && fread(p,1,d->sector_size,d->priv)==d->sector_size;
    /* Labelled superblock free-count adapter; never changes fixture bytes. */
    if (ok && no_headroom && l==1036/d->sector_size) memset((uint8_t *)p+1036%d->sector_size,0,4);
    return ok;
}
static bool image_write(block_dev_t *d,uint64_t l,const void *p) {
    if (offline) return false;
    if (write_cut==(long)write_count++) {offline=true;return false;}
    return !fseeko(d->priv,l*d->sector_size,SEEK_SET) && fwrite(p,1,d->sector_size,d->priv)==d->sector_size;
}
static bool image_flush(block_dev_t *d) {
    if (offline) return false;
    bool cut=flush_cut==(long)flush_count++;
    if (cut && !getenv("ACCOUNT_CUT_AFTER")) {offline=true;return false;}
    bool ok=!fflush(d->priv) && !fsync(fileno(d->priv));
    if (cut) {offline=true;return false;}
    return ok;
}
static account_workspace_t copied;
static creds_t actor;
static int64_t control(unsigned op,account_request_t *req,uint64_t pid) {return accounts_control(op,(uintptr_t)req,sizeof(*req),&actor,pid);}
static account_request_t snapshot(bool secret) {
    return (account_request_t){.size=64,.version=1,.passwd=(uintptr_t)copied.files[0],.group=(uintptr_t)copied.files[1],
        .shadow=secret ? (uintptr_t)copied.files[2] : 0,.passwd_bytes=8192,.group_bytes=8192,.shadow_bytes=secret ? 8192 : 0,.flags=secret};
}
int main(int argc,char **argv) {
    assert(argc==3);unsigned sector=strtoul(argv[2],NULL,10);FILE *f=fopen(argv[1],"r+b");assert(f);
    block_dev_t d={.priv=f,.sector_size=sector,.sector_count=256*1024*1024/sector,.read_sector=image_read,.write_sector=image_write,.flush=image_flush};
    vfs_init();creds_init_root(&actor);ext4_mount_t *fs=NULL;assert(!ext4_prepare_root(&d,true,&fs));
    assert(!accounts_prepare(fs));
    const char *paths[4]={"/etc","/etc/passwd","/etc/group","/etc/shadow"};vfs_node_t *nodes[4];int error;
    for (unsigned i=0;i<4;i++) {nodes[i]=vfs_lookup_at_creds(ext4_prepared_root(fs),paths[i],&actor,&error);assert(nodes[i]);}
    assert(!ext4_root_lock_accounts(fs,nodes));for (unsigned i=0;i<4;i++) vfs_node_put(nodes[i]);
    assert(!ext4_publish_root(fs));
    assert(enter());
    account_request_t identity={.size=64,.version=1};assert(!control(ACCOUNT_STATUS,&identity,101) && identity.generation);
    account_request_t blocked=snapshot(false);assert(control(ACCOUNT_SNAPSHOT,&blocked,101)==SYSCALL_EAGAIN);leave();
    if (getenv("ACCOUNT_RECOVER_ONLY")) {
        account_request_t recovered=snapshot(true);assert(!control(ACCOUNT_SNAPSHOT,&recovered,101));
        assert(!ext4_freeze_and_sync(fs));fclose(f);
        puts("fresh mounted account recovery PASS");return 0;
    }
    account_request_t invalid={.size=63,.version=1};assert(control(ACCOUNT_BEGIN,&invalid,101)==SYSCALL_EINVAL);
    invalid.size=64;invalid.version=2;assert(control(ACCOUNT_BEGIN,&invalid,101)==SYSCALL_EINVAL);
    invalid.version=1;assert(control(ACCOUNT_RECOVER,&invalid,101)==SYSCALL_EINVAL);
    invalid.flags=2;assert(control(ACCOUNT_SNAPSHOT,&invalid,101)==SYSCALL_EINVAL);
    invalid=snapshot(false);invalid.passwd=0;assert(control(ACCOUNT_SNAPSHOT,&invalid,101)==SYSCALL_EFAULT);
    invalid=snapshot(false);invalid.passwd_bytes=8191;assert(control(ACCOUNT_SNAPSHOT,&invalid,101)==SYSCALL_EINVAL);
    account_request_t s=snapshot(true);assert(!control(ACCOUNT_SNAPSHOT,&s,101));uint64_t old_generation=s.generation;
    actor.euid=1000;actor.cap_effective=0;s=snapshot(true);assert(control(ACCOUNT_SNAPSHOT,&s,101)==SYSCALL_EACCES);
    s=snapshot(false);assert(!control(ACCOUNT_SNAPSHOT,&s,101));
    account_request_t lease={.size=64,.version=1};assert(control(ACCOUNT_BEGIN,&lease,101)==SYSCALL_EPERM);creds_init_root(&actor);
    bool cached=fs->metadata_enabled;fs->metadata_enabled=false;
    no_headroom=true;assert(control(ACCOUNT_BEGIN,&lease,101)==SYSCALL_ENOSPC);no_headroom=false;
    fs->metadata_enabled=cached;
    assert(!control(ACCOUNT_BEGIN,&lease,101));account_request_t second={.size=64,.version=1};assert(control(ACCOUNT_BEGIN,&second,102)==SYSCALL_EAGAIN);
    invalid=(account_request_t){.size=64,.version=1,.generation=lease.generation,.token=lease.token+1};
    assert(control(ACCOUNT_FINISH,&invalid,101)==SYSCALL_EINVAL);
    s=snapshot(false);assert(control(ACCOUNT_SNAPSHOT,&s,102)==SYSCALL_EAGAIN);
    assert(vfs_unlink_creds("/etc/shadow",&actor)==-VFS_EROFS);
    assert(vfs_rename_creds("/etc","/moved-etc",&actor)==-VFS_EROFS);
    assert(vfs_rename_creds("/etc/shadow","/root/moved-shadow",&actor)==-VFS_EROFS);
    assert(!vfs_open_creds("/etc/shadow.publish",VFS_O_CREAT|VFS_O_WRONLY,&actor,&error) && error==-VFS_EROFS);
    assert(!vfs_open_creds("/etc/shadow",VFS_O_WRONLY,&actor,&error) && error==-VFS_EROFS);
    assert(!vfs_open_creds("/etc/passwd",VFS_O_WRONLY,&actor,&error) && error==-VFS_EROFS);
    /* Before intent, writer death cancels the lease. */
    accounts_process_exit(101);s=snapshot(true);assert(!control(ACCOUNT_SNAPSHOT,&s,102));
    lease=(account_request_t){.size=64,.version=1,.generation=s.generation};assert(!control(ACCOUNT_BEGIN,&lease,102));
    /* Change all three databases and a password, with a durable private home.
     * Linux crypt verification lives in userdb host tests. */
    assert(!vfs_mkdir_creds("/home/hostnew",0700,&actor));
    assert(!vfs_chown_creds("/home/hostnew",(creds_id_change_t){.value=1101},(creds_id_change_t){.value=1101},&actor));
    /* Fixed libcrypt vector: normal runs verify the actual generator; crash
     * runs reuse those exact bytes instead of repeating 100000 crypt rounds. */
    const char expected_hash[]="$5$rounds=100000$abcdefghijklmnop$JAlrLh9YVBYa4XSl2fqcB6N55eikn17ite1nvkjVHI0";
    char hash[78],shadow[512];
    if (getenv("ACCOUNT_FLUSH_CUT") || getenv("ACCOUNT_WRITE_CUT")) memcpy(hash,expected_hash,sizeof(hash));
    else {assert(db_crypt("a changed account password","abcdefghijklmnop",hash));assert(!strcmp(hash,expected_hash));}
    int n=snprintf(shadow,sizeof(shadow),"root:!:::::::\noperator:%s:::::::\nhostnew:%s:::::::\n",hash,hash);assert(n>0 && n<(int)sizeof(shadow));
    memcpy(copied.files[2],shadow,n);
    account_request_t update=s;update.flags=0;update.token=lease.token;update.shadow_bytes=n;
    const char added_user[]="hostnew:x:1101:1101::/home/hostnew:/bin/shell\n",added_group[]="hostnew:x:1101:\n";
    assert(update.passwd_bytes+sizeof(added_user)<ACCOUNT_FILE_MAX && update.group_bytes+sizeof(added_group)<ACCOUNT_FILE_MAX);
    memcpy(copied.files[0]+update.passwd_bytes,added_user,sizeof(added_user)-1);update.passwd_bytes+=sizeof(added_user)-1;
    memcpy(copied.files[1]+update.group_bytes,added_group,sizeof(added_group)-1);update.group_bytes+=sizeof(added_group)-1;
    fail_alloc=0;assert(control(ACCOUNT_UPDATE,&update,102)==SYSCALL_ENOMEM);fail_alloc=-1;
    flush_count=write_count=0;
    const char *fc=getenv("ACCOUNT_FLUSH_CUT"),*wc=getenv("ACCOUNT_WRITE_CUT");
    if (fc) flush_cut=strtol(fc,NULL,10);
    if (wc) write_cut=strtol(wc,NULL,10);
    if (fc || wc) {
        assert(control(ACCOUNT_UPDATE,&update,102)<0);
        accounts_process_exit(102);assert(offline);
        s=snapshot(true);assert(control(ACCOUNT_SNAPSHOT,&s,101)==SYSCALL_EIO);
        fclose(f);puts("account device interruption failed closed PASS");return 0;
    }
    assert(!control(ACCOUNT_UPDATE,&update,102));assert(update.generation>old_generation);
    printf("ACCOUNT_EVENTS flush=%u write=%u\n",flush_count,write_count);
    account_request_t finish={.size=64,.version=1,.generation=update.generation,.token=lease.token};assert(control(ACCOUNT_FINISH,&finish,103)==SYSCALL_EINVAL);
    assert(!control(ACCOUNT_FINISH,&finish,102));
    account_request_t check={.size=64,.version=1,.generation=old_generation};assert(control(ACCOUNT_CHECK,&check,101)==SYSCALL_EAGAIN);
    check.generation=update.generation;assert(!control(ACCOUNT_CHECK,&check,101));
    s=snapshot(true);assert(!control(ACCOUNT_SNAPSHOT,&s,101));assert(s.shadow_bytes==(unsigned)n && !memcmp(copied.files[2],shadow,n));
    vfs_node_t *sh=vfs_lookup_ref("/etc/shadow",&error);vfs_metadata_t m;assert(sh && !vfs_metadata(sh,&m) && !m.uid && !m.gid && m.mode==(VFS_S_IFREG|0600));vfs_node_put(sh);
    assert(vfs_unlink_creds("/etc/shadow",&actor)==-VFS_EROFS);
    lease=(account_request_t){.size=64,.version=1,.generation=s.generation};assert(!control(ACCOUNT_BEGIN,&lease,104));
    accounts_disable();accounts_process_exit(104);
    identity=(account_request_t){.size=64,.version=1};assert(!control(ACCOUNT_STATUS,&identity,105));
    s=snapshot(false);assert(control(ACCOUNT_SNAPSHOT,&s,105)==SYSCALL_EIO);
    assert(!ext4_freeze_and_sync(fs));account_redo_wipe(&copied);fclose(f);
    printf("actual EXT4/account service PASS sector=%u: snapshots, writer contention/death, stale decisions, OOM, shadow mode, immutable canonical writes\n",sector);
}
