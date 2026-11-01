#ifndef FORTRESS_KERNEL_UPDATE_H
#define FORTRESS_KERNEL_UPDATE_H
#include "install_fat.h"
#define KUPDATE_BANK_BYTES (8u*1024*1024)
#define KUPDATE_PREFIX_BYTES (32u*1024*1024)
/* Offline, exclusive caller; reads never mount a filesystem. Callbacks return
 * zero only on complete success. Any failure is terminal; no mutation retry. */
typedef struct {
    void *context;
    uint64_t sectors;
    int (*read)(void *,uint64_t,void *,unsigned);
    int (*write)(void *,uint64_t,const void *,unsigned);
    int (*flush)(void *);
} kernel_update_io_t;
typedef struct {
    install_fat_work_t fat;
    uint8_t primary[512],backup[512],entries[16384],backup_entries[16384];
    uint8_t verify[4096],ready[512];
    uint32_t kernel[2],readiness[2],configs[4],config_sizes[4],bootstrap,bootstrap_size;
    uint64_t esp,root,root_sectors;
    unsigned active;
    bool prepared;
} kernel_update_work_t;
/* Exact canonical menu shared by baseline personalization and updater admission.
 * Zero return means invalid input or insufficient capacity. */
size_t kernel_update_menu(char *,size_t,const char *,unsigned);
/* Read-only preflight binds a specific predecessor SHA256 (padded 8 MiB bank).
 * Workspace, prefix and candidate must be disjoint. Only the enrolled profile
 * (64 MiB ESP, 512-byte clusters, contiguous reserved files) is supported. */
int kernel_update_prepare(const kernel_update_io_t *,void *,size_t,kernel_update_work_t *,const char *);
/* Requires the same exclusively held I/O capability as prepare. Writes only the
 * inactive bank/readiness and four config selector sectors, with flush/readback
 * barriers. Root, GPT, FAT, directory and bootstrap bytes are never written. */
int kernel_update_apply(const kernel_update_io_t *,void *,kernel_update_work_t *,const void *,size_t);
#endif
