#ifndef FORTRESS_ACCOUNT_REDO_H
#define FORTRESS_ACCOUNT_REDO_H
#include "types.h"
#define ACCOUNT_FILE_MAX 8192u
#define ACCOUNT_INTENT_SIZE 160u
/* Fixed roles, never paths supplied by an intent. Adapter returns negative VFS
 * errors; remove of an absent role must succeed. Writes create complete files
 * with root ownership and mode 0600 (all staging) or canonical 0644/0644/0600.
 * The caller exclusively owns the database for the entire call. */
enum account_role {
    ACCOUNT_PASSWD, ACCOUNT_GROUP, ACCOUNT_SHADOW,
    ACCOUNT_STAGE_PASSWD, ACCOUNT_STAGE_GROUP, ACCOUNT_STAGE_SHADOW,
    ACCOUNT_TEMP_PASSWD, ACCOUNT_TEMP_GROUP, ACCOUNT_TEMP_SHADOW,
    ACCOUNT_INTENT_TEMP, ACCOUNT_PENDING, ACCOUNT_ROLE_COUNT
};
typedef struct {
    void *context;
    int (*read)(void *,unsigned,void *,size_t,size_t *);
    int (*write)(void *,unsigned,const void *,size_t);
    int (*remove)(void *,unsigned);
    int (*rename)(void *,unsigned,unsigned);
    int (*sync)(void *);
    /* Validate full database coherence, locked root and safe persistent homes.
     * No authentication or user process may run during this callback. */
    int (*validate)(void *,const char *const [3],const size_t [3]);
} account_io_t;
typedef struct { char files[3][ACCOUNT_FILE_MAX];size_t lengths[3];uint8_t intent[ACCOUNT_INTENT_SIZE]; } account_workspace_t;
/* Workspace is caller-owned heap storage, never a kernel/user stack frame.
 * Recovery with no intent validates canonical bytes before cleaning staging.
 * Every failure blocks authentication until recovery/validation succeeds. */
int account_redo_recover(const account_io_t *,account_workspace_t *);
int account_redo_update(const account_io_t *,account_workspace_t *,uint64_t generation);
void account_redo_wipe(account_workspace_t *);
#endif
