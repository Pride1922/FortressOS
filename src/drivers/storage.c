#include "storage.h"
#include "nvme.h"
#include "ahci.h"
#include "pci.h"
#include "serial.h"

static bool inventory_started, inventory_good;
static unsigned nvme_count,sata_count,selected;
static block_dev_t live_sata[AHCI_MAX_DISKS];
static bool sata(void) {return inventory_started && selected>=nvme_count;}
static block_dev_t *selected_sata(void) {return sata() ? ahci_get_disk(selected-nvme_count) : NULL;}

size_t storage_inventory_boot(void) {
    if (inventory_started) return storage_inventory_valid() ? nvme_count+sata_count : 0;
    inventory_started=true;
    pci_device_t controllers[9];
    size_t n=pci_find_all_devices(PCI_CLASS_STORAGE,PCI_SUBCLASS_STORAGE_NVME,PCI_PROGIF_STORAGE_NVME,controllers,9);
    if (!pci_inventory_complete() || n>8) {
        serial_puts("[STORAGE] Discovery rejected: incomplete PCI inventory or too many NVMe controllers\n");return 0;
    }
    if (n) {
        nvme_count=(unsigned)nvme_inventory_boot();
        if (nvme_count!=n) {
            serial_puts("[STORAGE] Discovery rejected: NVMe inventory failed\n");return 0;
        }
    }
    sata_count=(unsigned)ahci_inventory_boot();
    if (!ahci_inventory_valid()) {
        serial_puts("[STORAGE] Discovery rejected: AHCI inventory failed; combined disk list withheld\n");return 0;
    }
    if (nvme_count+sata_count>8) {
        serial_puts("[STORAGE] Discovery rejected: more than eight combined disks\n");return 0;
    }
    inventory_good=true;
    return storage_inventory_valid() ? nvme_count+sata_count : 0;
}
bool storage_inventory_valid(void) {
    return inventory_started && inventory_good && pci_inventory_complete() &&
        (!nvme_count || nvme_inventory_valid()) && ahci_inventory_valid();
}
bool storage_inventory_select(unsigned index) {
    if (!storage_inventory_valid() || index>=nvme_count+sata_count) return false;
    if (index<nvme_count && !nvme_inventory_select(index)) return false;
    if (index>=nvme_count && !ahci_get_disk(index-nvme_count)) return false;
    selected=index;return true;
}
bool storage_installer_identity(install_identity_v1_t *out) {
    if (inventory_started && !storage_inventory_valid()) return false;
    return sata() ? ahci_identity(selected-nvme_count,out) : nvme_installer_identity(out);
}
bool storage_is_fatal(void) {return sata() ? !selected_sata() : nvme_is_fatal();}
bool storage_is_dma_quarantined(void) {return sata() ? !selected_sata() : nvme_is_dma_quarantined();}
bool storage_init(void) {return sata() ? selected_sata()!=NULL : nvme_init();}
bool storage_has_unique_namespace(void) {return sata() ? selected_sata()!=NULL : nvme_has_unique_namespace();}
bool storage_root_geometry_supported(void) {
    block_dev_t *d=selected_sata();
    return sata() ? d && d->sector_size==512 && d->flush : nvme_root_geometry_supported();
}
uint32_t storage_get_sector_size(void) {
    block_dev_t *d=selected_sata();return sata() ? d ? d->sector_size : 0 : nvme_get_sector_size();
}
uint64_t storage_get_sector_count(void) {
    block_dev_t *d=selected_sata();return sata() ? d ? d->sector_count : 0 : nvme_get_sector_count();
}
bool storage_read_sectors(uint64_t lba,uint32_t count,void *bytes) {
    if (!sata()) return nvme_read_sectors(lba,count,bytes);
    return block_read_sectors(selected_sata(),lba,count,bytes);
}
bool storage_read_sector(uint64_t lba,void *bytes) {return storage_read_sectors(lba,1,bytes);}
bool storage_write_sectors(uint64_t lba,uint32_t count,const void *bytes) {
    if (!sata()) return nvme_write_sectors(lba,count,bytes);
    return block_write_sectors(selected_sata(),lba,count,bytes);
}
bool storage_flush(void) {return sata() ? block_flush(selected_sata()) : nvme_flush();}
bool storage_register_selected(void) {
    if (!sata()) return block_register_nvme();
    block_dev_t *d=selected_sata();return d && block_register_dev(d);
}
block_dev_t *storage_selected_disk(void) {return sata() ? selected_sata() : block_get_dev_by_name("nvme0n1");}
bool storage_quiesce(void) {return sata() ? ahci_quiesce() : nvme_quiesce();}
void storage_probe_live_sata(void) {
    size_t n=ahci_inventory_boot();
    if (!ahci_inventory_valid()) {serial_puts("[AHCI] SATA inventory unavailable; no SATA disks published\n");return;}
    for (unsigned i=0;i<n;i++) {
        block_dev_t *d=ahci_get_disk(i);
        if (!d) return;
        live_sata[i]=*d;
        live_sata[i].write_sector=NULL;live_sata[i].write_sectors=NULL;live_sata[i].flush=NULL;
        if (!block_register_dev(&live_sata[i])) {serial_puts("[AHCI] SATA registry capacity exhausted\n");return;}
    }
}
