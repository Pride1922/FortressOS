#define TEST_PERMISSIONS_ENFORCEMENT
#define main retained_permission_main
#include "perm_fs_host.c"
#undef main
#include "../src/kernel/pkg_boot_cli.h"
static uint8_t module[128*1024+128],readback[128*1024+1];
int main(void){
    vfs_init();creds_t root,user;creds_init_root(&root);
    user=(creds_t){.uid=1000,.euid=1000,.suid=1000,.gid=1000,.egid=1000,.sgid=1000};
    for(size_t i=0;i<sizeof module;i++)module[i]=(uint8_t)(i*31+7);
    assert(vfs_create_node_kernel("/bin",VFS_DIRECTORY,0,NULL));
    vfs_node_t *source=vfs_create_node_kernel("/bin/pkg",VFS_FILE,128*1024,module+64);assert(source);
    source->mode=VFS_S_IFREG|0755;source->mnt_flags=VFS_MNT_RDONLY;
    boot_info_t info={.installed_modules_valid=true,.has_initramfs=true,
        .initramfs_vaddr=(uintptr_t)module,.initramfs_size=sizeof module};
    info.installed_modules_valid=false;assert(pkg_boot_cli_publish(&info)==-VFS_EACCES);info.installed_modules_valid=true;
    source->uid=1000;assert(pkg_boot_cli_publish(&info)==-VFS_EACCES);source->uid=0;
    source->mode|=04000;assert(pkg_boot_cli_publish(&info)==-VFS_EACCES);source->mode=VFS_S_IFREG|0755;
    info.initramfs_size=64+source->size-1;assert(pkg_boot_cli_publish(&info)==-VFS_EACCES);info.initramfs_size=sizeof module;
    assert(!pkg_boot_cli_publish(&info));assert(pkg_boot_cli_publish(&info)==-VFS_EEXIST);
    int error=0;file_t *file=vfs_open_mode_creds("/run/pkg",VFS_O_RDONLY,0,&user,&error);assert(file&&!error);
    size_t at=0;for(;;){int64_t got=vfs_read(file,readback+at,sizeof readback-at);assert(got>=0&&got<=4096);if(!got)break;at+=(size_t)got;}
    assert(at==128*1024&&!memcmp(readback,module+64,at));
    vfs_metadata_t meta;void *image=NULL;assert(!vfs_exec_image(file,&user,&meta,&image));
    assert(meta.size==at&&meta.mode==(VFS_S_IFREG|0755)&&(meta.mnt_flags&VFS_MNT_RDONLY)&&(meta.mnt_flags&VFS_MNT_NOSUID));
    assert(image&&!memcmp(image,module+64,at));kfree(image);
    uint64_t position=7;
    assert(file->node->write(file->node,&position,false,"x",1)==-VFS_EROFS&&position==7);
    assert(file->node->truncate(file->node,0)==-VFS_EROFS);
    assert(run_pool[0].node.unlink(&run_pool[0].node,"pkg")==-VFS_EROFS);
    assert(!vfs_open_mode_creds("/run/pkg",VFS_O_RDWR,0,&root,&error)&&error==-VFS_EROFS);
    assert(vfs_chmod_creds("/run/pkg",0777,&root)==-VFS_EROFS);
    assert(vfs_unlink_creds("/run/pkg",&root)==-VFS_EROFS);
    assert(vfs_rename_creds("/run/pkg","/run/other",&root)==-VFS_EROFS);
    file_t *ordinary=vfs_open_mode_creds("/run/ordinary",VFS_O_CREAT|VFS_O_RDWR,0644,&root,&error);assert(ordinary);
    assert(vfs_write_creds(ordinary,readback,sizeof readback,&root)==4096);
    assert(vfs_write_creds(ordinary,readback,1,&root)==-VFS_ENOSPC);vfs_close(ordinary);
    assert(vfs_rename_creds("/run/ordinary","/run/pkg",&root)==-VFS_EROFS);
    vfs_close(file);
    vfs_boot_seal_namespace();assert(runfs_publish_boot_tool("late",module,16)==-VFS_EINVAL);
    puts("PASS bootstrap CLI alias: module bounds, metadata, exact bounded reads/EXEC snapshot, immutable root operations, ordinary quota, late-publication refusal");
}
