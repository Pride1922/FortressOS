#ifndef FORTRESS_DEVFS_H
#define FORTRESS_DEVFS_H
#include "vfs.h"
/* Boot-lifetime immutable nodes; attach before user publication. tty preserves
 * the existing terminal node identity. No internal NVMe exposure, no writes.
 * Raw reads join the boot-selected /mnt EXT4 or EXT2 exclusion, per sector;
 * entry holds no locks. Mount selection cannot change after publication. */
void devfs_init(vfs_node_t *root,vfs_node_t *tty,vfs_node_t *null_node);
void devfs_add_usb_partitions(void);
#endif
