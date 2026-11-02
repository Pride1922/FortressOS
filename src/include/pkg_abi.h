#ifndef FORTRESS_PKG_ABI_H
#define FORTRESS_PKG_ABI_H
#include "types.h"
/* SYS_PKGCTL: exact v1 request/result sizes, values only. No keys, paths,
 * devices, source pointers or caller-supplied admission decisions. */
enum { PKG_STATUS=1,PKG_ACQUIRE,PKG_RECOVER,PKG_RELEASE,PKG_INSTALL,PKG_REMOVE };
enum { PKG_INSTALL_SOURCE=7, PKG_UPGRADE_SOURCE=8 };
enum { PKG_BASE_STATUS=9,PKG_BASE_STAGE=10,PKG_BASE_CONFIRM=11,PKG_BASE_RECOVER=12 };
#define PKG_BASE_REBOOT 1u
#define PKG_BASE_UNCONFIRMED 2u
#define PKG_BASE_RECOVERY 4u
#define PKG_BASE_FENCED 8u
#define PKG_BASE_RETRY 16u
typedef struct {
    uint32_t size,version,flags,reserved;
    uint64_t running_build,selected_build,floor_build,abi,catalog;
    uint8_t artifact[32],platform[32];
    uint64_t reserved64;
} pkg_base_result_v1;
_Static_assert(sizeof(pkg_base_result_v1)==128,"base status ABI v1");
/* Additive v2 ingestion. v1 remains exactly 64 bytes. Descriptors identify only
 * source regular files to snapshot, never a policy, plan, device or verified flag.
 * Limits match the existing catalog/FPK parsers. Result remains v1. */
#define PKG_SOURCE_CATALOG_MAX (256u*1024u)
#define PKG_SOURCE_ARTIFACT_MAX (128u+16384u+8u*1024u*1024u)
enum { PKG_PHASE_IDLE,PKG_PHASE_RESERVED,PKG_PHASE_PENDING,PKG_PHASE_RETIRING };
typedef struct {
    uint32_t size,version,operation,reserved;
    uint64_t session;
    uint8_t package[32];
    uint64_t reserved64;
} pkg_request_v1;
typedef struct {
    pkg_request_v1 request;
    uint64_t catalog_fd,catalog_bytes,artifact_fd,artifact_bytes;
} pkg_request_v2;
/* v3 upgrade carries the previous artifact as an untrusted retained source.
 * Its SHA-256 must match the installed inventory; it needs no live old catalog. */
typedef struct {
    pkg_request_v2 source;
    uint64_t previous_fd,previous_bytes;
} pkg_request_v3;
_Static_assert(sizeof(pkg_request_v3)==112,"pkg upgrade request size");
_Static_assert(offsetof(pkg_request_v3,previous_fd)==96,"pkg previous descriptor");
_Static_assert(sizeof(pkg_request_v2)==96,"pkg request v2 size");
_Static_assert(offsetof(pkg_request_v2,catalog_fd)==64,"pkg catalog descriptor");
_Static_assert(offsetof(pkg_request_v2,catalog_bytes)==72,"pkg catalog bytes");
_Static_assert(offsetof(pkg_request_v2,artifact_fd)==80,"pkg artifact descriptor");
_Static_assert(offsetof(pkg_request_v2,artifact_bytes)==88,"pkg artifact bytes");
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
