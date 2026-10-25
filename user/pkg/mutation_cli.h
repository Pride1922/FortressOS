#ifndef FORTRESS_PKG_MUTATION_CLI_H
#define FORTRESS_PKG_MUTATION_CLI_H
#include "transaction_cli.h"
/* Private trusted embedding. A source registry resolves the copied package
 * name; callers cannot supply paths, devices, policy or keys. One callback owns
 * its durable result, even when subsequent output fails. Normal /bin/pkg binds
 * the versioned kernel transport; the kernel decides enrollment/admission. */
typedef struct {
    pkg_transaction_backend transactions;
    int (*mutate)(void *,bool remove,const char *package,size_t length);
} pkg_mutation_backend;
int pkg_mutation_cli(int argc,char **argv,const fpk_catalog_policy *,
                     const pkg_mutation_backend *);
#endif
