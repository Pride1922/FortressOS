#ifndef FORTRESS_SSHBUF_H
#define FORTRESS_SSHBUF_H

#include "types.h"

#define SSHBUF_MAX_ALLOC (64 * 1024)
#define SSHBUF_DEFAULT_CAP (4096)

#define SSH_ERR_SUCCESS          0
#define SSH_ERR_INVALID_ARG     -1
#define SSH_ERR_NO_MEM          -2
#define SSH_ERR_BUFFER_TOO_SMALL -3
#define SSH_ERR_INVALID_FORMAT  -4

typedef struct {
    uint8_t *data;
    size_t alloc;
    size_t size;
    size_t off;
    size_t max_size;
    bool read_only;
    bool dynamic;
} sshbuf_t;

int sshbuf_init(sshbuf_t *buf, size_t initial_alloc, size_t max_size);
int sshbuf_init_fixed(sshbuf_t *buf, void *storage, size_t capacity);
int sshbuf_init_from(sshbuf_t *buf, const void *data, size_t len);
void sshbuf_free(sshbuf_t *buf);
void sshbuf_reset(sshbuf_t *buf);

size_t sshbuf_len(const sshbuf_t *buf);
size_t sshbuf_size(const sshbuf_t *buf);
const uint8_t *sshbuf_ptr(const sshbuf_t *buf);
int sshbuf_consume(sshbuf_t *buf, size_t len);

int sshbuf_put_u8(sshbuf_t *buf, uint8_t val);
int sshbuf_put_u32(sshbuf_t *buf, uint32_t val);
int sshbuf_put_u64(sshbuf_t *buf, uint64_t val);
int sshbuf_put_raw(sshbuf_t *buf, const void *data, size_t len);
int sshbuf_put_string(sshbuf_t *buf, const void *data, size_t len);
int sshbuf_put_cstring(sshbuf_t *buf, const char *str);
int sshbuf_put_mpint(sshbuf_t *buf, const uint8_t *data, size_t len);

int sshbuf_get_u8(sshbuf_t *buf, uint8_t *val);
int sshbuf_get_u32(sshbuf_t *buf, uint32_t *val);
int sshbuf_get_u64(sshbuf_t *buf, uint64_t *val);
int sshbuf_get_raw(sshbuf_t *buf, void *dest, size_t len);
int sshbuf_get_string_ptr(sshbuf_t *buf, const uint8_t **ptr, size_t *len);
int sshbuf_get_mpint(sshbuf_t *buf, uint8_t *out, size_t max_out, size_t *out_len);
int sshbuf_peek_u8(const sshbuf_t *buf, uint8_t *val);

#endif /* FORTRESS_SSHBUF_H */
