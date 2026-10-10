/* Actual EXT2/VFS with pthread exclusion and host I/O adapters. No IRQ claim. */
#define TEST_PERMISSIONS_ENFORCEMENT
#define TEST_PERMISSIONS_INTERLEAVING
#include "ext4_host/spinlock.h"
#define HOST_SPINLOCK_H
#define main permissive_ext2_fixture_main
#include "ext2_host.c"
#undef main
#include "perm_interleave.h"
int main(int argc,char **argv) {
    assert(argc==3);FILE *fp=fopen(argv[1],"rb");assert(fp);assert(!fseek(fp,0,SEEK_END));
    disk_size=ftell(fp);rewind(fp);pending_disk=malloc(disk_size);durable_disk=malloc(disk_size);
    assert(pending_disk && durable_disk && fread(disk,1,disk_size,fp)==disk_size);fclose(fp);
    memcpy(durable_disk,disk,disk_size);reset();
    block_dev_t dev={.sector_size=512,.sector_count=disk_size/512,.read_sector=read_sector,.write_sector=write_sector,.flush=flush_sector};
    assert(ext2_mount_rw(&dev,"/mnt"));
    creds_t root,user;creds_init_root(&root);
    user=(creds_t){.uid=1001,.euid=1001,.suid=1001,.gid=1001,.egid=1001,.sgid=1001,.umask=0027};
    assert(!vfs_mkdir_creds("/mnt/public",0777,&root));assert(!vfs_chmod_creds("/mnt/public",0777,&root));
    assert(!vfs_mkdir_creds("/mnt/private",0700,&root));
    assert(!vfs_mkdir_creds("/mnt/group",02777,&root));assert(!vfs_chmod_creds("/mnt/group",02777,&root));
    creds_id_change_t no_uid={.keep=true},group44={.value=44};
    assert(!vfs_chown_creds("/mnt/group",no_uid,group44,&root));
    int group_error;file_t *group_file=vfs_open_mode_creds("/mnt/group/file",VFS_O_CREAT|VFS_O_RDWR,06777,&user,&group_error);assert(group_file);
    vfs_metadata_t group_meta;assert(!vfs_metadata(group_file->node,&group_meta) && group_meta.gid==44 && group_meta.mode==(VFS_S_IFREG|04750));vfs_close(group_file);
    assert(!vfs_mkdir_creds("/mnt/group/sub",0777,&user));
    vfs_node_t *group_dir=vfs_lookup_creds("/mnt/group/sub",&user,&group_error);assert(group_dir);
    assert(!vfs_metadata(group_dir,&group_meta) && group_meta.gid==44 && group_meta.mode==(VFS_S_IFDIR|02750));vfs_node_put(group_dir);
    assert(!vfs_mkdir_creds("/mnt/sticky",01777,&root));assert(!vfs_chmod_creds("/mnt/sticky",01777,&root));
    int error;file_t *f=vfs_open_mode_creds("/mnt/sticky/victim",VFS_O_CREAT|VFS_O_RDWR,0600,&root,&error);assert(f);vfs_close(f);
    uint8_t *before=malloc(disk_size*2);assert(before);memcpy(before,disk,disk_size);memcpy(before+disk_size,durable_disk,disk_size);
    size_t w=writes,q=flushes,allocated=live;
#define DENIED(expr,expected) do {assert((expr)==(expected));assert(writes==w && flushes==q && live==allocated);assert(!memcmp(before,disk,disk_size) && !memcmp(before+disk_size,durable_disk,disk_size));} while (0)
    DENIED(vfs_mkdir_creds("/mnt/private/denied",0777,&user),-VFS_EACCES);
    DENIED(vfs_chmod_creds("/mnt/private",0777,&user),-VFS_EPERM);
    creds_id_change_t uid={.value=1001},keep={.keep=true};
    DENIED(vfs_chown_creds("/mnt/private",uid,keep,&user),-VFS_EPERM);
    DENIED(vfs_unlink_creds("/mnt/sticky/victim",&user),-VFS_EPERM);
    DENIED(vfs_rename_creds("/mnt/sticky/victim","/mnt/sticky/victim",&user),-VFS_EPERM);
    assert(!vfs_lookup_creds("/mnt/private/../public",&user,&error) && error==-VFS_EACCES);
    assert(!vfs_lookup_creds("/mnt/missing/../public",&root,&error) && error==-VFS_ENOENT);
    vfs_node_t *up=vfs_lookup_creds("/mnt/..",&user,&error);assert(up==g_vfs_root);vfs_node_put(up);
    assert(!vfs_rename_creds("/mnt/private","/mnt/private",&root));
    f=vfs_open_mode_creds("/mnt/public/mode",VFS_O_CREAT|VFS_O_RDWR,06777,&user,&error);assert(f);
    vfs_metadata_t m;assert(!vfs_metadata(f->node,&m) && m.mode==(VFS_S_IFREG|06750) && m.uid==1001 && m.gid==1001);
    assert(vfs_write_creds(f,"abc",3,&user)==3);
    assert(!vfs_metadata(f->node,&m) && m.mode==(VFS_S_IFREG|0750));
    assert(!vfs_fchmod_creds(f,0,&user));assert(vfs_write_creds(f,"d",1,&user)==1);
    assert(!vfs_fchmod_creds(f,06770,&root));
    file_t *trunc=vfs_open_creds("/mnt/public/mode",VFS_O_WRONLY|VFS_O_TRUNC,&user,&error);assert(trunc);
    assert(!vfs_metadata(trunc->node,&m) && !m.size && m.mode==(VFS_S_IFREG|0770));vfs_close(trunc);
    uid.value=0xffffffff;creds_id_change_t gid={.value=0xabcdef01};
    assert(!vfs_chown_creds("/mnt/public/mode",uid,gid,&root));
    assert(!vfs_metadata(f->node,&m) && m.uid==uid.value && m.gid==gid.value);vfs_close(f);
    pthread_t worker=admission_start(1,"/mnt/public/race",&user);
    assert(!vfs_chmod_creds("/mnt/public",0700,&root));
    memcpy(before,disk,disk_size);memcpy(before+disk_size,durable_disk,disk_size);w=writes;q=flushes;allocated=live;
    DENIED(admission_finish(worker),-VFS_EACCES);
    assert(!vfs_chmod_creds("/mnt/public",0777,&root));
    f=vfs_open_creds("/mnt/sticky/race-victim",VFS_O_CREAT|VFS_O_RDWR,&user,&error);assert(f);vfs_close(f);
    worker=admission_start(2,"/mnt/sticky/race-victim",&user);
    assert(!vfs_rename_creds("/mnt/sticky/race-victim","/mnt/public/previous",&root));
    f=vfs_open_creds("/mnt/sticky/race-victim",VFS_O_CREAT|VFS_O_RDWR,&root,&error);assert(f);vfs_close(f);
    memcpy(before,disk,disk_size);memcpy(before+disk_size,durable_disk,disk_size);w=writes;q=flushes;allocated=live;
    DENIED(admission_finish(worker),-VFS_EPERM);
    puts("PASS EXT2 controlled VFS interleavings: chmod/new victim ownership revalidated under ext2_lock, no disk/flush effects on denial");
    assert(ext2_sync_all());fp=fopen(argv[2],"wbx");assert(fp && fwrite(durable_disk,1,disk_size,fp)==disk_size);fclose(fp);
    while (live) free(allocations[--live]);
    free(before);free(pending_disk);free(durable_disk);
    puts("PASS actual EXT2 actor admission: whole disk/flush/allocation denial, original walk, metadata, creation and admitted fd set-ID clearing");return 0;
}
