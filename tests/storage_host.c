/* Actual mixed-transport dispatcher; labelled NVMe/AHCI adapters below. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/drivers/storage.c"
static unsigned hardware_nvme=1,hardware_sata=2,nvme_selected;
static bool pci_complete=true,nvme_valid=true,sata_valid=true;
static block_dev_t sata_disks[8],*registered[16],nvme_disk;
static unsigned registrations,nvme_writes,sata_writes,last_port;
static bool read_sata(block_dev_t *d,uint64_t l,void *b) {
    assert(l<d->sector_count);last_port=(unsigned)(uintptr_t)d->priv;memset(b,0x22+last_port,512);return true;
}
static bool write_sata(block_dev_t *d,uint64_t l,const void *b) {(void)b;assert(l<d->sector_count);last_port=(unsigned)(uintptr_t)d->priv;sata_writes++;return true;}
static bool flush_sata(block_dev_t *d) {(void)d;return true;}
size_t pci_find_all_devices(uint8_t c,uint8_t s,uint8_t p,pci_device_t *out,size_t n) {
    (void)out;assert(c==1 && s==8 && p==2 && n==9);return hardware_nvme;
}
bool pci_inventory_complete(void) {return pci_complete;}
size_t nvme_inventory_boot(void) {return hardware_nvme;}
bool nvme_inventory_valid(void) {return nvme_valid;}
bool nvme_inventory_select(unsigned i) {nvme_selected=i;return nvme_valid && i<hardware_nvme;}
bool nvme_installer_identity(install_identity_v1_t *id) {memset(id,0,sizeof(*id));id->nsid=nvme_selected+1;id->bus=1;return nvme_valid;}
bool nvme_is_fatal(void) {return !nvme_valid;}
bool nvme_is_dma_quarantined(void) {return !nvme_valid;}
bool nvme_init(void) {return nvme_valid;}
bool nvme_has_unique_namespace(void) {return true;}
bool nvme_root_geometry_supported(void) {return true;}
uint32_t nvme_get_sector_size(void) {return 512;}
uint64_t nvme_get_sector_count(void) {return 32;}
bool nvme_read_sectors(uint64_t l,uint32_t n,void *b) {assert(l+n<=32);memset(b,0x11,n*512);return true;}
bool nvme_write_sectors(uint64_t l,uint32_t n,const void *b) {(void)b;assert(l+n<=32);nvme_writes++;return true;}
bool nvme_flush(void) {return true;}
bool nvme_quiesce(void) {nvme_valid=false;return true;}
size_t ahci_inventory_boot(void) {return hardware_sata;}
bool ahci_inventory_valid(void) {return sata_valid;}
block_dev_t *ahci_get_disk(unsigned i) {return sata_valid && i<hardware_sata ? &sata_disks[i] : NULL;}
bool ahci_identity(unsigned i,install_identity_v1_t *id) {memset(id,0,sizeof(*id));id->bus=2;id->nsid=i+1;return sata_valid;}
bool ahci_quiesce(void) {sata_valid=false;return true;}
bool block_register_dev(block_dev_t *d) {assert(registrations<16);registered[registrations++]=d;return true;}
bool block_register_nvme(void) {return block_register_dev(&nvme_disk);}
block_dev_t *block_get_dev_by_name(const char *name) {assert(!strcmp(name,"nvme0n1"));return &nvme_disk;}
bool block_flush(block_dev_t *d) {return d && d->flush && d->flush(d);}
bool block_read_sector(block_dev_t *d,uint64_t l,void *b) {return d && l<d->sector_count && d->read_sector(d,l,b);}
bool block_write_sector(block_dev_t *d,uint64_t l,const void *b) {return d && l<d->sector_count && d->write_sector && d->write_sector(d,l,b);}
void serial_puts(const char *s) {(void)s;}
static void reset(void) {
    inventory_started=inventory_good=false;nvme_count=sata_count=selected=0;
    hardware_nvme=1;hardware_sata=2;nvme_selected=0;pci_complete=nvme_valid=sata_valid=true;
    registrations=nvme_writes=sata_writes=0;
    for (unsigned i=0;i<8;i++) sata_disks[i]=(block_dev_t){.sector_size=512,.sector_count=32,
        .priv=(void *)(uintptr_t)i,.read_sector=read_sata,.write_sector=write_sata,.flush=flush_sata};
}
int main(void) {
    reset();assert(storage_inventory_boot()==3 && storage_inventory_valid());
    uint8_t data[512];install_identity_v1_t id;
    assert(storage_inventory_select(0) && storage_installer_identity(&id) && id.bus==1);
    assert(storage_read_sector(0,data) && data[0]==0x11 && storage_write_sectors(0,1,data));
    assert(nvme_writes==1 && sata_writes==0);
    assert(storage_inventory_select(2) && storage_installer_identity(&id) && id.bus==2 && id.nsid==2);
    assert(storage_read_sector(0,data) && data[0]==0x23 && last_port==1);
    assert(storage_write_sectors(31,1,data) && sata_writes==1 && nvme_writes==1);
    assert(!storage_write_sectors(32,1,data) && sata_writes==1 && storage_flush());
    assert(storage_register_selected() && registered[0]==&sata_disks[1]);
    assert(storage_selected_disk()==&sata_disks[1] && !storage_inventory_select(3));
    assert(storage_get_sector_size()==512 && storage_get_sector_count()==32 && storage_root_geometry_supported());
    sata_valid=false;assert(!storage_inventory_valid() && !storage_installer_identity(&id) && storage_is_fatal());
    reset();storage_probe_live_sata();assert(registrations==2 && !registered[0]->write_sector && !registered[1]->flush);
    assert(sata_disks[0].write_sector && sata_disks[1].flush); /* underlying private driver unchanged */
    reset();hardware_nvme=0;assert(storage_inventory_boot()==2 && storage_inventory_select(0) && storage_selected_disk()==&sata_disks[0]);
    assert(storage_quiesce() && !sata_valid && nvme_valid);
    reset();hardware_sata=0;assert(storage_inventory_boot()==1 && storage_inventory_select(0));
    assert(storage_register_selected() && registered[0]==&nvme_disk && storage_quiesce() && !nvme_valid && sata_valid);
    reset();hardware_nvme=8;assert(!storage_inventory_boot() && !storage_inventory_valid());
    reset();pci_complete=false;assert(!storage_inventory_boot());
    reset();sata_valid=false;assert(!storage_inventory_boot());
    reset(); /* Fixed-drive legacy profiles preserve their NVMe-only admission. */
    assert(storage_installer_identity(&id) && id.bus==1 && storage_read_sector(0,data));
    puts("PASS actual storage dispatcher: SATA-only, NVMe-only, mixed selection, read-only live views, non-target isolation, bounds and failed inventory");
    return 0;
}
