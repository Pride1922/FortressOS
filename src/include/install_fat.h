#ifndef FORTRESS_INSTALL_FAT_H
#define FORTRESS_INSTALL_FAT_H
#include "types.h"
#define INSTALL_FAT_BYTES (64u*1024*1024)
#define INSTALL_FAT_BITMAP_BYTES 32768u
#define INSTALL_FAT_SLOTS 6u
/* Private owned 64MiB FAT32 template only. No device I/O or allocation.
 * Complete allocation graph, both FAT copies and ASCII VFAT names are checked
 * before mutation. Slots must have their exact reserved capacity; shortening
 * frees their tails and updates both FSInfo copies. Existing paths stay intact. */
typedef struct {
    uint8_t owned[INSTALL_FAT_BITMAP_BYTES];
    uint32_t entry[INSTALL_FAT_SLOTS],first[INSTALL_FAT_SLOTS],capacity[INSTALL_FAT_SLOTS];
    uint32_t fat,data,clusters,cluster_bytes,fat_sectors,root,fsinfo,backup;
    uint32_t visits,free_clusters,next_free;
    unsigned found;
    unsigned names;
    uint32_t kernel_first,kernel_size;
    uint32_t bank_first,bank_size,ready_b_first,ready_b_size;
    uint32_t bootstrap_b_first,bootstrap_b_size,bootstrap_b_entry;
    uint32_t base_first[2],base_size[2],floor_first[2],floor_size[2];
    size_t backing_bytes;
    bool installed;
    uint8_t name_hashes[8192][16];
} install_fat_work_t;
typedef struct {
    const void *bootstrap,*config;
    size_t bootstrap_size,config_size;
} install_fat_patch_t;
/* Slots: /boot/bootstrap.tar 8MiB, /boot/readiness.v1 512 bytes;
 * four /limine.conf copies at root, boot, boot/limine and EFI/BOOT: 4KiB.
 * On rejection the template is untouched. Workspace must be disjoint. */
/* Read-only installed-volume inspection. The backing may be a prefix >=1 MiB;
 * every allocated data cluster must fit in that prefix. Slot sizes are actual
 * installed lengths rather than reserved template capacities. */
int install_fat_inspect(const void *,size_t,install_fat_work_t *);
/* Optional A/B template enrollment; both kernel files are identical 8 MiB ELF
 * banks. Zero means ordinary legacy template; one means validated A/B template. */
int install_fat_complete(const install_fat_work_t *);
int install_fat_banked(const install_fat_work_t *);
int install_fat_bank_readiness_lba(const install_fat_work_t *,uint64_t *);
int install_fat_validate(const void *,size_t,install_fat_work_t *);
/* Reserved-slot template validation plus exact /boot/fortress.elf bytes. */
int install_fat_kernel_matches(const void *,size_t,const void *,size_t,install_fat_work_t *);
int install_fat_personalize(void *,size_t,const install_fat_patch_t *,install_fat_work_t *,uint64_t *readiness_lba);
#endif
