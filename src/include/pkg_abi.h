#ifndef FORTRESS_PKG_ABI_H
#define FORTRESS_PKG_ABI_H
#include "types.h"
/* SYS_PKGCTL: exact v1 request/result sizes, values only. No keys, paths,
 * devices, source pointers or caller-supplied admission decisions. */
enum { PKG_STATUS=1,PKG_ACQUIRE,PKG_RECOVER,PKG_RELEASE,PKG_INSTALL,PKG_REMOVE };
enum { PKG_PHASE_IDLE,PKG_PHASE_RESERVED,PKG_PHASE_PENDING,PKG_PHASE_RETIRING };
typedef struct {
    uint32_t size,version,operation,reserved;
    uint64_t session;
    uint8_t package[32];
    uint64_t reserved64;
} pkg_request_v1;
typedef struct {
    uint32_t size,version,phase,reserved;
    uint64_t session,sequence,account_sequence,credential_generation;
    uint64_t remaining[2],charged[2];
} pkg_result_v1;
_Static_assert(sizeof(pkg_request_v1)==64,"pkg request v1 size");
_Static_assert(offsetof(pkg_request_v1,size)==0,"pkg size offset");
_Static_assert(offsetof(pkg_request_v1,version)==4,"pkg version offset");
_Static_assert(offsetof(pkg_request_v1,operation)==8,"pkg operation offset");
_Static_assert(offsetof(pkg_request_v1,reserved)==12,"pkg reserved32 offset");
_Static_assert(offsetof(pkg_request_v1,session)==16,"pkg session offset");
_Static_assert(offsetof(pkg_request_v1,package)==24,"pkg package offset");
_Static_assert(offsetof(pkg_request_v1,reserved64)==56,"pkg reserved offset");
_Static_assert(sizeof(pkg_result_v1)==80,"pkg result v1 size");
_Static_assert(offsetof(pkg_result_v1,phase)==8,"pkg result phase offset");
_Static_assert(offsetof(pkg_result_v1,reserved)==12,"pkg result reserved offset");
_Static_assert(offsetof(pkg_result_v1,session)==16,"pkg result session offset");
_Static_assert(offsetof(pkg_result_v1,sequence)==24,"pkg result sequence offset");
_Static_assert(offsetof(pkg_result_v1,account_sequence)==32,"pkg account offset");
_Static_assert(offsetof(pkg_result_v1,credential_generation)==40,"pkg credential offset");
_Static_assert(offsetof(pkg_result_v1,remaining)==48,"pkg remaining offset");
_Static_assert(offsetof(pkg_result_v1,charged)==64,"pkg charged offset");
#endif
