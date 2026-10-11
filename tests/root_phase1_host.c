/* Actual EXT4/VFS namespace and exec snapshots; host exclusion adapters. */
#define TEST_PERMISSIONS_ENFORCEMENT
#define main phase0_main
#include "ext4_mount_host.c"
#undef main
#include <unistd.h>

static bool image_read(block_dev_t *dev,uint64_t lba,void *bytes) {
    FILE *stream=dev->priv;
    return fseeko(stream,(off_t)(lba*dev->sector_size),SEEK_SET)==0 &&
        fread(bytes,1,dev->sector_size,stream)==dev->sector_size;
}
static bool image_write(block_dev_t *dev,uint64_t lba,const void *bytes) {
    FILE *stream=dev->priv;
    return fseeko(stream,(off_t)(lba*dev->sector_size),SEEK_SET)==0 &&
        fwrite(bytes,1,dev->sector_size,stream)==dev->sector_size;
}
static bool image_flush(block_dev_t *dev) {
    FILE *stream=dev->priv;
    return fflush(stream)==0 && fsync(fileno(stream))==0;
}

#include "../src/kernel/creds.c"
#include "../src/fs/permission_values.c"
#include "../src/kernel/spawn_security.h"

static size_t root_writes;
static bool counted_write(block_dev_t *dev,uint64_t lba,const void *bytes) {
    root_writes++;return image_write(dev,lba,bytes);
}
static int fake_access(vfs_node_t *node,unsigned mask,const creds_t *actor) {
    (void)node;(void)mask;(void)actor;return 0;
}
static int64_t short_read(vfs_node_t *n,uint64_t off,void *buf,size_t len) {
    (void)n;if (off || !len) return 0;
    size_t take=len<3 ? len : 3;memset(buf,0,take);return take;
}
int main(int argc,char **argv) {
    assert(argc==4);size_t expected_size;uint8_t *expected=load(argv[3],&expected_size);
    unsigned sector=strtoul(argv[2],NULL,10);
    FILE *stream=fopen(argv[1],"r+b");assert(stream);
    block_dev_t dev={.sector_size=sector,.sector_count=256*1024*1024/sector,
        .read_sector=image_read,.write_sector=counted_write,.flush=image_flush,.priv=stream};
    vfs_init();creds_t root,user;creds_init_root(&root);user=root;
    user.uid=user.euid=user.suid=user.gid=user.egid=user.sgid=1000;user.cap_effective=0;
    vfs_node_t *bootstrap=g_vfs_root,*tty=vfs_get_terminal_node();
    const char backing[]="stable bootstrap";
    vfs_node_t *boot=vfs_create_node("/bootstrap-proof",VFS_FILE,sizeof(backing),backing);assert(boot);
    boot->mode=VFS_S_IFREG|0755;file_t *retained=vfs_open("/bootstrap-proof",VFS_O_RDONLY);assert(retained);
    ext4_mount_t *fs=NULL;size_t baseline=live;
    assert(ext4_prepare_root(&dev,false,&fs)==-VFS_EROFS && !fs && live==baseline && !root_writes);
    fail_alloc=0;assert(ext4_prepare_root(&dev,true,&fs)==-VFS_ENOMEM && !fs && live==baseline);fail_alloc=-1;
    fail_read=0;assert(ext4_prepare_root(&dev,true,&fs)==-VFS_EIO && !fs && live==baseline && !root_writes);fail_read=-1;
    assert(!ext4_prepare_root(&dev,true,&fs));
    assert(!vfs_boot_namespace_ready());
    vfs_node_t *detached=ext4_prepared_root(fs);assert(detached && g_vfs_root==bootstrap);
    int err;vfs_node_t *login=vfs_lookup_at_creds(detached,"/bin/login",&root,&err);assert(login && !err);
    assert(!strcmp(login->path,"/bin/login"));
    assert(ext4_discard_prepared_root(fs)==-VFS_EAGAIN);vfs_node_put(login);
    assert(!ext4_freeze_and_sync(fs));assert(!ext4_discard_prepared_root(fs));assert(live==baseline && vfs_boot_namespace_ready());
    assert(!ext4_prepare_root(&dev,true,&fs));
#ifdef TEST_ROOT_PHASE2_ACCOUNTS
    const char *account_paths[]={"/etc","/etc/passwd","/etc/group","/etc/shadow"};
    vfs_node_t *account_nodes[4];
    for (unsigned i=0;i<4;i++) {account_nodes[i]=vfs_lookup_at_creds(ext4_prepared_root(fs),account_paths[i],&root,&err);assert(account_nodes[i]);}
    assert(!ext4_root_lock_accounts(fs,account_nodes));
    for (unsigned i=0;i<4;i++) {
        vfs_metadata_t value;assert(!vfs_metadata(account_nodes[i],&value) && (value.mnt_flags & VFS_MNT_RDONLY));
        assert(e4_state(account_nodes[i])==-VFS_EROFS);vfs_node_put(account_nodes[i]);
    }
#endif
    assert(!ext4_publish_root(fs));
#ifdef TEST_ROOT_PHASE2_ACCOUNTS
    bool handled=false;assert(!ext4_sync_installed_root(&handled) && handled);
    assert(!vfs_open_creds("/etc/shadow",VFS_O_RDWR,&root,&err) && err==-VFS_EROFS);
    assert(vfs_unlink_creds("/etc/shadow",&root)==-VFS_EROFS);
    assert(vfs_rename_creds("/etc/passwd","/passwd-moved",&root)==-VFS_EROFS);
    assert(vfs_chmod_creds("/etc/shadow",0644,&root)==-VFS_EROFS);
    assert(vfs_mkdir_creds("/etc/new-dir",0755,&root)==-VFS_EROFS);
    assert(vfs_rename_creds("/etc","/etc-moved",&root)==-VFS_EROFS);
    assert(vfs_unlink_creds("/etc",&root)==-VFS_EROFS);
    assert(!vfs_mkdir_creds("/new-etc",0755,&root));
    assert(vfs_rename_creds("/new-etc","/etc",&root)==-VFS_EROFS);
    assert(!vfs_unlink_creds("/new-etc",&root));
#endif
    assert(ext4_publish_root(fs)==-VFS_EINVAL);
    size_t old_reads=reads,old_writes=root_writes;ext4_mount_t *rejected=NULL;
    assert(ext4_prepare_root(&dev,true,&rejected)==-VFS_EPERM && !rejected && reads==old_reads && root_writes==old_writes);
    assert(vfs_lookup("/dev/tty")==tty && vfs_lookup("/dev")==g_boot_volatile[0]);
    assert(vfs_lookup("/tmp")==g_boot_volatile[2]);
    vfs_node_t *up=vfs_lookup_creds("/tmp/..",&user,&err);assert(up==g_vfs_root);vfs_node_put(up);
    assert(vfs_unlink("/dev")==-VFS_EPERM && vfs_rename("/run","/elsewhere")==-VFS_EPERM);
    up=vfs_lookup_creds("/../../bin/../",&user,&err);assert(up==g_vfs_root);vfs_node_put(up);
    assert(!vfs_bootstrap_lookup_creds("/bin/login",&root,&err));
    boot=vfs_bootstrap_lookup_creds("/bootstrap-proof",&root,&err);assert(boot && boot->data==backing);vfs_node_put(boot);
    file_t *login_file=vfs_open_exec_creds("/bin/login",&root,&err);assert(login_file);
    size_t pinned=live;vfs_metadata_t m;void *bytes=NULL;
    fail_alloc=0;assert(vfs_exec_image(login_file,&root,&m,&bytes)==-VFS_ENOMEM && !bytes);fail_alloc=-1;
    fail_read=0;assert(vfs_exec_image(login_file,&root,&m,&bytes)==-VFS_EIO && !bytes);fail_read=-1;
    assert(live==pinned);assert(!vfs_exec_image(login_file,&root,&m,&bytes));
    assert(m.size==expected_size && !memcmp(bytes,expected,expected_size) && !(m.mnt_flags & VFS_MNT_NOSUID));kfree(bytes);
    assert(!vfs_open_creds("/etc/shadow",VFS_O_RDONLY,&user,&err) && err==-VFS_EACCES);
    for (unsigned i=0;i<3;i++) {const char *hidden[]={"/dev/hidden","/run/hidden","/tmp/hidden"};
        assert(!vfs_lookup_creds(hidden[i],&root,&err) && err==-VFS_ENOENT);}
    file_t *sudo=vfs_open_exec_creds("/bin/sudo",&user,&err);assert(sudo);
    assert(!vfs_exec_image(sudo,&user,&m,&bytes));creds_t exec;
    assert(spawn_credentials(&user,&m,&exec) && !exec.euid);kfree(bytes);vfs_close(sudo);
    /* Mounted nodes retain authoritative bytes while unrelated cache leaves churn. */
    char path[64];
    for (unsigned i=0;i<1100;i++) {
        snprintf(path,sizeof(path),"/cache-%u",i);
        vfs_node_t *node=vfs_lookup_creds(path,&root,&err);
        if (!node && err==-VFS_ENOENT) node=vfs_create_attrs_ref(path,VFS_FILE,&(vfs_create_attrs_t){.mode=0755},&err);
        assert(node && !err);vfs_node_put(node);
    }
    assert(fs->nodes<=1024);
    for (unsigned i=0;i<1100;i++) {
        snprintf(path,sizeof(path),"/cache-%u",i);
        vfs_node_t *node=vfs_lookup_creds(path,&root,&err);assert(node && !err);vfs_node_put(node);
    }
    assert(((e4_node_t *)login_file->node)->opens && !vfs_exec_image(login_file,&root,&m,&bytes));kfree(bytes);
    for (unsigned i=0;i<100;i++) {assert(!vfs_exec_image(login_file,&root,&m,&bytes));kfree(bytes);}
    vfs_node_t *pins[1100]={0};unsigned count=0;
    for (;count<1100;count++) {
        snprintf(path,sizeof(path),"/cache-%u",count);
        pins[count]=vfs_lookup_creds(path,&root,&err);
        if (!pins[count]) {assert(err==-VFS_EFBIG);break;}
    }
    assert(count<1100 && fs->nodes==1024);
    for (unsigned i=0;i<count;i++) vfs_node_put(pins[i]);
    vfs_close(login_file);
    char retained_bytes[64];assert(vfs_read(retained,retained_bytes,sizeof(retained_bytes))==sizeof(backing));
    assert(!memcmp(retained_bytes,backing,sizeof(backing)));vfs_close(retained);
    assert(!ext4_freeze_and_sync(fs));
    sudo=vfs_open_exec_creds("/bin/sudo",&user,&err);assert(sudo);
    assert(!vfs_exec_image(sudo,&user,&m,&bytes) && (m.mnt_flags & VFS_MNT_RDONLY));kfree(bytes);vfs_close(sudo);
    vfs_boot_seal_namespace();assert(vfs_boot_publish_root(bootstrap)==-VFS_EPERM);
    /* Simulated reboot: production deliberately has no root unmount. */
    g_vfs_root=bootstrap;g_root_published=false;
    e4_active=NULL;fs->journal_mounted=false;e4_engine_busy=false;e4_engine_dispose(fs->engine);
    g_vfs_root->children=NULL;kfree(boot);kfree(g_vfs_root);g_vfs_root=NULL;
    assert(!live);assert(!fclose(stream));
    /* Legacy mounted data remains nosuid even with a set-ID inode. */
    stream=fopen(argv[1],"r+b");assert(stream);dev.priv=stream;vfs_init();
    assert(!ext4_mount_journal_fixture(&dev,"/mnt",admitted,&fs));
    sudo=vfs_open_exec_creds("/mnt/bin/sudo",&user,&err);assert(sudo);
    assert(!vfs_exec_image(sudo,&user,&m,&bytes));assert(m.mnt_flags & VFS_MNT_NOSUID);
    assert(!spawn_credentials(&user,&m,&exec) && exec.euid==1000);kfree(bytes);vfs_close(sudo);
    assert(!ext4_freeze_and_sync(fs));teardown();fclose(stream);
    /* Short/zero reads from immutable adapters fail without publishing bytes. */
    vfs_node_t fake={.type=VFS_FILE,.mode=VFS_S_IFREG|0755,.size=10,.read=short_read};
    file_t fake_file={.node=&fake};bytes=NULL;
    assert(vfs_exec_image(&fake_file,&root,&m,&bytes)==-VFS_EIO && !bytes && !live);
    fake.owns_nodes=true;
    assert(vfs_exec_image(&fake_file,&root,&m,&bytes)==-VFS_EOPNOTSUPP && !bytes && !live);
    fake.mnt_flags=VFS_MNT_NOSUID;fake.permission_actor=fake_access;
    assert(vfs_exec_image(&fake_file,&root,&m,&bytes)==-VFS_EIO && !bytes && !live);
    fake.size=VFS_MAX_EXEC_SIZE+1;assert(vfs_exec_image(&fake_file,&root,&m,&bytes)==-VFS_EFBIG && !bytes && !live);
    free(expected);
    puts("PASS Phase1 owned snapshots, root rollback/publication, volatile identity, DAC/set-ID/nosuid, IO/OOM, 1100-node churn, pinned exec, cleanup");
    return 0;
}
