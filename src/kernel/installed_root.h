#ifndef FORTRESS_INSTALLED_ROOT_H
#define FORTRESS_INSTALLED_ROOT_H
#include "boot_info.h"
#include "root_boot.h"
/* BSP boot supervisor only, before any user process publication. */
bool installed_root_requested(const boot_info_t *);
bool installed_root_boot(const boot_info_t *);
const char *installed_root_recovery_hash(void);
void installed_root_recovery_notice(void);
void installed_root_login_failed(void);
bool installed_root_quiesce(void);
#ifdef TEST_ROOT_PHASE2
/* Disposable fw_cfg gates only: failures cannot grant write admission. */
void installed_root_test_faults(bool flush_failure,bool replay_write_failure);
#endif
#endif
