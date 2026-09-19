#include "xhci_bot.h"
#include "string.h"
#include "serial.h"

static inline void xhci_clflush_range(const void *p, size_t len) {
#if defined(__x86_64__) || defined(_M_X64)
    uintptr_t start = (uintptr_t)p;
    uintptr_t end = start + len;
    start &= ~63ULL;
    for (; start < end; start += 64) {
        __asm__ volatile("clflush (%0)" :: "r"(start) : "memory");
    }
#else
    (void)p; (void)len;
#endif
}

static inline uint32_t bswap32(uint32_t val) {
    return ((val >> 24) & 0x000000ffu) |
           ((val >> 8)  & 0x0000ff00u) |
           ((val << 8)  & 0x00ff0000u) |
           ((val << 24) & 0xff000000u);
}

/* Helper to send command TRB to Command Ring and wait for completion */
static bool send_command(const xhci_rings_io_t *io,
                         xhci_dma_buffers_t *ring_dma,
                         xhci_trb_t cmd_trb,
                         xhci_trb_t *out_comp_event) {
    uint32_t dboff = io->read32(io->mmio_ctx, 0x14);
    uint32_t rtsoff = io->read32(io->mmio_ctx, 0x18);
    uint32_t intr0 = rtsoff + 0x20;

    uint32_t enq_idx = ring_dma->cmd_enqueue_idx;
    cmd_trb.control &= ~XHCI_TRB_C;
    if (ring_dma->cmd_cycle) cmd_trb.control |= XHCI_TRB_C;
    ring_dma->cmd_ring_virt[enq_idx] = cmd_trb;
    xhci_clflush_range(&ring_dma->cmd_ring_virt[enq_idx], sizeof(xhci_trb_t));
    __asm__ volatile("mfence" ::: "memory");

    ring_dma->cmd_enqueue_idx++;
    if (ring_dma->cmd_enqueue_idx == XHCI_RING_TRB_COUNT - 1) {
        ring_dma->cmd_ring_virt[XHCI_RING_TRB_COUNT - 1].control ^= XHCI_TRB_C;
        xhci_clflush_range(&ring_dma->cmd_ring_virt[XHCI_RING_TRB_COUNT - 1], sizeof(xhci_trb_t));
        ring_dma->cmd_enqueue_idx = 0;
        ring_dma->cmd_cycle ^= 1;
    }

    io->write32(io->mmio_ctx, dboff + 0, 0);

    for (unsigned ms = 0; ms <= 500; ++ms) {
        while (1) {
            volatile const xhci_trb_t *event = &ring_dma->event_ring_virt[ring_dma->event_dequeue_idx];
            __asm__ volatile("" ::: "memory");
            uint32_t c_bit = event->control & XHCI_TRB_C;
            if (c_bit != (ring_dma->event_cycle ? XHCI_TRB_C : 0)) break;

            uint32_t trb_type = (event->control & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT;
            uint32_t comp_code = (event->status >> 24) & 0xff;

            ring_dma->event_dequeue_idx = (ring_dma->event_dequeue_idx + 1) % XHCI_RING_TRB_COUNT;
            if (ring_dma->event_dequeue_idx == 0) ring_dma->event_cycle ^= 1;
            uint64_t new_erdp = ring_dma->event_ring_phys + (ring_dma->event_dequeue_idx * sizeof(xhci_trb_t));
            io->write32(io->mmio_ctx, intr0 + 0x18, ((uint32_t)new_erdp & ~0xfu) | (1u << 3));
            io->write32(io->mmio_ctx, intr0 + 0x1c, (uint32_t)(new_erdp >> 32));

            if (trb_type == XHCI_TRB_TYPE_CMD_COMPLETION_EVENT) {
                if (out_comp_event) *out_comp_event = *event;
                return (comp_code == XHCI_COMP_SUCCESS);
            }
        }
        io->delay_ms(io->mmio_ctx);
    }
    return false;
}

/* Helper to wait for a Transfer Event on a specific endpoint */
static bool wait_transfer_event(const xhci_rings_io_t *io,
                                xhci_dma_buffers_t *ring_dma,
                                uint8_t slot_id,
                                uint8_t dci,
                                uint32_t *out_residual) {
    uint32_t rtsoff = io->read32(io->mmio_ctx, 0x18);
    uint32_t intr0 = rtsoff + 0x20;

    for (unsigned ms = 0; ms <= 1000; ++ms) {
        while (1) {
            volatile const xhci_trb_t *event = &ring_dma->event_ring_virt[ring_dma->event_dequeue_idx];
            __asm__ volatile("" ::: "memory");
            uint32_t c_bit = event->control & XHCI_TRB_C;
            if (c_bit != (ring_dma->event_cycle ? XHCI_TRB_C : 0)) break;

            uint32_t trb_type = (event->control & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT;
            uint32_t comp_code = (event->status >> 24) & 0xff;
            uint8_t ev_slot = (event->control >> 24) & 0xff;
            uint8_t ev_dci  = (event->control >> 16) & 0x1f;

            ring_dma->event_dequeue_idx = (ring_dma->event_dequeue_idx + 1) % XHCI_RING_TRB_COUNT;
            if (ring_dma->event_dequeue_idx == 0) ring_dma->event_cycle ^= 1;
            uint64_t new_erdp = ring_dma->event_ring_phys + (ring_dma->event_dequeue_idx * sizeof(xhci_trb_t));
            io->write32(io->mmio_ctx, intr0 + 0x18, ((uint32_t)new_erdp & ~0xfu) | (1u << 3));
            io->write32(io->mmio_ctx, intr0 + 0x1c, (uint32_t)(new_erdp >> 32));

            if (trb_type == XHCI_TRB_TYPE_TRANSFER_EVENT && ev_slot == slot_id && ev_dci == dci) {
                if (out_residual) *out_residual = event->status & 0xffffff;
                return (comp_code == XHCI_COMP_SUCCESS || comp_code == XHCI_COMP_SHORT_PACKET);
            }
        }
        io->delay_ms(io->mmio_ctx);
    }
    return false;
}

/* Submit a Normal TRB on an endpoint ring and ring its doorbell */
static bool submit_normal_trb(const xhci_rings_io_t *io,
                              xhci_dma_buffers_t *ring_dma,
                              uint8_t slot_id,
                              uint8_t dci,
                              xhci_trb_t *ring_virt,
                              uint32_t *ring_idx,
                              uint8_t *ring_cycle,
                              uintptr_t buf_phys,
                              uint32_t length,
                              uint32_t *out_residual) {
    uint32_t dboff = io->read32(io->mmio_ctx, 0x14);
    uint32_t idx = *ring_idx;

    xhci_trb_t trb = {0};
    trb.parameter_low = (uint32_t)buf_phys;
    trb.parameter_high = (uint32_t)(buf_phys >> 32);
    trb.status = length;
    trb.control = (XHCI_TRB_TYPE_NORMAL << 10) | XHCI_TRB_IOC | (*ring_cycle ? XHCI_TRB_C : 0);

    ring_virt[idx] = trb;
    xhci_clflush_range(&ring_virt[idx], sizeof(xhci_trb_t));
    __asm__ volatile("mfence" ::: "memory");

    idx++;
    if (idx == XHCI_RING_TRB_COUNT - 1) {
        ring_virt[XHCI_RING_TRB_COUNT - 1].control ^= XHCI_TRB_C;
        xhci_clflush_range(&ring_virt[XHCI_RING_TRB_COUNT - 1], sizeof(xhci_trb_t));
        idx = 0;
        *ring_cycle ^= 1;
    }
    *ring_idx = idx;

    /* Ring Doorbell for this endpoint: Target = DCI */
    io->write32(io->mmio_ctx, dboff + slot_id * 4, dci);

    return wait_transfer_event(io, ring_dma, slot_id, dci, out_residual);
}

bool xhci_configure_bulk_endpoints(const xhci_rings_io_t *io,
                                   xhci_dma_buffers_t *ring_dma,
                                   const xhci_dev_dma_t *dev_dma,
                                   const xhci_bot_device_t *device,
                                   xhci_bot_rings_t *bot_rings) {
    if (!io || !ring_dma || !dev_dma || !device || !bot_rings) return false;

    uint32_t hcc1 = io->read32(io->mmio_ctx, 0x10);
    uint32_t ctx_shift = (hcc1 & (1 << 2)) ? 6 : 5; /* 64-byte vs 32-byte contexts */
    uint32_t ctx_dwords = 1u << (ctx_shift - 2);

    /* Calculate DCIs:
     * DCI = 2 * ep_num + (dir_in ? 1 : 0) */
    uint8_t in_ep_num = device->bulk_in_ep & 0x0f;
    uint8_t out_ep_num = device->bulk_out_ep & 0x0f;
    uint8_t in_dci = (in_ep_num * 2) + 1;
    uint8_t out_dci = (out_ep_num * 2);

    bot_rings->slot_id = device->slot_id;
    bot_rings->in_dci = in_dci;
    bot_rings->out_dci = out_dci;
    bot_rings->in_max_packet = device->bulk_in_max_packet ? device->bulk_in_max_packet : 512;
    bot_rings->out_max_packet = device->bulk_out_max_packet ? device->bulk_out_max_packet : 512;
    bot_rings->in_cycle = 1;
    bot_rings->out_cycle = 1;
    bot_rings->in_idx = 0;
    bot_rings->out_idx = 0;
    bot_rings->tag = 0x1000;

    /* Initialize Link TRBs at the end of both bulk rings */
    memset(bot_rings->bulk_in_ring_virt, 0, 4096);
    bot_rings->bulk_in_ring_virt[XHCI_RING_TRB_COUNT - 1].parameter_low = (uint32_t)bot_rings->bulk_in_ring_phys;
    bot_rings->bulk_in_ring_virt[XHCI_RING_TRB_COUNT - 1].parameter_high = (uint32_t)(bot_rings->bulk_in_ring_phys >> 32);
    bot_rings->bulk_in_ring_virt[XHCI_RING_TRB_COUNT - 1].control =
        (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_TC | XHCI_TRB_C;

    memset(bot_rings->bulk_out_ring_virt, 0, 4096);
    bot_rings->bulk_out_ring_virt[XHCI_RING_TRB_COUNT - 1].parameter_low = (uint32_t)bot_rings->bulk_out_ring_phys;
    bot_rings->bulk_out_ring_virt[XHCI_RING_TRB_COUNT - 1].parameter_high = (uint32_t)(bot_rings->bulk_out_ring_phys >> 32);
    bot_rings->bulk_out_ring_virt[XHCI_RING_TRB_COUNT - 1].control =
        (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_TC | XHCI_TRB_C;

    /* Build Input Context for Configure Endpoint */
    memset(dev_dma->input_ctx_virt, 0, 4096);
    uint32_t *ctrl_ctx = &dev_dma->input_ctx_virt[0];
    uint32_t *slot_ctx = &dev_dma->input_ctx_virt[1 * ctx_dwords];
    uint32_t *in_ep_ctx = &dev_dma->input_ctx_virt[(in_dci + 1) * ctx_dwords];
    uint32_t *out_ep_ctx = &dev_dma->input_ctx_virt[(out_dci + 1) * ctx_dwords];

    /* Add Context Flags: A0 (Slot Context) + Bulk IN DCI + Bulk OUT DCI */
    ctrl_ctx[1] = (1u << 0) | (1u << in_dci) | (1u << out_dci);

    /* Slot Context: Context Entries = max(in_dci, out_dci) */
    uint8_t max_dci = (in_dci > out_dci) ? in_dci : out_dci;
    slot_ctx[0] = ((uint32_t)device->speed << 20) | ((uint32_t)max_dci << 27);
    slot_ctx[1] = (uint32_t)device->port_num << 16;

    /* Endpoint Context: Bulk IN (Type 6) */
    in_ep_ctx[1] = (3u << 1) /* CErr=3 */ | (6u << 3) /* Bulk IN */ | ((uint32_t)bot_rings->in_max_packet << 16);
    in_ep_ctx[2] = ((uint32_t)bot_rings->bulk_in_ring_phys & ~0x3fu) | 1u; /* DCS=1 */
    in_ep_ctx[3] = (uint32_t)(bot_rings->bulk_in_ring_phys >> 32);
    in_ep_ctx[4] = 512; /* Average TRB length */

    /* Endpoint Context: Bulk OUT (Type 2) */
    out_ep_ctx[1] = (3u << 1) /* CErr=3 */ | (2u << 3) /* Bulk OUT */ | ((uint32_t)bot_rings->out_max_packet << 16);
    out_ep_ctx[2] = ((uint32_t)bot_rings->bulk_out_ring_phys & ~0x3fu) | 1u; /* DCS=1 */
    out_ep_ctx[3] = (uint32_t)(bot_rings->bulk_out_ring_phys >> 32);
    out_ep_ctx[4] = 512; /* Average TRB length */

    xhci_clflush_range(dev_dma->input_ctx_virt, 4096);
    __asm__ volatile("mfence" ::: "memory");

    /* Issue Configure Endpoint Command */
    xhci_trb_t cfg_cmd = {0};
    cfg_cmd.parameter_low = (uint32_t)dev_dma->input_ctx_phys;
    cfg_cmd.parameter_high = (uint32_t)(dev_dma->input_ctx_phys >> 32);
    cfg_cmd.control = (XHCI_TRB_TYPE_CONFIG_EP_CMD << 10) | ((uint32_t)device->slot_id << 24);

    xhci_trb_t comp_event = {0};
    return send_command(io, ring_dma, cfg_cmd, &comp_event);
}

bool xhci_bot_transfer(const xhci_rings_io_t *io,
                       xhci_dma_buffers_t *ring_dma,
                       const xhci_dev_dma_t *dev_dma,
                       xhci_bot_rings_t *bot_rings,
                       const void *cdb,
                       uint8_t cdb_len,
                       void *data,
                       uint32_t data_len,
                       bool dir_in) {
    if (!io || !ring_dma || !dev_dma || !bot_rings || !cdb || cdb_len == 0 || cdb_len > 16) return false;

    uint32_t cur_tag = ++bot_rings->tag;

    /* 1. Build and send CBW (31 bytes) at offset 0 of bounce buffer */
    usb_bot_cbw_t cbw = {0};
    cbw.dCBWSignature = USB_BOT_CBW_SIGNATURE;
    cbw.dCBWTag = cur_tag;
    cbw.dCBWDataTransferLength = data_len;
    cbw.bmCBWFlags = (data_len > 0 && dir_in) ? USB_BOT_CBW_FLAG_IN : USB_BOT_CBW_FLAG_OUT;
    cbw.bCBWLUN = 0;
    cbw.bCBWCBLength = cdb_len;
    memcpy(cbw.CBWCB, cdb, cdb_len);

    memcpy(dev_dma->bounce_buf_virt, &cbw, sizeof(cbw));
    xhci_clflush_range(dev_dma->bounce_buf_virt, sizeof(cbw));

    uint32_t resid = 0;
    if (!submit_normal_trb(io, ring_dma, bot_rings->slot_id, bot_rings->out_dci,
                           bot_rings->bulk_out_ring_virt, &bot_rings->out_idx, &bot_rings->out_cycle,
                           dev_dma->bounce_buf_phys, sizeof(cbw), &resid)) {
        return false;
    }

    /* 2. Data Stage (offset 128 of bounce buffer) if data_len > 0 */
    if (data_len > 0) {
        uintptr_t data_phys = dev_dma->bounce_buf_phys + 128;
        uint8_t *data_virt = dev_dma->bounce_buf_virt + 128;

        if (!dir_in && data) {
            memcpy(data_virt, data, data_len);
            xhci_clflush_range(data_virt, data_len);
        } else if (dir_in) {
            memset(data_virt, 0, data_len);
            xhci_clflush_range(data_virt, data_len);
        }

        uint8_t dci = dir_in ? bot_rings->in_dci : bot_rings->out_dci;
        xhci_trb_t *ring_virt = dir_in ? bot_rings->bulk_in_ring_virt : bot_rings->bulk_out_ring_virt;
        uint32_t *ring_idx = dir_in ? &bot_rings->in_idx : &bot_rings->out_idx;
        uint8_t *ring_cycle = dir_in ? &bot_rings->in_cycle : &bot_rings->out_cycle;

        if (!submit_normal_trb(io, ring_dma, bot_rings->slot_id, dci,
                               ring_virt, ring_idx, ring_cycle,
                               data_phys, data_len, &resid)) {
            return false;
        }

        if (dir_in && data) {
            memcpy(data, data_virt, data_len);
        }
    }

    /* 3. Read CSW (13 bytes) at offset 64 of bounce buffer via Bulk IN */
    uintptr_t csw_phys = dev_dma->bounce_buf_phys + 64;
    uint8_t *csw_virt = dev_dma->bounce_buf_virt + 64;
    memset(csw_virt, 0, sizeof(usb_bot_csw_t));
    xhci_clflush_range(csw_virt, sizeof(usb_bot_csw_t));

    if (!submit_normal_trb(io, ring_dma, bot_rings->slot_id, bot_rings->in_dci,
                           bot_rings->bulk_in_ring_virt, &bot_rings->in_idx, &bot_rings->in_cycle,
                           csw_phys, sizeof(usb_bot_csw_t), &resid)) {
        return false;
    }

    usb_bot_csw_t csw;
    memcpy(&csw, csw_virt, sizeof(csw));

    if (csw.dCSWSignature != USB_BOT_CSW_SIGNATURE || csw.dCSWTag != cur_tag) {
        return false;
    }

    return (csw.bCSWStatus == USB_BOT_CSW_STATUS_PASSED);
}

bool xhci_scsi_inquiry(const xhci_rings_io_t *io,
                       xhci_dma_buffers_t *ring_dma,
                       const xhci_dev_dma_t *dev_dma,
                       xhci_bot_rings_t *bot_rings,
                       scsi_inquiry_data_t *inq) {
    uint8_t cdb[6] = {SCSI_CMD_INQUIRY, 0, 0, 0, 36, 0};
    scsi_inquiry_data_t local_inq = {0};

    if (!xhci_bot_transfer(io, ring_dma, dev_dma, bot_rings, cdb, sizeof(cdb), &local_inq, 36, true)) {
        return false;
    }

    /* Cleanly copy vendor & product strings */
    memcpy(bot_rings->vendor, local_inq.vendor, 8);
    bot_rings->vendor[8] = '\0';
    memcpy(bot_rings->product, local_inq.product, 16);
    bot_rings->product[16] = '\0';

    if (inq) *inq = local_inq;
    return true;
}

bool xhci_scsi_test_unit_ready(const xhci_rings_io_t *io,
                               xhci_dma_buffers_t *ring_dma,
                               const xhci_dev_dma_t *dev_dma,
                               xhci_bot_rings_t *bot_rings) {
    uint8_t cdb[6] = {SCSI_CMD_TEST_UNIT_READY, 0, 0, 0, 0, 0};

    for (int retry = 0; retry < 3; ++retry) {
        if (xhci_bot_transfer(io, ring_dma, dev_dma, bot_rings, cdb, sizeof(cdb), NULL, 0, false)) {
            return true;
        }

        /* If check condition or not ready, issue REQUEST SENSE to clear Unit Attention */
        uint8_t sense_cdb[6] = {SCSI_CMD_REQUEST_SENSE, 0, 0, 0, 18, 0};
        scsi_sense_data_t sense = {0};
        xhci_bot_transfer(io, ring_dma, dev_dma, bot_rings, sense_cdb, sizeof(sense_cdb), &sense, 18, true);

        io->delay_ms(io->mmio_ctx);
    }
    return false;
}

bool xhci_scsi_read_capacity(const xhci_rings_io_t *io,
                             xhci_dma_buffers_t *ring_dma,
                             const xhci_dev_dma_t *dev_dma,
                             xhci_bot_rings_t *bot_rings,
                             uint64_t *out_sectors,
                             uint32_t *out_sector_size) {
    uint8_t cdb[10] = {SCSI_CMD_READ_CAPACITY_10, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    scsi_read_capacity_data_t cap = {0};

    if (!xhci_bot_transfer(io, ring_dma, dev_dma, bot_rings, cdb, sizeof(cdb), &cap, sizeof(cap), true)) {
        return false;
    }

    uint32_t last_lba = bswap32(cap.last_lba_be);
    uint32_t block_size = bswap32(cap.block_size_be);

    /* Overflow sentinel check per AGENTS.md */
    if (last_lba == 0xffffffffu) {
        return false;
    }

    /* Only 512 or 4096-byte sectors are supported per AGENTS.md §2 */
    if (block_size != 512 && block_size != 4096) {
        return false;
    }

    uint64_t sectors = (uint64_t)last_lba + 1;
    bot_rings->sector_count = sectors;
    bot_rings->sector_size = block_size;

    if (out_sectors) *out_sectors = sectors;
    if (out_sector_size) *out_sector_size = block_size;

    return true;
}

bool xhci_scsi_read_sector(const xhci_rings_io_t *io,
                           xhci_dma_buffers_t *ring_dma,
                           const xhci_dev_dma_t *dev_dma,
                           xhci_bot_rings_t *bot_rings,
                           uint64_t lba,
                           void *buf) {
    if (!buf || lba >= bot_rings->sector_count || lba > 0xffffffffu) return false;

    uint32_t lba32 = (uint32_t)lba;
    uint8_t cdb[10] = {
        SCSI_CMD_READ_10,
        0,
        (uint8_t)(lba32 >> 24),
        (uint8_t)(lba32 >> 16),
        (uint8_t)(lba32 >> 8),
        (uint8_t)(lba32),
        0,
        0, /* Transfer length MSB (1 sector) */
        1, /* Transfer length LSB (1 sector) */
        0
    };

    return xhci_bot_transfer(io, ring_dma, dev_dma, bot_rings, cdb, sizeof(cdb), buf, bot_rings->sector_size, true);
}
