#ifndef FORTRESS_PKG_BACKEND_H
#define FORTRESS_PKG_BACKEND_H
#include "pkg_owner.h"
#include "../../user/pkg/transaction_cli.h"
/* Kernel owner transport used by SYS_PKGCTL. Input/output
 * are kernel-owned copies. The transport validates exact writable output and
 * input ranges before calling; copyout never repeats this call. No pointers in
 * wire values. Owner comes from the current TCB's trusted birth binding. */
enum { PB_STATUS=1, PB_ACQUIRE, PB_RECOVER, PB_RELEASE };
typedef struct {
    uint32_t size, version, operation, reserved;
    uint64_t session, reserved64;
} pb_request;
typedef struct {
    uint32_t size, version, phase, reserved;
    uint64_t session, sequence, account_sequence, credential_generation;
    uint64_t remaining[2], charged[2];
} pb_result;
_Static_assert(sizeof(pb_request)==32,"private package request");
_Static_assert(sizeof(pb_result)==80,"private package result");
/* Root admission is a kernel-supplied authentication decision, never a user
 * flag. Backend and context live until all operation refs drain. Default build
 * has no caller. False admission performs no storage callbacks. The embedding
 * supplies immutable mount operations on every call; sessions retain only the
 * kernel mount identity, never callback addresses or request pointers. */
int pb_control(const pb_request *,pw_key,bool,const pkg_transaction_backend *,pb_result *);
bool pkg_backend_snapshot_valid(const pkg_transaction_status *);
#endif
