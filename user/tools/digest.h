#ifndef FORTRESS_DIGEST_H
#define FORTRESS_DIGEST_H
#include "types.h"
typedef enum { DIGEST_MD5, DIGEST_SHA256 } digest_kind_t;
/* Fixed memory independent of input length. update rejects bit-length overflow. */
typedef struct {
    uint32_t state[8];
    uint64_t bytes;
    uint8_t block[64];
    size_t used;
    digest_kind_t kind;
} digest_t;
void digest_init(digest_t *ctx, digest_kind_t kind);
bool digest_update(digest_t *ctx, const void *data, size_t size);
void digest_final(digest_t *ctx, uint8_t out[32]);
size_t digest_size(digest_kind_t kind);
int md5sum_main(int argc, char **argv);
int sha256sum_main(int argc, char **argv);
#endif
