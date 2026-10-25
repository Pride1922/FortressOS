#ifndef FORTRESS_PKG_EXECUTOR_H
#define FORTRESS_PKG_EXECUTOR_H
#include "pkg_plan.h"
#define PX_RECORD 8192u
#define PX_DIRS 64u
#define PX_SLOTS 17u
enum {PX_CLOSED,PX_BUILD,PX_COMMIT,PX_RETIRE,PX_DELETE,PX_ABORT};
/* Private trusted embedding, never a user request or a device selector. The
 * independent admission policy supplies fixed anchors, filesystem identity and
 * minimum record identity. Source/catalog backing is copied before mutations. */
typedef struct {uint32_t inode,generation;uint64_t physical;} px_anchor;
typedef struct {
    fpk_catalog_policy catalog;
    pi_resources resources;
    px_anchor anchors[8]; /* record, work, bin, docs, root, pkg, usr, usr/share */
    uint8_t filesystem[16],layout[32],minimum_hash[32];uint64_t minimum_sequence;
    uint8_t record_key[32]; /* kernel-only local MAC key; not a signing key */
    uintptr_t (*current_actor)(void);
    bool (*authorized)(void); /* called outside filesystem exclusion per step */
} px_policy;
/* Format is explicit little-endian, not this native scratch structure. */
typedef struct {
    uint64_t sequence,generation,floor,root,catalog,start_sequence,start_generation;
    uint32_t phase,installed,operation,made,published,trash,gone;
    uint8_t floor_hash[32],start_hash[32],plan_hash[32],catalog_hash[32],root_hash[32];
    uint64_t total[2],remaining[2],charged[2];
    struct {uint32_t inode,generation;uint64_t size;} files[PX_SLOTS];
    struct {uint32_t inode,generation;} directories[PX_DIRS];
    uint8_t deleted[8];
} px_record;
#endif
