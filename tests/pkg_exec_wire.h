#ifndef PKG_EXEC_WIRE_H
#define PKG_EXEC_WIRE_H
#include "pkg_backend_candidate.h"
/* Copied private kernel only: syscall 203. No target selectors or pointers. */
typedef struct {uint32_t size,version,operation,reserved;uint64_t session;char package[32];uint64_t reserved64;} mx_request;
_Static_assert(sizeof(mx_request)==64,"private mutation request");
#endif
