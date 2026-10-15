#ifndef FORTRESS_INSTALL_PAYLOAD_H
#define FORTRESS_INSTALL_PAYLOAD_H
#include "types.h"

/* Transport-independent installer workbench. This API grants no hardware access.
 * Caller owns immutable trusted payload bytes and an exclusive admitted target.
 * All callbacks return 0 only for exact completion. No write is retried. */
#define INSTALL_PAYLOAD_MANIFEST_BYTES 256u
#define INSTALL_ESP_BYTES (64u*1024*1024)
#define INSTALL_ROOT_BYTES (256u*1024*1024)
#define INSTALL_BIOS_BYTES (1024u*1024)
#define INSTALL_MIN_SECTORS (324u*2048)
#define INSTALL_ESP_LBA 4096u
#define INSTALL_ROOT_LBA 135168u
#define INSTALL_BIOS_LBA 2048u
#define INSTALL_CHUNK_BYTES 4096u

typedef enum {
    INSTALL_STAGE_VALIDATE, INSTALL_STAGE_ADMIT, INSTALL_STAGE_INVALIDATE,
    INSTALL_STAGE_LAYOUT, INSTALL_STAGE_PAYLOAD, INSTALL_STAGE_PROVISION,
    INSTALL_STAGE_VERIFY, INSTALL_STAGE_COMPLETE
} install_stage_t;
typedef enum { INSTALL_ESP, INSTALL_ROOT, INSTALL_BIOS, INSTALL_MBR, INSTALL_ROLES } install_role_t;
typedef struct {
    void *context;
    /* Role paths are fixed by adapter; manifests cannot supply pathnames. */
    int (*read)(void *,install_role_t,uint64_t,void *,size_t);
} install_source_t;
typedef struct {
    void *context;
    uint64_t sectors;
    uint32_t sector_size;
    /* Rechecks the caller's selected generation/digest and source/root exclusions.
     * dry-run never calls any target callback, including admission/flush. */
    int (*admit)(void *);
    int (*read)(void *,uint64_t,uint32_t,void *);
    int (*write)(void *,uint64_t,uint32_t,const void *);
    int (*flush)(void *);
    /* Controlled raw-to-filesystem handoff: provision, validate and freeze root.
     * No installed user code may run. Readiness is still invalid during this. */
    int (*provision)(void *);
    /* Optional unlocked, bounded stage notification; never authorizes I/O. */
    void (*progress)(void *,install_stage_t);
} install_target_t;
typedef struct {
    /* GPT on-disk little-endian UUIDs; all distinct, nonzero RFC4122 v4. */
    uint8_t disk_guid[16],partition_guid[3][16];
    /* Personalized ESP already contains zero readiness bytes in the fixed slot.
     * Slot is relative to ESP, sector-aligned, exact 512-byte file. Adapter must
     * validate its FAT chain, entry, both FATs and slot location before admission. */
    uint64_t readiness_lba;
    /* Zero preserves legacy fixed-root fixtures. Otherwise partition sectors;
     * filesystem expansion is owned by the exclusive provisioning callback. */
    uint64_t root_sectors;
    uint8_t readiness[512];
} install_plan_t;
typedef struct {
    install_stage_t stage;
    int error;
    uint64_t copied_bytes;
    bool complete;
} install_result_t;
/* Caller-owned heap/static workspace; no large frame on a 16KiB kernel stack.
 * Keep disjoint from immutable source/manifest/plan and adapter I/O storage. */
typedef struct {
    uint8_t data[4096],verify[4096],first[512],mbr[512];
} install_payload_work_t;
/* Installer-only expansion of the audited 4 KiB/32768-block/128-inode seed.
 * Caller retains exclusive target ownership and invalid readiness throughout.
 * No general mounted resize, journal or rollback authority. */
#define INSTALL_ROOT_MAX_BLOCKS 268435456u
typedef struct { uint8_t descriptors[262144],super[1024],block[4096],verify[4096]; } install_expand_work_t;
int install_root_expand(const install_target_t *,uint64_t root_sectors,install_expand_work_t *);
/* Manifest wire format (256 bytes): FOSPAY01 at 0, LE version=1 at 8,
 * LE size=256 at 12; four LE u64 role lengths at 16; role SHA256s at 48;
 * zero reserved [176,224); SHA256 of [0,224) at 224. Exact fixed lengths
 * 64MiB/256MiB/1MiB/440 bytes. Hashes detect corruption, not authenticity.
 * Validates all payload bytes before the first target callback. */
int install_payload_validate(const void *,size_t,const install_source_t *,uint8_t scratch[4096]);
int install_payload_deploy(const void *,size_t,const install_source_t *,
    const install_target_t *,const install_plan_t *,bool dry_run,install_payload_work_t *,install_result_t *);
#endif
