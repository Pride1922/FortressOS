/*
 * FortressOS TLS Trust Anchor Parser & Loader
 * Implements Phase TLS-2 specifications from docs/plans/TLS_PLAN.md §4.2.
 */

#include "tls.h"
#include "tls_internal.h"
#include "syscall_abi.h"

#if defined(TEST_HOST)
#include <string.h>
#else
#include "string.h"
#endif

/* NIST P-256 (secp256r1) field prime p256 */
static const uint8_t SECP256R1_P[32] = {
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

/* NIST P-384 (secp384r1) field prime p384 */
static const uint8_t SECP384R1_P[48] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe,
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff
};

static bool is_coord_valid(const uint8_t *coord, const uint8_t *p, size_t len) {
    bool all_zero = true;
    for (size_t i = 0; i < len; i++) {
        if (coord[i] != 0) {
            all_zero = false;
            break;
        }
    }
    if (all_zero) {
        return false;
    }
    return memcmp(coord, p, len) < 0;
}

static bool is_rsa_modulus_valid(const uint8_t *n, size_t nlen) {
    if (nlen < TLS_MIN_RSA_MODULUS_LEN || nlen > TLS_MAX_RSA_MODULUS_LEN) {
        return false;
    }
    size_t i = 0;
    while (i < nlen && n[i] == 0) {
        i++;
    }
    if (i == nlen) {
        return false;
    }
    size_t sig_bytes = nlen - i;
    unsigned char top = n[i];
    size_t top_bits = 0;
    while (top > 0) {
        top_bits++;
        top >>= 1;
    }
    size_t total_bits = (sig_bytes - 1) * 8 + top_bits;
    return (total_bits >= 2048 && total_bits <= 4096);
}

static bool is_rsa_exponent_valid(const uint8_t *e, size_t elen) {
    if (elen < 1 || elen > TLS_MAX_RSA_EXPONENT_LEN) {
        return false;
    }
    uint32_t eval = 0;
    for (size_t i = 0; i < elen; i++) {
        eval = (eval << 8) | e[i];
    }
    if ((eval & 1) == 0 || eval < 3) {
        return false;
    }
    return true;
}

static inline bool check_offset_bound(uint32_t offset, uint32_t len, uint32_t min_offset, size_t total_len) {
    if (offset < min_offset) {
        return false;
    }
    if (offset > (uint32_t)total_len) {
        return false;
    }
    if (len > (uint32_t)total_len - offset) {
        return false;
    }
    return true;
}

int fortress_tls_parse_anchors(const uint8_t *data, size_t len, fortress_trust_store_t *store) {
    if (!data || !store) {
        return TLS_ERR_INVALID_PARAM;
    }
    store->count = 0;

    /* 1. File size bound: between minimum header size and 256 KiB cap */
    if (len < 16 || len > TLS_MAX_ANCHOR_FILE_SIZE) {
        return TLS_ERR_ANCHOR_BOUNDS;
    }

    /* 2. Magic and Version check */
    if (memcmp(data, TLS_ANCHOR_MAGIC, 8) != 0) {
        return TLS_ERR_ANCHOR_MAGIC;
    }

    uint32_t version = (uint32_t)data[8] | ((uint32_t)data[9] << 8) |
                       ((uint32_t)data[10] << 16) | ((uint32_t)data[11] << 24);
    uint32_t count = (uint32_t)data[12] | ((uint32_t)data[13] << 8) |
                     ((uint32_t)data[14] << 16) | ((uint32_t)data[15] << 24);

    if (version != TLS_ANCHOR_VERSION) {
        return TLS_ERR_ANCHOR_MAGIC;
    }

    /* 3. Anchor count bounds: 1 <= count <= 256 */
    if (count < 1 || count > TLS_MAX_ANCHORS) {
        return TLS_ERR_ANCHOR_BOUNDS;
    }

    uint32_t table_bytes = count * 32U;
    uint32_t min_payload_offset = 16U + table_bytes;
    if (min_payload_offset > (uint32_t)len) {
        return TLS_ERR_ANCHOR_BOUNDS;
    }

    /* 4. Validate and unpack each descriptor */
    for (uint32_t i = 0; i < count; i++) {
        const fortress_anchor_desc_t *desc =
            (const fortress_anchor_desc_t *)(data + 16U + i * 32U);

        uint32_t dn_offset = desc->dn_offset;
        uint32_t dn_len = desc->dn_len;
        uint32_t flags = desc->flags;
        uint32_t key_type = desc->key_type;

        /* Distinguished Name validation */
        if (dn_len == 0 || dn_len > TLS_MAX_DN_LEN) {
            return TLS_ERR_ANCHOR_BOUNDS;
        }
        if (!check_offset_bound(dn_offset, dn_len, min_payload_offset, len)) {
            return TLS_ERR_ANCHOR_FORMAT;
        }

        /* Flags: 0 or 1 (BR_X509_TA_CA) */
        if (flags > 1) {
            return TLS_ERR_ANCHOR_FORMAT;
        }

        /* Key type: BR_KEYTYPE_RSA (1) or BR_KEYTYPE_EC (2) */
        if (key_type != BR_KEYTYPE_RSA && key_type != BR_KEYTYPE_EC) {
            return TLS_ERR_ANCHOR_FORMAT;
        }

        store->anchors[i].dn.data = (unsigned char *)(data + dn_offset);
        store->anchors[i].dn.len = (size_t)dn_len;
        store->anchors[i].flags = (flags != 0) ? BR_X509_TA_CA : 0;
        store->anchors[i].pkey.key_type = (unsigned char)key_type;

        if (key_type == BR_KEYTYPE_RSA) {
            uint32_t n_offset = desc->key.rsa.n_offset;
            uint32_t n_len = desc->key.rsa.n_len;
            uint32_t e_offset = desc->key.rsa.e_offset;
            uint32_t e_len = desc->key.rsa.e_len;

            if (!check_offset_bound(n_offset, n_len, min_payload_offset, len)) {
                return TLS_ERR_ANCHOR_FORMAT;
            }
            if (!check_offset_bound(e_offset, e_len, min_payload_offset, len)) {
                return TLS_ERR_ANCHOR_FORMAT;
            }

            if (!is_rsa_modulus_valid(data + n_offset, n_len)) {
                return TLS_ERR_ANCHOR_KEY;
            }
            if (!is_rsa_exponent_valid(data + e_offset, e_len)) {
                return TLS_ERR_ANCHOR_KEY;
            }

            store->anchors[i].pkey.key.rsa.n = (unsigned char *)(data + n_offset);
            store->anchors[i].pkey.key.rsa.nlen = (size_t)n_len;
            store->anchors[i].pkey.key.rsa.e = (unsigned char *)(data + e_offset);
            store->anchors[i].pkey.key.rsa.elen = (size_t)e_len;
        } else {
            uint32_t curve_id = desc->key.ec.curve_id;
            uint32_t q_offset = desc->key.ec.q_offset;
            uint32_t q_len = desc->key.ec.q_len;

            if (!check_offset_bound(q_offset, q_len, min_payload_offset, len)) {
                return TLS_ERR_ANCHOR_FORMAT;
            }

            const uint8_t *q = data + q_offset;

            if (curve_id == (uint32_t)BR_EC_secp256r1) {
                if (q_len != 65 || q[0] != 0x04) {
                    return TLS_ERR_ANCHOR_KEY;
                }
                if (!is_coord_valid(q + 1, SECP256R1_P, 32) ||
                    !is_coord_valid(q + 33, SECP256R1_P, 32)) {
                    return TLS_ERR_ANCHOR_KEY;
                }
            } else if (curve_id == (uint32_t)BR_EC_secp384r1) {
                if (q_len != 97 || q[0] != 0x04) {
                    return TLS_ERR_ANCHOR_KEY;
                }
                if (!is_coord_valid(q + 1, SECP384R1_P, 48) ||
                    !is_coord_valid(q + 49, SECP384R1_P, 48)) {
                    return TLS_ERR_ANCHOR_KEY;
                }
            } else if (curve_id == (uint32_t)BR_EC_curve25519) {
                if (q_len != 32) {
                    return TLS_ERR_ANCHOR_KEY;
                }
                bool non_zero = false;
                for (size_t b = 0; b < 32; b++) {
                    if (q[b] != 0) {
                        non_zero = true;
                        break;
                    }
                }
                if (!non_zero) {
                    return TLS_ERR_ANCHOR_KEY;
                }
            } else {
                return TLS_ERR_ANCHOR_KEY; /* Unsupported curve ID */
            }

            store->anchors[i].pkey.key.ec.curve = (int)curve_id;
            store->anchors[i].pkey.key.ec.q = (unsigned char *)(data + q_offset);
            store->anchors[i].pkey.key.ec.qlen = (size_t)q_len;
        }
    }

    store->count = (size_t)count;
    return TLS_SUCCESS;
}

int fortress_tls_load_anchors(const char *path, fortress_trust_store_t *store) {
    if (!path || !store) {
        return TLS_ERR_INVALID_PARAM;
    }
    store->count = 0;

    int fd = (int)tls_syscall(SYS_OPEN, (uintptr_t)path, 0 /* O_RDONLY */, 0, 0, 0, 0);
    if (fd < 0) {
        return TLS_ERR_ANCHOR_FILE;
    }

    size_t total_read = 0;
    while (total_read < sizeof(store->raw_buf)) {
        long n = tls_syscall(SYS_READ, (uintptr_t)fd,
                             (uintptr_t)(store->raw_buf + total_read),
                             (uintptr_t)(sizeof(store->raw_buf) - total_read), 0, 0, 0);
        if (n < 0) {
            tls_syscall(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
            return TLS_ERR_ANCHOR_FILE;
        }
        if (n == 0) {
            break;
        }
        total_read += (size_t)n;
    }

    /* Probe one additional byte to verify file does not exceed 256 KiB cap */
    uint8_t probe = 0;
    long probe_n = tls_syscall(SYS_READ, (uintptr_t)fd, (uintptr_t)&probe, 1, 0, 0, 0);
    tls_syscall(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);

    if (probe_n > 0) {
        return TLS_ERR_ANCHOR_BOUNDS;
    }

    return fortress_tls_parse_anchors(store->raw_buf, total_read, store);
}
