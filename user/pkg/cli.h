#ifndef FORTRESS_PKG_CLI_H
#define FORTRESS_PKG_CLI_H
#include "catalog.h"
/* Serialized process-local CLI. Optional immutable policy is supplied only by
 * trusted embedding, never by a package or command-line key option. Kernel UTC
 * replaces policy.now. Default entry has no provisioned policy, so verify
 * rejects. No install/remove/update commands or on-disk state writes. */
int pkg_cli(int argc,char **argv,const fpk_catalog_policy *policy);
int pkg_main(int argc,char **argv);
#endif
