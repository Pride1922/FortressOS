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

#ifdef __cplusplus
}
#endif

#endif /* FORTRESS_TLS_H */
