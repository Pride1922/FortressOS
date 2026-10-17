#include "sshbuf.h"
#include "crypto.h"
#include "string.h"

#ifdef CRYPTO_HOST_TEST
#include <stdlib.h>
void *ssh_alloc(size_t s) { return malloc(s); }
void ssh_free(void *p) { free(p); }
void *ssh_realloc(void *p, size_t s) { return realloc(p, s); }
#else
#define SSH_POOL_SIZE (256 * 1024)
static uint8_t s_pool[SSH_POOL_SIZE] __attribute__((aligned(16)));

typedef struct block_hdr {
    size_t size;
    int used;
    struct block_hdr *next;
} block_hdr_t;

static block_hdr_t *s_first_block = NULL;

static void pool_init(void) {
    if (s_first_block) return;
    s_first_block = (block_hdr_t *)s_pool;
    s_first_block->size = SSH_POOL_SIZE - sizeof(block_hdr_t);
    s_first_block->used = 0;
    s_first_block->next = NULL;
}

void *ssh_alloc(size_t s) {
    if (s == 0) return NULL;
    pool_init();
    s = (s + 15) & ~15ULL;
    block_hdr_t *curr = s_first_block;
    while (curr) {
        if (!curr->used && curr->size >= s) {
            if (curr->size >= s + sizeof(block_hdr_t) + 32) {
                block_hdr_t *next = (block_hdr_t *)((uint8_t *)curr + sizeof(block_hdr_t) + s);
                next->size = curr->size - s - sizeof(block_hdr_t);
                next->used = 0;
                next->next = curr->next;
                curr->next = next;
                curr->size = s;
            }
            curr->used = 1;
            return (void *)((uint8_t *)curr + sizeof(block_hdr_t));
        }
        curr = curr->next;
    }
    return NULL;
}

void ssh_free(void *p) {
    if (!p) return;
    pool_init();
    if ((uint8_t *)p < s_pool || (uint8_t *)p >= s_pool + SSH_POOL_SIZE) return;
    block_hdr_t *hdr = (block_hdr_t *)((uint8_t *)p - sizeof(block_hdr_t));
    hdr->used = 0;
    block_hdr_t *curr = s_first_block;
    while (curr && curr->next) {
        if (!curr->used && !curr->next->used) {
            curr->size += sizeof(block_hdr_t) + curr->next->size;
            curr->next = curr->next->next;
        } else {
            curr = curr->next;
        }
    }
}

void *ssh_realloc(void *p, size_t s) {
    if (!p) return ssh_alloc(s);
    if (s == 0) { ssh_free(p); return NULL; }
    block_hdr_t *hdr = (block_hdr_t *)((uint8_t *)p - sizeof(block_hdr_t));
    if (hdr->size >= s) return p;
    void *np = ssh_alloc(s);
    if (!np) return NULL;
    memcpy(np, p, hdr->size < s ? hdr->size : s);
    ssh_free(p);
    return np;
}
#endif

int sshbuf_init(sshbuf_t *buf, size_t initial_alloc, size_t max_size) {
    if (!buf) return SSH_ERR_INVALID_ARG;
    memset(buf, 0, sizeof(*buf));
    if (max_size == 0) max_size = SSHBUF_MAX_ALLOC;
    if (initial_alloc == 0) initial_alloc = SSHBUF_DEFAULT_CAP;
    if (initial_alloc > max_size) initial_alloc = max_size;

    buf->data = (uint8_t *)ssh_alloc(initial_alloc);
    if (!buf->data) return SSH_ERR_NO_MEM;
    buf->alloc = initial_alloc;
    buf->max_size = max_size;
    buf->dynamic = true;
    buf->read_only = false;
    return SSH_ERR_SUCCESS;
}

int sshbuf_init_fixed(sshbuf_t *buf, void *storage, size_t capacity) {
    if (!buf || !storage || capacity == 0) return SSH_ERR_INVALID_ARG;
    memset(buf, 0, sizeof(*buf));
    buf->data = (uint8_t *)storage;
    buf->alloc = capacity;
    buf->max_size = capacity;
    buf->dynamic = false;
    buf->read_only = false;
    return SSH_ERR_SUCCESS;
}

int sshbuf_init_from(sshbuf_t *buf, const void *data, size_t len) {
    if (!buf || (!data && len > 0)) return SSH_ERR_INVALID_ARG;
    memset(buf, 0, sizeof(*buf));
    buf->data = (uint8_t *)data;
    buf->alloc = len;
    buf->size = len;
    buf->max_size = len;
    buf->read_only = true;
    buf->dynamic = false;
    return SSH_ERR_SUCCESS;
}

void sshbuf_free(sshbuf_t *buf) {
    if (!buf) return;
    if (buf->dynamic && buf->data) {
        crypto_wipe(buf->data, buf->alloc);
        ssh_free(buf->data);
    }
    memset(buf, 0, sizeof(*buf));
}

void sshbuf_reset(sshbuf_t *buf) {
    if (!buf) return;
    buf->size = 0;
    buf->off = 0;
}

size_t sshbuf_len(const sshbuf_t *buf) {
    if (!buf || buf->off > buf->size) return 0;
    return buf->size - buf->off;
}

size_t sshbuf_size(const sshbuf_t *buf) {
    return buf ? buf->size : 0;
}

const uint8_t *sshbuf_ptr(const sshbuf_t *buf) {
    if (!buf || !buf->data || buf->off > buf->size) return NULL;
    return buf->data + buf->off;
}

int sshbuf_consume(sshbuf_t *buf, size_t len) {
    if (!buf) return SSH_ERR_INVALID_ARG;
    if (len > sshbuf_len(buf)) return SSH_ERR_BUFFER_TOO_SMALL;
    buf->off += len;
    return SSH_ERR_SUCCESS;
}

int sshbuf_compact(sshbuf_t *buf) {
    if (!buf) return SSH_ERR_INVALID_ARG;
    if (buf->read_only) return SSH_ERR_INVALID_ARG;
    if (buf->off == 0) return SSH_ERR_SUCCESS;
    size_t rem = buf->size - buf->off;
    if (rem > 0) {
        memmove(buf->data, buf->data + buf->off, rem);
    }
    buf->off = 0;
    buf->size = rem;
    return SSH_ERR_SUCCESS;
}

static int sshbuf_reserve(sshbuf_t *buf, size_t needed) {
    if (!buf) return SSH_ERR_INVALID_ARG;
    if (buf->read_only) return SSH_ERR_INVALID_ARG;
    if (needed > buf->max_size || buf->size > buf->max_size - needed)
        return SSH_ERR_BUFFER_TOO_SMALL;

    size_t target = buf->size + needed;
    if (target <= buf->alloc) return SSH_ERR_SUCCESS;

    if (!buf->dynamic) return SSH_ERR_BUFFER_TOO_SMALL;

    size_t new_alloc = buf->alloc ? buf->alloc : 1024;
    while (new_alloc < target) {
        if (new_alloc > buf->max_size / 2) {
            new_alloc = buf->max_size;
            break;
        }
        new_alloc *= 2;
    }
    if (new_alloc < target) return SSH_ERR_BUFFER_TOO_SMALL;

    uint8_t *new_data = (uint8_t *)ssh_realloc(buf->data, new_alloc);
    if (!new_data) return SSH_ERR_NO_MEM;
    buf->data = new_data;
    buf->alloc = new_alloc;
    return SSH_ERR_SUCCESS;
}

int sshbuf_put_u8(sshbuf_t *buf, uint8_t val) {
    int r = sshbuf_reserve(buf, 1);
    if (r) return r;
    buf->data[buf->size++] = val;
    return SSH_ERR_SUCCESS;
}

int sshbuf_put_u32(sshbuf_t *buf, uint32_t val) {
    int r = sshbuf_reserve(buf, 4);
    if (r) return r;
    buf->data[buf->size++] = (uint8_t)(val >> 24);
    buf->data[buf->size++] = (uint8_t)(val >> 16);
    buf->data[buf->size++] = (uint8_t)(val >> 8);
    buf->data[buf->size++] = (uint8_t)val;
    return SSH_ERR_SUCCESS;
}

int sshbuf_put_u64(sshbuf_t *buf, uint64_t val) {
    int r = sshbuf_reserve(buf, 8);
    if (r) return r;
    for (int i = 7; i >= 0; i--) {
        buf->data[buf->size++] = (uint8_t)(val >> (i * 8));
    }
    return SSH_ERR_SUCCESS;
}

int sshbuf_put_raw(sshbuf_t *buf, const void *data, size_t len) {
    if (!data && len > 0) return SSH_ERR_INVALID_ARG;
    if (len == 0) return SSH_ERR_SUCCESS;
    int r = sshbuf_reserve(buf, len);
    if (r) return r;
    memcpy(buf->data + buf->size, data, len);
    buf->size += len;
    return SSH_ERR_SUCCESS;
}

int sshbuf_put_string(sshbuf_t *buf, const void *data, size_t len) {
    if (len > UINT32_MAX) return SSH_ERR_INVALID_ARG;
    int r = sshbuf_put_u32(buf, (uint32_t)len);
    if (r) return r;
    return sshbuf_put_raw(buf, data, len);
}

int sshbuf_put_cstring(sshbuf_t *buf, const char *str) {
    size_t len = str ? strlen(str) : 0;
    return sshbuf_put_string(buf, str, len);
}

int sshbuf_put_mpint(sshbuf_t *buf, const uint8_t *data, size_t len) {
    if (!buf || (!data && len > 0)) return SSH_ERR_INVALID_ARG;
    /* Strip leading zeroes */
    while (len > 0 && *data == 0) {
        data++;
        len--;
    }
    if (len == 0) {
        return sshbuf_put_u32(buf, 0);
    }
    if (*data & 0x80) {
        int r = sshbuf_put_u32(buf, (uint32_t)(len + 1));
        if (r) return r;
        r = sshbuf_put_u8(buf, 0x00);
        if (r) return r;
        return sshbuf_put_raw(buf, data, len);
    }
    int r = sshbuf_put_u32(buf, (uint32_t)len);
    if (r) return r;
    return sshbuf_put_raw(buf, data, len);
}

int sshbuf_get_u8(sshbuf_t *buf, uint8_t *val) {
    if (!buf || !val) return SSH_ERR_INVALID_ARG;
    if (sshbuf_len(buf) < 1) return SSH_ERR_BUFFER_TOO_SMALL;
    *val = buf->data[buf->off++];
    return SSH_ERR_SUCCESS;
}

int sshbuf_get_u32(sshbuf_t *buf, uint32_t *val) {
    if (!buf || !val) return SSH_ERR_INVALID_ARG;
    if (sshbuf_len(buf) < 4) return SSH_ERR_BUFFER_TOO_SMALL;
    const uint8_t *p = buf->data + buf->off;
    *val = ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |
           ((uint32_t)p[3]);
    buf->off += 4;
    return SSH_ERR_SUCCESS;
}

int sshbuf_get_u64(sshbuf_t *buf, uint64_t *val) {
    if (!buf || !val) return SSH_ERR_INVALID_ARG;
    if (sshbuf_len(buf) < 8) return SSH_ERR_BUFFER_TOO_SMALL;
    const uint8_t *p = buf->data + buf->off;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | (uint64_t)p[i];
    }
    *val = v;
    buf->off += 8;
    return SSH_ERR_SUCCESS;
}

int sshbuf_get_raw(sshbuf_t *buf, void *dest, size_t len) {
    if (!buf || (!dest && len > 0)) return SSH_ERR_INVALID_ARG;
    if (len == 0) return SSH_ERR_SUCCESS;
    if (sshbuf_len(buf) < len) return SSH_ERR_BUFFER_TOO_SMALL;
    memcpy(dest, buf->data + buf->off, len);
    buf->off += len;
    return SSH_ERR_SUCCESS;
}

int sshbuf_get_string_ptr(sshbuf_t *buf, const uint8_t **ptr, size_t *len) {
    if (!buf || !ptr || !len) return SSH_ERR_INVALID_ARG;
    uint32_t slen = 0;
    int r = sshbuf_get_u32(buf, &slen);
    if (r) return r;
    if (sshbuf_len(buf) < slen) return SSH_ERR_INVALID_FORMAT;
    *ptr = buf->data + buf->off;
    *len = slen;
    buf->off += slen;
    return SSH_ERR_SUCCESS;
}

int sshbuf_get_mpint(sshbuf_t *buf, uint8_t *out, size_t max_out, size_t *out_len) {
    if (!buf || !out || !out_len) return SSH_ERR_INVALID_ARG;
    uint32_t raw_len = 0;
    int r = sshbuf_get_u32(buf, &raw_len);
    if (r) return r;
    if (sshbuf_len(buf) < raw_len) return SSH_ERR_INVALID_FORMAT;
    if (raw_len == 0) {
        *out_len = 0;
        return SSH_ERR_SUCCESS;
    }
    const uint8_t *p = buf->data + buf->off;
    /* Reject negative mpint */
    if (p[0] & 0x80) return SSH_ERR_INVALID_FORMAT;

    size_t actual_len = raw_len;
    if (p[0] == 0x00) {
        /* If raw_len == 1, value is 0 */
        if (raw_len == 1) {
            p++;
            actual_len--;
        } else if (!(p[1] & 0x80)) {
            /* RFC 4251 §5: unneeded leading zero is non-minimal / invalid */
            return SSH_ERR_INVALID_FORMAT;
        } else {
            p++;
            actual_len--;
        }
    }
    if (actual_len > max_out) return SSH_ERR_BUFFER_TOO_SMALL;
    if (actual_len > 0) memcpy(out, p, actual_len);
    *out_len = actual_len;
    buf->off += raw_len;
    return SSH_ERR_SUCCESS;
}

int sshbuf_peek_u8(const sshbuf_t *buf, uint8_t *val) {
    if (!buf || !val) return SSH_ERR_INVALID_ARG;
    if (sshbuf_len(buf) < 1) return SSH_ERR_BUFFER_TOO_SMALL;
    *val = buf->data[buf->off];
    return SSH_ERR_SUCCESS;
}
