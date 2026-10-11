#ifndef FORTRESS_INSTALL_NVME_H
#define FORTRESS_INSTALL_NVME_H
#include "boot_info.h"
/* Disposable installer boot only. Returns false without registering authority
 * for unknown source provenance, active storage, or a failed inventory. */
bool install_nvme_boot(const boot_info_t *,bool disposable);
#endif
