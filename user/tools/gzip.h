#ifndef FORTRESS_GZIP_H
#define FORTRESS_GZIP_H

#include "types.h"

#define GZIP_MAGIC1         0x1F
#define GZIP_MAGIC2         0x8B
#define GZIP_CM_DEFLATE     0x08

/* GZIP Header FLG bitmasks */
#define GZIP_FLG_FTEXT      0x01
#define GZIP_FLG_FHCRC      0x02
#define GZIP_FLG_FEXTRA     0x04
#define GZIP_FLG_FNAME      0x05 /* bit 3 is 0x08 */
#define GZIP_FLG_FNAME_MASK 0x08
#define GZIP_FLG_FCOMMENT   0x10
#define GZIP_FLG_RESERVED   0xE0

#define DEFLATE_WINDOW_SIZE 32768
#define GZIP_MAX_OUTPUT     (100ULL * 1024 * 1024) /* 100 MB decompression bomb limit */

/* Return status codes */
#define GZIP_OK                 0
#define GZIP_ERR_IO            -1
#define GZIP_ERR_TRUNCATED     -2
#define GZIP_ERR_CORRUPT       -3
#define GZIP_ERR_DIST_OVERFLOW -4
#define GZIP_ERR_BOMB          -5
#define GZIP_ERR_CRC           -6
#define GZIP_ERR_SIZE          -7
#define GZIP_ERR_MULTI_MEMBER  -8
#define GZIP_ERR_HEADER        -9

uint32_t gzip_crc32(uint32_t init, const void *buf, size_t len);
int gzip_main_internal(int argc, char **argv, bool default_decompress);
int gzip_main(int argc, char **argv);
int gunzip_main(int argc, char **argv);
/* Serialized, non-reentrant read-only decoder adapter. Callbacks return a
 * positive prefix, zero EOF (read only), or negative error. No CLI/file actions.
 * Shares the existing static decoder workspace; caller must exclude CLI use. */
typedef long (*gzip_read_fn)(void *, void *, size_t);
typedef long (*gzip_write_fn)(void *, const void *, size_t);
int gzip_decode_callbacks(gzip_read_fn read, gzip_write_fn write, void *context);

#endif /* FORTRESS_GZIP_H */
