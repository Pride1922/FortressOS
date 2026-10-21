#ifndef FORTRESS_PKG_VERIFY_H
#define FORTRESS_PKG_VERIFY_H
#include "format.h"
#include "../tools/digest.h"
/* Synchronous read-only stream. Callback <=requested, 0 EOF, negative error.
 * Caller-owned context belongs in BSS/heap, never the user stack. Serialized
 * calls only: existing gzip implementation owns one static decoder workspace.
 * Successful integrity checks are NOT signature authorization or ELF loading. */
typedef long (*fpk_read_fn)(void *, void *, size_t);
typedef struct {
    uint8_t header[128], manifest_bytes[FPK_MANIFEST_MAX], io[512], tar[512];
    uint8_t elf_header[64], elf_phdrs[16*56], hash[32];
    fpk_envelope envelope;
    fpk_manifest manifest;
    digest_t artifact_hash, payload_hash, member_hash;
    fpk_read_fn read;
    void *reader;
    uint64_t compressed, expanded, member_offset, phoff;
    size_t tar_used;
    unsigned member, zero_blocks;
    int failed, verified;
} fpk_verify_context;
/* expected_hash/length are external artifact identity (e.g. verified catalog).
 * Caller must separately establish their authenticity/platform/base policy.
 * Arguments and input buffers must not alias the mutable verifier context.
 * All failure paths leave verified=0. Context is scratch, invalid after error. */
int fpk_verify(fpk_verify_context *, fpk_read_fn, void *,
               uint64_t expected_length, const uint8_t expected_hash[32]);
#endif
