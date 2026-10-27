#ifndef FORTRESS_STORAGE_H
#define FORTRESS_STORAGE_H
#include "block.h"
#include "install_abi.h"

/* BSP-only boot/installer selection; never switch a published filesystem's
 * backing device. The selected inventory index is pinned by installer leases.
 * NVMe entries precede SATA entries, with at most eight combined targets. */
size_t storage_inventory_boot(void);
bool storage_inventory_valid(void);
bool storage_inventory_select(unsigned index);
bool storage_installer_identity(install_identity_v1_t *out);
bool storage_is_fatal(void);
bool storage_is_dma_quarantined(void);
bool storage_init(void);
bool storage_has_unique_namespace(void);
bool storage_root_geometry_supported(void);
uint32_t storage_get_sector_size(void);
uint64_t storage_get_sector_count(void);
bool storage_read_sector(uint64_t lba,void *bytes);
bool storage_read_sectors(uint64_t lba,uint32_t count,void *bytes);
bool storage_write_sectors(uint64_t lba,uint32_t count,const void *bytes);
bool storage_flush(void);
bool storage_register_selected(void);
block_dev_t *storage_selected_disk(void);
bool storage_quiesce(void);
/* Ordinary live sessions expose SATA inventory read-only. Installer/installed
 * profiles register only their admitted target, after existing identity gates. */
void storage_probe_live_sata(void);
#endif
