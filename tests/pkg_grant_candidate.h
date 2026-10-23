#ifndef PKG_GRANT_CANDIDATE_H
#define PKG_GRANT_CANDIDATE_H
#include <stdint.h>
#include <stddef.h>

/* Private candidate, deliberately outside production source/build paths.
 * No authorization, filesystem identity lookup or disk I/O is performed.
 * Buffers must be valid, disjoint caller-owned memory. Failure preserves out. */
enum { PG_IDLE, PG_RESERVED, PG_PENDING, PG_RETIRING };
typedef struct {
    uint64_t sequence, epoch, total[2], remaining[2], charged[2];
    uint16_t phase;
} pg_view;
int pg_decode(const uint8_t *wire, size_t length, pg_view *out);
/* Canonicalize a validated wire record into a separate exact-size buffer. */
int pg_encode(const uint8_t *wire, size_t length, uint8_t *out, size_t capacity);

/* Caller-supplied trusted expectations, NOT derived from the supplied records.
 * UUID order: filesystem, partition, installation, repository, enrollment.
 * Locators: ledger, ownership table, transaction directory. This private gate
 * checks record consistency, not actual inode/map ownership or boot authority. */
typedef struct {
    uint8_t uuid[5][16], profile[32], marker_sha256[32];
    uint64_t epoch;
    uint32_t inode[3], generation[3];
} pg_binding;
int pg_validate_set(const uint8_t *marker, size_t marker_length,
                    const uint8_t *ledger, size_t ledger_length,
                    const uint8_t *ownership, size_t ownership_length,
                    const pg_binding *expected, pg_view *out);

typedef struct {
    uint64_t free[2], remaining[2], charged[2], total[2];
    uint64_t capacity[2], baseline[2], reserve[2], sequence;
} pg_budget;
enum { PG_CHARGE, PG_REFUND, PG_TRANSFER, PG_RELEASE, PG_BASE_FREE, PG_ORDINARY };
/* Arithmetic preflight only. Caller must separately establish role provenance,
 * phase, actual allocation maps, unpinned reclaim and remaining-plan authority. */
int pg_budget_valid(const pg_budget *b);
int pg_plan(const pg_budget *before, unsigned operation,
            const uint64_t amount[2], pg_budget *out);
/* Private reclaim preflight: observed free counters and remaining ownership
 * must show the same physical release in EACH dimension. Unlink/pin death
 * alone is a zero refund. Caller proves inode identity, maps and bitmap frees,
 * then journals records with those frees; failure preserves out. */
int pg_reclaim(const pg_budget *before, const uint64_t observed_free[2],
               const uint64_t owned_after[2], pg_budget *out);
#endif
