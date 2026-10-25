#ifndef FORTRESS_PKG_PLAN_H
#define FORTRESS_PKG_PLAN_H
#include "../../user/pkg/catalog.h"
#define PI_FILES 16u
#define PI_WIRE 8192u
#define PI_INVENTORY_MAX (256u*1024u)
enum {PI_INSTALL=1,PI_REMOVE=2,PI_NOOP=3};
/* Trusted embedding supplies a coherent admitted-root observation. Presence is
 * not ownership: removal compares every identity/metadata/hash to the durable
 * inventory. No inode, path, map, budget or verified flag comes from Ring 3. */
typedef struct {
    uint32_t inode,generation,mode,uid,gid,links,type,pins;
    uint64_t size,blocks;
    uint8_t hash[32];
} pi_object;
typedef struct {
    uint64_t sequence,generation,build_floor,build,inventory_bytes;
    uint32_t installed,file_count;
    uint8_t package[32],command[32];uint16_t package_length,command_length;
    uint8_t hash[32],artifact[32],floor_artifact[32];
    pi_object owned[PI_FILES];
} pi_inventory;
typedef struct {
    uint32_t block_size,journal_images,orphan_slots;
    uint64_t free[2],remaining[2],capacity[2],memory_bytes;
    uint8_t platform[32];uint64_t base;
    const fpk_span *base_names;size_t base_count;
} pi_resources;
typedef struct {
    uint16_t role,mode,path_length,reserved;
    uint64_t size,blocks;
    uint8_t hash[32],path[100];
    uint32_t inode,generation;
} pi_file;
typedef struct {
    uint32_t operation,file_count,parent_count,reserved;
    uint64_t sequence,generation,build,root,catalog,artifact_bytes;
    uint64_t total[2],memory_bytes,journal_images,orphan_slots;
    uint8_t package[32],command[32],platform[32],artifact[32],catalog_hash[32],previous[32],base_hash[32],hash[32];
    uint16_t package_length,command_length;
    pi_file files[PI_FILES];
} pi_plan;
typedef struct {fpk_verify_context verify;pi_plan candidate;uint8_t wire[PI_WIRE];} pi_workspace;
/* Whole artifact/catalog verification runs afresh. All inputs remain immutable
 * and accessible; serialized gzip verifier. Scratch/out and immutable inputs
 * must be disjoint. No allocation or filesystem action. Output preserved on
 * every failure. Consumers must check return values; alias rejection can leave
 * old scratch unchanged. Stream callback/context are trusted, disjoint storage. */
int pi_prepare(unsigned,const uint8_t *,size_t,const fpk_catalog_policy *,
               const uint8_t *,size_t,fpk_read_fn,void *,const pi_inventory *,
               const pi_object [PI_FILES],const pi_resources *,pi_workspace *,pi_plan *);
/* Canonical value encoding; never serialize native structure padding. This
 * encoding is not a signature or authority to execute a supplied plan. */
int pi_encode(const pi_plan *,uint8_t [PI_WIRE]);
#endif
