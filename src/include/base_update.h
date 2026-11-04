#ifndef FORTRESS_BASE_UPDATE_H
#define FORTRESS_BASE_UPDATE_H
#include "kernel_update.h"
#include "boot_timing.h"
#include "root_boot.h"
#include "../../user/pkg/base.h"
#define BASE_ESP_BYTES INSTALL_FAT_BYTES
#define BASE_STATE_CONFIRMED 1u
#define BASE_STATE_READY 2u
#define BASE_STATE_PREPARING 3u
#define BASE_STATE_FLOOR 4u
typedef struct {
    uint64_t build,abi,catalog,root,generation;
    uint32_t phase,slot;
    uint8_t platform[32],catalog_hash[32],root_hash[32],artifact[32];
    uint8_t kernel_hash[32],bootstrap_hash[32],readiness_hash[32];
} base_state;
typedef struct {
    kernel_update_work_t disk;
    uint32_t bootstrap[2],state_offset[2],floor_offset[2];
    base_state states[2],floors[2];
    unsigned running,invalid,floor_valid,floor;
    bool mixed,wrote;
} base_update_work;
size_t base_update_menu(char *,size_t,const char *,unsigned);
int base_state_decode(const uint8_t [512],base_state *);
void base_state_encode(const base_state *,uint8_t [512]);
/* Owned host template only. Exact bank sizes, no disk I/O. */
int base_update_seed(void *,size_t,base_update_work *,uint64_t,uint64_t,const uint8_t [32],const uint8_t [512]);
/* All buffers are private and disjoint; caller serializes writes and pins disk.
 * Only enumerated ESP extents are written. No root/GPT/FAT/directory writes. */
int base_update_prepare(const kernel_update_io_t *,void *,base_update_work *,const uint8_t [512]);
/* Optional boot-only proof. The caller validated these immutable bytes and
 * digests against the same running readiness record in this boot. */
typedef struct {
    const void *kernel,*bootstrap;
    root_readiness_digests_t digests;
} base_loaded_bank;
int base_update_prepare_timed(const kernel_update_io_t *,void *,base_update_work *,const uint8_t [512],
                              boot_timing_t *,const base_loaded_bank *);
int base_update_apply(const kernel_update_io_t *,void *,base_update_work *,const base_artifact *,const base_state *,const void *,size_t);
int base_update_select(const kernel_update_io_t *,void *,base_update_work *,bool);
#endif
