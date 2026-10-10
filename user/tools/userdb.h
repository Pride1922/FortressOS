#ifndef FORTRESS_USERDB_H
#define FORTRESS_USERDB_H
#include "types.h"
#define DB_MAX_RECORDS 16
#define DB_FILE_MAX 8192
#define DB_LINE_MAX 512
#define DB_NAME_MAX 32
#define DB_PASSWORD_MAX 128
/* Bounded live-media policy: reject hashes requiring >100000 rounds rather
 * than clamp them to a different hash or permit unbounded login work. */
#define DB_ROUNDS_MAX 100000u
typedef struct { char name[32],home[128],shell[128];uint32_t uid,gid; } db_user_t;
typedef struct { char name[32],members[256];uint32_t gid; } db_group_t;
typedef struct { char name[32],hash[96];bool restricted; } db_shadow_t;
typedef struct {
    db_user_t users[16];db_group_t groups[16];db_shadow_t shadows[16];
    unsigned nusers,ngroups,nshadows;
} userdb_t;
/* Explicit byte lengths, no allocation, failure never publishes output.
 * Parser scratch is process-local static storage for the current single-user-
 * thread runtime (4 KiB user stack); parser calls must not be reentrant. */
bool db_passwd(userdb_t *,const char *,size_t);
bool db_group(userdb_t *,const char *,size_t);
bool db_shadow(userdb_t *,const char *,size_t);
bool db_validate(const userdb_t *);
const db_user_t *db_user_name(const userdb_t *,const char *);
const db_user_t *db_user_id(const userdb_t *,uint32_t);
const db_group_t *db_group_id(const userdb_t *,uint32_t);
const db_shadow_t *db_shadow_name(const userdb_t *,const char *);
bool db_groups(const userdb_t *,const db_user_t *,uint32_t out[16],unsigned *count);
bool db_hash_valid(const char *);
bool db_verify(const char *password,const char *hash);
void db_wipe(void *,size_t);
/* Syscall adapters are separate from the pure parser/crypt implementation. */
bool db_load(userdb_t *,bool shadow);
#endif
