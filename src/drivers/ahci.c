#include "ahci.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "apic.h"
#include "spinlock.h"
#include "serial.h"
#include "string.h"
#include "../../user/tools/digest.h"

/* Intel AHCI 1.3.1, sections 3, 4, 10.1 and 10.3. All MMIO is UC/NX,
 * all payload DMA goes through a private bounce page, never caller memory. */
#define MAX_HBAS 4u
#define MMIO_BASE 0xffffffffe4000000ULL
#define CAP 0x00u
#define GHC 0x04u
#define PI 0x0cu
#define CAP2 0x24u
#define BOHC 0x28u
#define CLB 0x00u
#define CLBU 0x04u
#define FB 0x08u
#define FBU 0x0cu
#define IS 0x10u
#define IE 0x14u
#define CMD 0x18u
#define TFD 0x20u
#define SIG 0x24u
#define SSTS 0x28u
#define SCTL 0x2cu
#define SERR 0x30u
#define SACT 0x34u
#define CI 0x38u
#define CMD_ST 1u
#define CMD_FRE (1u << 4)
#define CMD_FR (1u << 14)
#define CMD_CR (1u << 15)
#define ATA_ERROR_MASK ((1u << 30) | (1u << 29) | (1u << 28) | (1u << 27) | (1u << 26) | (1u << 24))
#define POLL_LIMIT 10000000u

typedef struct {
    pci_device_t pci;
    uintptr_t mmio;
    uint32_t cap;
    uintptr_t probe_fis;
    bool owned;
} hba_t;
typedef struct {
    hba_t *hba;
    unsigned port;
    uintptr_t regs, descriptors, bounce;
    bool live, failed;
    ahci_geometry_t geometry;
    block_dev_t disk;
} port_t;
static hba_t hbas[MAX_HBAS];
static port_t ports[AHCI_MAX_DISKS];
static unsigned hba_count, port_count, disk_count;
static port_t *disks[AHCI_MAX_DISKS];
static bool scanned, coherent;
/* ext2 (rank 1) may call this driver. No allocations, VMM operations, sleeping
 * or scheduler calls occur under this rank-2 leaf I/O lock. */
static spinlock_t io_lock = SPINLOCK_RANKED(2, "ahci_io");

static uint32_t rd(uintptr_t address) {
#ifdef TEST_AHCI_HOST
    extern uint32_t ahci_test_read(uintptr_t);
    return ahci_test_read(address);
#else
    return *(volatile uint32_t *)address;
#endif
}
static void wr(uintptr_t address, uint32_t value) {
#ifdef TEST_AHCI_HOST
    extern void ahci_test_write(uintptr_t, uint32_t);
    ahci_test_write(address, value);
#else
    *(volatile uint32_t *)address = value;
#endif
}
static uint64_t clock_read(void) {
#ifdef TEST_AHCI_HOST
    extern uint64_t ahci_test_clock(void);
    return ahci_test_clock();
#else
    uint32_t low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
#endif
}
/* Local elapsed TSC only. Runtime callers hold IRQ exclusion and cannot migrate;
 * no cross-CPU TSC origin comparison or timer-interrupt dependency. */
typedef struct {uint64_t start,last,hz;unsigned stagnant,fallback;} deadline_t;
static deadline_t deadline_begin(void) {
    uint64_t now=clock_read();
    return (deadline_t){.start=now,.last=now,.hz=apic_poll_clock_hz()};
}
static bool expired(deadline_t *d, unsigned ms) {
    uint64_t now=clock_read();
    d->stagnant=now==d->last ? d->stagnant+1 : 0;d->last=now;
    if (d->stagnant>=POLL_LIMIT) return true;
    if (d->hz && d->hz<=UINT64_MAX/30000) return now-d->start>=d->hz*ms/1000;
    return ++d->fallback>=POLL_LIMIT;
}
static bool wait_clear(uintptr_t reg, uint32_t mask, unsigned ms) {
    deadline_t deadline=deadline_begin();
    for (;;) {
        if (!(rd(reg) & mask)) return true;
        if (expired(&deadline, ms)) break;
        __asm__ volatile("pause" ::: "memory");
    }
    return false;
}
static bool stop(port_t *p) {
    wr(p->regs + CMD, rd(p->regs + CMD) & ~CMD_ST);
    if (!wait_clear(p->regs + CMD, CMD_CR, 500)) return false;
    wr(p->regs + CMD, rd(p->regs + CMD) & ~CMD_FRE);
    return wait_clear(p->regs + CMD, CMD_FR, 500);
}
static bool pinned(hba_t *h) {
    pci_device_t *d = &h->pci;
    return pci_read_config32(d->segment,d->bus,d->device,d->function,PCI_REG_VENDOR_ID) ==
               ((uint32_t)d->vendor_id | (uint32_t)d->device_id << 16) &&
           (pci_read_config32(d->segment,d->bus,d->device,d->function,8) >> 8) == 0x010601u &&
           pci_read_config32(d->segment,d->bus,d->device,d->function,PCI_REG_BAR5) == (uint32_t)d->bar[5] &&
           (pci_read_config16(d->segment,d->bus,d->device,d->function,PCI_REG_COMMAND) & 6) == 6;
}
static bool healthy(port_t *p) {
    return p->live && !p->failed && pinned(p->hba) &&
           (rd(p->regs + SSTS) & 0xf0f) == 0x103 && rd(p->regs + SIG) == 0x101 &&
           !(rd(p->regs + IS) & ((1u<<6)|(1u<<22))); /* sticky connect/PHY change */
}
static bool fail(port_t *p) {
    p->failed = true;
    /* Never reuse/free DMA after a command error or uncertain completion, even
     * if the bounded engine stop succeeds. No hidden reset/retry writes. */
    if (pinned(p->hba)) (void)stop(p);
    return false;
}

bool ahci_decode_identify(const uint16_t w[256], ahci_geometry_t *out) {
    if (!w || !out || (w[0] & 0x8000) || (w[49] & 0x300) != 0x300 ||
        (w[83] & 0xc400) != 0x4400 || (w[128] & 4)) return false;
    ahci_geometry_t g = {0};
    g.sectors = (uint64_t)w[100] | (uint64_t)w[101] << 16 |
                (uint64_t)w[102] << 32 | (uint64_t)w[103] << 48;
    if (!g.sectors || g.sectors > (1ULL << 48)) return false;
    g.sector_size = 512;
    if ((w[106] & 0xc000) == 0x4000 && (w[106] & 0x1000)) {
        uint32_t words = (uint32_t)w[117] | (uint32_t)w[118] << 16;
        if (words != 256 && words != 2048) return false;
        g.sector_size = words * 2;
    }
    g.flush_ext = (w[83] & (1u << 13)) != 0;
    for (unsigned i = 0; i < 20; i++) g.serial[i] = (char)(w[10+i/2] >> ((i&1) ? 0 : 8));
    for (unsigned i = 0; i < 40; i++) g.model[i] = (char)(w[27+i/2] >> ((i&1) ? 0 : 8));
    if ((w[84] & 0xc100) == 0x4100)
        for (unsigned i = 0; i < 8; i++) g.wwn[i] = (uint8_t)(w[108+i/2] >> ((i&1) ? 0 : 8));
    *out = g;
    return true;
}

/* 32-byte command header at 0; received FIS at 1024; 128-byte-aligned command
 * table at 1280, one 16-byte PRDT entry at table+128. Everything fits one page. */
static bool command(port_t *p, uint8_t opcode, uint64_t lba, uint32_t count,
                    size_t bytes, bool write) {
    if (!healthy(p) || bytes > PAGE_SIZE || (rd(p->regs+CI) | rd(p->regs+SACT))) return fail(p);
    if (!wait_clear(p->regs+TFD, 0x88, 5000)) return fail(p);
    uint8_t *desc = vmm_phys_to_virt(p->descriptors);
    uint32_t *header = (void *)desc;
    uint8_t *fis = desc + 1280;
    uint32_t *prdt = (void *)(fis + 128);
    memset(desc, 0, 1024);
    memset(fis, 0, 256);
    header[0] = 5u | (write ? 1u << 6 : 0) | (bytes ? 1u << 16 : 0);
    header[2] = (uint32_t)(p->descriptors + 1280);
    header[3] = (uint32_t)((p->descriptors + 1280) >> 32);
    fis[0] = 0x27; fis[1] = 0x80; fis[2] = opcode;
    fis[4] = (uint8_t)lba; fis[5] = (uint8_t)(lba >> 8); fis[6] = (uint8_t)(lba >> 16);
    fis[7] = 0x40;
    fis[8] = (uint8_t)(lba >> 24); fis[9] = (uint8_t)(lba >> 32); fis[10] = (uint8_t)(lba >> 40);
    fis[12] = (uint8_t)count; fis[13] = (uint8_t)(count >> 8);
    if (bytes) {
        prdt[0] = (uint32_t)p->bounce; prdt[1] = (uint32_t)(p->bounce >> 32);
        prdt[3] = (uint32_t)bytes - 1;
    }
    wr(p->regs+SERR, UINT32_MAX); wr(p->regs+IS, UINT32_MAX);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    wr(p->regs+CI, 1);
    deadline_t deadline=deadline_begin();
    for (;;) {
        if ((rd(p->regs+IS) & ATA_ERROR_MASK) || (rd(p->regs+TFD) & 0x21)) return fail(p);
        if (!(rd(p->regs+CI) & 1)) {
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            if ((rd(p->regs+IS) & ATA_ERROR_MASK) || (rd(p->regs+TFD) & 0xa9) ||
                (bytes && *(volatile uint32_t *)(desc+4) != bytes)) return fail(p);
            return true;
        }
        if (expired(&deadline, 5000)) break;
        __asm__ volatile("pause" ::: "memory");
    }
    return fail(p);
}
static bool transfer(block_dev_t *d, uint64_t lba, uint32_t count, void *bytes, bool write) {
    if (!d || !bytes || !count || !d->sector_size || count > PAGE_SIZE/d->sector_size ||
        lba >= d->sector_count || count > d->sector_count-lba) return false;
    port_t *p = d->priv;
    if (!p || (write && !p->geometry.flush_ext)) return false;
    uint64_t irq = spin_lock_irqsave(&io_lock);
    /* Quarantined DMA pages may still belong to hardware. Reject before even
     * zeroing/copying the bounce buffer, not merely before ringing PxCI. */
    if (!healthy(p)) {
        (void)fail(p);
        spin_unlock_irqrestore(&io_lock,irq);return false;
    }
    size_t n = (size_t)count * d->sector_size;
    void *bounce = vmm_phys_to_virt(p->bounce);
    if (write) memcpy(bounce, bytes, n); else memset(bounce, 0, n);
    bool ok = command(p, write ? 0x35 : 0x25, lba, count, n, write);
    if (ok && !write) memcpy(bytes, bounce, n);
    spin_unlock_irqrestore(&io_lock, irq);
    return ok;
}
static bool read_run(block_dev_t *d,uint64_t l,uint32_t n,void *b) {return transfer(d,l,n,b,false);}
static bool write_run(block_dev_t *d,uint64_t l,uint32_t n,const void *b) {return transfer(d,l,n,(void *)b,true);}
static bool read_sector(block_dev_t *d,uint64_t l,void *b) {return read_run(d,l,1,b);}
static bool write_sector(block_dev_t *d,uint64_t l,const void *b) {return write_run(d,l,1,b);}
static bool flush_disk(block_dev_t *d) {
    port_t *p = d ? d->priv : NULL;
    if (!p || !p->geometry.flush_ext) return false;
    uint64_t irq = spin_lock_irqsave(&io_lock);
    bool ok = command(p, 0xea, 0, 0, 0, false);
    spin_unlock_irqrestore(&io_lock, irq);
    return ok;
}
/* Boot classification with a private receive-only FIS page and ST kept clear.
 * Firmware need not have enumerated a live link. No ATA/ATAPI command is issued;
 * COMRESET, if necessary, requests its signature, never retries failed I/O. */
static bool classify_link(port_t *p,uint32_t *signature) {
    *signature=rd(p->regs+SIG);
    if (*signature && *signature!=UINT32_MAX) return true;
    uint64_t hz=apic_poll_clock_hz();
    if (!hz || hz>UINT64_MAX/30000 || (rd(p->regs+CI)|rd(p->regs+SACT))) return false;
    hba_t *h=p->hba;
    if (!h->probe_fis) {
        uintptr_t page=pmm_alloc_page();
        if (!page) return false;
        if (!(h->cap&(1u<<31)) && page>UINT32_MAX-PAGE_SIZE+1) {pmm_free_page(page);return false;}
        h->probe_fis=page;
    }
    /* Reuse only after the previous classifier proved FRE=FR=0. Any stop
     * failure terminates the entire boot inventory and retains this page. */
    memset(vmm_phys_to_virt(h->probe_fis),0,PAGE_SIZE);
    wr(p->regs+FB,(uint32_t)h->probe_fis);wr(p->regs+FBU,(uint32_t)(h->probe_fis>>32));
    wr(p->regs+CMD,rd(p->regs+CMD)|CMD_FRE);
    *signature=rd(p->regs+SIG);
    bool ok=true;
    if (!*signature || *signature==UINT32_MAX) {
        uint32_t control=rd(p->regs+SCTL)&~15u;
        wr(p->regs+SCTL,control|1u);(void)rd(p->regs+SCTL);
        deadline_t pulse=deadline_begin();
        while (!expired(&pulse,1)) __asm__ volatile("pause" ::: "memory");
        wr(p->regs+SCTL,control);(void)rd(p->regs+SCTL);
        if (pulse.stagnant>=POLL_LIMIT) ok=false;
    }
    deadline_t deadline=deadline_begin();
    while (ok) {
        *signature=rd(p->regs+SIG);
        if ((rd(p->regs+SSTS)&0xf0f)==0x103 && *signature && *signature!=UINT32_MAX) break;
        if (expired(&deadline,3000)) {ok=false;break;}
        __asm__ volatile("pause" ::: "memory");
    }
    wr(p->regs+CMD,rd(p->regs+CMD)&~CMD_FRE);
    return wait_clear(p->regs+CMD,CMD_FR,500) && ok;
}
static bool init_port(hba_t *h, unsigned port) {
    port_t probe = {.hba=h, .port=port, .regs=h->mmio+0x100+port*0x80};
    uint32_t original = rd(probe.regs+CMD);
    if (!stop(&probe)) return false;
    wr(probe.regs+IE, 0);
    /* Do not inherit firmware's automatic partial/slumber policy. This first
     * driver keeps the link active rather than treating an idle-power state as
     * removal. Change power policy only with the command engine stopped. */
    wr(probe.regs+SCTL,(rd(probe.regs+SCTL)&~0xf00u)|0x300u);
    if ((rd(h->mmio+CAP2)&0x18)==0x18) {
        uint32_t sleep=rd(probe.regs+0x44);
        if (sleep&2) wr(probe.regs+0x44,sleep&~1u); /* disable supported aggressive DevSleep */
    }
    wr(probe.regs+CMD, (rd(probe.regs+CMD) & ~((15u<<28)|(1u<<27)|(1u<<26)|(1u<<23))) | (1u<<28) | 6u);
    deadline_t deadline=deadline_begin();
    unsigned det;
    for (;;) {
        uint32_t status=rd(probe.regs+SSTS);det=status&15;
        if ((status&0xf0f)==0x103) break;
        if (!det && (!(h->cap & (1u<<27)) || (original & 2))) return true; /* empty port */
        if (expired(&deadline,3000)) return det==0;
        __asm__ volatile("pause" ::: "memory");
    }
    uint32_t signature;
    if (!classify_link(&probe,&signature)) return false;
    if (signature!=0x101) return true; /* ATAPI/SEM/PMP: unsupported, no commands */
    if (port_count==AHCI_MAX_DISKS) return false;
    port_t *p=&ports[port_count++]; *p=probe;
    p->descriptors=pmm_alloc_page(); p->bounce=pmm_alloc_page();
    if (!p->descriptors || !p->bounce || (!(h->cap & (1u<<31)) &&
        (p->descriptors>UINT32_MAX-PAGE_SIZE+1 || p->bounce>UINT32_MAX-PAGE_SIZE+1))) {
        if (p->descriptors) pmm_free_page(p->descriptors);
        if (p->bounce) pmm_free_page(p->bounce);
        p->descriptors=p->bounce=0;return false;
    }
    memset(vmm_phys_to_virt(p->descriptors),0,PAGE_SIZE);
    memset(vmm_phys_to_virt(p->bounce),0,PAGE_SIZE);
    wr(p->regs+CLB,(uint32_t)p->descriptors);wr(p->regs+CLBU,(uint32_t)(p->descriptors>>32));
    wr(p->regs+FB,(uint32_t)(p->descriptors+1024));wr(p->regs+FBU,(uint32_t)((p->descriptors+1024)>>32));
    /* From here every DMA frame is retained for the boot, including failures. */
    p->live=true;
    wr(p->regs+SERR,UINT32_MAX);wr(p->regs+IS,UINT32_MAX);
    wr(p->regs+CMD,rd(p->regs+CMD)|CMD_FRE);
    if (!wait_clear(p->regs+TFD,0x88,30000)) return fail(p); /* bounded HDD spin-up */
    wr(p->regs+CMD,rd(p->regs+CMD)|CMD_ST);
    if (!command(p,0xec,0,0,512,false) ||
        !ahci_decode_identify(vmm_phys_to_virt(p->bounce),&p->geometry)) return fail(p);
    block_dev_t *d=&p->disk;
    memcpy(d->name,"sata0",6);d->name[4]=(char)('0'+disk_count);
    d->sector_size=p->geometry.sector_size;d->sector_count=p->geometry.sectors;
    d->read_sector=read_sector;d->read_sectors=read_run;d->max_run_bytes=PAGE_SIZE;d->priv=p;
    if (p->geometry.flush_ext) {d->write_sector=write_sector;d->write_sectors=write_run;d->flush=flush_disk;}
    disks[disk_count++]=p;
    serial_puts("[AHCI] SATA disk ");serial_puts(d->name);
    serial_puts(" sectors=");serial_print_dec(d->sector_count);
    serial_puts(" sector_size=");serial_print_dec(d->sector_size);
    serial_puts(d->flush ? " flush=EXT\n" : " read-only (no FLUSH CACHE EXT)\n");
    return true;
}

size_t ahci_inventory_boot(void) {
    if (scanned) return ahci_inventory_valid() ? disk_count : 0;
    scanned=true;coherent=false;
    pci_device_t devices[MAX_HBAS+1];
    size_t n=pci_find_all_devices(PCI_CLASS_STORAGE,PCI_SUBCLASS_STORAGE_SATA,1,devices,MAX_HBAS+1);
    if (!pci_inventory_complete() || n>MAX_HBAS) return 0;
    for (unsigned i=0;i<n;i++) {
        hba_t *h=&hbas[hba_count++];h->pci=devices[i];
        if (!h->pci.bar[5] || h->pci.bar_is_io[5] || h->pci.bar_is_64[5] ||
            h->pci.bar_prefetch[5] || (h->pci.bar[5]&0x3ffu) || h->pci.bar[5]>UINT32_MAX-12287) {
            serial_puts("[AHCI] Unsupported controller BAR5\n");return 0;
        }
        uintptr_t base=MMIO_BASE+(uintptr_t)i*0x10000;
        uintptr_t physical=h->pci.bar[5]&~(PAGE_SIZE-1);
        h->mmio=base+(h->pci.bar[5]&(PAGE_SIZE-1));
        uint16_t c=pci_read_config16(h->pci.segment,h->pci.bus,h->pci.device,h->pci.function,PCI_REG_COMMAND);
        pci_write_config16(h->pci.segment,h->pci.bus,h->pci.device,h->pci.function,PCI_REG_COMMAND,
                           c|PCI_COMMAND_MEMORY_SPACE|PCI_COMMAND_BUS_MASTER|PCI_COMMAND_INT_DISABLE);
        for (unsigned page=0;page<3;page++)
            if (vmm_map_page(vmm_get_kernel_pml4_virt(),base+page*PAGE_SIZE,physical+page*PAGE_SIZE,
                PTE_PRESENT|PTE_WRITABLE|PTE_PCD|PTE_PWT|PTE_NX|PTE_DEVICE)!=VMM_OK) return 0;
        h->cap=rd(h->mmio+CAP);
        if (rd(h->mmio+CAP2)&1) {
            wr(h->mmio+BOHC,rd(h->mmio+BOHC)|2);
            if (!wait_clear(h->mmio+BOHC,0x11,2500)) return 0;
        }
        wr(h->mmio+GHC,(rd(h->mmio+GHC)|(1u<<31))&~2u);
        if (!(rd(h->mmio+GHC)&(1u<<31))) return 0;
        h->owned=true;
        uint32_t implemented=rd(h->mmio+PI);
        if ((h->cap&31)!=31 && (implemented>>((h->cap&31)+1))) return 0;
        for (unsigned port=0;port<32;port++)
            if ((implemented&(1u<<port)) && !init_port(h,port)) {
                serial_puts("[AHCI] Port initialization rejected: ");serial_print_dec(port);
                if (pinned(h)) {
                    uintptr_t r=h->mmio+0x100+port*0x80;
                    serial_puts(" SSTS=");serial_print_hex(rd(r+SSTS));
                    serial_puts(" CMD=");serial_print_hex(rd(r+CMD));
                    serial_puts(" SIG=");serial_print_hex(rd(r+SIG));
                    serial_puts(" TFD=");serial_print_hex(rd(r+TFD));
                }
                serial_puts("\n");return 0;
            }
    }
    coherent=true;
    return ahci_inventory_valid() ? disk_count : 0;
}
bool ahci_inventory_valid(void) {
    if (!scanned || !coherent || !pci_inventory_complete()) return false;
    for (unsigned i=0;i<hba_count;i++) if (!hbas[i].owned || !pinned(&hbas[i])) return false;
    for (unsigned i=0;i<disk_count;i++) if (!healthy(disks[i])) return false;
    return true;
}
block_dev_t *ahci_get_disk(unsigned index) {
    return coherent && index<disk_count && healthy(disks[index]) ? &disks[index]->disk : NULL;
}
bool ahci_identity(unsigned index,install_identity_v1_t *out) {
    if (!out || index>=disk_count || !ahci_inventory_valid()) return false;
    port_t *p=disks[index];ahci_geometry_t fresh;
    uint64_t irq=spin_lock_irqsave(&io_lock);
    bool ok=command(p,0xec,0,0,512,false) && ahci_decode_identify(vmm_phys_to_virt(p->bounce),&fresh);
    if (ok && (fresh.sectors!=p->geometry.sectors || fresh.sector_size!=p->geometry.sector_size ||
        fresh.flush_ext!=p->geometry.flush_ext || memcmp(fresh.serial,p->geometry.serial,20) ||
        memcmp(fresh.model,p->geometry.model,40) || memcmp(fresh.wwn,p->geometry.wwn,8))) ok=fail(p);
    if (ok) {
        pci_device_t *d=&p->hba->pci;
        install_identity_v1_t id={.size=sizeof(id),.version=1,.generation=1,
            .sector_count=fresh.sectors,.sector_size=fresh.sector_size,.nsid=p->port+1,
            .segment=d->segment,.bus=d->bus,.device=d->device,.function=d->function,
            .flags=fresh.sector_size==512 && fresh.flush_ext ? INSTALL_ID_ELIGIBLE : 0};
        memcpy(id.serial,fresh.serial,20);memcpy(id.model,fresh.model,40);memcpy(id.eui64,fresh.wwn,8);
        digest_t hash;digest_init(&hash,DIGEST_SHA256);(void)digest_update(&hash,&id,128);digest_final(&hash,id.digest);
        *out=id;
    }
    spin_unlock_irqrestore(&io_lock,irq);return ok;
}
bool ahci_quiesce(void) {
    uint64_t irq=spin_lock_irqsave(&io_lock);bool ok=coherent;
    for (unsigned i=0;i<port_count;i++) if (ports[i].live) {
        if (ports[i].failed || !pinned(ports[i].hba) || !stop(&ports[i])) ok=false;
        ports[i].failed=true;
    }
    spin_unlock_irqrestore(&io_lock,irq);return ok;
}
