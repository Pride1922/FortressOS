#include "xhci_ports.h"
#include "boot_timing.h"
#include <string.h>

static uint64_t elapsed_ticks(uint64_t start) {
    uint64_t end = BOOT_TIMING_READ();
    return end >= start ? end - start : 0;
}

static bool fits(const xhci_rings_io_t *io, uint32_t off, uint32_t len) {
    return !(off & 3) && off <= io->mmio_size && len <= io->mmio_size - off;
}

static void drain_event_ring(const xhci_rings_io_t *io, const xhci_dma_buffers_t *dma) {
    if (!dma || !dma->event_ring_virt) return;
    uint32_t rtsoff = io->read32(io->mmio_ctx, 0x18);
    if ((rtsoff & 31) || !fits(io, rtsoff, 0x40)) return;
    uint32_t intr0 = rtsoff + 0x20;

    /* Drain up to 256 events if posted */
    for (uint32_t i = 0; i < XHCI_RING_TRB_COUNT; ++i) {
        volatile const xhci_trb_t *event = &dma->event_ring_virt[i];
        __asm__ volatile("" ::: "memory");
        /* If cycle bit is set, clear it and acknowledge ERDP */
        if (event->control & XHCI_TRB_C) {
            uint64_t next_erdp = dma->event_ring_phys + ((i + 1) % XHCI_RING_TRB_COUNT) * sizeof(xhci_trb_t);
            io->write32(io->mmio_ctx, intr0 + 0x18, ((uint32_t)next_erdp & ~0xfu) | (1u << 3));
            io->write32(io->mmio_ctx, intr0 + 0x1c, (uint32_t)(next_erdp >> 32));
        } else {
            break;
        }
    }
}

static bool reset_usb2_ports(const xhci_rings_io_t *io, uint32_t op,
                             xhci_port_report_t *report) {
    uint64_t power_changed = 0, pending = 0, completed = 0;
    uint64_t reset_start[XHCI_MAX_ROOT_PORTS];
    uint64_t recovery_start[XHCI_MAX_ROOT_PORTS];

    /* Power all attached USB 2.0 ports before the shared power-good wait. */
    uint64_t power_start = BOOT_TIMING_READ();
    for (uint32_t i = 0; i < report->total_ports; ++i) {
        xhci_port_info_t *info = &report->ports[i];
        if (!info->connected || info->protocol_major != 2) continue;
        uint32_t off = op + 0x400 + i * 0x10;
        uint32_t raw = io->read32(io->mmio_ctx, off);
        info->enabled = false;
        if (raw == UINT32_MAX || !(raw & XHCI_PORTSC_CCS)) continue;
        pending |= 1ULL << i;
        if (!(raw & XHCI_PORTSC_PP)) {
            io->write32(io->mmio_ctx, off, (raw & XHCI_PORTSC_WRITE_MASK) | XHCI_PORTSC_PP);
            power_changed |= 1ULL << i;
        }
    }
    if (power_changed) {
        for (unsigned ms = 0; ms < 20; ++ms)
            if (!io->delay_ms(io->mmio_ctx)) return false;
    }
    uint64_t power_ticks = elapsed_ticks(power_start);

    /* Issue every reset before polling any of them. */
    for (uint32_t i = 0; i < report->total_ports; ++i) {
        uint64_t bit = 1ULL << i;
        if (!(pending & bit)) continue;
        uint32_t off = op + 0x400 + i * 0x10;
        uint32_t raw = io->read32(io->mmio_ctx, off);
        if (power_changed & bit) report->ports[i].power_ticks = power_ticks;
        if (raw == UINT32_MAX || !(raw & XHCI_PORTSC_CCS)) {
            pending &= ~bit;
            continue;
        }
        reset_start[i] = BOOT_TIMING_READ();
        io->write32(io->mmio_ctx, off, (raw & XHCI_PORTSC_WRITE_MASK) | XHCI_PORTSC_PR);
    }

    for (unsigned ms = 0; pending && ms <= 100; ++ms) {
        for (uint32_t i = 0; i < report->total_ports; ++i) {
            uint64_t bit = 1ULL << i;
            if (!(pending & bit)) continue;
            uint32_t raw = io->read32(io->mmio_ctx, op + 0x400 + i * 0x10);
            if (raw != UINT32_MAX && !(raw & XHCI_PORTSC_PR)) {
                report->ports[i].reset_ticks = elapsed_ticks(reset_start[i]);
                recovery_start[i] = BOOT_TIMING_READ();
                pending &= ~bit;
                completed |= bit;
            }
        }
        if (pending && !io->delay_ms(io->mmio_ctx)) return false;
    }
    for (uint32_t i = 0; i < report->total_ports; ++i) {
        if (pending & (1ULL << i))
            report->ports[i].reset_ticks = elapsed_ticks(reset_start[i]);
    }

    /* Give even the last completed port its full 20 ms recovery. */
    if (completed) {
        for (unsigned ms = 0; ms < 20; ++ms)
            if (!io->delay_ms(io->mmio_ctx)) return false;
    }
    for (uint32_t i = 0; i < report->total_ports; ++i) {
        if (!(completed & (1ULL << i))) continue;
        xhci_port_info_t *info = &report->ports[i];
        info->recovery_ticks = elapsed_ticks(recovery_start[i]);
        uint32_t off = op + 0x400 + i * 0x10;
        uint32_t raw = io->read32(io->mmio_ctx, off);
        if (raw == UINT32_MAX || !(raw & XHCI_PORTSC_CCS) || (raw & XHCI_PORTSC_PR))
            continue;
        io->write32(io->mmio_ctx, off,
                    (raw & XHCI_PORTSC_WRITE_MASK) | XHCI_PORTSC_PRC | XHCI_PORTSC_CSC);
        raw = io->read32(io->mmio_ctx, off);
        if (raw == UINT32_MAX || !(raw & XHCI_PORTSC_CCS)) continue;
        info->raw_portsc = raw;
        info->enabled = (raw & XHCI_PORTSC_PED) != 0;
        info->speed = (uint8_t)((raw & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT);
    }
    return true;
}

bool xhci_discover_and_reset_ports(const xhci_rings_io_t *io,
                                   const xhci_dma_buffers_t *dma,
                                   xhci_port_report_t *report,unsigned settled_ms,bool trace_attachment) {
    if (!report) return false;
    memset(report, 0, sizeof(*report));

    if (!fits(io, 0, 0x20)) return false;
    uint32_t cap = io->read32(io->mmio_ctx, 0);
    uint32_t op = cap & 0xff;
    if (cap == UINT32_MAX || op < 0x20 || !fits(io, op, 0x400)) return false;

    uint32_t hcs1 = io->read32(io->mmio_ctx, 4);
    uint32_t hcc1 = io->read32(io->mmio_ctx, 0x10);
    if (hcs1 == UINT32_MAX || hcc1 == UINT32_MAX) return false;

    uint32_t max_ports = (hcs1 >> 24) & 0xff;
    if (!max_ports) return false;
    if (max_ports > XHCI_MAX_ROOT_PORTS) max_ports = XHCI_MAX_ROOT_PORTS;
    report->total_ports = max_ports;

    /* 1. Walk Extended Capabilities to determine Supported Protocols */
    uint32_t ext = (hcc1 >> 16) * 4;
    unsigned loop_guard = 0;
    while (ext) {
        if (++loop_guard > 256 || ext < 0x20 || !fits(io, ext, 16)) break;
        uint32_t cap_hdr = io->read32(io->mmio_ctx, ext);
        if (cap_hdr == UINT32_MAX || !(cap_hdr & 0xff)) break;

        uint32_t cap_id = cap_hdr & 0xff;
        uint32_t next = ((cap_hdr >> 8) & 0xff) * 4;

        if (cap_id == 2) { /* Supported Protocol Capability */
            uint32_t name = io->read32(io->mmio_ctx, ext + 4);
            if (name == 0x20425355) { /* "USB " */
                uint32_t major = (cap_hdr >> 24) & 0xff;
                uint32_t port_info = io->read32(io->mmio_ctx, ext + 8);
                uint32_t port_offset = port_info & 0xff;
                uint32_t port_count = (port_info >> 8) & 0xff;

                for (uint32_t p = port_offset; p < port_offset + port_count && p <= max_ports; ++p) {
                    if (p > 0) {
                        report->ports[p - 1].protocol_major = (uint8_t)major;
                        if (major == 2) report->usb2_port_count++;
                        else if (major == 3) report->usb3_port_count++;
                    }
                }
            }
        }
        ext = next ? ext + next : 0;
    }

    /* HCRST starts ports disconnected. Allow attached devices time to signal
     * again before the one-shot boot scan. This is bounded PIT polling, not
     * runtime hot-plug; no DMA/event-ring ownership is changed here. */
    uint64_t settle_start = BOOT_TIMING_READ();
    for (unsigned ms = settled_ms;; ++ms) {
        if (trace_attachment && (ms == settled_ms || ms % 10 == 0 || ms >= 1000)) {
            /* Sample only. The final scan still decides which ports to reset. */
            uint64_t sample_ticks = elapsed_ticks(settle_start);
            for (uint32_t i = 0; i < max_ports; ++i) {
                uint32_t off = op + 0x400 + i * 0x10;
                if (!fits(io, off, 4)) break;
                xhci_port_info_t *info = &report->ports[i];
                if (info->attachment_seen) continue;
                uint32_t raw = io->read32(io->mmio_ctx, off);
                if (raw != UINT32_MAX && (raw & XHCI_PORTSC_CCS)) {
                    info->attachment_seen = true;
                    info->attachment_ticks = sample_ticks;
                }
            }
        }
        if (ms >= 1000) break;
        if (!io->delay_ms(io->mmio_ctx)) return false;
    }
    report->settle_ticks = elapsed_ticks(settle_start);

    /* 2. Inspect each root port */
    for (uint32_t p = 1; p <= max_ports; ++p) {
        uint32_t portsc_off = op + 0x400 + (p - 1) * 0x10;
        if (!fits(io, portsc_off, 4)) break;

        uint32_t raw = io->read32(io->mmio_ctx, portsc_off);
        if (raw == UINT32_MAX) continue;

        xhci_port_info_t *info = &report->ports[p - 1];
        info->port_num = (uint8_t)p;
        info->raw_portsc = raw;
        info->connected = (raw & XHCI_PORTSC_CCS) != 0;
        info->enabled = (raw & XHCI_PORTSC_PED) != 0;
        info->speed = (uint8_t)((raw & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT);

        if (!info->connected) continue;
        report->connected_count++;

        if (info->protocol_major == 3) {
            /* USB 3.0 attachment: ensure power, issue port reset, then wait
             * for the link to reach U0 (operational). SuperSpeed ports report
             * operational state via PLS rather than PED alone. */
            if (!(raw & XHCI_PORTSC_PP)) {
                uint64_t power_start = BOOT_TIMING_READ();
                io->write32(io->mmio_ctx, portsc_off,
                            (raw & XHCI_PORTSC_WRITE_MASK) | XHCI_PORTSC_PP);
                for (int i = 0; i < 20; ++i) io->delay_ms(io->mmio_ctx);
                info->power_ticks = elapsed_ticks(power_start);
                raw = io->read32(io->mmio_ctx, portsc_off);
            }

            uint64_t reset_start = BOOT_TIMING_READ();
            io->write32(io->mmio_ctx, portsc_off,
                        (raw & XHCI_PORTSC_WRITE_MASK) | XHCI_PORTSC_PR);

            bool reset_done = false;
            for (unsigned ms = 0; ms <= 100; ++ms) {
                raw = io->read32(io->mmio_ctx, portsc_off);
                if (raw != UINT32_MAX && !(raw & XHCI_PORTSC_PR)) {
                    reset_done = true;
                    break;
                }
                io->delay_ms(io->mmio_ctx);
            }

            info->reset_ticks = elapsed_ticks(reset_start);
            if (reset_done) {
                uint64_t recovery_start = BOOT_TIMING_READ();
                /* Wait for PLS to reach U0. PLS = bits [8:5], U0 = 0. */
                for (unsigned ms = 0; ms <= 100; ++ms) {
                    raw = io->read32(io->mmio_ctx, portsc_off);
                    if (raw != UINT32_MAX && ((raw >> 5) & 0x7) == 0) break;
                    io->delay_ms(io->mmio_ctx);
                }

                info->recovery_ticks = elapsed_ticks(recovery_start);
                raw = io->read32(io->mmio_ctx, portsc_off);
                info->raw_portsc = raw;
                info->enabled = (raw & XHCI_PORTSC_PED) != 0;
                info->speed = (uint8_t)((raw & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT);

                io->write32(io->mmio_ctx, portsc_off,
                            (raw & XHCI_PORTSC_WRITE_MASK) | XHCI_PORTSC_PRC | XHCI_PORTSC_CSC);
            }
        }
    }

    if (!reset_usb2_ports(io, op, report)) return false;

    /* 3. Drain and acknowledge any port status events generated by resets */
    drain_event_ring(io, dma);

    return true;
}
