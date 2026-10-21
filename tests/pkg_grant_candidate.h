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
#endif
