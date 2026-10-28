#ifndef FORTRESS_PKG_TRUST_FLOOR_H
#define FORTRESS_PKG_TRUST_FLOOR_H
#include "pkg_boot_policy.h"
#define TF_BYTES 1024u
#define TF_BODY 640u
#define TF_CONTRACT 3u
/* Checkpoint signed by the independent selector role. Immutable
 * accessible inputs, disjoint out. No allocation, I/O or private keys. */
typedef struct {
    uint64_t epoch,counter[5],issued,expires; /* root, catalog, build, grant, state */
    uint32_t contract;
    uint8_t identity[80],platform[32],kernel[32],bootstrap[32];
    uint8_t root[32],catalog[32],artifact[32],selector[32],locator[128],wire_hash[32];
} tf_floor;
typedef struct {
    uint64_t counter[5];
    uint8_t root[32],catalog[32],artifact[32];
} tf_observed;
int tf_decode(const uint8_t wire[TF_BYTES],const ps_policy *,tf_floor *);
int tf_transition(const tf_floor *,const tf_floor *,uint64_t now);
int tf_admit(const tf_floor *,const tf_observed *);
/* Check the trusted minimum and the exact known pre-content/completion offers.
 * A future/unknown durable checkpoint never falls back to an older policy. */
int tf_boot(const tf_floor *,const tf_floor *,const tf_floor *,const tf_floor *,const ps_policy *);
#endif
