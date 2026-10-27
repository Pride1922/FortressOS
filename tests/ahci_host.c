/* Actual ahci.c, with labelled PCI/MMIO/DMA/clock adapters, not real hardware. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define TEST_AHCI_HOST
#define TEST_SMP_MEMORY
#include "../src/drivers/ahci.c"

static uint32_t registers[0x4000/4];
static uint8_t ram[32][4096], media[32][512];
static uint16_t identify_words[256];
static unsigned allocated,freed,issued,writes,flushes,lock_depth;
static unsigned allocation_limit=32;
static bool high_dma;
static uint16_t pci_command;
static uint64_t clock_value;
static uint64_t clock_hz=1000000,clock_step=1000;
static unsigned delayed_reads;
static unsigned wake_reads;
static bool resetting;
static uint32_t reset_signature;
static unsigned resets;
static bool timeout_command,error_command,partial,stuck_stop,bios_stuck,replaced;
static unsigned controllers=1;
static bool complete=true;
static unsigned offset(uintptr_t address) {assert(address>=MMIO_BASE && address<MMIO_BASE+0x4000);return (unsigned)(address-MMIO_BASE);}
void *vmm_phys_to_virt(uintptr_t address) {
    assert(address>=0x100000 && address<0x100000+sizeof(ram));
    return (uint8_t *)ram+address-0x100000;
}
uintptr_t pmm_alloc_page(void) {
    if (allocated==allocation_limit) return 0;
    assert(allocated<32);return (high_dma ? 0x100000000ULL : 0x100000)+(allocated++)*4096;
}
void pmm_free_page(uintptr_t p) {(void)p;freed++;}
uint64_t apic_poll_clock_hz(void) {return clock_hz;}
uint64_t ahci_test_clock(void) {clock_value+=clock_step;return clock_value;}
uint64_t spin_lock_irqsave(spinlock_t *lock) {(void)lock;assert(!lock_depth++);return 0;}
void spin_unlock_irqrestore(spinlock_t *lock,uint64_t flags) {(void)lock;(void)flags;assert(lock_depth--==1);}
void serial_puts(const char *s) {(void)s;}
void serial_print_dec(uint64_t n) {(void)n;}
void serial_print_hex(uint64_t n) {(void)n;}
uint64_t *vmm_get_kernel_pml4_virt(void) {return NULL;}
int vmm_map_page(uint64_t *root,uintptr_t va,uintptr_t pa,uint64_t flags) {
    (void)root;assert(va>=MMIO_BASE && va<MMIO_BASE+0x4000);
    assert(pa>=0xfebf1000 && pa<0xfebf4000);
    assert((flags&(PTE_DEVICE|PTE_NX|PTE_PCD|PTE_PWT))==(PTE_DEVICE|PTE_NX|PTE_PCD|PTE_PWT));return VMM_OK;
}
size_t pci_find_all_devices(uint8_t c,uint8_t s,uint8_t p,pci_device_t *out,size_t cap) {
    assert(c==1 && s==6 && p==1 && cap==5);
    if (controllers==1) *out=(pci_device_t){.vendor_id=0x8086,.device_id=0x2922,.bar={0,0,0,0,0,0xfebf1000}};
    return controllers;
}
bool pci_inventory_complete(void) {return complete;}
uint32_t pci_read_config32(uint16_t s,uint8_t b,uint8_t d,uint8_t f,uint16_t reg) {
    (void)s;(void)b;(void)d;(void)f;
    if (reg==0) return replaced ? UINT32_MAX : 0x29228086;
    if (reg==8) return 0x01060100;
    assert(reg==PCI_REG_BAR5);return 0xfebf1000;
}
uint16_t pci_read_config16(uint16_t s,uint8_t b,uint8_t d,uint8_t f,uint16_t reg) {
    (void)s;(void)b;(void)d;(void)f;assert(reg==PCI_REG_COMMAND);return pci_command;
}
void pci_write_config16(uint16_t s,uint8_t b,uint8_t d,uint8_t f,uint16_t reg,uint16_t value) {
    (void)s;(void)b;(void)d;(void)f;assert(reg==PCI_REG_COMMAND);pci_command=value;
}
uint32_t ahci_test_read(uintptr_t address) {
    unsigned at=offset(address);
    if (at==0x100+SSTS && wake_reads && !--wake_reads) registers[at/4]=0x103;
    if (at==0x100+CMD && delayed_reads) {return --delayed_reads ? CMD_CR : 0;}
    if (at==0x100+CMD && stuck_stop) return registers[at/4]|CMD_CR;
    return registers[at/4];
}
void ahci_test_write(uintptr_t address,uint32_t value) {
    unsigned at=offset(address);
    if (at==BOHC) {registers[at/4]=bios_stuck ? value|0x11 : value&~0x11u;return;}
    if (at>=0x100) {
        unsigned reg=(at-0x100)%0x80;
        if (reg==SCTL) {
            unsigned base=at-SCTL;
            if ((value&15)==1) {
                assert(!(registers[(base+CMD)/4]&(CMD_ST|CMD_CR)) && (registers[(base+CMD)/4]&CMD_FRE));
                assert(registers[(base+FB)/4]);
                resetting=true;
            } else if (resetting && !(value&15)) {
                resetting=false;resets++;
                registers[(base+SIG)/4]=reset_signature;registers[(base+TFD)/4]=0x50;
            }
        }
        if (reg==IS || reg==SERR) {registers[at/4]&=~value;return;}
        if (reg==CMD) {
            if (!(value&CMD_ST)) value&=~CMD_CR;
            if (!(value&CMD_FRE)) value&=~CMD_FR;
        }
        if (reg==CI && value==1) {
            issued++;
            registers[at/4]=1;
            if (timeout_command) return;
            unsigned base=at-CI;
            uint64_t clb=registers[(base+CLB)/4]|(uint64_t)registers[(base+CLBU)/4]<<32;
            uint32_t *header=vmm_phys_to_virt(clb);
            assert((header[0]&0x1f)==5);
            uint64_t ct=header[2]|(uint64_t)header[3]<<32;
            uint8_t *fis=vmm_phys_to_virt(ct);
            assert(fis[0]==0x27 && fis[1]==0x80 && fis[7]==0x40);
            uint32_t *prdt=(void *)(fis+128);
            uint64_t data=prdt[0]|(uint64_t)prdt[1]<<32;
            size_t bytes=(header[0]>>16) ? (prdt[3]&0x3fffff)+1 : 0;
            uint64_t lba=(uint64_t)fis[4]|(uint64_t)fis[5]<<8|(uint64_t)fis[6]<<16|
                (uint64_t)fis[8]<<24|(uint64_t)fis[9]<<32|(uint64_t)fis[10]<<40;
            if (fis[2]==0xec) {assert(bytes==512);memcpy(vmm_phys_to_virt(data),identify_words,512);}
            else if (fis[2]==0xea) {assert(!bytes && !(header[0]&(1u<<6)));flushes++;}
            else {
                assert(lba<32 && bytes && bytes<=4096 && bytes==(size_t)(fis[12]|fis[13]<<8)*512);
                assert(lba+bytes/512<=32);
                if (fis[2]==0x25) {assert(!(header[0]&(1u<<6)));memcpy(vmm_phys_to_virt(data),media[lba],bytes);}
                else {assert(fis[2]==0x35 && (header[0]&(1u<<6)));memcpy(media[lba],vmm_phys_to_virt(data),bytes);writes++;}
            }
            header[1]=partial ? (uint32_t)bytes/2 : (uint32_t)bytes;
            registers[(base+TFD)/4]=error_command ? 0x51 : 0x50;
            registers[at/4]=0;
            return;
        }
    }
    registers[at/4]=value;
}
static void reset(void) {
    memset(hbas,0,sizeof(hbas));memset(ports,0,sizeof(ports));memset(disks,0,sizeof(disks));
    hba_count=port_count=disk_count=0;scanned=coherent=false;
    memset(registers,0,sizeof(registers));memset(ram,0,sizeof(ram));memset(identify_words,0,sizeof(identify_words));
    allocated=freed=issued=writes=flushes=lock_depth=0;pci_command=0;clock_value=0;
    allocation_limit=32;high_dma=false;
    clock_hz=1000000;clock_step=1000;delayed_reads=wake_reads=0;
    resetting=false;resets=0;reset_signature=0x101;
    timeout_command=error_command=partial=stuck_stop=bios_stuck=replaced=false;controllers=1;complete=true;
    registers[CAP/4]=(1u<<31)|5;registers[PI/4]=3;
    registers[(0x100+CMD)/4]=CMD_ST|CMD_FRE|2;
    registers[(0x100+SSTS)/4]=0x103;registers[(0x100+SIG)/4]=0x101;
    registers[(0x180+CMD)/4]=2;registers[(0x180+SSTS)/4]=0x103;registers[(0x180+SIG)/4]=0xeb140101;
    identify_words[49]=0x300;identify_words[83]=0x6400;identify_words[100]=32;
    identify_words[10]=0x5331;identify_words[27]=0x5341;
}
int main(void) {
    reset();ahci_geometry_t g;
    assert(ahci_decode_identify(identify_words,&g) && g.sectors==32 && g.sector_size==512 && g.flush_ext);
    identify_words[106]=0x5000;identify_words[117]=2048;
    assert(ahci_decode_identify(identify_words,&g) && g.sector_size==4096);
    identify_words[117]=4096;assert(!ahci_decode_identify(identify_words,&g));
    reset();identify_words[103]=1;assert(!ahci_decode_identify(identify_words,&g));
    reset();identify_words[83]=0x4000;assert(!ahci_decode_identify(identify_words,&g));
    reset();identify_words[128]=4;assert(!ahci_decode_identify(identify_words,&g));
    reset();assert(ahci_inventory_boot()==1 && ahci_inventory_valid() && allocated==2 && !freed && issued==1);
    block_dev_t *d=ahci_get_disk(0);assert(d && !strcmp(d->name,"sata0") && !ahci_get_disk(1));
    uint8_t bytes[4096];memset(media[0],0xa5,512);memset(media[31],0x5a,512);
    assert(d->read_sector(d,0,bytes) && bytes[0]==0xa5);
    assert(d->read_sector(d,31,bytes) && bytes[511]==0x5a);
    unsigned before=issued;
    assert(!d->read_sector(d,32,bytes) && !d->read_sectors(d,31,2,bytes) && !d->read_sectors(d,0,0,bytes));
    assert(!d->read_sectors(d,UINT64_MAX,2,bytes) && !d->read_sectors(d,0,9,bytes) && issued==before);
    memset(bytes,0x77,sizeof(bytes));assert(d->write_sectors(d,4,8,bytes) && writes==1 && media[11][511]==0x77);
    assert(d->flush(d) && flushes==1);
    install_identity_v1_t id;assert(ahci_identity(0,&id) && id.flags==INSTALL_ID_ELIGIBLE && id.nsid==1 && id.serial[0]=='S');
    identify_words[10]++;assert(!ahci_identity(0,&id) && !ahci_get_disk(0) && !freed);
    reset();identify_words[83]&=~(1u<<13);assert(ahci_inventory_boot()==1);d=ahci_get_disk(0);
    assert(d && !d->write_sector && !d->write_sectors && !d->flush && ahci_identity(0,&id) && !id.flags);
    reset();assert(ahci_inventory_boot()==1);d=ahci_get_disk(0);partial=true;memset(bytes,0xcc,sizeof(bytes));
    assert(!d->read_sector(d,0,bytes) && bytes[0]==0xcc && !ahci_get_disk(0) && !freed);
    before=issued;assert(!d->read_sector(d,0,bytes) && issued==before);
    reset();assert(ahci_inventory_boot()==1);d=ahci_get_disk(0);timeout_command=true;
    assert(!d->read_sector(d,0,bytes) && !ahci_inventory_valid() && !freed && !ahci_quiesce());
    uint8_t retained[4096];memcpy(retained,ram[1],4096);memset(bytes,0x3c,sizeof(bytes));before=issued;
    assert(!d->write_sector(d,0,bytes) && !memcmp(retained,ram[1],4096) && before==issued);
    reset();assert(ahci_inventory_boot()==1);d=ahci_get_disk(0);error_command=true;
    assert(!d->read_sector(d,0,bytes) && !freed);
    reset();stuck_stop=true;assert(!ahci_inventory_boot() && !allocated && !ahci_inventory_valid());
    reset();registers[CAP2/4]=1;bios_stuck=true;assert(!ahci_inventory_boot() && !allocated);
    reset();controllers=5;assert(!ahci_inventory_boot() && !allocated);
    reset();complete=false;assert(!ahci_inventory_boot() && !allocated);
    reset();assert(ahci_inventory_boot()==1);replaced=true;assert(!ahci_inventory_valid() && !ahci_identity(0,&id));
    reset();assert(ahci_inventory_boot()==1 && ahci_quiesce() && !ahci_get_disk(0) && !freed);
    reset();registers[(0x180+SIG)/4]=0x101;
    assert(ahci_inventory_boot()==2 && allocated==4 && ahci_get_disk(0)!=ahci_get_disk(1));
    assert(ports[0].descriptors!=ports[1].descriptors && ports[0].bounce!=ports[1].bounce);
    assert(ahci_identity(1,&id) && id.nsid==2);
    reset();clock_hz=100000000;clock_step=1;delayed_reads=POLL_LIMIT+4;
    assert(wait_clear(MMIO_BASE+0x100+CMD,CMD_CR,500)); /* fast CPU cannot shorten wall deadline */
    reset();clock_step=0;stuck_stop=true;
    assert(!wait_clear(MMIO_BASE+0x100+CMD,CMD_CR,500)); /* frozen clock remains bounded */
    reset();registers[CAP2/4]=0x18;registers[(0x100+0x44)/4]=3;
    registers[(0x100+SSTS)/4]=0x603;registers[(0x100+CMD)/4]|=(1u<<26)|(1u<<27)|(1u<<23);wake_reads=100;
    assert(ahci_inventory_boot()==1 && !wake_reads);
    assert((registers[(0x100+SCTL)/4]&0xf00)==0x300 && !(registers[(0x100+CMD)/4]&((1u<<26)|(1u<<27)|(1u<<23))));
    assert(registers[(0x100+0x44)/4]==2);
    reset();registers[(0x100+SIG)/4]=UINT32_MAX;registers[(0x100+TFD)/4]=0x7f;
    assert(ahci_inventory_boot()==1 && resets==1 && allocated==3);
    reset();registers[(0x100+SIG)/4]=UINT32_MAX;reset_signature=0xeb140101;
    assert(ahci_inventory_boot()==0 && ahci_inventory_valid() && resets==1 && allocated==1 && !issued);
    reset();allocation_limit=1;
    assert(!ahci_inventory_boot() && allocated==1 && freed==1 && !issued && !ports[0].live);
    reset();registers[CAP/4]&=~(1u<<31);high_dma=true;
    assert(!ahci_inventory_boot() && allocated==2 && freed==2 && !issued && !ports[0].live);
    reset();registers[CAP/4]&=~(1u<<31);
    assert(ahci_inventory_boot()==1 && !registers[(0x100+CLBU)/4] && !registers[(0x100+FBU)/4]);
    puts("PASS actual AHCI driver: geometry, DMA framing, bounds, reads/writes/flush, identity, errors, timeout, quarantine, handoff, inventory, quiesce");
    return 0;
}
