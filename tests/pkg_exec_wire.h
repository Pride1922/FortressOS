#ifndef PKG_EXEC_WIRE_H
#define PKG_EXEC_WIRE_H
#include "pkg_backend_candidate.h"
/* Copied private kernel only: syscall 203. No target selectors or pointers. */
typedef struct {uint32_t size,version,operation,reserved;uint64_t session;char package[32];uint64_t reserved64;} mx_request;
_Static_assert(sizeof(mx_request)==64,"private mutation request");
_Static_assert(offsetof(mx_request,session)==16,"private mutation session offset");
_Static_assert(offsetof(mx_request,package)==24,"private mutation package offset");
_Static_assert(offsetof(mx_request,reserved64)==56,"private mutation reserved offset");
#endif
