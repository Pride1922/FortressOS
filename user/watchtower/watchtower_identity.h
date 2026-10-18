#ifndef WATCHTOWER_IDENTITY_H
#define WATCHTOWER_IDENTITY_H
#include "watchtower.h"
#include "../tools/userdb.h"
#include "syscall_abi.h"
/* Name or decimal ID must resolve to an existing account/group. Output stays
 * unchanged on error; no shadow/password database is needed. */
bool watchtower_resolve_identity(const userdb_t *db, const char *user,
                                 const char *group, spawn_identity_t *out);
bool watchtower_prepare_identity(const char *user,const char *group,spawn_identity_t *out);
#endif
