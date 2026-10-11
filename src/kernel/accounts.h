#ifndef FORTRESS_ACCOUNTS_H
#define FORTRESS_ACCOUNTS_H
#include "ext4.h"
#include "account_abi.h"
/* Boot-only, detached root; replay and complete validation precede login. */
int accounts_prepare(ext4_mount_t *);
/* Trusted installer-only detached namespace. Homes must already be validated.
 * Uses the actual durable redo engine without enabling installed authentication
 * or publishing account-service namespace state. Hashes only; no plaintext. */
int accounts_provision_detached(ext4_mount_t *,const char *const [3],const size_t [3]);
void accounts_disable(void);
void accounts_abandon(void);
void accounts_process_exit(uint64_t);
int64_t accounts_control(unsigned,uintptr_t,size_t,const creds_t *,uint64_t);
int64_t accounts_seed(uintptr_t,size_t,const creds_t *);
#endif
