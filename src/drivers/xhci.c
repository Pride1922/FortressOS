#include "xhci.h"
#include "xhci_reset.h"
#include "xhci_rings.h"
#include "xhci_ports.h"
#include "xhci_dev.h"
#include "xhci_bot.h"
#include "gpt.h"
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "serial.h"
#include "spinlock.h"
#include <string.h>

static xhci_rings_io_t s_rings_io;
static xhci_dma_buffers_t s_dma;
static xhci_dev_dma_t s_dev_dma;
static xhci_bot_rings_t s_bot_rings;
static bool s_usb_storage_ready = false;
static xhci_bot_error_t s_flush_error;
static bool s_flush_error_pending;

/* Dedicated boot-only UC/NX window, separate from LAPIC/IOAPIC and heap. */
#define XHCI_PROBE_VIRT 0xffffffffe1000000ULL
#define XHCI_MAX_APERTURE (1024u * 1024u)

static uint32_t config_read(const pci_device_t *d, uint16_t off) {
    return pci_read_config32(d->segment, d->bus, d->device, d->function, off);
}
static void config_write(const pci_device_t *d, uint16_t off, uint32_t v) {
    pci_write_config32(d->segment, d->bus, d->device, d->function, off, v);
}
static void command_write(const pci_device_t *d, uint16_t v) {
    pci_write_config16(d->segment, d->bus, d->device, d->function, PCI_REG_COMMAND, v);
}
static uint32_t mmio_read(void *ctx, uint32_t off) {
    return *(volatile uint32_t *)((uintptr_t)ctx + off);
}
static void mmio_write(void *ctx, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)((uintptr_t)ctx + off) = v;
    __asm__ volatile("" ::: "memory");
}

/* Boot-only PIT channel 2, no scheduler ticks/IRQs required. As with APIC
 * calibration, restore speaker/gate control; the timer channel is not shared
 * with a running sound driver. Outer protocol waits count these 1 ms periods;
 * the iteration cap additionally bounds broken timer hardware. */
static bool delay_ms(void *ctx) {
    (void)ctx;
    uint8_t saved = inb(0x61);
    outb(0x61, saved & ~3u);
    outb(0x43, 0xb0);
    outb(0x42, (uint8_t)1193);
    outb(0x42, (uint8_t)(1193 >> 8));
    outb(0x61, (saved & ~3u) | 1);
    bool done = false;
    for (unsigned i = 0; i < 1000000; ++i) {
        if (inb(0x61) & 0x20) { done = true; break; }
        __asm__ volatile("pause" ::: "memory");
    }
    outb(0x61, saved);
    return done;
}

static void print_value(const char *name, uint32_t value) {
    serial_puts(name);
    serial_print_hex(value);
}

/* Only called in unlocked boot thread context, after the reset state machine
 * returns its snapshot. No logging inside polling/error callbacks. */
static void print_snapshot(const xhci_reset_result_t *r) {
    serial_puts("[USB 9G.1b] ");
    print_value("CAP=", r->cap);
    print_value(" HCSPARAMS1=", r->hcs1);
    print_value(" HCSPARAMS2=", r->hcs2);
    serial_puts("\n[USB 9G.1b] ");
    print_value("HCCPARAMS1=", r->hcc1);
    print_value(" DBOFF=", r->dboff);
    print_value(" RTSOFF=", r->rtsoff);
    serial_puts("\n[USB 9G.1b] ");
    print_value("Legacy capability offset=", r->legacy);
    serial_puts(r->legacy ? "\n" : " (no handoff semaphore)\n");
    serial_puts("[USB 9G.1b] ");
    print_value("USBCMD=", r->command);
    print_value(" USBSTS=", r->status);
    print_value(" PAGESIZE=", r->pagesize);
    serial_puts("\n");
    if (r->error) {
        serial_puts("[USB 9G.1b] Unavailable: ");
        serial_puts(r->error);
        print_value(" offset=", r->failed_offset);
        print_value(" value=", r->last_value);
        serial_puts("; returning to shell\n");
    }
}

static xhci_dump_record_t g_dump_record;
static bool g_dump_pending = false;

void usb_dump_state(void) {
    spin_debug_assert_unheld();
    if (!g_dump_pending && !g_dump_record.valid) return;
    g_dump_pending = false;
    serial_puts("[USB DUMP] Controller USBCMD=");
    serial_print_hex(g_dump_record.usbcmd);
    serial_puts(" USBSTS=");
    serial_print_hex(g_dump_record.usbsts);
    serial_puts(" PAGESIZE=");
    serial_print_hex(g_dump_record.pagesize);
    serial_puts("\n[USB DUMP] CmdRing EnqueueIdx=");
    serial_print_hex(g_dump_record.cmd_enqueue_idx);
    serial_puts(" CycleState=");
    serial_print_hex(g_dump_record.cmd_cycle_state);
    serial_puts("\n[USB DUMP] Last Submitted TRB: param=");
    serial_print_hex(g_dump_record.last_submitted_trb.parameter_low);
    serial_puts(" status=");
    serial_print_hex(g_dump_record.last_submitted_trb.status);
    serial_puts(" ctrl=");
    serial_print_hex(g_dump_record.last_submitted_trb.control);
    serial_puts("\n[USB DUMP] EventRing DequeueIdx=");
    serial_print_hex(g_dump_record.event_dequeue_idx);
    serial_puts(" CycleState=");
    serial_print_hex(g_dump_record.event_cycle_state);
    serial_puts(" CompCode=");
    serial_print_hex(g_dump_record.completion_code);
    serial_puts("\n[USB DUMP] Last Completed TRB: param=");
    serial_print_hex(g_dump_record.last_completed_trb.parameter_low);
    serial_puts(" status=");
    serial_print_hex(g_dump_record.last_completed_trb.status);
    serial_puts(" ctrl=");
    serial_print_hex(g_dump_record.last_completed_trb.control);
    serial_puts("\n");
    if (g_dump_record.error_msg) {
        serial_puts("[USB DUMP] Diagnostic note: ");
        serial_puts(g_dump_record.error_msg);
        serial_puts("\n");
    }
}

void xhci_boot_probe(const boot_info_t *boot_info) {
    static bool attempted;
    if (attempted) return;
    attempted = true;
    spin_debug_assert_unheld();
    pci_device_t d;
    if (!pci_find_device(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB, PCI_PROGIF_USB_XHCI, &d)) {
        serial_puts("[USB 9G.1b] No xHCI controller; skipped\n");
        return;
    }
    const char *error = "unsupported BAR0";
    uint32_t low = config_read(&d, PCI_REG_BAR0);
    uint32_t type = low & PCI_BAR_MEM_TYPE_MASK;
    bool wide = type == PCI_BAR_MEM_TYPE_64;
    if ((d.header_type & 0x7f) != PCI_HEADER_TYPE_NORMAL || !d.bar[0] ||
        low == UINT32_MAX || (low & (PCI_BAR_IO_SPACE | PCI_BAR_MEM_PREFETCH)) ||
        (type != PCI_BAR_MEM_TYPE_32 && !wide)) goto rejected;
    uint32_t high = wide ? config_read(&d, PCI_REG_BAR1) : 0;
    uint16_t original = (uint16_t)config_read(&d, PCI_REG_COMMAND);
    if (original == UINT16_MAX) goto rejected;

    /* BAR probing requires decode off. Stop PCI mastering first and never
     * restore firmware DMA: this checkpoint allocates/reclaims no DMA memory.
     * No operational register writes occur until ownership is granted. */
    uint16_t disabled = (original & ~(PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_IO_SPACE |
                                     PCI_COMMAND_BUS_MASTER)) | PCI_COMMAND_INT_DISABLE;
    command_write(&d, disabled);
    if ((uint16_t)config_read(&d, PCI_REG_COMMAND) != disabled) {
        error = "PCI decode/master disable failed";
        goto rejected;
    }
    config_write(&d, PCI_REG_BAR0, UINT32_MAX);
    if (wide) config_write(&d, PCI_REG_BAR1, UINT32_MAX);
    uint32_t mask_low = config_read(&d, PCI_REG_BAR0);
    uint32_t mask_high = wide ? config_read(&d, PCI_REG_BAR1) : UINT32_MAX;
    /* Always restore both halves before considering the sizing result. */
    if (wide) config_write(&d, PCI_REG_BAR1, high);
    config_write(&d, PCI_REG_BAR0, low);
    if (config_read(&d, PCI_REG_BAR0) != low ||
        (wide && config_read(&d, PCI_REG_BAR1) != high)) {
        error = "BAR restore failed; PCI decode remains disabled";
        goto rejected;
    }
    uint64_t mask = ((uint64_t)mask_high << 32) | (mask_low & ~0xfu);
    uint64_t size = ~mask + 1;
    uint64_t base = ((uint64_t)high << 32) | (low & ~0xfu);
    if (mask_low == UINT32_MAX || size < 4096 || size > XHCI_MAX_APERTURE ||
        (size & (size - 1)) || (base & (size - 1)) ||
        base > PTE_ADDR_MASK || size - 1 > (PTE_ADDR_MASK | 4095) - base) {
        error = "BAR size/base rejected (requires aligned 4 KiB..1 MiB aperture)";
        goto rejected;
    }
    /* Reject a BAR pointing into RAM, boot modules or other owned memory. */
    for (size_t i = 0; i < boot_info->memmap_entry_count; ++i) {
        const struct limine_memmap_entry *m = &boot_info->memmap_entries[i];
        if (m->type == LIMINE_MEMMAP_RESERVED) continue;
        if (m->length > UINT64_MAX - m->base ||
            (base < m->base + m->length && m->base < base + size)) {
            error = "BAR overlaps owned boot memory";
            goto rejected;
        }
    }
    serial_puts("[USB 9G.1b] BAR base=");
    serial_print_hex(base);
    serial_puts(" size=");
    serial_print_hex(size);
    serial_puts("; bus mastering disabled\n");

    uint64_t *pml4 = vmm_get_kernel_pml4_virt();
    uint32_t mapped = 0;
    for (; mapped < size; mapped += 4096) {
        uintptr_t va = XHCI_PROBE_VIRT + mapped;
        if (vmm_is_mapped(pml4, va) ||
            vmm_map_page(pml4, va, base + mapped,
                         PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT | PTE_NX) != VMM_OK) {
            error = "MMIO map failed or window occupied";
            goto unmap;
        }
    }
    command_write(&d, disabled | PCI_COMMAND_MEMORY_SPACE);
    if ((uint16_t)config_read(&d, PCI_REG_COMMAND) != (disabled | PCI_COMMAND_MEMORY_SPACE)) {
        error = "PCI memory decode enable failed";
        goto unmap;
    }
    xhci_reset_io_t io = {(void *)XHCI_PROBE_VIRT, (uint32_t)size,
                          mmio_read, mmio_write, delay_ms};
    xhci_reset_result_t result;
    bool ok = xhci_reset_controller(&io, &result);
    print_snapshot(&result);
    if (!ok) {
        error = NULL;
        goto unmap;
    }
    serial_puts("[USB 9G.1b] PASS: reset complete; halted, CNR=0\n");

    /* Phase 9G.1c/d/e: Hardware Parameters & Structure Allocation */
    uint32_t op_off = mmio_read((void *)XHCI_PROBE_VIRT, 0) & 0xff;
    uint32_t hcs1 = mmio_read((void *)XHCI_PROBE_VIRT, 0x04);
    uint32_t hcs2 = mmio_read((void *)XHCI_PROBE_VIRT, 0x08);
    uint32_t max_slots = hcs1 & 0xff;
    uint32_t sp_count = (((hcs2 >> 16) & 0x3e0) | ((hcs2 >> 27) & 0x1f));
    if (sp_count > 128) sp_count = 128;

    uintptr_t cmd_phys = pmm_alloc_page();
    uintptr_t event_phys = pmm_alloc_page();
    uintptr_t erst_phys = pmm_alloc_page();

    uintptr_t dcbaa_phys = pmm_alloc_page();
    uintptr_t sp_arr_phys = (sp_count > 0) ? pmm_alloc_page() : 0;
    uintptr_t sp_pages[128] = {0};
    bool sp_oom = false;
    for (uint32_t i = 0; i < sp_count; ++i) {
        sp_pages[i] = pmm_alloc_page();
        if (!sp_pages[i]) sp_oom = true;
    }
    uintptr_t input_ctx_phys = pmm_alloc_page();
    uintptr_t output_ctx_phys = pmm_alloc_page();
    uintptr_t ep0_ring_phys = pmm_alloc_page();
    uintptr_t bounce_buf_phys = pmm_alloc_page();

    if (!cmd_phys || !event_phys || !erst_phys || !dcbaa_phys ||
        (sp_count > 0 && !sp_arr_phys) || sp_oom ||
        !input_ctx_phys || !output_ctx_phys || !ep0_ring_phys || !bounce_buf_phys) {
        serial_puts("[USB 9G.1c] OOM: allocating DMA frames failed\n");
        if (cmd_phys) pmm_free_page(cmd_phys);
        if (event_phys) pmm_free_page(event_phys);
        if (erst_phys) pmm_free_page(erst_phys);
        if (dcbaa_phys) pmm_free_page(dcbaa_phys);
        if (sp_arr_phys) pmm_free_page(sp_arr_phys);
        for (uint32_t i = 0; i < sp_count; ++i) {
            if (sp_pages[i]) pmm_free_page(sp_pages[i]);
        }
        if (input_ctx_phys) pmm_free_page(input_ctx_phys);
        if (output_ctx_phys) pmm_free_page(output_ctx_phys);
        if (ep0_ring_phys) pmm_free_page(ep0_ring_phys);
        if (bounce_buf_phys) pmm_free_page(bounce_buf_phys);
        error = NULL;
        goto unmap;
    }

    /* Program CONFIG and DCBAAP while controller is halted */
    mmio_write((void *)XHCI_PROBE_VIRT, op_off + 0x38, max_slots);

    uint64_t *dcbaa_virt = (uint64_t *)vmm_phys_to_virt(dcbaa_phys);
    memset(dcbaa_virt, 0, 4096);
    if (sp_count > 0 && sp_arr_phys) {
        uint64_t *sp_arr = (uint64_t *)vmm_phys_to_virt(sp_arr_phys);
        for (uint32_t i = 0; i < sp_count; ++i) {
            sp_arr[i] = sp_pages[i];
        }
        dcbaa_virt[0] = sp_arr_phys;
    }

    mmio_write((void *)XHCI_PROBE_VIRT, op_off + 0x30, (uint32_t)dcbaa_phys);
    mmio_write((void *)XHCI_PROBE_VIRT, op_off + 0x34, (uint32_t)(dcbaa_phys >> 32));

    xhci_dma_buffers_t dma = {
        .cmd_ring_virt = (xhci_trb_t *)vmm_phys_to_virt(cmd_phys),
        .event_ring_virt = (xhci_trb_t *)vmm_phys_to_virt(event_phys),
        .erst_virt = (xhci_erst_entry_t *)vmm_phys_to_virt(erst_phys),
        .cmd_ring_phys = cmd_phys,
        .event_ring_phys = event_phys,
        .erst_phys = erst_phys,
        .cmd_enqueue_idx = 0,
        .cmd_cycle = 1,
        .event_dequeue_idx = 0,
        .event_cycle = 1,
    };

    xhci_dev_dma_t dev_dma = {
        .dcbaa_phys = dcbaa_phys,
        .dcbaa_virt = dcbaa_virt,
        .scratchpad_array_phys = sp_arr_phys,
        .scratchpad_array_virt = sp_arr_phys ? (uint64_t *)vmm_phys_to_virt(sp_arr_phys) : NULL,
        .scratchpad_count = sp_count,
        .input_ctx_phys = input_ctx_phys,
        .input_ctx_virt = (uint32_t *)vmm_phys_to_virt(input_ctx_phys),
        .output_ctx_phys = output_ctx_phys,
        .output_ctx_virt = (uint32_t *)vmm_phys_to_virt(output_ctx_phys),
        .ep0_ring_phys = ep0_ring_phys,
        .ep0_ring_virt = (xhci_trb_t *)vmm_phys_to_virt(ep0_ring_phys),
        .bounce_buf_phys = bounce_buf_phys,
        .bounce_buf_virt = (uint8_t *)vmm_phys_to_virt(bounce_buf_phys),
    };
    for (uint32_t i = 0; i < sp_count; ++i) {
        dev_dma.scratchpad_pages[i] = sp_pages[i];
    }

    xhci_rings_io_t rings_io = {
        .mmio_ctx = (void *)XHCI_PROBE_VIRT,
        .mmio_size = (uint32_t)size,
        .read32 = mmio_read,
        .write32 = mmio_write,
        .delay_ms = delay_ms,
    };

    /* Enable bus mastering for DMA transfers */
    command_write(&d, disabled | PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER);
    if ((uint16_t)config_read(&d, PCI_REG_COMMAND) !=
        (disabled | PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER)) {
        serial_puts("[USB 9G.1c] PCI bus mastering enable failed\n");
        error = NULL;
        goto unmap;
    }

    /* Verify rings (starts controller and verifies No-Op command) */
    bool rings_ok = xhci_verify_rings(&rings_io, &dma, &g_dump_record);

    if (rings_ok) {
        serial_puts("[USB 9G.1c] PASS: No-Op command completed; Command Completion Event received\n");

        /* Phase 9G.1d: Root Port Discovery & Reset */
        xhci_port_report_t port_report;
        if (xhci_discover_and_reset_ports(&rings_io, &dma, &port_report)) {
            serial_puts("[USB 9G.1d] Root ports: total=");
            serial_print_hex(port_report.total_ports);
            serial_puts(" usb2=");
            serial_print_hex(port_report.usb2_port_count);
            serial_puts(" usb3=");
            serial_print_hex(port_report.usb3_port_count);
            serial_puts(" connected=");
            serial_print_hex(port_report.connected_count);
            serial_puts("\n");

            for (uint32_t p = 1; p <= port_report.total_ports; ++p) {
                const xhci_port_info_t *pi = &port_report.ports[p - 1];
                if (!pi->connected) continue;
                serial_puts("[USB 9G.1d] Port ");
                serial_print_hex(p);
                if (pi->protocol_major == 3) {
                    serial_puts(": SuperSpeed device attached; unsupported in Phase 9G, skipped\n");
                } else if (pi->protocol_major == 2) {
                    serial_puts(": USB 2.0 device attached; ");
                    if (pi->enabled) {
                        serial_puts("reset complete, enabled, speed=");
                        if (pi->speed == XHCI_SPEED_HIGH) serial_puts("High-Speed (480 Mbps)\n");
                        else if (pi->speed == XHCI_SPEED_FULL) serial_puts("Full-Speed (12 Mbps)\n");
                        else serial_puts("Low-Speed (unsupported for storage)\n");
                    } else {
                        serial_puts("reset failed to enable port\n");
                    }
                }
            }

            if (port_report.connected_count == 0) {
                serial_puts("[USB 9G.1d] PASS: Root port scan complete (no devices attached)\n");
            } else {
                serial_puts("[USB 9G.1d] PASS: Root port scan complete\n");
                /* Phase 9G.1e: Device Addressing & Descriptors across connected USB 2.0 ports */
                xhci_bot_device_t bot_dev = {0};
                bool found_storage = false;

                for (uint32_t p = 1; p <= port_report.total_ports; ++p) {
                    const xhci_port_info_t *pi = &port_report.ports[p - 1];
                    if (!pi->connected || !pi->enabled || pi->protocol_major != 2) continue;
                    if (pi->speed != XHCI_SPEED_HIGH && pi->speed != XHCI_SPEED_FULL) continue;

                    serial_puts("[USB 9G.1e] Probing Port ");
                    serial_print_hex(p);
                    serial_puts(" for BOT Mass Storage...\n");

                    bool dev_ok = xhci_enumerate_device(&rings_io, &dma, &dev_dma,
                                                        (uint8_t)p, pi->speed, &bot_dev);

                    if (dev_ok && bot_dev.is_valid_bot_storage) {
                        found_storage = true;
                        serial_puts("[USB 9G.1e] PASS: Device addressed on Slot ");
                        serial_print_hex(bot_dev.slot_id);
                        serial_puts(" (Port ");
                        serial_print_hex(bot_dev.port_num);
                        serial_puts(")\n");
                        serial_puts("[USB 9G.1e] PASS: BOT Mass Storage device found on Slot ");
                        serial_print_hex(bot_dev.slot_id);
                        serial_puts(" (Port ");
                        serial_print_hex(bot_dev.port_num);
                        serial_puts(")\n[USB 9G.1e] VID=");
                        serial_print_hex(bot_dev.vendor_id);
                        serial_puts(" PID=");
                        serial_print_hex(bot_dev.product_id);
                        serial_puts(" EP0_MAX=");
                        serial_print_hex(bot_dev.ep0_max_packet);
                        serial_puts("\n[USB 9G.1e] Bulk-In EP=");
                        serial_print_hex(bot_dev.bulk_in_ep);
                        serial_puts(" (max=");
                        serial_print_hex(bot_dev.bulk_in_max_packet);
                        serial_puts(") Bulk-Out EP=");
                        serial_print_hex(bot_dev.bulk_out_ep);
                        serial_puts(" (max=");
                        serial_print_hex(bot_dev.bulk_out_max_packet);
                        serial_puts(")\n[USB 9G.1e] PASS: BOT Mass Storage device configured and ready for 9G.2 block I/O\n");

                        /* Phase 9G.2: Bulk-Only Transport & Read-Only Block Device ("sda") */
                        uintptr_t bulk_in_phys = pmm_alloc_page();
                        uintptr_t bulk_out_phys = pmm_alloc_page();
                        s_bot_rings.bulk_in_ring_phys = bulk_in_phys;
                        s_bot_rings.bulk_in_ring_virt = (xhci_trb_t *)vmm_phys_to_virt(bulk_in_phys);
                        s_bot_rings.bulk_out_ring_phys = bulk_out_phys;
                        s_bot_rings.bulk_out_ring_virt = (xhci_trb_t *)vmm_phys_to_virt(bulk_out_phys);

                        if (xhci_configure_bulk_endpoints(&rings_io, &dma, &dev_dma, &bot_dev, &s_bot_rings)) {
                            serial_puts("[USB 9G.2] Bulk endpoints configured (In=0x");
                            serial_print_hex(s_bot_rings.in_dci);
                            serial_puts(" Out=0x");
                            serial_print_hex(s_bot_rings.out_dci);
                            serial_puts(")\n");

                            scsi_inquiry_data_t inq = {0};
                            if (xhci_scsi_inquiry(&rings_io, &dma, &dev_dma, &s_bot_rings, &inq)) {
                                serial_puts("[USB 9G.2] SCSI INQUIRY: Vendor=\"");
                                serial_puts(s_bot_rings.vendor);
                                serial_puts("\" Product=\"");
                                serial_puts(s_bot_rings.product);
                                serial_puts("\"\n");
                            }

                            xhci_scsi_test_unit_ready(&rings_io, &dma, &dev_dma, &s_bot_rings);

                            uint64_t sectors = 0;
                            uint32_t sector_size = 0;
                            if (xhci_scsi_read_capacity(&rings_io, &dma, &dev_dma, &s_bot_rings, &sectors, &sector_size)) {
                                serial_puts("[USB 9G.2] SCSI Capacity: LBA count=");
                                serial_print_hex((uint32_t)(sectors >> 32));
                                serial_print_hex((uint32_t)sectors);
                                serial_puts(" Sector size=");
                                serial_print_dec(sector_size);
                                serial_puts(" bytes\n");

                                s_rings_io = rings_io;
                                s_dma = dma;
                                s_dev_dma = dev_dma;
                                s_usb_storage_ready = true;

                                 if (block_register_usb()) {
                                    serial_puts("[USB 9G.2] PASS: Registered block device \"sda\"\n");

                                    block_dev_t *sda = block_get_dev_by_name("sda");
                                    if (sda) {
                                        uint8_t sector0[512];
                                        if (block_read_sector(sda, 0, sector0)) {
                                            uint16_t sig = (uint16_t)sector0[510] | ((uint16_t)sector0[511] << 8);
                                            if (sig == 0xAA55) {
                                                serial_puts("[USB 9G.2] PASS: Sector 0 read verified (MBR signature 0xAA55)\n");
                                            }
                                        }

                                        if (gpt_parse(sda)) {
                                            serial_puts("[USB 9G.2] PASS: GPT partition table parsed on sda (partitions=");
                                            serial_print_dec((uint32_t)gpt_get_partition_count());
                                            serial_puts(")\n");
                                        }
                                    }

                                    /* Phase 9G.4: Probe cache policy and set durability mode.
                                     * Must be called in thread context with no subsystem lock.
                                     * bot_rings is stable: only set once during boot, never freed. */
                                    serial_puts("[USB 9G.4] Probing USB cache durability policy...\n");
                                    xhci_bot_probe_durability(&s_rings_io, &s_dma, &s_dev_dma, &s_bot_rings);
                                }
                            }
                        }
                        break;
                    } else {
                        serial_puts("[USB 9G.1e] Port ");
                        serial_print_hex(p);
                        serial_puts(": ");
                        serial_puts(bot_dev.error_msg ? bot_dev.error_msg : "not storage");
                        serial_puts(" (if_cls=");
                        serial_print_hex(bot_dev.if_class);
                        serial_puts(" sub=");
                        serial_print_hex(bot_dev.if_subclass);
                        serial_puts(" proto=");
                        serial_print_hex(bot_dev.if_proto);
                        serial_puts(")\n");
                    }
                }

                if (!found_storage) {
                    serial_puts("[USB 9G.1e] FAIL: No supported BOT Mass Storage device found; DMA quarantined\n");
                    serial_puts("[USB 9G.1e] DIAG: step=");
                    serial_print_hex(bot_dev.step);
                    serial_puts(" cfg_len=");
                    serial_print_hex(bot_dev.total_cfg_len);
                    serial_puts(" if_cls=");
                    serial_print_hex(bot_dev.if_class);
                    serial_puts(" sub=");
                    serial_print_hex(bot_dev.if_subclass);
                    serial_puts(" proto=");
                    serial_print_hex(bot_dev.if_proto);
                    serial_puts("\n[USB 9G.1e] DIAG: slot_state=");
                    serial_print_hex(bot_dev.slot_state);
                    serial_puts(" ep0_state=");
                    serial_print_hex(bot_dev.ep0_state);
                    serial_puts(" comp_code=");
                    serial_print_hex(bot_dev.last_comp_code);
                    serial_puts(" resid=");
                    serial_print_hex(bot_dev.last_residual);
                    serial_puts(" trb=");
                    serial_print_hex(bot_dev.last_trb_param);
                    serial_puts("\n[USB 9G.1e] DIAG: dev_hdr=");
                    for (int i = 0; i < 8; ++i) {
                        serial_print_hex(bot_dev.raw_desc_hdr[i]);
                        serial_puts(" ");
                    }
                    serial_puts("\n[USB 9G.1e] DIAG: cfg_hdr=");
                    for (int i = 0; i < 9; ++i) {
                        serial_print_hex(bot_dev.raw_cfg_hdr[i]);
                        serial_puts(" ");
                    }
                    serial_puts("\n");
                }
            }
        } else {
            serial_puts("[USB 9G.1d] Port discovery failed\n");
        }

        if (s_usb_storage_ready) {
            /* Keep xHCI controller and DMA active for runtime block device I/O */
            return;
        }

        /* Halt controller & disable bus mastering upon probe completion */
        uint32_t cmd_reg = mmio_read((void *)XHCI_PROBE_VIRT, op_off + 0);
        mmio_write((void *)XHCI_PROBE_VIRT, op_off + 0, cmd_reg & ~1u);
        for (unsigned i = 0; i <= 100; ++i) {
            if (mmio_read((void *)XHCI_PROBE_VIRT, op_off + 4) & 1u) break;
            delay_ms(NULL);
        }
        command_write(&d, disabled | PCI_COMMAND_MEMORY_SPACE);

        /* Reclaim allocated frames on clean success */
        pmm_free_page(cmd_phys);
        pmm_free_page(event_phys);
        pmm_free_page(erst_phys);
        pmm_free_page(dcbaa_phys);
        if (sp_arr_phys) pmm_free_page(sp_arr_phys);
        for (uint32_t i = 0; i < sp_count; ++i) pmm_free_page(sp_pages[i]);
        pmm_free_page(input_ctx_phys);
        pmm_free_page(output_ctx_phys);
        pmm_free_page(ep0_ring_phys);
    } else {
        serial_puts("[USB 9G.1c] FAIL: ring verification failed; DMA frames quarantined\n");
        g_dump_pending = true;
    }

    error = NULL;
unmap:
    /* Keep PCI mastering and interrupts off, even on ownership/reset failure.
     * Only CPU mappings are removed; no controller-owned memory is freed. */
    command_write(&d, disabled);
    while (mapped) {
        mapped -= 4096;
        vmm_unmap_page(pml4, XHCI_PROBE_VIRT + mapped);
    }
    if (g_dump_pending) usb_dump_state();
    if (!error) return;
rejected:
    serial_puts("[USB 9G.1b] Unavailable: ");
    serial_puts(error);
    serial_puts("; returning to shell\n");
}

bool usb_is_initialized(void) {
    return s_usb_storage_ready;
}

uint32_t usb_get_sector_size(void) {
    return s_bot_rings.sector_size;
}

uint64_t usb_get_sector_count(void) {
    return s_bot_rings.sector_count;
}

bool usb_block_read(block_dev_t *dev, uint64_t lba, void *buf) {
    (void)dev;
    if (!s_usb_storage_ready) return false;
    return xhci_scsi_read_sector(&s_rings_io, &s_dma, &s_dev_dma, &s_bot_rings, lba, buf);
}

bool usb_block_write(block_dev_t *dev, uint64_t lba, const void *buf) {
    (void)dev;
    if (!s_usb_storage_ready) return false;
    return xhci_scsi_write_sector(&s_rings_io, &s_dma, &s_dev_dma, &s_bot_rings, lba, buf);
}

bool usb_block_flush(block_dev_t *dev) {
    (void)dev;
    if (!s_usb_storage_ready) return false;
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    /* Use the durability-mode-aware barrier; this also latches READ_ONLY on failure */
    bool ok = xhci_bot_flush_barrier(&s_rings_io, &s_dma, &s_dev_dma, &s_bot_rings);
    if (!ok && !s_flush_error_pending) {
        s_flush_error = s_bot_rings.last_error;
        s_flush_error_pending = true;
    }
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory");
    return ok;
}

void usb_report_flush_failure(void) {
    spin_debug_assert_unheld();
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    bool pending = s_flush_error_pending;
    xhci_bot_error_t error = s_flush_error;
    s_flush_error_pending = false;
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory");
    if (!pending) return;
    serial_puts("[USB FLUSH] SYNCHRONIZE CACHE failed; ");
    print_value("CSW status=", error.csw_status);
    if (error.sense_valid) {
        print_value(" sense=", error.sense_key);
        print_value(" ASC=", error.asc);
        print_value(" ASCQ=", error.ascq);
        if (error.sense_key == 5 && error.asc == 0x20 && error.ascq == 0)
            serial_puts(" (unsupported command)");
        else if (error.sense_key == 5 && error.asc == 0x24 && error.ascq == 0)
            serial_puts(" (invalid command field)");
        else if (error.sense_key == 7)
            serial_puts(" (data protect)");
    } else {
        serial_puts("; no valid sense data");
    }
    if (error.transport_failed)
        serial_puts("; transport unavailable until reboot; DMA buffers retained");
    serial_puts("\n");
}

usb_durability_mode_t usb_get_durability_mode(void) {
    if (!s_usb_storage_ready) return USB_DURABILITY_UNKNOWN;
    return xhci_bot_get_durability_mode(&s_bot_rings);
}
