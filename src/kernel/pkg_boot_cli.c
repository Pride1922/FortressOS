#include "pkg_boot_cli.h"
#include "vfs.h"
#include "runfs.h"
int pkg_boot_cli_publish(const boot_info_t *info){
    if(!info||!info->installed_modules_valid||!info->has_initramfs||!info->initramfs_vaddr||
       !info->initramfs_size||info->initramfs_size>UINTPTR_MAX-info->initramfs_vaddr)return -VFS_EACCES;
    creds_t root;creds_init_root(&root);int error=0;
    vfs_node_t *source=vfs_bootstrap_lookup_creds("/bin/pkg",&root,&error);
    if(!source)return error?error:-VFS_ENOENT;
    /* Only a direct immutable archive entry is eligible; never delegate
     * metadata/data decisions to a mutable filesystem adapter. */
    vfs_metadata_t meta;int r=source->owns_nodes||source->metadata||source->read||source->write_actor?
        -VFS_EACCES:vfs_metadata(source,&meta);
    if(!r){
        uintptr_t start=info->initramfs_vaddr,address=(uintptr_t)source->data;
        if(meta.type!=VFS_FILE||meta.mode!=(VFS_S_IFREG|0755)||meta.uid||meta.gid||
           !(meta.mnt_flags&VFS_MNT_RDONLY)||!meta.size||meta.size>VFS_MAX_EXEC_SIZE||
           !source->data||source->write||source->truncate||address<start||
           address-start>info->initramfs_size||meta.size>info->initramfs_size-(address-start))r=-VFS_EACCES;
        else r=runfs_publish_boot_tool("pkg",source->data,meta.size);
    }
    vfs_node_put(source);return r;
}
