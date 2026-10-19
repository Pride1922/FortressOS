#ifndef FORTRESS_TLS_H
#define FORTRESS_TLS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "bearssl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Time boundaries (TLS_PLAN.md §2.4) */
#define TLS_TIME_FLOOR_EPOCH    1767225600ULL /* 2026-01-01 00:00:00 UTC */
#define TLS_TIME_CEILING_EPOCH  1893456000ULL /* 2030-01-01 00:00:00 UTC */

/* Buffer sizes (TLS_PLAN.md §2.1, §3.3) */
#define TLS_IOBUF_SIZE          BR_SSL_BUFSIZE_BIDI /* 33178 bytes */

/* FortressOS TLS Error Codes */
#define TLS_SUCCESS                   0
#define TLS_ERR_INVALID_PARAM        -1
#define TLS_ERR_CLOCK_UNSYNCHRONIZED  -2
#define TLS_ERR_ENTROPY_FAILURE      -3
#define TLS_ERR_HANDSHAKE_FAILED     -4
#define TLS_ERR_IO                   -5
#define TLS_ERR_TIMEOUT              -6
#define TLS_ERR_PEER_CLOSED          -7
#define TLS_ERR_TRUNCATED            -8
#define TLS_ERR_NOT_READY            -9
#define TLS_ERR_ANCHOR_INVALID       -10
#define TLS_ERR_RSA_KEY_TOO_SMALL    -11
#define TLS_ERR_ANCHOR_FILE          -12
#define TLS_ERR_ANCHOR_MAGIC         -13
#define TLS_ERR_ANCHOR_FORMAT        -14
#define TLS_ERR_ANCHOR_BOUNDS        -15
#define TLS_ERR_ANCHOR_KEY           -16

/* Trust Store & Runtime Anchor Parser Bounds (TLS_PLAN.md §4.2) */
#define TLS_MAX_ANCHOR_FILE_SIZE     262144 /* 256 KiB hard cap */
#define TLS_MAX_ANCHORS              256    /* Maximum 256 trust anchors */
#define TLS_MAX_DN_LEN               1024   /* Maximum DN length in bytes */
#define TLS_MIN_RSA_MODULUS_LEN      256    /* 2048-bit minimum */
#define TLS_MAX_RSA_MODULUS_LEN      512    /* 4096-bit maximum */
#define TLS_MAX_RSA_EXPONENT_LEN     4      /* e <= 2^32 - 1 */
#define TLS_MAX_EC_POINT_LEN         133    /* Curve public point byte cap */

#define TLS_ANCHOR_MAGIC             "FOSANC01"
#define TLS_ANCHOR_VERSION           1

/* 32-byte packed binary trust anchor descriptor */
typedef struct {
    uint32_t dn_offset;
    uint32_t dn_len;
    uint32_t flags;     /* 0 = standard, 1 = BR_X509_TA_CA */
    uint32_t key_type;  /* 1 = BR_KEYTYPE_RSA, 2 = BR_KEYTYPE_EC */
    union {
        struct {
            uint32_t n_offset;
            uint32_t n_len;
            uint32_t e_offset;
            uint32_t e_len;
        } rsa;
        struct {
            uint32_t curve_id; /* BR_EC_secp256r1 (23), BR_EC_secp384r1 (24), BR_EC_curve25519 (29) */
            uint32_t q_offset;
            uint32_t q_len;
            uint32_t _pad;
        } ec;
    } key;
} __attribute__((packed)) fortress_anchor_desc_t;

/* Static BSS Trust Store container (TLS_PLAN.md §3.3) */
typedef struct fortress_trust_store {
    uint8_t raw_buf[TLS_MAX_ANCHOR_FILE_SIZE];
    br_x509_trust_anchor anchors[TLS_MAX_ANCHORS];
    size_t count;
} fortress_trust_store_t;

/* Context structure for FortressOS TLS client session.
 * Resides exclusively in static BSS as specified in TLS_PLAN.md §3.3.
 */
typedef struct fortress_tls {
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    br_sslio_context ioc;
    unsigned char iobuf[TLS_IOBUF_SIZE];
    int fd;
    uint64_t current_deadline;
    int last_error;
    int br_last_error;
    bool handshake_done;
    bool peer_closed;
    bool client_closed;
} fortress_tls_t;

/* Platform Callbacks / Hooks (TLS_PLAN.md §6) */
int tls_clock_hook(uint32_t *days_out, uint32_t *seconds_out);
int tls_rand_hook(void *buf, size_t len);
int tls_send_hook(void *ctx, const unsigned char *buf, size_t len);
int tls_recv_hook(void *ctx, unsigned char *buf, size_t len);

/* Testing / Mock Hook Callback Type */
typedef long (*tls_syscall_fn)(long nr, uintptr_t a, uintptr_t b, uintptr_t c,
                               uintptr_t d, uintptr_t e, uintptr_t f);
void fortress_tls_set_syscall_hook(tls_syscall_fn hook);

/* Client lifecycle functions */
int fortress_tls_client_init(fortress_tls_t *ctx,
                             const br_x509_trust_anchor *anchors,
                             size_t num_anchors);

int fortress_tls_connect(fortress_tls_t *ctx,
                         int fd,
                         const char *sni_host,
                         uint64_t handshake_deadline);

int fortress_tls_read(fortress_tls_t *ctx,
                      void *buf,
                      size_t len,
                      uint64_t deadline);

int fortress_tls_read_all(fortress_tls_t *ctx,
                          void *buf,
                          size_t len,
                          uint64_t deadline);

int fortress_tls_write(fortress_tls_t *ctx,
                       const void *buf,
                       size_t len,
                       uint64_t deadline);

int fortress_tls_write_all(fortress_tls_t *ctx,
                           const void *buf,
                           size_t len,
                           uint64_t deadline);

int fortress_tls_flush(fortress_tls_t *ctx,
                       uint64_t deadline);

int fortress_tls_close(fortress_tls_t *ctx,
                       uint64_t teardown_deadline);

int fortress_tls_get_error(const fortress_tls_t *ctx);
const char *fortress_tls_strerror(int err);

/* Trust Anchor Parser & Loader functions (TLS_PLAN.md §4.2) */
int fortress_tls_parse_anchors(const uint8_t *data, size_t len, fortress_trust_store_t *store);
int fortress_tls_load_anchors(const char *path, fortress_trust_store_t *store);

#ifdef __cplusplus
}
#endif

#endif /* FORTRESS_TLS_H */
