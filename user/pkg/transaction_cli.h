#ifndef FORTRESS_PKG_TRANSACTION_CLI_H
#define FORTRESS_PKG_TRANSACTION_CLI_H
#include "cli.h"
/* Trusted embedding only, serialized calls. Not a syscall or descriptor ABI.
 * Backend/context/policy and argv remain valid for the complete call. Status
 * must be read-only. Recover revalidates authority and owns its durable result;
 * output failure must never cause a retry, rollback or a second recovery call.
 * No implementation of this backend is linked into the default /bin/pkg. */
enum { PKG_TX_IDLE, PKG_TX_RESERVED, PKG_TX_PENDING, PKG_TX_RETIRING };
typedef struct {
    unsigned phase;
    uint64_t sequence,account_sequence,credential_generation;
    uint64_t remaining[2],charged[2];
} pkg_transaction_status;
typedef struct {
    void *context;
    int (*status)(void *,pkg_transaction_status *);
    int (*recover)(void *,pkg_transaction_status *);
} pkg_transaction_backend;
int pkg_transaction_cli(int argc,char **argv,const fpk_catalog_policy *,
                        const pkg_transaction_backend *);
#endif
