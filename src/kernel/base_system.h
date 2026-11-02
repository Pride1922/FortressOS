#ifndef FORTRESS_BASE_SYSTEM_H
#define FORTRESS_BASE_SYSTEM_H
#include "boot_info.h"
#include "pkg_abi.h"
/* Boot-only before root replay. Disk selection must already be pinned. */
bool base_system_boot(const boot_info_t *);
void base_system_published(void);
bool base_system_version(uint64_t *,uint8_t [32]);
int base_system_catalog_check(uint64_t,const uint8_t [32],uint64_t,const uint8_t [32]);
/* Called only while pkg_service holds source credit and a retained writer op. */
int base_system_operation(unsigned,const uint8_t *,size_t,const uint8_t *,size_t,pkg_base_result_v1 *);
#endif
