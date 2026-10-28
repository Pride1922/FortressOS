#ifndef FORTRESS_PKG_BOOT_POLICY_H
#define FORTRESS_PKG_BOOT_POLICY_H
#include "types.h"
/* Independently supplied public authority and boot measurements. No private keys.
 * This native policy is not an on-disk encoding or a provisioning mechanism. */
typedef struct {
 uint8_t selector_keys[8][32],enrollment_keys[8][32],catalog_keys[8][32];
 uint32_t selector_count,selector_threshold,enrollment_count,enrollment_threshold,catalog_count,catalog_threshold;
 uint8_t uuid[5][16],profile[32],platform[32],kernel[32],bootstrap[32];
 uint8_t root_hash[32],catalog_hash[32],selector_floor_hash[32];
 uint64_t now,epoch_floor,root_version,catalog_revision_floor,base_version;
} ps_policy;
#endif
