#ifndef FORTRESS_PKG_CATALOG_H
#define FORTRESS_PKG_CATALOG_H
#include "verify.h"
#define FPK_CATALOG_MAX (256u*1024u)
/* Trusted caller policy, NOT parsed from the untrusted catalog. No key loading,
 * root rotation, persistent floor publication or production provisioning here. */
typedef struct {
    uint8_t repository[16], keys[8][32];
    unsigned key_count, threshold;
    uint64_t root_version, revision_floor, now;
    uint8_t floor_sha256[32]; /* full catalog identity when revision == floor */
} fpk_catalog_policy;
typedef struct {
    const uint8_t *entries;
    uint16_t count;
    uint64_t revision;
    uint8_t sha256[32];
} fpk_catalog;
typedef struct {
    fpk_span package;
    uint64_t build, length;
    const uint8_t *sha256;
} fpk_catalog_target;
/* Candidate wire: signed 72-byte role/UUID/version/time/count header, sorted
 * 81-byte entries, then sorted unique 96-byte signer-ID/signature records.
 * Immutable borrowed input; outputs cannot alias inputs and stay unchanged on
 * error. No host/runtime allocations or mutation. Zero success, -1 rejection. */
int fpk_catalog_verify(const uint8_t *,size_t,const fpk_catalog_policy *,fpk_catalog *);
int fpk_catalog_find(const fpk_catalog *,const uint8_t *,size_t,fpk_catalog_target *);
/* Verifies catalog afresh, resolves requested package, verifies artifact, then
 * binds artifact manifest package/build to catalog. No installation authority,
 * platform/installed build floor or durable trust-floor publication implied. */
int fpk_verify_authorized(fpk_verify_context *,const uint8_t *,size_t,
                         const fpk_catalog_policy *,const uint8_t *,size_t,
                         fpk_read_fn,void *);
#endif
