#ifndef FORTRESS_INSTALL_NVME_H
#define FORTRESS_INSTALL_NVME_H
#include "boot_info.h"
/* Disposable installer boot only. Returns false without registering authority
 * for unknown source provenance, active storage, or a failed inventory. */
bool install_nvme_boot(const boot_info_t *,bool disposable);
#ifdef INSTALLER_PHYSICAL
/* Separate drive-bound image; no generic physical-write authorization. */
bool install_nvme_physical_boot(const boot_info_t *);
#endif
#ifdef TEST_INSTALLER_INSPECT
/* Identity-only hardware inspection. No read/write/flush/deploy callbacks. */
bool install_nvme_inspect_boot(const boot_info_t *);
#endif
#endif
