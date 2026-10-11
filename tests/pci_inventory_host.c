#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/drivers/pci.c"
static uint8_t table[sizeof(acpi_sdt_header_t)+8+17*sizeof(acpi_mcfg_allocation_t)];
static bool present;
acpi_sdt_header_t *acpi_find_table(const char *s) {assert(!strcmp(s,"MCFG"));return present ? (void *)table : NULL;}
void serial_puts(const char *s) {(void)s;}
void serial_print_dec(uint64_t n) {(void)n;}
void serial_print_hex(uint64_t n) {(void)n;}
static acpi_mcfg_allocation_t *setup(unsigned count) {
    memset(table,0,sizeof(table));present=true;
    ((acpi_sdt_header_t *)table)->length=sizeof(acpi_sdt_header_t)+8+count*sizeof(acpi_mcfg_allocation_t);
    acpi_mcfg_allocation_t *a=(void *)(table+sizeof(acpi_sdt_header_t)+8);
    for (unsigned i=0;i<count;i++) {a[i].base_address=0x100000000ULL+(uint64_t)i*0x10000000;a[i].pci_segment_group=i;a[i].end_bus_number=255;}
    return a;
}
int main(void) {
    present=false;pci_init(0);assert(pci_inventory_complete() && !pci_is_mcfg_available());
    setup(1);pci_init(0);assert(pci_inventory_complete());
    setup(17);pci_init(0);assert(!pci_inventory_complete() && pci_get_segment_count()==16);
    setup(1);((acpi_sdt_header_t *)table)->length++;pci_init(0);assert(!pci_inventory_complete());
    acpi_mcfg_allocation_t *a=setup(1);a->start_bus_number=2;a->end_bus_number=1;pci_init(0);assert(!pci_inventory_complete());
    a=setup(2);a[1].pci_segment_group=0;pci_init(0);assert(!pci_inventory_complete());
    a=setup(1);a->base_address=0;pci_init(0);assert(!pci_inventory_complete());
    setup(0);pci_init(0);assert(!pci_inventory_complete());
    puts("PASS actual PCI firmware inventory completeness: absent legacy, truncation, malformed ranges and overlap");
}
