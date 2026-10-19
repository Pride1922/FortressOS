#if defined(TEST_HOST)
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE 1
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#endif

#include "tls.h"
#include "tls_internal.h"
#include "syscall_abi.h"

#if !defined(TEST_HOST)
#include "string.h"
#define ETIMEDOUT 110
#endif

/* Custom / Mock Syscall Hook for Testing */
static tls_syscall_fn s_syscall_hook = NULL;

void fortress_tls_set_syscall_hook(tls_syscall_fn hook) {
    s_syscall_hook = hook;
}

long tls_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c,
                 uintptr_t d, uintptr_t e, uintptr_t f) {
    if (s_syscall_hook) {
        return s_syscall_hook(nr, a, b, c, d, e, f);
    }
#if defined(TEST_HOST)
    if (nr == SYS_CLOCK_GETTIME) {
        struct timespec hts;
        if (clock_gettime((clockid_t)a, &hts) != 0) {
            return -1;
        }
        timespec_t *ts = (timespec_t *)b;
        if (ts) {
            ts->tv_sec = hts.tv_sec;
            ts->tv_nsec = hts.tv_nsec;
        }
        return 0;
    } else if (nr == SYS_GETRANDOM) {
        void *buf = (void *)a;
        size_t len = (size_t)b;
        FILE *f = fopen("/dev/urandom", "rb");
        if (!f) return -1;
        size_t r = fread(buf, 1, len, f);
        fclose(f);
        return (long)r;
    } else if (nr == SYS_SEND_UNTIL) {
        int fd = (int)a;
        const void *buf = (const void *)b;
        size_t len = (size_t)c;
        uint64_t deadline = (uint64_t)e;
        if (deadline > 0) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            uint64_t now_ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
            if (now_ms >= deadline) {
                return -110; /* -ETIMEDOUT */
            }
            int timeout_ms = (int)(deadline - now_ms);
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLOUT;
            int pr = poll(&pfd, 1, timeout_ms);
            if (pr == 0) {
                return -110; /* -ETIMEDOUT */
            }
            if (pr < 0) {
                return -errno;
            }
        }
        ssize_t n = send(fd, buf, len, 0);
        if (n < 0) return -errno;
        return (long)n;
    } else if (nr == SYS_RECV_UNTIL) {
        int fd = (int)a;
        void *buf = (void *)b;
        size_t cap = (size_t)c;
        uint64_t deadline = (uint64_t)e;
        if (deadline > 0) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            uint64_t now_ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
            if (now_ms >= deadline) {
                return -110; /* -ETIMEDOUT */
            }
            int timeout_ms = (int)(deadline - now_ms);
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLIN;
            int pr = poll(&pfd, 1, timeout_ms);
            if (pr == 0) {
                return -110; /* -ETIMEDOUT */
            }
            if (pr < 0) {
                return -errno;
            }
        }
        ssize_t n = recv(fd, buf, cap, 0);
        if (n < 0) return -errno;
        return (long)n;
    } else if (nr == SYS_OPEN) {
        const char *pathname = (const char *)a;
        int flags = (int)b;
        mode_t mode = (mode_t)c;
        int hfd = open(pathname, flags, mode);
        if (hfd < 0) return -errno;
        return hfd;
    } else if (nr == SYS_READ) {
        int hfd = (int)a;
        void *buf = (void *)b;
        size_t count = (size_t)c;
        ssize_t n = read(hfd, buf, count);
        if (n < 0) return -errno;
        return (long)n;
    } else if (nr == SYS_CLOSE) {
        int hfd = (int)a;
        if (close(hfd) < 0) return -errno;
        return 0;
    }
    return -38; /* -ENOSYS */
#else
    register uintptr_t r10 __asm__("r10") = d;
    register uintptr_t r8  __asm__("r8")  = e;
    register uintptr_t r9  __asm__("r9")  = f;
    __asm__ volatile("syscall"
        : "+a"(nr)
        : "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory", "cc");
    return nr;
#endif
}

/* Platform Clock Hook (TLS_PLAN.md §2.4, §6) */
int tls_clock_hook(uint32_t *days_out, uint32_t *seconds_out) {
    if (!days_out || !seconds_out) {
        return TLS_ERR_INVALID_PARAM;
    }
    timespec_t ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    long ret = tls_syscall(SYS_CLOCK_GETTIME, CLOCK_REALTIME, (uintptr_t)&ts, 0, 0, 0, 0);
    if (ret != 0) {
        return TLS_ERR_CLOCK_UNSYNCHRONIZED;
    }
    if ((uint64_t)ts.tv_sec < TLS_TIME_FLOOR_EPOCH || (uint64_t)ts.tv_sec > TLS_TIME_CEILING_EPOCH) {
        return TLS_ERR_CLOCK_UNSYNCHRONIZED;
    }
    uint64_t sec = (uint64_t)ts.tv_sec;
    *days_out = (uint32_t)(sec / 86400ULL) + 719528U;
    *seconds_out = (uint32_t)(sec % 86400ULL);
    return TLS_SUCCESS;
}

/* Platform Entropy Hook (TLS_PLAN.md §6) */
int tls_rand_hook(void *buf, size_t len) {
    if (!buf || len < 32) {
        return TLS_ERR_INVALID_PARAM;
    }
    long ret = tls_syscall(SYS_GETRANDOM, (uintptr_t)buf, len, 0, 0, 0, 0);
    if (ret < (long)len) {
        return TLS_ERR_ENTROPY_FAILURE;
    }
    return TLS_SUCCESS;
}

/* Platform Send Hook (TLS_PLAN.md §3.1, §6) */
int tls_send_hook(void *ctx_ptr, const unsigned char *buf, size_t len) {
    fortress_tls_t *ctx = (fortress_tls_t *)ctx_ptr;
    if (!ctx || ctx->fd < 0 || !buf) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    long ret = tls_syscall(SYS_SEND_UNTIL, ctx->fd, (uintptr_t)buf, len, 0, ctx->current_deadline, 0);
    if (ret > 0) {
        return (int)ret;
    }
    if (ret == -110 || ret == -ETIMEDOUT) {
        ctx->last_error = TLS_ERR_TIMEOUT;
    } else {
        ctx->last_error = TLS_ERR_IO;
    }
    return -1;
}

/* Platform Recv Hook (TLS_PLAN.md §3.1, §6) */
int tls_recv_hook(void *ctx_ptr, unsigned char *buf, size_t len) {
    fortress_tls_t *ctx = (fortress_tls_t *)ctx_ptr;
    if (!ctx || ctx->fd < 0 || !buf) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    long ret = tls_syscall(SYS_RECV_UNTIL, ctx->fd, (uintptr_t)buf, len, 0, ctx->current_deadline, 0);
    if (ret > 0) {
        return (int)ret;
    }
    if (ret == 0) {
        ctx->peer_closed = true;
        ctx->last_error = TLS_ERR_PEER_CLOSED;
        return -1;
    }
    if (ret == -110 || ret == -ETIMEDOUT) {
        ctx->last_error = TLS_ERR_TIMEOUT;
    } else {
        ctx->last_error = TLS_ERR_IO;
    }
    return -1;
}

/* Supported curves mask: Curve25519 (29), secp384r1 (24), secp256r1 (23) */
#define FORTRESS_SUPPORTED_CURVES \
    ((1U << BR_EC_curve25519) | (1U << BR_EC_secp384r1) | (1U << BR_EC_secp256r1))

static const unsigned char *fortress_ec_generator(int curve, size_t *len) {
    return br_ec_all_m31.generator(curve, len);
}

static const unsigned char *fortress_ec_order(int curve, size_t *len) {
    return br_ec_all_m31.order(curve, len);
}

static size_t fortress_ec_xoff(int curve, size_t *len) {
    return br_ec_all_m31.xoff(curve, len);
}

static uint32_t fortress_ec_mul(unsigned char *G, size_t Glen,
    const unsigned char *kb, size_t kblen, int curve) {
    return br_ec_all_m31.mul(G, Glen, kb, kblen, curve);
}

static size_t fortress_ec_mulgen(unsigned char *R,
    const unsigned char *x, size_t xlen, int curve) {
    return br_ec_all_m31.mulgen(R, x, xlen, curve);
}

static uint32_t fortress_ec_muladd(unsigned char *A, const unsigned char *B, size_t len,
    const unsigned char *x, size_t xlen,
    const unsigned char *y, size_t ylen, int curve) {
    return br_ec_all_m31.muladd(A, B, len, x, xlen, y, ylen, curve);
}

static const br_ec_impl s_fortress_ec_m31 = {
    (uint32_t)FORTRESS_SUPPORTED_CURVES,
    fortress_ec_generator,
    fortress_ec_order,
    fortress_ec_xoff,
    fortress_ec_mul,
    fortress_ec_mulgen,
    fortress_ec_muladd
};

/* Enforces RSA >= 2048-bit key size constraint (TLS_PLAN.md §2.2) */
static uint32_t fortress_rsa_pkcs1_vrfy(const unsigned char *x, size_t xlen,
    const unsigned char *hash_oid, size_t hash_len,
    const br_rsa_public_key *pk, unsigned char *hash_out)
{
    if (!pk || !pk->n) {
        return 0;
    }
    const unsigned char *n = pk->n;
    size_t nlen = pk->nlen;
    while (nlen > 0 && *n == 0) {
        n++;
        nlen--;
    }
    /* Reject RSA keys < 2048 bits (256 bytes) */
    if (nlen < 256) {
        return 0;
    }
    return br_rsa_i31_pkcs1_vrfy(x, xlen, hash_oid, hash_len, pk, hash_out);
}

/* Strict AEAD Cipher Suites (TLS_PLAN.md §2.2) */
static const uint16_t s_cipher_suites[] = {
    BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
    BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384
};

int fortress_tls_client_init(fortress_tls_t *ctx,
                             const br_x509_trust_anchor *anchors,
                             size_t num_anchors)
{
    if (!ctx) {
        return TLS_ERR_INVALID_PARAM;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = -1;

    /* 1. Reset client context and enforce TLS 1.2 exclusively */
    br_ssl_client_zero(&ctx->sc);
    br_ssl_engine_set_versions(&ctx->sc.eng, BR_TLS12, BR_TLS12);

    /* 2. Configure strictly allowed AEAD suites */
    br_ssl_engine_set_suites(&ctx->sc.eng, s_cipher_suites,
        sizeof(s_cipher_suites) / sizeof(s_cipher_suites[0]));

    /* 3. Initialize minimal X.509 chain validator with SHA-256 for DN hashing */
    br_x509_minimal_init(&ctx->xc, &br_sha256_vtable, anchors, num_anchors);

    /* 4. Configure curve support */
    br_ssl_engine_set_ec(&ctx->sc.eng, &s_fortress_ec_m31);

    /* 5. Configure RSA and ECDSA signature verifiers */
    br_ssl_engine_set_rsavrfy(&ctx->sc.eng, fortress_rsa_pkcs1_vrfy);
    br_x509_minimal_set_rsa(&ctx->xc, fortress_rsa_pkcs1_vrfy);

    br_ssl_engine_set_ecdsa(&ctx->sc.eng, &br_ecdsa_i31_vrfy_asn1);
    br_x509_minimal_set_ecdsa(&ctx->xc, &s_fortress_ec_m31, &br_ecdsa_i31_vrfy_asn1);

    /* 6. Configure hash implementations (SHA-256, SHA-384, SHA-512) */
    br_ssl_engine_set_hash(&ctx->sc.eng, br_sha256_ID, &br_sha256_vtable);
    br_ssl_engine_set_hash(&ctx->sc.eng, br_sha384_ID, &br_sha384_vtable);
    br_ssl_engine_set_hash(&ctx->sc.eng, br_sha512_ID, &br_sha512_vtable);

    br_x509_minimal_set_hash(&ctx->xc, br_sha256_ID, &br_sha256_vtable);
    br_x509_minimal_set_hash(&ctx->xc, br_sha384_ID, &br_sha384_vtable);
    br_x509_minimal_set_hash(&ctx->xc, br_sha512_ID, &br_sha512_vtable);

    /* 7. Configure PRF implementations (SHA-256 and SHA-384) */
    br_ssl_engine_set_prf_sha256(&ctx->sc.eng, &br_tls12_sha256_prf);
    br_ssl_engine_set_prf_sha384(&ctx->sc.eng, &br_tls12_sha384_prf);

    /* 8. Configure AEAD symmetric encryption (AES-GCM via aes_ct and ChaCha20-Poly1305) */
    br_ssl_engine_set_gcm(&ctx->sc.eng,
                          &br_sslrec_in_gcm_vtable,
                          &br_sslrec_out_gcm_vtable);
    br_ssl_engine_set_aes_ctr(&ctx->sc.eng, &br_aes_ct_ctr_vtable);
    br_ssl_engine_set_ghash(&ctx->sc.eng, &br_ghash_ctmul32);
    br_ssl_engine_set_chapol(&ctx->sc.eng,
                             &br_sslrec_in_chapol_vtable,
                             &br_sslrec_out_chapol_vtable);
    br_ssl_engine_set_chacha20(&ctx->sc.eng, &br_chacha20_ct_run);
    br_ssl_engine_set_poly1305(&ctx->sc.eng, &br_poly1305_ctmul32_run);

    /* 9. Link X.509 validator into SSL engine */
    br_ssl_engine_set_x509(&ctx->sc.eng, &ctx->xc.vtable);

    /* 10. Link static bidirectional I/O buffer */
    br_ssl_engine_set_buffer(&ctx->sc.eng, ctx->iobuf, sizeof(ctx->iobuf), 1);

    /* 11. Initialize simplified I/O engine context */
    br_sslio_init(&ctx->ioc, &ctx->sc.eng, tls_recv_hook, ctx, tls_send_hook, ctx);

    return TLS_SUCCESS;
}

int fortress_tls_connect(fortress_tls_t *ctx,
                         int fd,
                         const char *sni_host,
                         uint64_t handshake_deadline)
{
    if (!ctx || fd < 0 || !sni_host) {
        return TLS_ERR_INVALID_PARAM;
    }
    ctx->fd = fd;
    ctx->current_deadline = handshake_deadline;
    ctx->handshake_done = false;
    ctx->peer_closed = false;
    ctx->client_closed = false;
    ctx->last_error = TLS_SUCCESS;
    ctx->br_last_error = BR_ERR_OK;

    /* Obtain verified validation time */
    uint32_t days = 0, seconds = 0;
    int clock_res = tls_clock_hook(&days, &seconds);
    if (clock_res != TLS_SUCCESS) {
        ctx->last_error = TLS_ERR_CLOCK_UNSYNCHRONIZED;
        return TLS_ERR_CLOCK_UNSYNCHRONIZED;
    }
    br_x509_minimal_set_time(&ctx->xc, days, seconds);

    /* Obtain cryptographic entropy */
    unsigned char seed[32];
    int rand_res = tls_rand_hook(seed, sizeof(seed));
    if (rand_res != TLS_SUCCESS) {
        ctx->last_error = TLS_ERR_ENTROPY_FAILURE;
        return TLS_ERR_ENTROPY_FAILURE;
    }
    br_ssl_engine_inject_entropy(&ctx->sc.eng, seed, sizeof(seed));

    /* Reset client engine with SNI host */
    if (!br_ssl_client_reset(&ctx->sc, sni_host, 0)) {
        ctx->last_error = TLS_ERR_HANDSHAKE_FAILED;
        ctx->br_last_error = br_ssl_engine_last_error(&ctx->sc.eng);
        return TLS_ERR_HANDSHAKE_FAILED;
    }

    /* Initialize simplified I/O engine with caller's callbacks */
    br_sslio_init(&ctx->ioc, &ctx->sc.eng, tls_recv_hook, ctx, tls_send_hook, ctx);

    /* Flush and drive handshake to completion */
    int res = br_sslio_flush(&ctx->ioc);
    if (res < 0 || (br_ssl_engine_current_state(&ctx->sc.eng) & BR_SSL_CLOSED)) {
        ctx->br_last_error = br_ssl_engine_last_error(&ctx->sc.eng);
        if (ctx->last_error == TLS_SUCCESS) {
            ctx->last_error = TLS_ERR_HANDSHAKE_FAILED;
        }
        return ctx->last_error;
    }

    ctx->handshake_done = true;
    return TLS_SUCCESS;
}

int fortress_tls_read(fortress_tls_t *ctx,
                      void *buf,
                      size_t len,
                      uint64_t deadline)
{
    if (!ctx || !buf || !ctx->handshake_done) {
        return TLS_ERR_INVALID_PARAM;
    }
    if (len == 0) {
        return 0;
    }
    ctx->current_deadline = deadline;
    int rlen = br_sslio_read(&ctx->ioc, buf, len);
    if (rlen < 0) {
        ctx->br_last_error = br_ssl_engine_last_error(&ctx->sc.eng);
        unsigned state = br_ssl_engine_current_state(&ctx->sc.eng);
        if ((state & BR_SSL_CLOSED) != 0 && ctx->br_last_error == BR_ERR_OK) {
            return 0;
        }
        if (ctx->last_error == TLS_SUCCESS) {
            ctx->last_error = ctx->peer_closed ? TLS_ERR_PEER_CLOSED : TLS_ERR_IO;
        }
        return ctx->last_error;
    }
    return rlen;
}

int fortress_tls_read_all(fortress_tls_t *ctx,
                          void *buf,
                          size_t len,
                          uint64_t deadline)
{
    if (!ctx || !buf || !ctx->handshake_done) {
        return TLS_ERR_INVALID_PARAM;
    }
    ctx->current_deadline = deadline;
    int res = br_sslio_read_all(&ctx->ioc, buf, len);
    if (res < 0) {
        ctx->br_last_error = br_ssl_engine_last_error(&ctx->sc.eng);
        if (ctx->last_error == TLS_SUCCESS) {
            ctx->last_error = ctx->peer_closed ? TLS_ERR_PEER_CLOSED : TLS_ERR_IO;
        }
        return ctx->last_error;
    }
    return TLS_SUCCESS;
}

int fortress_tls_write(fortress_tls_t *ctx,
                       const void *buf,
                       size_t len,
                       uint64_t deadline)
{
    if (!ctx || !buf || !ctx->handshake_done) {
        return TLS_ERR_INVALID_PARAM;
    }
    if (len == 0) {
        return 0;
    }
    ctx->current_deadline = deadline;
    int wlen = br_sslio_write(&ctx->ioc, buf, len);
    if (wlen < 0) {
        ctx->br_last_error = br_ssl_engine_last_error(&ctx->sc.eng);
        if (ctx->last_error == TLS_SUCCESS) {
            ctx->last_error = TLS_ERR_IO;
        }
        return ctx->last_error;
    }
    return wlen;
}

int fortress_tls_write_all(fortress_tls_t *ctx,
                           const void *buf,
                           size_t len,
                           uint64_t deadline)
{
    if (!ctx || !buf || !ctx->handshake_done) {
        return TLS_ERR_INVALID_PARAM;
    }
    ctx->current_deadline = deadline;
    int res = br_sslio_write_all(&ctx->ioc, buf, len);
    if (res < 0) {
        ctx->br_last_error = br_ssl_engine_last_error(&ctx->sc.eng);
        if (ctx->last_error == TLS_SUCCESS) {
            ctx->last_error = TLS_ERR_IO;
        }
        return ctx->last_error;
    }
    return TLS_SUCCESS;
}

int fortress_tls_flush(fortress_tls_t *ctx,
                       uint64_t deadline)
{
    if (!ctx || !ctx->handshake_done) {
        return TLS_ERR_INVALID_PARAM;
    }
    ctx->current_deadline = deadline;
    int res = br_sslio_flush(&ctx->ioc);
    if (res < 0) {
        ctx->br_last_error = br_ssl_engine_last_error(&ctx->sc.eng);
        if (ctx->last_error == TLS_SUCCESS) {
            ctx->last_error = TLS_ERR_IO;
        }
        return ctx->last_error;
    }
    return TLS_SUCCESS;
}

int fortress_tls_close(fortress_tls_t *ctx,
                       uint64_t teardown_deadline)
{
    if (!ctx) {
        return TLS_ERR_INVALID_PARAM;
    }
    if (!ctx->handshake_done) {
        return TLS_SUCCESS;
    }
    ctx->current_deadline = teardown_deadline;
    int ok = br_sslio_close(&ctx->ioc);
    ctx->br_last_error = br_ssl_engine_last_error(&ctx->sc.eng);
    if (ok && ctx->br_last_error == BR_ERR_OK) {
        ctx->client_closed = true;
        return TLS_SUCCESS;
    }
    ctx->last_error = TLS_ERR_TRUNCATED;
    return TLS_ERR_TRUNCATED;
}

int fortress_tls_get_error(const fortress_tls_t *ctx) {
    if (!ctx) return TLS_ERR_INVALID_PARAM;
    if (ctx->last_error != TLS_SUCCESS) return ctx->last_error;
    if (ctx->br_last_error != BR_ERR_OK) return ctx->br_last_error;
    return br_ssl_engine_last_error(&ctx->sc.eng);
}

const char *fortress_tls_strerror(int err) {
    switch (err) {
        case TLS_SUCCESS: return "success";
        case TLS_ERR_INVALID_PARAM: return "invalid parameter";
        case TLS_ERR_CLOCK_UNSYNCHRONIZED: return "system clock uncalibrated or before 2026-01-01";
        case TLS_ERR_ENTROPY_FAILURE: return "entropy harvest failed (SYS_GETRANDOM short read)";
        case TLS_ERR_HANDSHAKE_FAILED: return "TLS handshake failed";
        case TLS_ERR_IO: return "transport I/O error";
        case TLS_ERR_TIMEOUT: return "operation deadline exceeded";
        case TLS_ERR_PEER_CLOSED: return "remote peer abruptly closed connection";
        case TLS_ERR_TRUNCATED: return "connection truncated (missing or timed out close_notify)";
        case TLS_ERR_NOT_READY: return "TLS engine not initialized or ready";
        case TLS_ERR_ANCHOR_INVALID: return "trust anchors invalid or not configured";
        case TLS_ERR_RSA_KEY_TOO_SMALL: return "RSA key smaller than 2048 bits rejected";
        case TLS_ERR_ANCHOR_FILE: return "trust anchor file missing or unreadable";
        case TLS_ERR_ANCHOR_MAGIC: return "invalid trust anchor file magic or version";
        case TLS_ERR_ANCHOR_FORMAT: return "malformed trust anchor descriptor or offset";
        case TLS_ERR_ANCHOR_BOUNDS: return "trust anchor count or file size exceeds bounds";
        case TLS_ERR_ANCHOR_KEY: return "insecure or invalid public key in trust anchor";

        /* BearSSL Protocol and Cryptographic Error Codes */
        case BR_ERR_BAD_STATE: return "invalid SSL engine state";
        case BR_ERR_UNSUPPORTED_VERSION: return "unsupported protocol version";
        case BR_ERR_BAD_VERSION: return "bad protocol version";
        case BR_ERR_BAD_LENGTH: return "invalid record length";
        case BR_ERR_TOO_LARGE: return "record exceeds maximum length";
        case BR_ERR_BAD_MAC: return "record authentication tag verification failed";
        case BR_ERR_NO_RANDOM: return "no entropy provided to SSL engine";
        case BR_ERR_UNKNOWN_TYPE: return "unknown record type";
        case BR_ERR_UNEXPECTED: return "unexpected record received";
        case BR_ERR_BAD_CCS: return "unexpected ChangeCipherSpec record";
        case BR_ERR_BAD_ALERT: return "invalid alert message";
        case BR_ERR_BAD_HANDSHAKE: return "invalid handshake message";
        case BR_ERR_OVERSIZED_ID: return "session ID too large";
        case BR_ERR_BAD_CIPHER_SUITE: return "server selected unsupported cipher suite";
        case BR_ERR_BAD_COMPRESSION: return "compression rejected";
        case BR_ERR_BAD_FRAGLEN: return "invalid record fragment length";
        case BR_ERR_BAD_SECRENEG: return "secure renegotiation failed";
        case BR_ERR_EXTRA_EXTENSION: return "duplicate or unexpected extension";
        case BR_ERR_BAD_SNI: return "invalid server name indication";
        case BR_ERR_BAD_HELLO_DONE: return "invalid ServerHelloDone";
        case BR_ERR_LIMIT_EXCEEDED: return "record limit exceeded";
        case BR_ERR_BAD_FINISHED: return "invalid Finished verify data";
        case BR_ERR_RESUME_MISMATCH: return "session resumption mismatch";
        case BR_ERR_INVALID_ALGORITHM: return "unsupported cryptographic algorithm";
        case BR_ERR_BAD_SIGNATURE: return "handshake signature verification failed";
        case BR_ERR_WRONG_KEY_USAGE: return "key usage does not permit operation";
        case BR_ERR_NO_CLIENT_AUTH: return "client certificate requested but unavailable";
        case BR_ERR_IO: return "transport I/O error or unexpected socket close";

        /* BearSSL X.509 Minimal Validator Error Codes */
        case BR_ERR_X509_INVALID_VALUE: return "malformed X.509 certificate structure";
        case BR_ERR_X509_TRUNCATED: return "truncated X.509 certificate data";
        case BR_ERR_X509_EMPTY_CHAIN: return "server provided empty certificate chain";
        case BR_ERR_X509_INNER_TRUNC: return "X.509 certificate inner structure truncated";
        case BR_ERR_X509_BAD_TAG_CLASS: return "invalid ASN.1 tag class in certificate";
        case BR_ERR_X509_BAD_TAG_VALUE: return "invalid ASN.1 tag value in certificate";
        case BR_ERR_X509_INDEFINITE_LENGTH: return "indefinite ASN.1 length rejected";
        case BR_ERR_X509_EXTRA_ELEMENT: return "extra elements in certificate structure";
        case BR_ERR_X509_UNEXPECTED: return "unexpected element in certificate";
        case BR_ERR_X509_NOT_CONSTRUCTED: return "expected constructed ASN.1 tag";
        case BR_ERR_X509_NOT_PRIMITIVE: return "expected primitive ASN.1 tag";
        case BR_ERR_X509_PARTIAL_BYTE: return "invalid bit string encoding";
        case BR_ERR_X509_BAD_BOOLEAN: return "invalid boolean encoding";
        case BR_ERR_X509_OVERFLOW: return "integer overflow in certificate parser";
        case BR_ERR_X509_BAD_DN: return "malformed distinguished name";
        case BR_ERR_X509_BAD_TIME: return "invalid date/time representation";
        case BR_ERR_X509_UNSUPPORTED: return "unsupported certificate feature";
        case BR_ERR_X509_LIMIT_EXCEEDED: return "certificate exceeds parser limits";
        case BR_ERR_X509_WRONG_KEY_TYPE: return "certificate public key type mismatch";
        case BR_ERR_X509_BAD_SIGNATURE: return "certificate signature verification failed";
        case BR_ERR_X509_TIME_UNKNOWN: return "validation time unknown";
        case BR_ERR_X509_EXPIRED: return "certificate expired or not yet valid";
        case BR_ERR_X509_BAD_SERVER_NAME: return "server certificate does not match requested hostname";
        case BR_ERR_X509_NOT_TRUSTED: return "certificate chain not trusted by any root anchor";

        default: return "unknown TLS error";
    }
}

/*
 * Platform PRNG seeder implementation: BearSSL calls this once during engine initialization.
 * Returning 0 indicates that no automatic platform seeder is used, ensuring that all entropy
 * is injected exclusively and deterministically via tls_rand_hook() (SYS_GETRANDOM).
 */
br_prng_seeder br_prng_seeder_system(const char **name)
{
    if (name != NULL) {
        *name = "fortress_getrandom";
    }
    return 0;
}
