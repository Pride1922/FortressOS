#ifndef FORTRESS_PKG_SERVICE_H
#define FORTRESS_PKG_SERVICE_H
#include "pkg_backend.h"
struct tcb;
/* Boot-only publication after authenticated projected-root validation,
 * recovery/account preparation and actual root publication. No default caller.
 * The immutable backend and its mount context must outlive every operation. */
typedef struct {
    pkg_transaction_backend transactions;
    bool (*admitted)(void);
    int (*mutate)(void *,bool,const uint8_t [32],pkg_transaction_status *);
} pkg_kernel_backend;
bool pkg_service_bind(const pkg_kernel_backend *);
int64_t pkg_service_control(uintptr_t,uintptr_t,uint64_t);
void pkg_owner_exit(struct tcb *);
void pkg_owner_exec(struct tcb *);
void pkg_owner_reap(struct tcb *);
#endif
