#ifndef FORTRESS_BASE_ARTIFACT_H
#define FORTRESS_BASE_ARTIFACT_H
#include "catalog.h"
#include "base_release.h"
#define BASE_HEADER_BYTES 256u
#define BASE_BANK_BYTES (8u*1024u*1024u)
/* The catalog v1 object bound is unchanged. Individual banks remain 8 MiB. */
#define BASE_ARTIFACT_MAX (128u+FPK_MANIFEST_MAX+FPK_COMPRESSED_MAX)
/* Fixed namespace map for layout 1; no artifact-supplied paths or scripts. */
typedef struct {
    uint64_t build,predecessor_min,predecessor_max,abi;
    const uint8_t *platform,*kernel,*bootstrap,*kernel_hash,*bootstrap_hash;
    size_t kernel_bytes,bootstrap_bytes;
} base_artifact;
int base_archive_command(const uint8_t *,size_t,fpk_span);
int base_archive_release(const uint8_t *,size_t,uint64_t *,uint64_t *,uint8_t [32]);
int base_artifact_parse(const uint8_t *,size_t,base_artifact *);
int base_artifact_verify(const uint8_t *,size_t,const uint8_t *,size_t,
                         const fpk_catalog_policy *,base_artifact *);
#endif
