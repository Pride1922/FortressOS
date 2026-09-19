#include "xhci_rings.h"

#define RUN       (1u << 0)
#define HALTED    (1u << 0)
#define HSE       (1u << 2)
#define CNR       (1u << 11)

static bool fits(const xhci_rings_io_t *io, uint32_t off, uint32_t len) {
    return !(off & 3) && off <= io->mmio_size && len <= io->mmio_size - off;
}

static bool fail(xhci_dump_record_t *d, const char *msg) {
    if (d) d->error_msg = msg;
    return false;
}

static bool poll_reg(const xhci_rings_io_t *io, uint32_t off, uint32_t mask,
                     uint32_t wanted, unsigned ms) {
    for (unsigned i = 0; i <= ms; ++i) {
        uint32_t val = io->read32(io->mmio_ctx, off);
        if (val == UINT32_MAX) return false;
        if ((val & mask) == wanted) return true;
        if (i == ms) return false;
        if (!io->delay_ms(io->mmio_ctx)) return false;
    }
    return false;
}

bool xhci_verify_rings(const xhci_rings_io_t *io,
                       xhci_dma_buffers_t *dma,
                       xhci_dump_record_t *dump) {
    if (dump) {
        *dump = (xhci_dump_record_t){0};
        dump->valid = true;
    }

    if (!fits(io, 0, 0x20)) return fail(dump, "capability aperture too short");
    uint32_t cap = io->read32(io->mmio_ctx, 0);
    uint32_t op = cap & 0xff;
    if (cap == UINT32_MAX || op < 0x20 || !fits(io, op, 0x30))
        return fail(dump, "invalid operational register offset");

    uint32_t dboff = io->read32(io->mmio_ctx, 0x14);
    uint32_t rtsoff = io->read32(io->mmio_ctx, 0x18);
    if ((dboff & 3) || !fits(io, dboff, 4))
        return fail(dump, "invalid doorbell offset");
    if ((rtsoff & 31) || !fits(io, rtsoff, 0x40))
        return fail(dump, "invalid runtime register offset");

    /* Controller must currently be halted before ring configuration */
    uint32_t usbsts = io->read32(io->mmio_ctx, op + 0x04);
    if (usbsts == UINT32_MAX || !(usbsts & HALTED))
        return fail(dump, "controller not halted prior to ring setup");

    /* Validate alignment of physical DMA buffers (64-byte minimum) */
    if ((dma->cmd_ring_phys & 0x3f) || (dma->event_ring_phys & 0x3f) || (dma->erst_phys & 0x3f))
        return fail(dump, "unaligned DMA buffer physical address");

    /* Initialize Command Ring: 255 command TRBs + 1 Link TRB at end */
    for (uint32_t i = 0; i < XHCI_RING_TRB_COUNT; ++i) {
        dma->cmd_ring_virt[i] = (xhci_trb_t){0};
    }
    dma->cmd_ring_virt[XHCI_RING_TRB_COUNT - 1].parameter_low = (uint32_t)dma->cmd_ring_phys;
    dma->cmd_ring_virt[XHCI_RING_TRB_COUNT - 1].parameter_high = (uint32_t)(dma->cmd_ring_phys >> 32);
    dma->cmd_ring_virt[XHCI_RING_TRB_COUNT - 1].status = 0;
    dma->cmd_ring_virt[XHCI_RING_TRB_COUNT - 1].control =
        (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_TC | XHCI_TRB_C;

    /* Initialize Event Ring (all zeroed) */
    for (uint32_t i = 0; i < XHCI_RING_TRB_COUNT; ++i) {
        dma->event_ring_virt[i] = (xhci_trb_t){0};
    }

    /* Initialize ERST: 1 segment pointing to Event Ring */
    dma->erst_virt[0].ring_segment_base_address = dma->event_ring_phys;
    dma->erst_virt[0].ring_segment_size = XHCI_RING_TRB_COUNT;
    dma->erst_virt[0].reserved = 0;

    /* Program Primary Interrupter (Interrupter 0) */
    uint32_t intr0 = rtsoff + 0x20;
    io->write32(io->mmio_ctx, intr0 + 0x00, 1u); /* Clear IP, IE=0 (polled) */
    io->write32(io->mmio_ctx, intr0 + 0x04, 0u); /* IMOD = 0 */
    io->write32(io->mmio_ctx, intr0 + 0x08, 1u); /* ERSTSZ = 1 */
    io->write32(io->mmio_ctx, intr0 + 0x10, (uint32_t)dma->erst_phys);
    io->write32(io->mmio_ctx, intr0 + 0x14, (uint32_t)(dma->erst_phys >> 32));
    io->write32(io->mmio_ctx, intr0 + 0x18, ((uint32_t)dma->event_ring_phys & ~0xfu) | (1u << 3));
    io->write32(io->mmio_ctx, intr0 + 0x1c, (uint32_t)(dma->event_ring_phys >> 32));

    /* Program Command Ring Control Register (CRCR) */
    io->write32(io->mmio_ctx, op + 0x18, ((uint32_t)dma->cmd_ring_phys & ~0x3fu) | 1u); /* RCS=1 */
    io->write32(io->mmio_ctx, op + 0x1c, (uint32_t)(dma->cmd_ring_phys >> 32));

    /* Start Controller: set USBCMD.RS = 1 */
    uint32_t usbcmd = io->read32(io->mmio_ctx, op + 0x00);
    io->write32(io->mmio_ctx, op + 0x00, usbcmd | RUN);

    if (!poll_reg(io, op + 0x04, HALTED, 0, 100)) {
        return fail(dump, "controller failed to start (HCH stuck at 1)");
    }

    /* Prepare No-Op Command TRB at index 0 */
    dma->cmd_ring_virt[0].parameter_low = 0;
    dma->cmd_ring_virt[0].parameter_high = 0;
    dma->cmd_ring_virt[0].status = 0;
    dma->cmd_ring_virt[0].control = (XHCI_TRB_TYPE_NOOP_CMD << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_C;
    __asm__ volatile("" ::: "memory");

    if (dump) {
        dump->last_submitted_trb = dma->cmd_ring_virt[0];
        dump->cmd_enqueue_idx = 1;
        dump->cmd_cycle_state = 1;
    }

    /* Ring Doorbell 0 for Host Controller Command */
    io->write32(io->mmio_ctx, dboff + 0, 0);

    /* Poll Event Ring for Command Completion Event */
    bool completed = false;
    uint32_t event_idx = 0;
    uint32_t event_cycle = 1;

    for (unsigned ms = 0; ms <= 500; ++ms) {
        while (1) {
            volatile const xhci_trb_t *event = &dma->event_ring_virt[event_idx];
            __asm__ volatile("" ::: "memory");
            uint32_t c_bit = event->control & XHCI_TRB_C;
            if (c_bit != (event_cycle ? XHCI_TRB_C : 0)) {
                break; /* No more events currently pending */
            }

            /* Found an event posted by hardware */
            uint32_t trb_type = (event->control & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT;
            uint32_t comp_code = (event->status >> 24) & 0xff;
            uint64_t cmd_ptr = ((uint64_t)event->parameter_high << 32) | event->parameter_low;

            if (dump) {
                dump->last_completed_trb = *(const xhci_trb_t *)event;
                dump->event_dequeue_idx = event_idx;
                dump->event_cycle_state = event_cycle;
                dump->completion_code = comp_code;
            }

            /* Advance dequeue pointer and acknowledge to controller via ERDP */
            event_idx = (event_idx + 1) % XHCI_RING_TRB_COUNT;
            if (event_idx == 0) event_cycle ^= 1;
            uint64_t new_erdp = dma->event_ring_phys + (event_idx * sizeof(xhci_trb_t));
            io->write32(io->mmio_ctx, intr0 + 0x18, ((uint32_t)new_erdp & ~0xfu) | (1u << 3));
            io->write32(io->mmio_ctx, intr0 + 0x1c, (uint32_t)(new_erdp >> 32));

            if (trb_type == XHCI_TRB_TYPE_CMD_COMPLETION_EVENT) {
                if (cmd_ptr == dma->cmd_ring_phys && comp_code == XHCI_COMP_SUCCESS) {
                    completed = true;
                } else {
                    if (dump) dump->error_msg = "mismatched or failed completion event";
                }
                goto poll_done;
            } else if (trb_type == XHCI_TRB_TYPE_PORT_STATUS_EVENT) {
                /* Acknowledge and consume port status change events so they do not clog the ring */
                continue;
            } else {
                /* Other events - consume and continue */
                continue;
            }
        }

        if (ms == 500) {
            fail(dump, "command completion timeout");
            break;
        }
        if (!io->delay_ms(io->mmio_ctx)) {
            fail(dump, "PIT wait failed during poll");
            break;
        }
    }
poll_done:
    if (!completed) {
        /* Stop Controller on failure */
        usbcmd = io->read32(io->mmio_ctx, op + 0x00);
        io->write32(io->mmio_ctx, op + 0x00, usbcmd & ~RUN);
        poll_reg(io, op + 0x04, HALTED, HALTED, 100);
    } else {
        dma->cmd_enqueue_idx = 1;
        dma->cmd_cycle = 1;
        dma->event_dequeue_idx = event_idx;
        dma->event_cycle = event_cycle;
    }

    /* Capture final register state */
    if (dump) {
        dump->usbcmd = io->read32(io->mmio_ctx, op + 0x00);
        dump->usbsts = io->read32(io->mmio_ctx, op + 0x04);
        dump->pagesize = io->read32(io->mmio_ctx, op + 0x08);
    }

    return completed;
}
