/* Actual driver's pinned inventory/context selection with PCI adapters.
 * No MMIO, DMA completion or hardware acceptance claim. */
#include <assert.h>
#include <stdio.h>
#include "../src/drivers/nvme.c"
static bool complete=true;
static unsigned scans,corrupt;
bool pci_inventory_complete(void) {return complete;}
size_t pci_find_all_devices(uint8_t c,uint8_t s,uint8_t p,pci_device_t *out,size_t n) {
    (void)c;(void)s;(void)p;(void)out;(void)n;scans++;return 0;
}
uint32_t pci_read_config32(uint16_t segment,uint8_t bus,uint8_t dev,uint8_t fn,uint16_t offset) {
    assert(!segment && !bus && !fn && (dev==2 || dev==3));
    if (offset==PCI_REG_VENDOR_ID) return corrupt==1 ? UINT32_MAX : 0x00101234;
    if (offset==8) return corrupt==2 ? 0 : 0x01080200;
    if (offset==PCI_REG_BAR0) return corrupt==3 ? 0xf0001000 : 0xf0000000;
    assert(!"unexpected configuration read");return 0;
}
int main(void) {
    inventory_pinned=true;inventory_count=2;
    for (unsigned i=0;i<2;i++) {
        inventory[i]=(pci_device_t){.device=i+2,.vendor_id=0x1234,.device_id=0x10,.bar={0xf0000000}};
        active_state=&states[i];g_initialized=true;g_io_sq_tail=3+i;
        g_dma_buf_phys=0x1000*(i+1);
    }
    assert(nvme_inventory_valid() && !scans);
    assert(nvme_inventory_select(1) && g_io_sq_tail==4 && g_dma_buf_phys==0x2000);
    g_io_sq_tail=9;
    assert(nvme_inventory_select(0) && g_io_sq_tail==3 && g_dma_buf_phys==0x1000);
    assert(nvme_inventory_select(1) && g_io_sq_tail==9);
    for (corrupt=1;corrupt<=3;corrupt++) assert(!nvme_inventory_valid());
    corrupt=0;complete=false;assert(!nvme_inventory_valid());complete=true;
    active_state=&states[1];g_dma_quarantined=true;assert(!nvme_inventory_select(1));
    assert(nvme_inventory_select(0));assert(!nvme_inventory_select(2));
    assert(!scans);
    puts("PASS actual NVMe pinned inventory: bounded fresh PCI checks without rescans, distinct controller state, replacement/BAR/class/completeness/quarantine rejection");
}
