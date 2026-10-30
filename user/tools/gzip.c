#include "common.h"
#include "gzip.h"

static gzip_read_fn callback_read;
static gzip_write_fn callback_write;
static void *callback_context;
static bool callback_failed;

/* =========================================================================
 * Static Storage (Zero dynamic allocation, BSS-allocated)
 * ========================================================================= */

#define COMP_WINDOW_SIZE    65536
#define COMP_WSIZE          32768
#define COMP_HASH_BITS      13
#define COMP_HASH_SIZE      (1 << COMP_HASH_BITS) /* 8192 */
#define COMP_HASH_MASK      (COMP_HASH_SIZE - 1)
#define COMP_MAX_MATCH      258
#define COMP_MIN_MATCH      3
#define COMP_MAX_CHAIN      32

#define GZIP_IO_BUF_SIZE    4096
#define MAX_TREE_NODES      600

/* CRC-32 Table */
static uint32_t s_crc32_table[256];
static bool     s_crc32_ready = false;

/* Decompressor State */
static uint8_t  s_decomp_window[DEFLATE_WINDOW_SIZE];
static uint8_t  s_decomp_in_buf[GZIP_IO_BUF_SIZE];
static uint8_t  s_decomp_out_buf[GZIP_IO_BUF_SIZE];
static size_t   s_decomp_in_pos;
static size_t   s_decomp_in_len;
static bool     s_decomp_in_eof;
static uint64_t s_decomp_bit_buf;
static unsigned s_decomp_bits_left;
static size_t   s_decomp_window_pos;
static size_t   s_decomp_out_pos;
static uint64_t s_decomp_total_out;
static uint32_t s_decomp_crc_raw;

/* Canonical Huffman Trees */
static int16_t  s_tree_left[MAX_TREE_NODES];
static int16_t  s_tree_right[MAX_TREE_NODES];
static int      s_tree_next_node;

/* Static tables for decompression to ensure stack frame < 1 KiB */
static uint8_t  s_fixed_lit_len_tbl[288];
static uint8_t  s_fixed_dist_len_tbl[32];
static uint8_t  s_clen_lengths[19];
static uint8_t  s_litdist_lengths[288 + 32];

/* Precomputed Fixed Huffman Codes for Compressor */
static uint16_t s_fixed_lit_code[288];
static uint8_t  s_fixed_lit_len[288];
static uint8_t  s_fixed_dist_code[32];
static bool     s_fixed_ready = false;

#ifndef FORTRESS_KERNEL_BUILD
/* Compressor State */
static uint8_t  s_comp_buf[COMP_WINDOW_SIZE];
static uint16_t s_comp_head[COMP_HASH_SIZE];
static uint16_t s_comp_prev[COMP_WSIZE];
static uint8_t  s_comp_out_buf[GZIP_IO_BUF_SIZE];
static size_t   s_comp_out_pos;
static uint64_t s_comp_bit_buf;
static unsigned s_comp_bits_in_buf;
#endif

/* Shared RFC 1951 Base Tables */
static const uint16_t s_length_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t s_length_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t s_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const uint8_t s_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
static const uint8_t s_clen_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/* =========================================================================
 * Low-Level I/O Helpers
 * ========================================================================= */

static long tool_write_all(int fd, const void *buf, size_t count) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t rem = count;
    while (rem > 0) {
        long n = callback_write ? callback_write(callback_context,p,rem) :
            tool_syscall(SYS_WRITE, (uintptr_t)fd, (uintptr_t)p, rem);
        if (n<=0 || (size_t)n>rem) {
            if (callback_write) callback_failed=true;
            return -1;
        }
        p += (size_t)n;
        rem -= (size_t)n;
    }
    return (long)count;
}

/* =========================================================================
 * CRC-32 (RFC 1952 / IEEE 802.3 Polynomial 0xEDB88320)
 * ========================================================================= */

static void crc32_init_table(void) {
    if (s_crc32_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            c = (c & 1) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
        }
        s_crc32_table[i] = c;
    }
    s_crc32_ready = true;
}

uint32_t gzip_crc32(uint32_t init, const void *buf, size_t len) {
    crc32_init_table();
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t c = init ^ 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        c = s_crc32_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFU;
}

static inline uint32_t crc32_update_byte(uint32_t c, uint8_t b) {
    return s_crc32_table[(c ^ b) & 0xFF] ^ (c >> 8);
}

/* =========================================================================
 * Bit Operations & Huffman Tree Construction
 * ========================================================================= */

static inline uint32_t bit_reverse(uint32_t code, unsigned len) {
    uint32_t res = 0;
    for (unsigned i = 0; i < len; i++) {
        res = (res << 1) | (code & 1);
        code >>= 1;
    }
    return res;
}

static void init_fixed_tables(void) {
    if (s_fixed_ready) return;
    for (int i = 0; i <= 143; i++) {
        s_fixed_lit_len[i] = 8;
        s_fixed_lit_code[i] = (uint16_t)bit_reverse(48 + i, 8);
    }
    for (int i = 144; i <= 255; i++) {
        s_fixed_lit_len[i] = 9;
        s_fixed_lit_code[i] = (uint16_t)bit_reverse(400 + (i - 144), 9);
    }
    for (int i = 256; i <= 279; i++) {
        s_fixed_lit_len[i] = 7;
        s_fixed_lit_code[i] = (uint16_t)bit_reverse(0 + (i - 256), 7);
    }
    for (int i = 280; i <= 287; i++) {
        s_fixed_lit_len[i] = 8;
        s_fixed_lit_code[i] = (uint16_t)bit_reverse(192 + (i - 280), 8);
    }
    for (int i = 0; i < 32; i++) {
        s_fixed_dist_code[i] = (uint8_t)bit_reverse(i, 5);
    }
    s_fixed_ready = true;
}

/* Build a canonical binary tree in s_tree_left / s_tree_right */
static bool build_canonical_tree(const uint8_t *lengths, int num_symbols, int *root) {
    uint16_t bl_count[16];
    uint16_t next_code[16];
    for (int i = 0; i < 16; i++) {
        bl_count[i] = 0;
        next_code[i] = 0;
    }

    int max_len = 0;
    int num_present = 0;
    for (int i = 0; i < num_symbols; i++) {
        uint8_t len = lengths[i];
        if (len > 15) return false;
        if (len > 0) {
            bl_count[len]++;
            if (len > max_len) max_len = len;
            num_present++;
        }
    }

    if (num_present == 0) {
        *root = -1; /* Empty tree */
        return true;
    }

    uint16_t code = 0;
    bl_count[0] = 0;
    for (int bits = 1; bits <= 15; bits++) {
        code = (uint16_t)((code + bl_count[bits - 1]) << 1);
        next_code[bits] = code;
    }

    /* Check Kraft inequality */
    if (code + bl_count[15] > (1 << 15) && num_present > 1) {
        return false;
    }

    /* Allocate root node */
    if (s_tree_next_node >= MAX_TREE_NODES) return false;
    int r = s_tree_next_node++;
    s_tree_left[r] = 0;
    s_tree_right[r] = 0;
    *root = r;

    /* Degenerate case: single symbol with length 1 */
    if (num_present == 1) {
        for (int i = 0; i < num_symbols; i++) {
            if (lengths[i] > 0) {
                s_tree_left[r] = (int16_t)(-(i + 1));
                s_tree_right[r] = (int16_t)(-(i + 1));
                return true;
            }
        }
    }

    for (int i = 0; i < num_symbols; i++) {
        uint8_t len = lengths[i];
        if (len == 0) continue;
        uint16_t c = next_code[len]++;

        int cur = r;
        for (int b = len - 1; b >= 0; b--) {
            int bit = (c >> b) & 1;
            int16_t *child = bit ? &s_tree_right[cur] : &s_tree_left[cur];
            if (b == 0) {
                if (*child != 0) return false; /* Overlap */
                *child = (int16_t)(-(i + 1));
            } else {
                if (*child < 0) return false; /* Prefix collision */
                if (*child == 0) {
                    if (s_tree_next_node >= MAX_TREE_NODES) return false;
                    int n = s_tree_next_node++;
                    s_tree_left[n] = 0;
                    s_tree_right[n] = 0;
                    *child = (int16_t)n;
                    cur = n;
                } else {
                    cur = *child;
                }
            }
        }
    }
    return true;
}

/* =========================================================================
 * Decompression Engine (Bounded DEFLATE / Inflate, RFC 1951 + RFC 1952)
 * ========================================================================= */

static void decomp_bit_init(void) {
    s_decomp_in_pos = 0;
    s_decomp_in_len = 0;
    s_decomp_in_eof = false;
    s_decomp_bit_buf = 0;
    s_decomp_bits_left = 0;
}

static bool decomp_refill(int in_fd) {
    while (s_decomp_bits_left <= 56) {
        if (s_decomp_in_pos >= s_decomp_in_len) {
            long n = callback_read ? callback_read(callback_context,s_decomp_in_buf,sizeof(s_decomp_in_buf)) :
                tool_syscall(SYS_READ, (uintptr_t)in_fd, (uintptr_t)s_decomp_in_buf, sizeof(s_decomp_in_buf));
            if (callback_read && (n<0 || (size_t)n>sizeof(s_decomp_in_buf))) {
                callback_failed=true;
                return false;
            }
            if (n <= 0) {
                s_decomp_in_eof = true;
                return (s_decomp_bits_left > 0);
            }
            s_decomp_in_pos = 0;
            s_decomp_in_len = (size_t)n;
        }
        uint8_t b = s_decomp_in_buf[s_decomp_in_pos++];
        s_decomp_bit_buf |= ((uint64_t)b) << s_decomp_bits_left;
        s_decomp_bits_left += 8;
    }
    return true;
}

static inline uint32_t decomp_get_bits(int in_fd, unsigned n, bool *ok) {
    if (n == 0) {
        *ok = true;
        return 0;
    }
    while (s_decomp_bits_left < n) {
        /* EOF can leave a positive number of bits smaller than the request.
         * Refill reports those residual bits, but cannot make progress. */
        (void)decomp_refill(in_fd);
        if (s_decomp_bits_left < n) {
            *ok = false;
            return 0;
        }
    }
    uint32_t val = (uint32_t)(s_decomp_bit_buf & ((1ULL << n) - 1));
    s_decomp_bit_buf >>= n;
    s_decomp_bits_left -= n;
    *ok = true;
    return val;
}

static inline void decomp_align_byte(void) {
    unsigned drop = s_decomp_bits_left % 8;
    s_decomp_bit_buf >>= drop;
    s_decomp_bits_left -= drop;
}

static int decomp_decode_symbol(int in_fd, int root) {
    if (root < 0) return GZIP_ERR_CORRUPT;
    int cur = root;
    while (cur >= 0) {
        bool ok = false;
        uint32_t bit = decomp_get_bits(in_fd, 1, &ok);
        if (!ok) return GZIP_ERR_TRUNCATED;
        int16_t next = bit ? s_tree_right[cur] : s_tree_left[cur];
        if (next == 0) return GZIP_ERR_CORRUPT;
        if (next < 0) {
            return -(next + 1);
        }
        cur = next;
    }
    return GZIP_ERR_CORRUPT;
}

static int decomp_emit_byte(int out_fd, uint8_t b) {
    if (s_decomp_total_out >= GZIP_MAX_OUTPUT) {
        return GZIP_ERR_BOMB;
    }
    s_decomp_window[s_decomp_window_pos] = b;
    s_decomp_window_pos = (s_decomp_window_pos + 1) & (DEFLATE_WINDOW_SIZE - 1);

    s_decomp_out_buf[s_decomp_out_pos++] = b;
    if (s_decomp_out_pos == sizeof(s_decomp_out_buf)) {
        if (out_fd >= 0) {
            long w = tool_write_all(out_fd, s_decomp_out_buf, s_decomp_out_pos);
            if (w < 0) return GZIP_ERR_IO;
        }
        s_decomp_out_pos = 0;
    }
    s_decomp_crc_raw = crc32_update_byte(s_decomp_crc_raw, b);
    s_decomp_total_out++;
    return GZIP_OK;
}

static int decomp_flush(int out_fd) {
    if (s_decomp_out_pos > 0 && out_fd >= 0) {
        long w = tool_write_all(out_fd, s_decomp_out_buf, s_decomp_out_pos);
        if (w < 0) return GZIP_ERR_IO;
        s_decomp_out_pos = 0;
    }
    return GZIP_OK;
}

static int decompress_huffman_block(int in_fd, int out_fd, int lit_root, int dist_root) {
    for (;;) {
        int sym = decomp_decode_symbol(in_fd, lit_root);
        if (sym < 0) return sym;

        if (sym < 256) {
            int err = decomp_emit_byte(out_fd, (uint8_t)sym);
            if (err) return err;
        } else if (sym == 256) {
            /* End of block */
            break;
        } else if (sym <= 285) {
            unsigned len_idx = (unsigned)(sym - 257);
            bool ok = false;
            uint32_t extra = decomp_get_bits(in_fd, s_length_extra[len_idx], &ok);
            if (!ok) return GZIP_ERR_TRUNCATED;
            uint32_t length = s_length_base[len_idx] + extra;

            int dist_sym = decomp_decode_symbol(in_fd, dist_root);
            if (dist_sym < 0) return dist_sym;
            if (dist_sym >= 30) return GZIP_ERR_CORRUPT;

            uint32_t dist_extra_bits = decomp_get_bits(in_fd, s_dist_extra[dist_sym], &ok);
            if (!ok) return GZIP_ERR_TRUNCATED;
            uint32_t distance = s_dist_base[dist_sym] + dist_extra_bits;

            if (distance == 0 || distance > s_decomp_total_out || distance > DEFLATE_WINDOW_SIZE) {
                return GZIP_ERR_DIST_OVERFLOW;
            }

            for (uint32_t i = 0; i < length; i++) {
                size_t src = (s_decomp_window_pos + DEFLATE_WINDOW_SIZE - distance) & (DEFLATE_WINDOW_SIZE - 1);
                uint8_t b = s_decomp_window[src];
                int err = decomp_emit_byte(out_fd, b);
                if (err) return err;
            }
        } else {
            return GZIP_ERR_CORRUPT;
        }
    }
    return GZIP_OK;
}

static int decompress_stream(int in_fd, int out_fd) {
    crc32_init_table();
    init_fixed_tables();
    decomp_bit_init();

    s_decomp_window_pos = 0;
    s_decomp_out_pos = 0;
    s_decomp_total_out = 0;
    s_decomp_crc_raw = 0xFFFFFFFFU;

    /* 1. Parse GZIP Header (10 bytes min) */
    bool ok = false;
    uint32_t id1 = decomp_get_bits(in_fd, 8, &ok);
    uint32_t id2 = decomp_get_bits(in_fd, 8, &ok);
    if (!ok || id1 != GZIP_MAGIC1 || id2 != GZIP_MAGIC2) {
        return GZIP_ERR_HEADER;
    }
    uint32_t cm = decomp_get_bits(in_fd, 8, &ok);
    if (!ok || cm != GZIP_CM_DEFLATE) {
        return GZIP_ERR_HEADER;
    }
    uint32_t flg = decomp_get_bits(in_fd, 8, &ok);
    if (!ok || (flg & GZIP_FLG_RESERVED)) {
        return GZIP_ERR_HEADER; /* Reject unknown/reserved flags */
    }

    /* MTIME (4 bytes), XFL (1 byte), OS (1 byte) */
    (void)decomp_get_bits(in_fd, 16, &ok);
    (void)decomp_get_bits(in_fd, 16, &ok);
    (void)decomp_get_bits(in_fd, 8, &ok);
    (void)decomp_get_bits(in_fd, 8, &ok);
    if (!ok) return GZIP_ERR_TRUNCATED;

    /* FEXTRA */
    if (flg & GZIP_FLG_FEXTRA) {
        uint32_t xlen = decomp_get_bits(in_fd, 16, &ok);
        if (!ok) return GZIP_ERR_TRUNCATED;
        for (uint32_t i = 0; i < xlen; i++) {
            (void)decomp_get_bits(in_fd, 8, &ok);
            if (!ok) return GZIP_ERR_TRUNCATED;
        }
    }

    /* FNAME */
    if (flg & GZIP_FLG_FNAME_MASK) {
        for (;;) {
            uint32_t c = decomp_get_bits(in_fd, 8, &ok);
            if (!ok) return GZIP_ERR_TRUNCATED;
            if (c == 0) break;
        }
    }

    /* FCOMMENT */
    if (flg & GZIP_FLG_FCOMMENT) {
        for (;;) {
            uint32_t c = decomp_get_bits(in_fd, 8, &ok);
            if (!ok) return GZIP_ERR_TRUNCATED;
            if (c == 0) break;
        }
    }

    /* FHCRC */
    if (flg & GZIP_FLG_FHCRC) {
        (void)decomp_get_bits(in_fd, 16, &ok);
        if (!ok) return GZIP_ERR_TRUNCATED;
    }

    /* 2. Process DEFLATE blocks */
    bool bfinal = false;
    while (!bfinal) {
        uint32_t bfinal_bit = decomp_get_bits(in_fd, 1, &ok);
        uint32_t btype = decomp_get_bits(in_fd, 2, &ok);
        if (!ok) return GZIP_ERR_TRUNCATED;
        bfinal = (bfinal_bit != 0);

        if (btype == 0) {
            /* Stored / Uncompressed Block */
            decomp_align_byte();
            uint32_t len = decomp_get_bits(in_fd, 16, &ok);
            uint32_t nlen = decomp_get_bits(in_fd, 16, &ok);
            if (!ok) return GZIP_ERR_TRUNCATED;
            if ((len ^ 0xFFFF) != nlen) return GZIP_ERR_CORRUPT;

            for (uint32_t i = 0; i < len; i++) {
                uint32_t b = decomp_get_bits(in_fd, 8, &ok);
                if (!ok) return GZIP_ERR_TRUNCATED;
                int err = decomp_emit_byte(out_fd, (uint8_t)b);
                if (err) return err;
            }
        } else if (btype == 1) {
            /* Fixed Huffman Block */
            for (int i = 0; i <= 143; i++) s_fixed_lit_len_tbl[i] = 8;
            for (int i = 144; i <= 255; i++) s_fixed_lit_len_tbl[i] = 9;
            for (int i = 256; i <= 279; i++) s_fixed_lit_len_tbl[i] = 7;
            for (int i = 280; i <= 287; i++) s_fixed_lit_len_tbl[i] = 8;
            for (int i = 0; i < 32; i++) s_fixed_dist_len_tbl[i] = 5;

            s_tree_next_node = 0;
            int lit_root = 0, dist_root = 0;
            if (!build_canonical_tree(s_fixed_lit_len_tbl, 288, &lit_root)) return GZIP_ERR_CORRUPT;
            if (!build_canonical_tree(s_fixed_dist_len_tbl, 32, &dist_root)) return GZIP_ERR_CORRUPT;

            int err = decompress_huffman_block(in_fd, out_fd, lit_root, dist_root);
            if (err) return err;
        } else if (btype == 2) {
            /* Dynamic Huffman Block */
            uint32_t hlit = decomp_get_bits(in_fd, 5, &ok) + 257;
            uint32_t hdist = decomp_get_bits(in_fd, 5, &ok) + 1;
            uint32_t hclen = decomp_get_bits(in_fd, 4, &ok) + 4;
            if (!ok) return GZIP_ERR_TRUNCATED;
            if (hlit > 286 || hdist > 32) return GZIP_ERR_CORRUPT;

            for (int i = 0; i < 19; i++) s_clen_lengths[i] = 0;
            for (uint32_t i = 0; i < hclen; i++) {
                s_clen_lengths[s_clen_order[i]] = (uint8_t)decomp_get_bits(in_fd, 3, &ok);
                if (!ok) return GZIP_ERR_TRUNCATED;
            }

            s_tree_next_node = 0;
            int clen_root = 0;
            if (!build_canonical_tree(s_clen_lengths, 19, &clen_root)) return GZIP_ERR_CORRUPT;

            /* Decode code lengths for literal/length and distance trees */
            uint32_t total_codes = hlit + hdist;
            uint32_t idx = 0;
            while (idx < total_codes) {
                int sym = decomp_decode_symbol(in_fd, clen_root);
                if (sym < 0) return sym;
                if (sym <= 15) {
                    s_litdist_lengths[idx++] = (uint8_t)sym;
                } else if (sym == 16) {
                    if (idx == 0) return GZIP_ERR_CORRUPT;
                    uint32_t rep = 3 + decomp_get_bits(in_fd, 2, &ok);
                    if (!ok) return GZIP_ERR_TRUNCATED;
                    if (idx + rep > total_codes) return GZIP_ERR_CORRUPT;
                    uint8_t prev = s_litdist_lengths[idx - 1];
                    while (rep--) s_litdist_lengths[idx++] = prev;
                } else if (sym == 17) {
                    uint32_t rep = 3 + decomp_get_bits(in_fd, 3, &ok);
                    if (!ok) return GZIP_ERR_TRUNCATED;
                    if (idx + rep > total_codes) return GZIP_ERR_CORRUPT;
                    while (rep--) s_litdist_lengths[idx++] = 0;
                } else if (sym == 18) {
                    uint32_t rep = 11 + decomp_get_bits(in_fd, 7, &ok);
                    if (!ok) return GZIP_ERR_TRUNCATED;
                    if (idx + rep > total_codes) return GZIP_ERR_CORRUPT;
                    while (rep--) s_litdist_lengths[idx++] = 0;
                } else {
                    return GZIP_ERR_CORRUPT;
                }
            }

            /* EOB symbol 256 must be present */
            if (s_litdist_lengths[256] == 0) return GZIP_ERR_CORRUPT;

            s_tree_next_node = 0;
            int lit_root = 0, dist_root = 0;
            if (!build_canonical_tree(s_litdist_lengths, (int)hlit, &lit_root)) return GZIP_ERR_CORRUPT;
            if (!build_canonical_tree(s_litdist_lengths + hlit, (int)hdist, &dist_root)) return GZIP_ERR_CORRUPT;

            int err = decompress_huffman_block(in_fd, out_fd, lit_root, dist_root);
            if (err) return err;
        } else {
            return GZIP_ERR_CORRUPT; /* Reserved btype 3 */
        }
    }

    int flush_err = decomp_flush(out_fd);
    if (flush_err) return flush_err;

    /* 3. Trailer: align to byte boundary, read CRC32 and ISIZE */
    decomp_align_byte();
    uint32_t crc_b0 = decomp_get_bits(in_fd, 8, &ok);
    uint32_t crc_b1 = decomp_get_bits(in_fd, 8, &ok);
    uint32_t crc_b2 = decomp_get_bits(in_fd, 8, &ok);
    uint32_t crc_b3 = decomp_get_bits(in_fd, 8, &ok);
    uint32_t isize_b0 = decomp_get_bits(in_fd, 8, &ok);
    uint32_t isize_b1 = decomp_get_bits(in_fd, 8, &ok);
    uint32_t isize_b2 = decomp_get_bits(in_fd, 8, &ok);
    uint32_t isize_b3 = decomp_get_bits(in_fd, 8, &ok);
    if (!ok) return GZIP_ERR_TRUNCATED;

    uint32_t exp_crc = crc_b0 | (crc_b1 << 8) | (crc_b2 << 16) | (crc_b3 << 24);
    uint32_t exp_isize = isize_b0 | (isize_b1 << 8) | (isize_b2 << 16) | (isize_b3 << 24);

    uint32_t actual_crc = s_decomp_crc_raw ^ 0xFFFFFFFFU;
    if (actual_crc != exp_crc) {
        return GZIP_ERR_CRC;
    }
    if ((uint32_t)(s_decomp_total_out & 0xFFFFFFFFU) != exp_isize) {
        return GZIP_ERR_SIZE;
    }

    /* Reject multi-member if extra data follows */
    (void)decomp_refill(in_fd);
    if (s_decomp_bits_left >= 8 || s_decomp_in_pos < s_decomp_in_len) {
        return GZIP_ERR_MULTI_MEMBER;
    }

    return GZIP_OK;
}

/* =========================================================================
 * Compression Engine (RFC 1951 DEFLATE + RFC 1952 GZIP)
 * ========================================================================= */

int gzip_decode_callbacks(gzip_read_fn read, gzip_write_fn write, void *context) {
    if (!read || !write || callback_read) return GZIP_ERR_IO;
    callback_read=read; callback_write=write; callback_context=context;
    callback_failed=false;
    int result=decompress_stream(0,1);
    if (callback_failed) result=GZIP_ERR_IO;
    callback_read=NULL; callback_write=NULL; callback_context=NULL;
    return result;
}

#ifndef FORTRESS_KERNEL_BUILD
static void comp_bit_init(void) {
    s_comp_out_pos = 0;
    s_comp_bit_buf = 0;
    s_comp_bits_in_buf = 0;
}

static int comp_put_bits(int out_fd, uint32_t val, unsigned n) {
    s_comp_bit_buf |= ((uint64_t)val) << s_comp_bits_in_buf;
    s_comp_bits_in_buf += n;
    while (s_comp_bits_in_buf >= 8) {
        uint8_t b = (uint8_t)(s_comp_bit_buf & 0xFF);
        s_comp_bit_buf >>= 8;
        s_comp_bits_in_buf -= 8;

        s_comp_out_buf[s_comp_out_pos++] = b;
        if (s_comp_out_pos == sizeof(s_comp_out_buf)) {
            long w = tool_write_all(out_fd, s_comp_out_buf, s_comp_out_pos);
            if (w < 0) return GZIP_ERR_IO;
            s_comp_out_pos = 0;
        }
    }
    return GZIP_OK;
}

static int comp_flush_bytes(int out_fd) {
    if (s_comp_bits_in_buf > 0) {
        uint8_t b = (uint8_t)(s_comp_bit_buf & 0xFF);
        s_comp_bit_buf = 0;
        s_comp_bits_in_buf = 0;
        s_comp_out_buf[s_comp_out_pos++] = b;
    }
    if (s_comp_out_pos > 0) {
        long w = tool_write_all(out_fd, s_comp_out_buf, s_comp_out_pos);
        if (w < 0) return GZIP_ERR_IO;
        s_comp_out_pos = 0;
    }
    return GZIP_OK;
}

static inline void get_length_code(uint32_t len, uint32_t *code, uint32_t *extra, unsigned *extra_bits) {
    for (int i = 28; i >= 0; i--) {
        if (len >= s_length_base[i]) {
            *code = (uint32_t)(257 + i);
            *extra_bits = s_length_extra[i];
            *extra = len - s_length_base[i];
            return;
        }
    }
    *code = 257;
    *extra_bits = 0;
    *extra = 0;
}

static inline void get_dist_code(uint32_t dist, uint32_t *code, uint32_t *extra, unsigned *extra_bits) {
    for (int i = 29; i >= 0; i--) {
        if (dist >= s_dist_base[i]) {
            *code = (uint32_t)i;
            *extra_bits = s_dist_extra[i];
            *extra = dist - s_dist_base[i];
            return;
        }
    }
    *code = 0;
    *extra_bits = 0;
    *extra = 0;
}

static int compress_stream(int in_fd, int out_fd) {
    crc32_init_table();
    init_fixed_tables();
    comp_bit_init();

    /* 1. Emit 10-byte standard GZIP header */
    uint8_t hdr[10] = {
        GZIP_MAGIC1, GZIP_MAGIC2, GZIP_CM_DEFLATE, 0x00, /* ID1, ID2, CM, FLG */
        0x00, 0x00, 0x00, 0x00,                         /* MTIME */
        0x02,                                           /* XFL = 2 (max compression) */
        0x03                                            /* OS = 3 (Unix) */
    };
    if (tool_write_all(out_fd, hdr, sizeof(hdr)) < 0) return GZIP_ERR_IO;

    /* 2. Emit BFINAL=1, BTYPE=01 (Fixed Huffman) block header: 3 bits (011_2 = 3) */
    if (comp_put_bits(out_fd, 0x03, 3) != GZIP_OK) return GZIP_ERR_IO;

    /* Initialize hash table */
    for (int i = 0; i < COMP_HASH_SIZE; i++) s_comp_head[i] = 0xFFFF;
    for (int i = 0; i < COMP_WSIZE; i++) s_comp_prev[i] = 0xFFFF;

    size_t strstart = 0;
    size_t lookahead = 0;
    bool in_eof = false;
    uint32_t crc_raw = 0xFFFFFFFFU;
    uint64_t total_in = 0;

    /* Fill initial lookahead buffer */
    while (lookahead < sizeof(s_comp_buf)) {
        long rd = tool_syscall(SYS_READ, (uintptr_t)in_fd, (uintptr_t)(s_comp_buf + lookahead), sizeof(s_comp_buf) - lookahead);
        if (rd < 0) return GZIP_ERR_IO;
        if (rd == 0) {
            in_eof = true;
            break;
        }
        for (size_t k = 0; k < (size_t)rd; k++) {
            crc_raw = crc32_update_byte(crc_raw, s_comp_buf[lookahead + k]);
        }
        total_in += (size_t)rd;
        lookahead += (size_t)rd;
    }

    uint32_t ins_h = 0;
    if (lookahead >= 2) {
        ins_h = (((uint32_t)s_comp_buf[0] << 6) ^ ((uint32_t)s_comp_buf[1] << 3)) & COMP_HASH_MASK;
    }

    while (lookahead > 0) {
        /* Slide window if strstart reaches the second half (32 KiB) */
        if (strstart >= COMP_WSIZE) {
            size_t bytes_to_copy = (strstart + lookahead) - COMP_WSIZE;
            for (size_t i = 0; i < bytes_to_copy; i++) {
                s_comp_buf[i] = s_comp_buf[i + COMP_WSIZE];
            }
            strstart -= COMP_WSIZE;

            /* Adjust hash table positions */
            for (int i = 0; i < COMP_HASH_SIZE; i++) {
                uint16_t pos = s_comp_head[i];
                s_comp_head[i] = (pos >= COMP_WSIZE && pos != 0xFFFF) ? (uint16_t)(pos - COMP_WSIZE) : 0xFFFF;
            }
            for (int i = 0; i < COMP_WSIZE; i++) {
                uint16_t pos = s_comp_prev[i];
                s_comp_prev[i] = (pos >= COMP_WSIZE && pos != 0xFFFF) ? (uint16_t)(pos - COMP_WSIZE) : 0xFFFF;
            }

            /* Read more into lookahead buffer */
            if (!in_eof) {
                size_t space = sizeof(s_comp_buf) - (strstart + lookahead);
                while (space > 0) {
                    long n = tool_syscall(SYS_READ, (uintptr_t)in_fd, (uintptr_t)(s_comp_buf + strstart + lookahead), space);
                    if (n < 0) return GZIP_ERR_IO;
                    if (n == 0) {
                        in_eof = true;
                        break;
                    }
                    for (size_t k = 0; k < (size_t)n; k++) {
                        crc_raw = crc32_update_byte(crc_raw, s_comp_buf[strstart + lookahead + k]);
                    }
                    total_in += (size_t)n;
                    lookahead += (size_t)n;
                    space -= (size_t)n;
                    if (space < 4096) break;
                }
            }
        }

        /* Match finding */
        uint32_t match_len = 0;
        uint32_t match_dist = 0;

        if (lookahead >= COMP_MIN_MATCH) {
            ins_h = (((ins_h << 3) ^ (uint32_t)s_comp_buf[strstart + 2]) & COMP_HASH_MASK);
            uint16_t cur_match = s_comp_head[ins_h];
            s_comp_prev[strstart & (COMP_WSIZE - 1)] = cur_match;
            s_comp_head[ins_h] = (uint16_t)strstart;

            unsigned chain = COMP_MAX_CHAIN;
            size_t max_search = lookahead < COMP_MAX_MATCH ? lookahead : COMP_MAX_MATCH;

            while (cur_match != 0xFFFF && chain-- > 0) {
                size_t dist = strstart - cur_match;
                if (dist > COMP_WSIZE) break;

                /* Compare bytes */
                size_t l = 0;
                while (l < max_search && s_comp_buf[strstart + l] == s_comp_buf[cur_match + l]) {
                    l++;
                }

                if (l > match_len) {
                    match_len = (uint32_t)l;
                    match_dist = (uint32_t)dist;
                    if (match_len >= COMP_MAX_MATCH) break;
                }
                cur_match = s_comp_prev[cur_match & (COMP_WSIZE - 1)];
            }
        }

        if (match_len >= COMP_MIN_MATCH) {
            uint32_t lcode, lextra, lextra_bits;
            get_length_code(match_len, &lcode, &lextra, &lextra_bits);

            uint32_t dcode, dextra, dextra_bits;
            get_dist_code(match_dist, &dcode, &dextra, &dextra_bits);

            if (comp_put_bits(out_fd, s_fixed_lit_code[lcode], s_fixed_lit_len[lcode]) != GZIP_OK) return GZIP_ERR_IO;
            if (lextra_bits > 0) {
                if (comp_put_bits(out_fd, lextra, lextra_bits) != GZIP_OK) return GZIP_ERR_IO;
            }
            if (comp_put_bits(out_fd, s_fixed_dist_code[dcode], 5) != GZIP_OK) return GZIP_ERR_IO;
            if (dextra_bits > 0) {
                if (comp_put_bits(out_fd, dextra, dextra_bits) != GZIP_OK) return GZIP_ERR_IO;
            }

            /* Advance and insert intermediate hashes */
            strstart++;
            lookahead--;
            for (uint32_t k = 1; k < match_len; k++) {
                if (lookahead >= COMP_MIN_MATCH) {
                    ins_h = (((ins_h << 3) ^ (uint32_t)s_comp_buf[strstart + 2]) & COMP_HASH_MASK);
                    s_comp_prev[strstart & (COMP_WSIZE - 1)] = s_comp_head[ins_h];
                    s_comp_head[ins_h] = (uint16_t)strstart;
                }
                strstart++;
                lookahead--;
            }
        } else {
            uint8_t lit = s_comp_buf[strstart];
            if (comp_put_bits(out_fd, s_fixed_lit_code[lit], s_fixed_lit_len[lit]) != GZIP_OK) return GZIP_ERR_IO;
            strstart++;
            lookahead--;
        }
    }

    /* 3. Emit End of Block (symbol 256) */
    if (comp_put_bits(out_fd, s_fixed_lit_code[256], s_fixed_lit_len[256]) != GZIP_OK) return GZIP_ERR_IO;
    if (comp_flush_bytes(out_fd) != GZIP_OK) return GZIP_ERR_IO;

    /* 4. Emit 8-byte Trailer (CRC32 and ISIZE) */
    uint32_t final_crc = crc_raw ^ 0xFFFFFFFFU;
    uint32_t isize = (uint32_t)(total_in & 0xFFFFFFFFU);
    uint8_t trailer[8];
    trailer[0] = (uint8_t)(final_crc & 0xFF);
    trailer[1] = (uint8_t)((final_crc >> 8) & 0xFF);
    trailer[2] = (uint8_t)((final_crc >> 16) & 0xFF);
    trailer[3] = (uint8_t)((final_crc >> 24) & 0xFF);
    trailer[4] = (uint8_t)(isize & 0xFF);
    trailer[5] = (uint8_t)((isize >> 8) & 0xFF);
    trailer[6] = (uint8_t)((isize >> 16) & 0xFF);
    trailer[7] = (uint8_t)((isize >> 24) & 0xFF);

    if (tool_write_all(out_fd, trailer, sizeof(trailer)) < 0) return GZIP_ERR_IO;
    return GZIP_OK;
}

/* =========================================================================
 * CLI Driver & File Operations
 * ========================================================================= */

static const char *gzip_error_string(int err) {
    switch (err) {
        case GZIP_ERR_IO:            return "input/output error";
        case GZIP_ERR_TRUNCATED:     return "unexpected end of file";
        case GZIP_ERR_CORRUPT:       return "invalid compressed data";
        case GZIP_ERR_DIST_OVERFLOW: return "invalid compressed data (distance overflow)";
        case GZIP_ERR_BOMB:          return "decompressed size exceeds maximum limit (100MB)";
        case GZIP_ERR_CRC:           return "CRC error";
        case GZIP_ERR_SIZE:          return "length error";
        case GZIP_ERR_MULTI_MEMBER:  return "multi-member gzip files not supported";
        case GZIP_ERR_HEADER:        return "not in gzip format";
        default:                     return "unknown error";
    }
}

static bool ends_with(const char *str, const char *suffix) {
    size_t slen = tool_length(str);
    size_t xlen = tool_length(suffix);
    if (slen < xlen) return false;
    return tool_equal(str + (slen - xlen), suffix);
}

static int print_gzip_help(const char *prog) {
    (void)tool_syscall(SYS_WRITE, 1, (uintptr_t)"Usage: ", 7);
    (void)tool_syscall(SYS_WRITE, 1, (uintptr_t)prog, tool_length(prog));
    (void)tool_syscall(SYS_WRITE, 1, (uintptr_t)" [OPTION]... [FILE]...\n\n", 24);
    (void)tool_syscall(SYS_WRITE, 1, (uintptr_t)"Compress or decompress FILEs (by default, in place).\n\n", 54);
    (void)tool_syscall(SYS_WRITE, 1, (uintptr_t)"  -c, --stdout      write on standard output, keep original files unchanged\n", 76);
    (void)tool_syscall(SYS_WRITE, 1, (uintptr_t)"  -d, --decompress  decompress\n", 31);
    (void)tool_syscall(SYS_WRITE, 1, (uintptr_t)"  -f, --force       force overwrite of output file\n", 51);
    (void)tool_syscall(SYS_WRITE, 1, (uintptr_t)"  -k, --keep        keep (don't delete) input files\n", 52);
    (void)tool_syscall(SYS_WRITE, 1, (uintptr_t)"  -1 .. -9          compression speed/ratio\n", 44);
    (void)tool_syscall(SYS_WRITE, 1, (uintptr_t)"  -h, --help        display this help and exit\n\n", 48);
    (void)tool_syscall(SYS_WRITE, 1, (uintptr_t)"With no FILE, or when FILE is -, read standard input.\n", 54);
    return 0;
}

int gzip_main_internal(int argc, char **argv, bool default_decompress) {
    bool decompress = default_decompress;
    bool write_stdout = false;
    bool keep_original = false;
    bool force = false;

    const char *prog = "gzip";
    if (argc > 0 && argv[0]) {
        if (ends_with(argv[0], "gunzip")) {
            decompress = true;
            prog = "gunzip";
        }
    }

    int optind = 1;
    for (; optind < argc; optind++) {
        const char *arg = argv[optind];
        if (arg[0] != '-' || arg[1] == '\0') break;
        if (tool_equal(arg, "--")) {
            optind++;
            break;
        }
        if (tool_equal(arg, "-h") || tool_equal(arg, "--help")) {
            return print_gzip_help(prog);
        }
        if (tool_equal(arg, "--stdout")) {
            write_stdout = true;
            continue;
        }
        if (tool_equal(arg, "--decompress") || tool_equal(arg, "--uncompress")) {
            decompress = true;
            continue;
        }
        if (tool_equal(arg, "--keep")) {
            keep_original = true;
            continue;
        }
        if (tool_equal(arg, "--force")) {
            force = true;
            continue;
        }

        /* Combined short flags (e.g. -dc, -cd, -dcf) */
        bool valid = true;
        for (size_t j = 1; arg[j] != '\0'; j++) {
            char c = arg[j];
            if (c == 'c') write_stdout = true;
            else if (c == 'd') decompress = true;
            else if (c == 'k') keep_original = true;
            else if (c == 'f') force = true;
            else if (c >= '1' && c <= '9') { /* Accepted compression level */ }
            else if (c == 'h') return print_gzip_help(prog);
            else { valid = false; break; }
        }
        if (!valid) {
            tool_error(prog, "invalid option; use --help for usage", arg);
            return 1;
        }
    }

    /* Stdin / Stdout stream mode */
    if (optind >= argc || (optind + 1 == argc && tool_equal(argv[optind], "-"))) {
        int err = decompress ? decompress_stream(0, 1) : compress_stream(0, 1);
        if (err != GZIP_OK) {
            tool_error(prog, gzip_error_string(err), "<stdin>");
            return 1;
        }
        return 0;
    }

    int final_status = 0;

    for (int i = optind; i < argc; i++) {
        const char *src_path = argv[i];
        if (tool_equal(src_path, "-")) {
            int err = decompress ? decompress_stream(0, 1) : compress_stream(0, 1);
            if (err != GZIP_OK) {
                tool_error(prog, gzip_error_string(err), "<stdin>");
                final_status = 1;
            }
            continue;
        }

        vfs_stat_t st;
        long sr = tool_syscall(SYS_STAT, (uintptr_t)src_path, (uintptr_t)&st, 0);
        if (sr < 0) {
            tool_sys_error(prog, "cannot stat", src_path, sr);
            final_status = 1;
            continue;
        }
        if (st.type == VFS_DIRECTORY) {
            tool_error(prog, "is a directory -- ignored", src_path);
            final_status = 1;
            continue;
        }

        char dest_path[VFS_MAX_PATH];
        dest_path[0] = '\0';

        if (!write_stdout) {
            size_t slen = tool_length(src_path);
            if (decompress) {
                if (!ends_with(src_path, ".gz")) {
                    tool_error(prog, "unknown suffix -- ignored", src_path);
                    final_status = 1;
                    continue;
                }
                size_t dlen = slen - 3;
                if (dlen >= sizeof(dest_path)) {
                    tool_error(prog, "filename too long", src_path);
                    final_status = 1;
                    continue;
                }
                for (size_t k = 0; k < dlen; k++) dest_path[k] = src_path[k];
                dest_path[dlen] = '\0';
            } else {
                if (slen + 3 >= sizeof(dest_path)) {
                    tool_error(prog, "filename too long", src_path);
                    final_status = 1;
                    continue;
                }
                for (size_t k = 0; k < slen; k++) dest_path[k] = src_path[k];
                dest_path[slen] = '.';
                dest_path[slen + 1] = 'g';
                dest_path[slen + 2] = 'z';
                dest_path[slen + 3] = '\0';
            }

            /* Check if destination already exists */
            if (!force) {
                vfs_stat_t dst_st;
                long dst_r = tool_syscall(SYS_STAT, (uintptr_t)dest_path, (uintptr_t)&dst_st, 0);
                if (dst_r == 0) {
                    tool_error(prog, "output file already exists; use -f to overwrite", dest_path);
                    final_status = 1;
                    continue;
                }
            }
        }

        long in_fd = tool_syscall(SYS_OPEN, (uintptr_t)src_path, VFS_O_RDONLY, 0);
        if (in_fd < 0) {
            tool_sys_error(prog, "cannot open input file", src_path, in_fd);
            final_status = 1;
            continue;
        }

        long out_fd = 1;
        bool created_dest = false;
        if (!write_stdout) {
            out_fd = tool_syscall(SYS_OPEN, (uintptr_t)dest_path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, 0644);
            if (out_fd < 0) {
                tool_sys_error(prog, "cannot create output file", dest_path, out_fd);
                tool_syscall(SYS_CLOSE, in_fd, 0, 0);
                final_status = 1;
                continue;
            }
            created_dest = true;
        }

        int err = decompress ? decompress_stream((int)in_fd, (int)out_fd)
                             : compress_stream((int)in_fd, (int)out_fd);

        tool_syscall(SYS_CLOSE, in_fd, 0, 0);
        if (created_dest) {
            tool_syscall(SYS_CLOSE, out_fd, 0, 0);
        }

        if (err != GZIP_OK) {
            tool_error(prog, gzip_error_string(err), src_path);
            if (created_dest) {
                (void)tool_syscall(SYS_UNLINK, (uintptr_t)dest_path, 0, 0);
            }
            final_status = 1;
        } else {
            /* Success: delete original file unless -k or -c */
            if (!write_stdout && !keep_original) {
                (void)tool_syscall(SYS_UNLINK, (uintptr_t)src_path, 0, 0);
            }
        }
    }

    return final_status;
}

#ifndef GUNZIP_STANDALONE
int gzip_main(int argc, char **argv) {
    return gzip_main_internal(argc, argv, false);
}
#endif
#endif
