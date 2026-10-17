#ifndef FORTRESS_SSH_HOSTKEY_H
#define FORTRESS_SSH_HOSTKEY_H
#include "types.h"

#define SSH_HOSTKEY_MAX 1024u
#define SSH_HOSTKEY_PUBLIC_MAX 128u
#define SSH_HOSTKEY_FINGERPRINT_MAX 51u
/* Exclusively owned caller workspace, disjoint from all input/output. No
 * allocation or global mutable state. Every valid-workspace call wipes it.
 * Inputs/output must be accessible, nonoverlapping ranges. On failure outputs
 * and output length remain untouched. Success lengths exclude a trailing NUL;
 * encoded/exported text is NUL terminated. Private outputs require caller wipe.
 * Decode accepts only the bounded unencrypted single Ed25519 profile. */
typedef struct {
    uint8_t binary[768];
    char text[SSH_HOSTKEY_MAX];
    uint8_t public_key[32],secret_key[64],signature[64];
} ssh_hostkey_workspace;
int ssh_hostkey_encode(const uint8_t secret[64],uint32_t checkint,
                       char *out,size_t capacity,size_t *length,ssh_hostkey_workspace *);
int ssh_hostkey_decode(const char *text,size_t length,uint8_t secret[64],
                       ssh_hostkey_workspace *);
int ssh_hostkey_generate(char *out,size_t capacity,size_t *length,ssh_hostkey_workspace *);
int ssh_hostkey_public(const uint8_t public_key[32],char *out,size_t capacity,
                       size_t *length,ssh_hostkey_workspace *);
int ssh_hostkey_fingerprint(const uint8_t public_key[32],char *out,size_t capacity,
                            size_t *length,ssh_hostkey_workspace *);
#endif
