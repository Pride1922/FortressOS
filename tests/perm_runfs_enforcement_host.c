/* Actual VFS/runfs, pthread lock adapter. No IRQ or guest claim. */
#define TEST_PERMISSIONS_ENFORCEMENT
#define TEST_PERMISSIONS_INTERLEAVING
#define main permissive_fixture_main
#include "perm_fs_host.c"
#undef main
#include "perm_interleave.h"

static void unchanged(const run_node_t *before) {
    assert(!memcmp(before,run_pool,sizeof(run_pool)));
}
int main(void) {
    vfs_init();int error;
    creds_t root,user,other;creds_init_root(&root);
    user=(creds_t){.uid=1001,.euid=1001,.suid=1001,.gid=1002,.egid=1002,.sgid=1002,.umask=0027};
    other=user;other.uid=other.euid=other.suid=2001;
    assert(!vfs_mkdir_creds("/tmp/private",0700,&user));
    assert(!vfs_mkdir_creds("/tmp/public",0777,&root));
    assert(!vfs_mkdir_creds("/tmp/group",02777,&root));assert(!vfs_chmod_creds("/tmp/group",02777,&root));
    creds_id_change_t no_uid={.keep=true},group44={.value=44};
    assert(!vfs_chown_creds("/tmp/group",no_uid,group44,&root));
    file_t *group_file=vfs_open_mode_creds("/tmp/group/file",VFS_O_CREAT|VFS_O_RDWR,06777,&user,&error);assert(group_file);
    vfs_metadata_t group_meta;assert(!vfs_metadata(group_file->node,&group_meta) && group_meta.gid==44 && group_meta.mode==(VFS_S_IFREG|04750));vfs_close(group_file);
    assert(!vfs_mkdir_creds("/tmp/group/sub",0777,&user));
    vfs_node_t *group_dir=vfs_lookup_creds("/tmp/group/sub",&user,&error);assert(group_dir);
    assert(!vfs_metadata(group_dir,&group_meta) && group_meta.gid==44 && group_meta.mode==(VFS_S_IFDIR|02750));vfs_node_put(group_dir);
    assert(!vfs_lookup_creds("/tmp/private/../public",&other,&error) && error==-VFS_EACCES);
    assert(!vfs_lookup_creds("/tmp/missing/../public",&root,&error) && error==-VFS_ENOENT);
    file_t *file=vfs_open_mode_creds("/tmp/private/file",VFS_O_CREAT|VFS_O_RDWR,06777,&user,&error);
    assert(file && !error);
    vfs_metadata_t m;assert(!vfs_metadata(file->node,&m));
    assert(m.mode==(VFS_S_IFREG|06750) && m.uid==1001 && m.gid==1002);
    assert(!vfs_lookup_creds("/tmp/private/file/.",&user,&error) && error==-VFS_ENOTDIR);
    assert(!vfs_lookup_creds("/tmp/private/file/..",&user,&error) && error==-VFS_ENOTDIR);
    assert(!vfs_lookup_creds("/tmp/private/file/",&user,&error) && error==-VFS_ENOTDIR);
    assert(vfs_write_creds(file,"abc",3,&user)==3);
    assert(!vfs_metadata(file->node,&m) && m.mode==(VFS_S_IFREG|0750));
    assert(!vfs_fchmod_creds(file,06770,&root));
    assert(vfs_write_creds(file,NULL,0,&user)==0);
    assert(!vfs_metadata(file->node,&m) && m.mode==(VFS_S_IFREG|06770));
    assert(!vfs_fchmod_creds(file,0,&user));
    assert(vfs_write_creds(file,"d",1,&user)==1); /* admitted fd survives chmod */
    run_node_t *before=malloc(sizeof(run_pool));assert(before);
    memcpy(before,run_pool,sizeof(run_pool));
    assert(vfs_unlink_creds("/tmp/private",&other)==-VFS_EPERM);unchanged(before);
    assert(vfs_rename_creds("/tmp/private","/tmp/stolen",&other)==-VFS_EPERM);unchanged(before);
    assert(vfs_rename_creds("/tmp/private","/tmp/private",&other)==-VFS_EPERM);unchanged(before);
    assert(vfs_chmod_creds("/tmp/private",0777,&other)==-VFS_EPERM);unchanged(before);
    assert(vfs_mkdir_creds("/run/denied",0777,&user)==-VFS_EACCES);unchanged(before);
    assert(!vfs_open_creds("/tmp/private/file",VFS_O_WRONLY|VFS_O_TRUNC,&user,&error) && error==-VFS_EACCES);unchanged(before);
    assert(!vfs_fchmod_creds(file,06770,&root));
    file_t *trunc=vfs_open_creds("/tmp/private/file",VFS_O_WRONLY|VFS_O_TRUNC,&user,&error);
    assert(trunc && !vfs_metadata(trunc->node,&m) && !m.size && m.mode==(VFS_S_IFREG|0770));
    assert(!vfs_close(trunc));
    assert(!vfs_rename_creds("/tmp/private/file","/tmp/private/file",&user));
    assert(vfs_rename_creds("/tmp/private/file/","/tmp/private/file",&user)==-VFS_ENOTDIR);
    assert(!vfs_rename_creds("/tmp/private","/tmp/renamed",&user));
    assert(!vfs_lookup_creds("/tmp/private",&user,&error) && error==-VFS_ENOENT);
    vfs_node_t *up=vfs_lookup_creds("/tmp/renamed/..",&user,&error);
    assert(up==&run_pool[1].node);vfs_node_put(up);
    creds_id_change_t uid={.value=0xffffffff},gid={.value=0xabcdef01};
    assert(!vfs_chown_creds("/tmp/renamed/file",uid,gid,&root));
    assert(!vfs_metadata(file->node,&m) && m.uid==uid.value && m.gid==gid.value);
    assert(!vfs_unlink_creds("/tmp/renamed/file",&user));
    assert(!vfs_fchmod_creds(file,0600,&root));
    assert(vfs_write_creds(file,"e",1,&user)==1);assert(!vfs_close(file));
    assert(!vfs_unlink_creds("/tmp/renamed",&user));
    file=vfs_open_mode_creds("/tmp/zero",VFS_O_CREAT|VFS_O_RDWR,0,&user,&error);
    assert(file && vfs_write_creds(file,"x",1,&user)==1);assert(!vfs_close(file));
    assert(!vfs_unlink_creds("/tmp/zero",&user));
    char path[VFS_MAX_PATH];assert(!vfs_join_path("/tmp","private/../public",path,sizeof(path)));
    assert(!strcmp(path,"/tmp/private/../public"));
    assert(!vfs_canonical_path("/tmp/private/../.././dev/",path,sizeof(path)) && !strcmp(path,"/dev"));
    free(before);
    assert(!vfs_mkdir_creds("/tmp/race",0777,&root));assert(!vfs_chmod_creds("/tmp/race",0777,&root));
    pthread_t worker=admission_start(1,"/tmp/race/new",&user);
    assert(!vfs_chmod_creds("/tmp/race",0700,&root));
    run_node_t *snapshot=malloc(sizeof(run_pool));assert(snapshot);memcpy(snapshot,run_pool,sizeof(run_pool));
    for (unsigned i=0;i<RUNFS_NODES;i++) if (!strcmp(snapshot[i].node.path,"/tmp/race")) {assert(snapshot[i].refs==1);snapshot[i].refs--;}
    assert(admission_finish(worker)==-VFS_EACCES);unchanged(snapshot);free(snapshot);
    assert(!vfs_chmod_creds("/tmp/race",01777,&root));
    file=vfs_open_creds("/tmp/race/victim",VFS_O_CREAT|VFS_O_RDWR,&user,&error);assert(file);vfs_close(file);
    worker=admission_start(2,"/tmp/race/victim",&user);
    assert(!vfs_rename_creds("/tmp/race/victim","/tmp/previous",&root));
    file=vfs_open_creds("/tmp/race/victim",VFS_O_CREAT|VFS_O_RDWR,&root,&error);assert(file);vfs_close(file);
    snapshot=malloc(sizeof(run_pool));assert(snapshot);memcpy(snapshot,run_pool,sizeof(run_pool));
    for (unsigned i=0;i<RUNFS_NODES;i++) if (!strcmp(snapshot[i].node.path,"/tmp/race")) {assert(snapshot[i].refs==1);snapshot[i].refs--;}
    assert(admission_finish(worker)==-VFS_EPERM);unchanged(snapshot);free(snapshot);
    puts("PASS runfs controlled VFS interleavings: chmod after old admission; rename/new-owner after name resolution, full pool unchanged on denial");
    parts[0].parent=&usb;parts[0].block_dev=(block_dev_t){.name="sdap1",.sector_size=512,.sector_count=8};devfs_add_usb_partitions();
    assert(!vfs_open_creds("/dev/sdap1",VFS_O_RDONLY,&user,&error) && error==-VFS_EACCES);
    creds_t disk_user=user;disk_user.groups[0]=6;disk_user.ngroups=1;
    assert(!vfs_open_creds("/dev/sdap1",VFS_O_RDONLY,&disk_user,&error) && error==-VFS_EPERM);
    disk_user.cap_effective=CAP_SYS_RAWIO;
    file=vfs_open_creds("/dev/sdap1",VFS_O_RDONLY,&disk_user,&error);assert(file);vfs_close(file);
    assert(!vfs_open_creds("/dev/sdap1",VFS_O_WRONLY,&root,&error) && error==-VFS_EROFS);
    vfs_node_t nodev={.mode=VFS_S_IFCHR|0666,.mnt_flags=VFS_MNT_NODEV};
    assert(vfs_permission(&nodev,VFS_MAY_READ,&root)==-VFS_EACCES);
    assert(vfs_chmod_creds("/dev/null",0600,&user)==-VFS_EPERM);
    puts("PASS actual runfs admission: original walk, sticky/no-op, zero-effect denial, umask, metadata, retained descriptor and set-ID publication");
    return 0;
}
