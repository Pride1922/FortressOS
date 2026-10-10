#ifndef FORTRESS_CREDS_H
#define FORTRESS_CREDS_H
#include "types.h"

/* Internal value API, not a syscall ABI or a publication/lifetime mechanism.
 * Inputs must be stable values; output must be exclusively owned. These
 * helpers take no locks and never access a process or user memory. */
#define CREDS_MAX_GROUPS 16
#define CAP_DAC_OVERRIDE    (1ULL << 0)
#define CAP_DAC_READ_SEARCH (1ULL << 1)
#define CAP_FOWNER          (1ULL << 2)
#define CAP_CHOWN           (1ULL << 3)
#define CAP_SETUID          (1ULL << 4)
#define CAP_SETGID          (1ULL << 5)
#define CAP_KILL            (1ULL << 6)
#define CAP_NET_BIND        (1ULL << 7)
#define CAP_SYS_ADMIN       (1ULL << 8)
#define CAP_SYS_RAWIO       (1ULL << 9)
#define CAP_SYS_BOOT        (1ULL << 10)
#define CAP_FSETID          (1ULL << 11)
#define CAP_ALL             ((1ULL << 12) - 1)

typedef struct creds {
    uint32_t uid, euid, suid;
    uint32_t gid, egid, sgid;
    uint32_t groups[CREDS_MAX_GROUPS];
    uint16_t ngroups, umask;
    uint32_t reserved;
    uint64_t cap_effective;
} creds_t;
_Static_assert(sizeof(creds_t) == 104, "credential value size");
_Static_assert(offsetof(creds_t, groups) == 24, "credential groups offset");
_Static_assert(offsetof(creds_t, ngroups) == 88, "credential count offset");
_Static_assert(offsetof(creds_t, reserved) == 92, "credential reserved offset");
_Static_assert(offsetof(creds_t, cap_effective) == 96, "credential caps offset");

enum creds_result { CREDS_OK = 0, CREDS_EPERM = 1, CREDS_EINVAL = 22 };
/* Explicit keep flags preserve the full uint32_t ID range. A future syscall
 * adapter must define its own unchanged-value encoding. */
typedef struct { uint32_t value; bool keep; } creds_id_change_t;
void creds_init_root(creds_t *out);
bool creds_valid(const creds_t *value);
/* All failures leave output unchanged. In-place old == out is supported.
 * Successful outputs have zero reserved/unused group storage. Ordinary
 * inheritance never replenishes capabilities, including for UID zero. */
int creds_inherit(const creds_t *old, creds_t *out);
int creds_setresuid(const creds_t *old, creds_id_change_t real,
                    creds_id_change_t effective, creds_id_change_t saved, creds_t *out);
int creds_setresgid(const creds_t *old, creds_id_change_t real,
                    creds_id_change_t effective, creds_id_change_t saved, creds_t *out);
int creds_setgroups(const creds_t *old, const uint32_t *groups, size_t count, creds_t *out);
int creds_capset(const creds_t *old, uint64_t mask, creds_t *out);
int creds_umask(const creds_t *old, uint32_t mask, creds_t *out);
#endif
