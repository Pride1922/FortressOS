#ifndef FORTRESS_TLS_INTERNAL_H
#define FORTRESS_TLS_INTERNAL_H

#include "tls.h"
#include "syscall_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Internal syscall dispatcher for FortressOS TLS subsystem.
 * Respects mock hook (if configured via fortress_tls_set_syscall_hook).
 */
long tls_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c,
                 uintptr_t d, uintptr_t e, uintptr_t f);

#ifdef __cplusplus
}
#endif

#endif /* FORTRESS_TLS_INTERNAL_H */
