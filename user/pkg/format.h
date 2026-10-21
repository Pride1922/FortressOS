#ifndef FORTRESS_PKG_FORMAT_H
#define FORTRESS_PKG_FORMAT_H
#include <stdint.h>
#include <stddef.h>

/* PKG-1 candidate envelope only. Success is structural, never authorization.
 * No native-struct wire encoding. Caller owns buffers; no allocation or I/O. */
#define FPK_HEADER_SIZE 128u
#define FPK_MANIFEST_MAX 16384u
#define FPK_COMPRESSED_MAX (8u * 1024u * 1024u)
#define FPK_EXPANDED_MAX (16u * 1024u * 1024u)
typedef struct {
    uint32_t manifest_bytes;
    uint64_t compressed_bytes, expanded_bytes, container_bytes;
    uint8_t manifest_sha256[32], payload_sha256[32];
} fpk_envelope;
/* Exactly 128 bytes; leaves output unchanged on all errors. No aliasing. */
int fpk_parse_envelope(const uint8_t *bytes, size_t length, fpk_envelope *out);
/* Length-delimited identity; lowercase alnum with single interior hyphens. */
int fpk_identity_valid(const uint8_t *bytes, size_t length);
typedef struct { const uint8_t *bytes; uint16_t length; } fpk_span;
typedef struct {
    uint8_t role;
    uint16_t mode;
    uint64_t size;
    const uint8_t *sha256;
    fpk_span path;
} fpk_file;
typedef struct {
    uint64_t build, base_min, base_max, archive_bytes;
    const uint8_t *platform_id;
    fpk_span package, command, version, description;
    uint16_t file_count;
    fpk_file files[16];
} fpk_manifest;
/* Borrowed immutable views: input must outlive output and cannot alias it.
 * Failure leaves output unchanged. Success checks syntax/policy, not trust,
 * platform compatibility, base ownership, ELF or payload hashes. */
int fpk_parse_manifest(const uint8_t *bytes, size_t length, fpk_manifest *out);
#endif
