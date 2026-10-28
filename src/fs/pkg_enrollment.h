#ifndef FORTRESS_PKG_ENROLLMENT_H
#define FORTRESS_PKG_ENROLLMENT_H
#include <stdint.h>
#include <stddef.h>
#define PE_BODY 2464u
#define PE_MAX (PE_BODY+8u*96u)
#define PE_SLOT 1024u
/* Read-only enrollment codec. No I/O, key loading, durable floor publication,
 * inode lookup, mount admission or production caller. Caller owns accessible
 * immutable inputs; writable outputs must be disjoint from every input. */
typedef struct {
    uint8_t keys[8][32],uuid[5][16],descriptor_sha256[32];
    unsigned key_count,threshold;
    uint64_t epoch;
} pe_policy;
typedef struct { const uint8_t *wire; size_t length; } pe_enrollment;
typedef struct {
    uint64_t sequence,operation_generation,size;
    uint32_t inode,generation,parent,parent_generation;
    unsigned active,role,phase,extents,ranges;
    const uint8_t *wire;
} pe_origin;
int pe_verify(const uint8_t *,size_t,const pe_policy *,pe_enrollment *);
/* Enrollment must be the retained successful result of pe_verify. Expected
 * sequence and account generation come from independent durable state, not slot.
 * Slot checksum is corruption detection only. All failures preserve output. */
int pe_origin_decode(const uint8_t *,size_t,const pe_enrollment *,
                     uint64_t sequence,uint64_t account_generation,pe_origin *);
/* Canonical wire-to-wire encoding: validates first, then copies exact bytes.
 * Does not construct or bless arbitrary native structs or sign records. */
int pe_origin_encode(const uint8_t *,size_t,const pe_enrollment *,uint64_t,uint64_t,
                     uint8_t *,size_t);
#endif
