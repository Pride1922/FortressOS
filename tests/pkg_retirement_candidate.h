#ifndef PKG_RETIREMENT_CANDIDATE_H
#define PKG_RETIREMENT_CANDIDATE_H
#include "pkg_grant_candidate.h"
/* Private pure candidate. No mount caller, filesystem lookup, journal writer,
 * signature verification or production format migration. Disjoint valid input
 * buffers are caller-owned; all failures preserve outputs. */
/* Independent trusted embedding expectations, never adopted from these
 * records. Pinning an opaque capability hash does not verify its authority. */
typedef struct {
    pg_binding grant;
    uint8_t capability[32],account_profile[32];
    uint8_t transaction[16],trust[32],state[32],intent[32];
    uint32_t inode,generation;
    uint16_t role;
} pr_binding;
typedef struct {
    const uint8_t *marker,*grant,*owners,*ledger,*account,*origin;
    size_t marker_n,grant_n,owners_n,ledger_n,account_n,origin_n;
} pr_set;
typedef struct {
    uint64_t sequence,epoch,account_sequence,credential_generation;
    uint64_t total[2],remaining[2],charged[2],owned[2],size;
    uint32_t inode,generation,flags;
    uint16_t role,classification;
} pr_view;
enum { PR_REPLAY,PR_UNLINK,PR_REFUND };
typedef struct { uint64_t refunded[2],sequence;unsigned operation; } pr_change;
int pr_decode(const uint8_t *,size_t,pr_view *);
int pr_encode(const uint8_t *,size_t,uint8_t *,size_t);
int pr_validate(const pr_set *,const pr_binding *,pr_view *);
/* Observations must come from the actual before/planned allocation audit, not
 * ledger guesses. Caller must compare the expected generation under exclusion
 * and commit all five mutable records with the actual frees in one journal
 * transaction. This pure check does not establish that atomic commit exists. */
int pr_transition(const pr_set *,const pr_set *,const pr_binding *,
                  const uint64_t free_before[2],const uint64_t free_after[2],
                  pr_change *);
#endif
