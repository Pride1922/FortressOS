#ifndef FORTRESS_ROOT_BOOT_H
#define FORTRESS_ROOT_BOOT_H
#include "types.h"
#include "gpt.h"

typedef enum { ROOT_BOOT_LIVE, ROOT_BOOT_INSTALLED, ROOT_BOOT_RECOVERY } root_boot_mode_t;
typedef struct {
    root_boot_mode_t mode;
    gpt_guid_t partuuid;
    bool has_root, valid, explicit_mode;
} root_boot_config_t;
/* Bounded exact-token parsing. Invalid input never authorizes device I/O.
 * Any root-related token opts out of legacy live diagnostics even on failure. */
root_boot_config_t root_boot_parse(const char *,size_t,bool truncated);
typedef struct { char install_id[33],partuuid[37],filesystem_uuid[37],hash[96]; } root_recovery_t;
bool root_recovery_parse(const void *,size_t,root_recovery_t *);
bool root_readiness_validate(const void *,size_t,const root_recovery_t *,
                             const void *kernel,size_t,const void *bootstrap,size_t);
bool root_manifest_validate(const void *,size_t,const root_recovery_t *);
#endif
