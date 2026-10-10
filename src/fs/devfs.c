#include "devfs.h"
#include "gpt.h"
#include "ext2.h"
#include "ext4.h"
#include "string.h"

static vfs_node_t directory,console_node,partitions[GPT_MAX_PARTITIONS];
static bool initialized,partitions_added;
static int64_t read_partition(vfs_node_t *node,uint64_t offset,void *buf,size_t len) {
    block_dev_t *dev=node->fs_private;
    uint8_t sector[4096];
    if (!dev || !dev->sector_size || dev->sector_size>sizeof(sector)) return -VFS_EIO;
    if (offset>=node->size) return 0;
    if (len>node->size-offset) len=node->size-offset;
    if (len>64u*1024u) len=64u*1024u;
    size_t copied=0;
    while (copied<len) {
        uint64_t at=offset+copied;size_t within=at%dev->sector_size;
        size_t n=dev->sector_size-within;if (n>len-copied) n=len-copied;
        bool handled;
        bool ok=ext4_device_read_sector(dev,at/dev->sector_size,sector,&handled);
        if (!handled) ok=ext2_device_read_sector(dev,at/dev->sector_size,sector);
        if (!ok) return copied ? (int64_t)copied : -VFS_EIO;
        memcpy((uint8_t *)buf+copied,sector+within,n);copied+=n;
    }
    return copied;
}
void devfs_init(vfs_node_t *root,vfs_node_t *tty,vfs_node_t *null_node) {
    partitions_added=false;
    memset(partitions,0,sizeof(partitions));
    initialized=true;
    directory=(vfs_node_t){.type=VFS_DIRECTORY,.mode=VFS_S_IFDIR|0755};
    memcpy(directory.name,"dev",4);memcpy(directory.path,"/dev",5);
    directory.parent=root;directory.next=root->children;root->children=&directory;
    tty->mode=VFS_S_IFCHR|0666;tty->gid=5;tty->parent=&directory;
    null_node->mode=VFS_S_IFCHR|0666;null_node->parent=&directory;
    console_node=*tty;console_node.mode=VFS_S_IFCHR|0620;
    memcpy(console_node.name,"console",8);memcpy(console_node.path,"/dev/console",13);
    directory.children=tty;tty->next=null_node;null_node->next=&console_node;console_node.next=NULL;
}
void devfs_add_usb_partitions(void) {
    if (!initialized || partitions_added) return;
    partitions_added=true;unsigned count=0;
    for (size_t i=0;i<gpt_get_partition_count() && count<GPT_MAX_PARTITIONS;i++) {
        gpt_partition_t *part=gpt_get_partition(i);
        if (!part || !part->parent || strcmp(part->parent->name,"sda")) continue;
        block_dev_t *dev=&part->block_dev;
        if (!dev->sector_size || dev->sector_size>4096 || dev->sector_count>UINT64_MAX/dev->sector_size) continue;
        vfs_node_t *n=&partitions[count++];
        *n=(vfs_node_t){.type=VFS_FILE,.mode=VFS_S_IFBLK|0660,.gid=6,
            .mnt_flags=VFS_MNT_RDONLY,.size=dev->sector_count*dev->sector_size,.read=read_partition,.fs_private=dev,.parent=&directory};
        size_t length=strlen(dev->name);if (length>=VFS_MAX_NAME) continue;
        memcpy(n->name,dev->name,length+1);memcpy(n->path,"/dev/",5);memcpy(n->path+5,dev->name,length+1);
        n->next=directory.children;directory.children=n;
    }
}
