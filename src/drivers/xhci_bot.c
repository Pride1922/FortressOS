#include "xhci_bot.h"
#include "string.h"
#if __STDC_HOSTED__
/* Host unit-test build: provide a silent stub for serial output. */
#  include <stdio.h>
static inline void serial_puts(const char *s) { (void)s; }
static inline void serial_print_hex(uint64_t value) { (void)value; }
#else
/* Freestanding kernel build: use real serial driver. */
#  include "serial.h"
#endif

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
        ring_dma->cmd_ring_virt[XHCI_RING_TRB_COUNT - 1].control =
            (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_TC | (ring_dma->cmd_cycle ? XHCI_TRB_C : 0);
        xhci_clflush_range(&ring_dma->cmd_ring_virt[XHCI_RING_TRB_COUNT - 1], sizeof(xhci_trb_t));
        ring_dma->cmd_enqueue_idx = 0;
        ring_dma->cmd_cycle ^= 1;
    }

    __asm__ volatile("mfence" ::: "memory");
    io->write32(io->mmio_ctx, dboff + 0, 0);

    for (unsigned ms = 0; ms <= 500; ++ms) {
        for (unsigned drained = 0; drained < XHCI_RING_TRB_COUNT; ++drained) {
            volatile const xhci_trb_t *event = &ring_dma->event_ring_virt[ring_dma->event_dequeue_idx];
            __asm__ volatile("" ::: "memory");
            uint32_t c_bit = event->control & XHCI_TRB_C;
            if (c_bit != (ring_dma->event_cycle ? XHCI_TRB_C : 0)) break;

            uint32_t trb_type = (event->control & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT;
            uint32_t comp_code = (event->status >> 24) & 0xff;
            xhci_trb_t snapshot = *event;

            ring_dma->event_dequeue_idx = (ring_dma->event_dequeue_idx + 1) % XHCI_RING_TRB_COUNT;
            if (ring_dma->event_dequeue_idx == 0) ring_dma->event_cycle ^= 1;
            uint64_t new_erdp = ring_dma->event_ring_phys + (ring_dma->event_dequeue_idx * sizeof(xhci_trb_t));
            io->write32(io->mmio_ctx, intr0 + 0x18, ((uint32_t)new_erdp & ~0xfu) | (1u << 3));
            io->write32(io->mmio_ctx, intr0 + 0x1c, (uint32_t)(new_erdp >> 32));

            if (trb_type == XHCI_TRB_TYPE_CMD_COMPLETION_EVENT) {
                uint64_t ptr = ((uint64_t)snapshot.parameter_high << 32) | snapshot.parameter_low;
                if (out_comp_event) *out_comp_event = snapshot;
                return ptr == ring_dma->cmd_ring_phys + enq_idx * sizeof(xhci_trb_t) &&
                       (snapshot.control >> 24) == (cmd_trb.control >> 24) &&
                       comp_code == XHCI_COMP_SUCCESS;
            }
        }
        if (!io->delay_ms(io->mmio_ctx)) return false;
    }
    return false;
}

/* Helper to wait for a Transfer Event on a specific endpoint */
static bool wait_transfer_event(const xhci_rings_io_t *io,
                                xhci_dma_buffers_t *ring_dma,
                                uint8_t slot_id,
                                uint8_t dci,
                                uintptr_t submitted_phys,
                                uint32_t *out_residual, uint8_t *out_code) {
    if (out_code) *out_code = 0;
    uint32_t rtsoff = io->read32(io->mmio_ctx, 0x18);
    uint32_t intr0 = rtsoff + 0x20;

    for (unsigned ms = 0; ms <= 1000; ++ms) {
        for (unsigned drained = 0; drained < XHCI_RING_TRB_COUNT; ++drained) {
            volatile const xhci_trb_t *event = &ring_dma->event_ring_virt[ring_dma->event_dequeue_idx];
            __asm__ volatile("" ::: "memory");
            uint32_t c_bit = event->control & XHCI_TRB_C;
            if (c_bit != (ring_dma->event_cycle ? XHCI_TRB_C : 0)) break;

            uint32_t trb_type = (event->control & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT;
            uint32_t comp_code = (event->status >> 24) & 0xff;
            uint8_t ev_slot = (event->control >> 24) & 0xff;
            uint8_t ev_dci  = (event->control >> 16) & 0x1f;
            uint64_t completed_phys = ((uint64_t)event->parameter_high << 32) | event->parameter_low;
            uint32_t residual = event->status & 0xffffff;
            bool event_data = (event->control & (1u << 2)) != 0;

            ring_dma->event_dequeue_idx = (ring_dma->event_dequeue_idx + 1) % XHCI_RING_TRB_COUNT;
            if (ring_dma->event_dequeue_idx == 0) ring_dma->event_cycle ^= 1;
            uint64_t new_erdp = ring_dma->event_ring_phys + (ring_dma->event_dequeue_idx * sizeof(xhci_trb_t));
            io->write32(io->mmio_ctx, intr0 + 0x18, ((uint32_t)new_erdp & ~0xfu) | (1u << 3));
            io->write32(io->mmio_ctx, intr0 + 0x1c, (uint32_t)(new_erdp >> 32));

            if (trb_type == XHCI_TRB_TYPE_TRANSFER_EVENT && ev_slot == slot_id && ev_dci == dci) {
                if (event_data || completed_phys != submitted_phys) return false;
                if (out_code) *out_code = (uint8_t)comp_code;
                if (out_residual) *out_residual = residual;
                return comp_code == XHCI_COMP_SUCCESS || comp_code == XHCI_COMP_SHORT_PACKET;
            }
        }
        if (!io->delay_ms(io->mmio_ctx)) return false;
    }
    return false;
}

/* Submit a Normal TRB on an endpoint ring and ring its doorbell */
static bool submit_normal_trb(const xhci_rings_io_t *io,
                              xhci_dma_buffers_t *ring_dma,
                              uint8_t slot_id,
                              uint8_t dci,
                              uintptr_t ring_phys,
                              xhci_trb_t *ring_virt,
                              uint32_t *ring_idx,
                              uint8_t *ring_cycle,
                              uintptr_t buf_phys,
                              uint32_t length,
                              uint32_t *out_residual, uint8_t *out_code) {
    uint32_t dboff = io->read32(io->mmio_ctx, 0x14);
    uint32_t idx = *ring_idx;
    uintptr_t submitted_phys = ring_phys + idx * sizeof(xhci_trb_t);

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
        ring_virt[XHCI_RING_TRB_COUNT - 1].control =
            (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_TC | (*ring_cycle ? XHCI_TRB_C : 0);
        xhci_clflush_range(&ring_virt[XHCI_RING_TRB_COUNT - 1], sizeof(xhci_trb_t));
        __asm__ volatile("mfence" ::: "memory");
        idx = 0;
        *ring_cycle ^= 1;
    }
    *ring_idx = idx;

    /* Ring Doorbell for this endpoint: Target = DCI */
    io->write32(io->mmio_ctx, dboff + slot_id * 4, dci);

    return wait_transfer_event(io, ring_dma, slot_id, dci, submitted_phys, out_residual, out_code);
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
    bot_rings->ep0_enqueue_idx = device->ep0_enqueue_idx;
    bot_rings->ep0_cycle = device->ep0_cycle;

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

    /* Endpoint Context: Bulk IN. Type 6 for both USB 2.0 and SuperSpeed. */
    uint32_t burst_in = (device->speed == XHCI_SPEED_SUPER ||
                         device->speed == XHCI_SPEED_SUPER_PLUS)
                        ? (uint32_t)device->bulk_in_max_burst : 0u;
    uint32_t burst_out = (device->speed == XHCI_SPEED_SUPER ||
                          device->speed == XHCI_SPEED_SUPER_PLUS)
                         ? (uint32_t)device->bulk_out_max_burst : 0u;

    in_ep_ctx[1]  = (3u << 1) | (6u << 3) | (burst_in << 8)  | ((uint32_t)bot_rings->in_max_packet << 16);
    out_ep_ctx[1] = (3u << 1) | (2u << 3) | (burst_out << 8) | ((uint32_t)bot_rings->out_max_packet << 16);
    in_ep_ctx[2] = ((uint32_t)bot_rings->bulk_in_ring_phys & ~0x3fu) | 1u;
    in_ep_ctx[3] = (uint32_t)(bot_rings->bulk_in_ring_phys >> 32);
    in_ep_ctx[4] = 512;
    out_ep_ctx[2] = ((uint32_t)bot_rings->bulk_out_ring_phys & ~0x3fu) | 1u;
    out_ep_ctx[3] = (uint32_t)(bot_rings->bulk_out_ring_phys >> 32);
    out_ep_ctx[4] = 512;

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

/* Polling only: no logging/allocation under the caller's ext2 lock. */
bool xhci_bot_transfer(const xhci_rings_io_t *io,
                       xhci_dma_buffers_t *ring_dma,
                       const xhci_dev_dma_t *dev_dma,
                       xhci_bot_rings_t *bot_rings,
                       const void *cdb, uint8_t cdb_len,
                       void *data, uint32_t data_len, bool dir_in) {
    if (!io || !ring_dma || !dev_dma || !bot_rings || !cdb ||
        cdb_len == 0 || cdb_len > 16 || data_len > 4096 ||
        (data_len && !data)) return false;
    if (bot_rings->transport_failed || bot_rings->latched_offline) return false;
    bot_rings->last_error = (xhci_bot_error_t){.opcode = ((const uint8_t *)cdb)[0]};
    bot_rings->data_transferred = 0;

    uint32_t cur_tag = ++bot_rings->tag;
    usb_bot_cbw_t cbw = {0};
    cbw.dCBWSignature = USB_BOT_CBW_SIGNATURE;
    cbw.dCBWTag = cur_tag;
    cbw.dCBWDataTransferLength = data_len;
    cbw.bmCBWFlags = (data_len && dir_in) ? USB_BOT_CBW_FLAG_IN : USB_BOT_CBW_FLAG_OUT;
    cbw.bCBWCBLength = cdb_len;
    memcpy(cbw.CBWCB, cdb, cdb_len);
    memcpy(dev_dma->bounce_buf_virt, &cbw, sizeof(cbw));
    xhci_clflush_range(dev_dma->bounce_buf_virt, sizeof(cbw));

    uint32_t resid = 0, data_resid = 0;
    uint8_t code = 0;
    bot_rings->last_error.phase = 1;
    if (!submit_normal_trb(io, ring_dma, bot_rings->slot_id, bot_rings->out_dci,
                          bot_rings->bulk_out_ring_phys,
                          bot_rings->bulk_out_ring_virt, &bot_rings->out_idx, &bot_rings->out_cycle,
                          dev_dma->bounce_buf_phys, sizeof(cbw), &resid, &code) || resid)
        goto transport_error;

    /* CBW DMA has completed. Reuse the page at offset zero so a full 4096-byte
     * sector fits. The CSW area is reused only after data DMA/copy completes. */
    if (data_len) {
        uint8_t *data_virt = dev_dma->bounce_buf_virt;
        if (dir_in) memset(data_virt, 0, data_len);
        else memcpy(data_virt, data, data_len);
        xhci_clflush_range(data_virt, data_len);
        uint8_t dci = dir_in ? bot_rings->in_dci : bot_rings->out_dci;
        xhci_trb_t *ring = dir_in ? bot_rings->bulk_in_ring_virt : bot_rings->bulk_out_ring_virt;
        uint32_t *idx = dir_in ? &bot_rings->in_idx : &bot_rings->out_idx;
        uint8_t *cycle = dir_in ? &bot_rings->in_cycle : &bot_rings->out_cycle;
        bot_rings->last_error.phase = 2;
        bool data_ok = submit_normal_trb(io, ring_dma, bot_rings->slot_id, dci,
                              dir_in ? bot_rings->bulk_in_ring_phys : bot_rings->bulk_out_ring_phys,
                              ring, idx, cycle, dev_dma->bounce_buf_phys, data_len, &data_resid, &code);
        if (data_resid > data_len) goto transport_error;
        if (!data_ok) {
            if (code != XHCI_COMP_STALL_ERROR) goto transport_error;
            bot_rings->last_error.completion_code = code;
            /* This matching STALL ended the sole outstanding data TD. Copy
             * completed IN bytes before using EP0; never replay OUT payload. */
            if (dir_in) memcpy(data, data_virt, data_len - data_resid);
            if (!xhci_bot_endpoint_reset(io, ring_dma, dev_dma, bot_rings, dci))
                goto transport_error;
        }
        if (dir_in) memcpy(data, data_virt, data_len - data_resid);
        bot_rings->data_transferred = data_len - data_resid;
    }

    uint8_t *csw_virt = dev_dma->bounce_buf_virt + 64;
    bot_rings->last_error.phase = 3;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        memset(csw_virt, 0, sizeof(usb_bot_csw_t));
        xhci_clflush_range(csw_virt, sizeof(usb_bot_csw_t));
        bool csw_ok = submit_normal_trb(io, ring_dma, bot_rings->slot_id, bot_rings->in_dci,
                          bot_rings->bulk_in_ring_phys,
                          bot_rings->bulk_in_ring_virt, &bot_rings->in_idx, &bot_rings->in_cycle,
                          dev_dma->bounce_buf_phys + 64, sizeof(usb_bot_csw_t), &resid, &code);
        if (csw_ok) {
            if (resid) goto transport_error;
            break;
        }
        if (code != XHCI_COMP_STALL_ERROR || attempt != 0) goto transport_error;
        bot_rings->last_error.completion_code = code;
        if (!xhci_bot_endpoint_reset(io, ring_dma, dev_dma, bot_rings, bot_rings->in_dci))
            goto transport_error;
    }
    usb_bot_csw_t csw;
    memcpy(&csw, csw_virt, sizeof(csw));
    if (csw.dCSWSignature != USB_BOT_CSW_SIGNATURE || csw.dCSWTag != cur_tag ||
        csw.dCSWDataResidue > data_len || csw.bCSWStatus > USB_BOT_CSW_STATUS_FAILED)
        goto transport_error;
    bot_rings->last_error.csw_status = csw.bCSWStatus;
    if (csw.bCSWStatus == USB_BOT_CSW_STATUS_FAILED) {
        bot_rings->last_error.command_failed = true;
        return false;
    }
    /* A complete valid CSW ends DMA ownership, but incomplete data is not success. */
    if (dir_in && data_resid == csw.dCSWDataResidue &&
        ((cbw.CBWCB[0] == SCSI_CMD_REQUEST_SENSE && bot_rings->data_transferred >= 8) ||
         cbw.CBWCB[0] == SCSI_CMD_MODE_SENSE_6 || cbw.CBWCB[0] == SCSI_CMD_MODE_SENSE_10))
        return true; /* Variable-length replies; callers validate their headers. */
    return data_resid == 0 && csw.dCSWDataResidue == 0;

transport_error:
    bot_rings->last_error.completion_code = code;
    bot_rings->transport_failed = true;
    bot_rings->last_error.transport_failed = true;
    return false;
}

/* Only after a valid command-failed CSW. Never submit another transfer after
 * timeout/stall/protocol desynchronization; those buffers remain quarantined. */
static bool scsi_request_sense(const xhci_rings_io_t *io,
                               xhci_dma_buffers_t *ring_dma,
                               const xhci_dev_dma_t *dev_dma,
                               xhci_bot_rings_t *bot_rings,
                               xhci_bot_error_t *error) {
    uint8_t cdb[6] = {SCSI_CMD_REQUEST_SENSE, 0, 0, 0, 18, 0};
    uint8_t sense[18] = {0};
    bool ok = xhci_bot_transfer(io, ring_dma, dev_dma, bot_rings,
                                cdb, sizeof(cdb), sense, sizeof(sense), true);
    if (ok) {
        uint8_t response = sense[0] & 0x7f;
        if ((response == 0x70 || response == 0x71) && sense[7] >= 6 &&
            bot_rings->data_transferred >= 14) {
            error->sense_valid = true;
            error->sense_key = sense[2] & 0x0f;
            error->asc = sense[12];
            error->ascq = sense[13];
        } else if (response == 0x72 || response == 0x73) {
            error->sense_valid = true;
            error->sense_key = sense[1] & 0x0f;
            error->asc = sense[2];
            error->ascq = sense[3];
        }
        error->sense_response = response;
    }
    error->transport_failed = bot_rings->transport_failed;
    bot_rings->last_error = *error;
    return ok && error->sense_valid;
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

        if (!bot_rings->last_error.command_failed || bot_rings->transport_failed) break;
        xhci_bot_error_t error = bot_rings->last_error;
        if (!scsi_request_sense(io, ring_dma, dev_dma, bot_rings, &error)) break;
        if (!io->delay_ms(io->mmio_ctx)) break;
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
    if (!bot_rings || !buf || lba >= bot_rings->sector_count || lba > 0xffffffffu) return false;

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

bool xhci_scsi_write_sector(const xhci_rings_io_t *io,
                            xhci_dma_buffers_t *ring_dma,
                            const xhci_dev_dma_t *dev_dma,
                            xhci_bot_rings_t *bot_rings,
                            uint64_t lba,
                            const void *buf) {
    if (!bot_rings || !buf || lba >= bot_rings->sector_count || lba > 0xffffffffu) return false;

    uint32_t lba32 = (uint32_t)lba;
    uint8_t cdb[10] = {
        SCSI_CMD_WRITE_10,
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

    return xhci_bot_transfer(io, ring_dma, dev_dma, bot_rings, cdb, sizeof(cdb), (void *)buf, bot_rings->sector_size, false);
}

bool xhci_scsi_sync_cache(const xhci_rings_io_t *io,
                          xhci_dma_buffers_t *ring_dma,
                          const xhci_dev_dma_t *dev_dma,
                          xhci_bot_rings_t *bot_rings) {
    uint8_t cdb[10] = {
        SCSI_CMD_SYNCHRONIZE_CACHE_10,
        0, /* immed = 0: wait for completion on physical media */
        0, 0, 0, 0, /* Starting LBA = 0 */
        0,          /* Group number = 0 */
        0, 0,       /* Number of blocks = 0 (all blocks) */
        0           /* Control = 0 */
    };

    if (!bot_rings) return false;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        if (xhci_bot_transfer(io, ring_dma, dev_dma, bot_rings,
                              cdb, sizeof(cdb), NULL, 0, false)) return true;
        if (!bot_rings->last_error.command_failed || bot_rings->transport_failed) return false;
        xhci_bot_error_t error = bot_rings->last_error;
        if (!scsi_request_sense(io, ring_dma, dev_dma, bot_rings, &error)) return false;
        /* Retry only current UNIT ATTENTION, once. Never reinterpret an illegal
         * opcode/field or deferred write error as successful synchronization. */
        if ((error.sense_response != 0x70 && error.sense_response != 0x72) ||
            error.sense_key != 6) return false;
    }
    return false;
}

/* Only for a matching STALL event on the sole outstanding bulk TD. No
 * recovery after timeouts, pointer mismatches or a latched transport failure.
 * Reset Endpoint transitions Halted -> Stopped; Stop Endpoint is invalid here.
 * No allocations, logging, IRQ enables or buffer reclamation in this path. */
bool xhci_bot_endpoint_reset(const xhci_rings_io_t *io,
                             xhci_dma_buffers_t *ring_dma,
                             const xhci_dev_dma_t *dev_dma,
                             xhci_bot_rings_t *bot_rings,
                             uint8_t dci) {
    if (!io || !ring_dma || !dev_dma || !bot_rings) return false;
    if (bot_rings->transport_failed || bot_rings->latched_offline) return false;
    if ((dci != bot_rings->in_dci && dci != bot_rings->out_dci) ||
        !dev_dma->ep0_ring_virt || bot_rings->ep0_enqueue_idx >= XHCI_RING_TRB_COUNT - 1)
        goto latch;

    xhci_trb_t reset = {0};
    reset.control = (XHCI_TRB_TYPE_RESET_EP_CMD << 10) |
                    ((uint32_t)bot_rings->slot_id << 24) | ((uint32_t)dci << 16);
    if (!send_command(io, ring_dma, reset, NULL)) goto latch;

    /* CLEAR_FEATURE(ENDPOINT_HALT): two TRBs on the existing EP0 ring.
     * Continue its own producer cycle, even if either TRB crosses the Link. */
    uint8_t ep_addr = (uint8_t)((dci >> 1) | ((dci & 1u) ? 0x80u : 0u));
    xhci_trb_t stages[2] = {0};
    stages[0].parameter_low = (USB_REQ_CLEAR_FEATURE << 8) | USB_RT_ENDPOINT_OUT;
    stages[0].parameter_high = ep_addr;
    stages[0].status = 8;
    stages[0].control = (XHCI_TRB_TYPE_SETUP_STAGE << 10) | (1u << 6);
    stages[1].control = (XHCI_TRB_TYPE_STATUS_STAGE << 10) | XHCI_TRB_IOC | (1u << 16);
    uintptr_t status_phys = 0;
    for (unsigned stage = 0; stage < 2; ++stage) {
        uint32_t idx = bot_rings->ep0_enqueue_idx;
        stages[stage].control |= bot_rings->ep0_cycle ? XHCI_TRB_C : 0;
        dev_dma->ep0_ring_virt[idx] = stages[stage];
        xhci_clflush_range(&dev_dma->ep0_ring_virt[idx], sizeof(xhci_trb_t));
        status_phys = dev_dma->ep0_ring_phys + idx * sizeof(xhci_trb_t);
        if (++idx == XHCI_RING_TRB_COUNT - 1) {
            xhci_trb_t *link = &dev_dma->ep0_ring_virt[idx];
            link->parameter_low = (uint32_t)dev_dma->ep0_ring_phys;
            link->parameter_high = (uint32_t)(dev_dma->ep0_ring_phys >> 32);
            link->status = 0;
            link->control = (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_TC |
                            (bot_rings->ep0_cycle ? XHCI_TRB_C : 0);
            xhci_clflush_range(link, sizeof(*link));
            idx = 0;
            bot_rings->ep0_cycle ^= 1;
        }
        bot_rings->ep0_enqueue_idx = idx;
    }
    __asm__ volatile("mfence" ::: "memory");
    uint32_t dboff = io->read32(io->mmio_ctx, 0x14);
    io->write32(io->mmio_ctx, dboff + bot_rings->slot_id * 4, 1);
    uint32_t residual = 0;
    uint8_t code = 0;
    if (!wait_transfer_event(io, ring_dma, bot_rings->slot_id, 1,
                             status_phys, &residual, &code) ||
        code != XHCI_COMP_SUCCESS || residual) goto latch;

    /* Skip the completed stalled TD, preserving the bulk producer cycle.
     * No bulk doorbell until both sides have cleared their halt. */
    bool is_in = dci == bot_rings->in_dci;
    uintptr_t ring_phys = is_in ? bot_rings->bulk_in_ring_phys : bot_rings->bulk_out_ring_phys;
    uint32_t idx = is_in ? bot_rings->in_idx : bot_rings->out_idx;
    uint8_t cycle = is_in ? bot_rings->in_cycle : bot_rings->out_cycle;
    uintptr_t ptr = ring_phys + idx * sizeof(xhci_trb_t);
    xhci_trb_t setdq = {0};
    setdq.parameter_low = (uint32_t)ptr | (cycle ? 1u : 0u);
    setdq.parameter_high = (uint32_t)(ptr >> 32);
    setdq.control = (XHCI_TRB_TYPE_SET_TR_DEQUEUE_CMD << 10) |
                    ((uint32_t)bot_rings->slot_id << 24) | ((uint32_t)dci << 16);
    if (!send_command(io, ring_dma, setdq, NULL)) goto latch;
    return true;
latch:
    bot_rings->latched_offline = true;
    return false;
}

/* =============================================================================
 * Commit 2: MODE SENSE Cache Policy Discovery
 *
 * Probes MODE SENSE(6), falling back to MODE SENSE(10), for caching page 0x08.
 * Separately tests SYNCHRONIZE CACHE(10) with IMMED=0.
 * Never changes device settings (no MODE SELECT).
 * ============================================================================= */

/* Parse a caching mode page starting at *page_data (page code 0x08).
 * page_len is the page length field from the mode page header (bytes after page code and length).
 * Returns true and sets *wce, *rcd if the page is valid. */
static bool parse_caching_page(const uint8_t *page_data, uint8_t page_len,
                               bool *wce, bool *rcd) {
    /* SPC mode page 0x08 layout:
     *   byte 0: page code (0x08) possibly | 0x80 (PS bit)
     *   byte 1: page length (must be >= 18 for the full page, but >= 2 for WCE/RCD)
     *   byte 2: flags: bit 2 = WCE, bit 0 = RCD
     *   ...
     * Minimum useful length: page_len >= 2 (byte 2 is offset 2 from page start) */
    if (page_len < 2) return false;
    if ((page_data[0] & 0x3fu) != 0x08u) return false; /* wrong page code */
    *wce = (page_data[2] & (1u << 2)) != 0;
    *rcd = (page_data[2] & (1u << 0)) != 0;
    return true;
}

bool xhci_scsi_probe_cache_policy(const xhci_rings_io_t *io,
                                  xhci_dma_buffers_t *ring_dma,
                                  const xhci_dev_dma_t *dev_dma,
                                  xhci_bot_rings_t *bot_rings,
                                  scsi_durability_info_t *info) {
    if (!info || !bot_rings) return false;
    memset(info, 0, sizeof(*info));
    if (bot_rings->transport_failed || bot_rings->latched_offline) return false;

    /* ---- MODE SENSE(6) for caching page 0x08, current values ---- */
    info->ms6_attempted = true;
    {
        /* Allocation length 28: 4-byte header + up to 24 bytes for one mode page */
        uint8_t cdb6[6] = {SCSI_CMD_MODE_SENSE_6, 0,
                           0x08u, /* PC=00 (current), page=0x08 */
                           0,     /* SubPage = 0 */
                           28,    /* Allocation length */
                           0};
        uint8_t buf[28] = {0};
        bool ok = xhci_bot_transfer(io, ring_dma, dev_dma, bot_rings,
                                    cdb6, sizeof(cdb6), buf, sizeof(buf), true);
        if (ok || bot_rings->last_error.command_failed) {
            /* command_failed: device rejected opcode or field (ILLEGAL REQUEST).
             * We still record any partially transferred bytes for diagnostics. */
            uint8_t transferred = (uint8_t)(bot_rings->data_transferred < 28 ?
                                            bot_rings->data_transferred : 28);
            if (transferred > 0) {
                memcpy(info->raw_ms6, buf, transferred);
                info->ms6_len = transferred;
            }
            if (ok && transferred >= 4) {
                /* MODE SENSE(6) header: byte 0 = mode data length (excludes itself),
                 * byte 1 = medium type, byte 2 = device-specific (WP in bit 7),
                 * byte 3 = block descriptor length. */
                uint8_t hdr_len = buf[0]; /* mode data length, not counting byte 0 */
                bool write_protect = (buf[2] & 0x80u) != 0;
                uint8_t bd_len = buf[3];
                /* Validate: total must be hdr_len+1, block descriptor must fit */
                if ((uint16_t)hdr_len + 1u <= 28u && bd_len < hdr_len) {
                    uint8_t page_off = 4u + bd_len; /* offset of first mode page */
                    if (page_off + 2u <= transferred) {
                        uint8_t page_code = buf[page_off] & 0x3fu;
                        uint8_t page_len  = buf[page_off + 1u];
                        bool wce = false, rcd = false;
                        if (page_code == 0x08u && page_off + 2u + page_len <= transferred &&
                            parse_caching_page(&buf[page_off], page_len, &wce, &rcd)) {
                            info->wce = wce;
                            info->rcd = rcd;
                            info->write_protect = write_protect;
                            info->ms6_ok = true;
                            info->probed = true;
                        }
                    }
                }
            }
        }
        /* If transport_failed during MODE SENSE(6) we cannot proceed */
        if (bot_rings->transport_failed || bot_rings->latched_offline) return false;
    }

/* ---- MODE SENSE(10) fallback if (6) was not attempted or page not found ---- */
if (!info->ms6_ok && !bot_rings->transport_failed && !bot_rings->latched_offline) {
    info->ms10_attempted = true;
    uint8_t cdb10[10] = {SCSI_CMD_MODE_SENSE_10, 0,
                         0x08u, /* PC=00, page=0x08 */
                         0,     /* SubPage = 0 */
                         0, 0, 0,
                         0, 32, /* Allocation length MSB/LSB = 32 */
                         0};
    uint8_t buf[32] = {0};
    bool ok = xhci_bot_transfer(io, ring_dma, dev_dma, bot_rings,
                                cdb10, sizeof(cdb10), buf, sizeof(buf), true);

    /* A rejected command (CSW status FAILED) is not a transport failure.
     * Continue to classification; the flags we collected tell the story. */
    if (!bot_rings->transport_failed && !bot_rings->latched_offline) {
        uint8_t transferred = (uint8_t)(bot_rings->data_transferred < 32 ?
                                        bot_rings->data_transferred : 32);
        if (transferred > 0) {
            memcpy(info->raw_ms10, buf, transferred);
            info->ms10_len = transferred;
        }
        if (ok && transferred >= 8) {
            /* MODE SENSE(10) header: bytes 0-1 = mode data length (big-endian, not counting first 2),
             * byte 2 = medium type, byte 3 = device-specific (WP in bit 7),
             * bytes 6-7 = block descriptor length. */
            uint16_t hdr_len = ((uint16_t)buf[0] << 8) | buf[1];
            bool write_protect = (buf[3] & 0x80u) != 0;
            uint16_t bd_len = ((uint16_t)buf[6] << 8) | buf[7];
            uint16_t page_off = 8u + bd_len;
            if (page_off + 2u <= transferred && (uint32_t)hdr_len + 2u <= 32u) {
                uint8_t page_code = buf[page_off] & 0x3fu;
                uint8_t page_len  = buf[page_off + 1u];
                bool wce = false, rcd = false;
                if (page_code == 0x08u && page_off + 2u + page_len <= transferred &&
                    parse_caching_page(&buf[page_off], page_len, &wce, &rcd)) {
                    info->wce = wce;
                    info->rcd = rcd;
                    info->write_protect = write_protect;
                    info->ms10_ok = true;
                    info->probed = true;
                }
            }
        }
    }
}

/* ---- SYNCHRONIZE CACHE probe (always runs unless transport is dead) ---- */
if (!bot_rings->transport_failed && !bot_rings->latched_offline) {
    /* Issue SYNCHRONIZE CACHE(10) with IMMED=0 to test whether the device
     * supports durable flushing. Command rejection is a valid outcome and
     * must not be conflated with transport failure. */
    info->sync_attempted = true;
    info->sync_ok = xhci_scsi_sync_cache(io, ring_dma, dev_dma, bot_rings);
    /* xhci_scsi_sync_cache returns false either on CSW status FAILED
     * (command rejected — device just doesn't support it) or on transport
     * failure. Only the latter sets bot_rings->transport_failed. The flags
     * below already capture which happened. */
}

/* ---- Classify based on all evidence collected ---- */
if (bot_rings->transport_failed || bot_rings->latched_offline) {
    info->policy = USB_DURABILITY_READ_ONLY;
} else if (info->ms6_ok && !info->wce) {
    info->policy = USB_DURABILITY_WRITE_THROUGH;
} else if (info->ms10_ok && !info->wce) {
    info->policy = USB_DURABILITY_WRITE_THROUGH;
} else if (info->sync_ok) {
    info->policy = USB_DURABILITY_SYNC_BACKED;
} else if (info->wce && !info->sync_ok) {
    info->policy = USB_DURABILITY_READ_ONLY;
} else {
    /* No caching page on either MODE SENSE form, no working sync, but the
     * transport is healthy. This is the standard consumer USB stick case.
     * Assume write-through, matching Linux and Windows behavior. */
    info->policy = USB_DURABILITY_ASSUMED_WRITE_THROUGH;
}

return info->probed || info->ms10_attempted;
}

/* =============================================================================
 * Commit 3: Durability State Machine
 * ============================================================================= */

usb_durability_mode_t xhci_bot_get_durability_mode(const xhci_bot_rings_t *bot_rings) {
    if (!bot_rings) return USB_DURABILITY_UNKNOWN;
    return bot_rings->durability_mode;
}

void xhci_bot_probe_durability(const xhci_rings_io_t *io,
                               xhci_dma_buffers_t *ring_dma,
                               const xhci_dev_dma_t *dev_dma,
                               xhci_bot_rings_t *bot_rings) {
    if (!bot_rings) return;
    if (bot_rings->transport_failed || bot_rings->latched_offline) {
        bot_rings->durability_mode = USB_DURABILITY_READ_ONLY;
        return;
    }

    scsi_durability_info_t info;
    bool probed = xhci_scsi_probe_cache_policy(io, ring_dma, dev_dma, bot_rings, &info);
    (void)probed;
    bot_rings->durability_info = info;

    if (bot_rings->transport_failed || bot_rings->latched_offline) {
        bot_rings->durability_mode = USB_DURABILITY_READ_ONLY;
    } else {
        bot_rings->durability_mode = (usb_durability_mode_t)info.policy;
    }

    /* Print [USB DURABILITY] diagnostic */
    serial_puts("[USB DURABILITY] Device: sda (");
    serial_puts(bot_rings->vendor[0] ? bot_rings->vendor : "?");
    serial_puts(" ");
    serial_puts(bot_rings->product[0] ? bot_rings->product : "?");
    serial_puts(")\n");

    serial_puts("[USB DURABILITY] MODE SENSE(6) page 0x08: ");
    serial_puts(info.ms6_ok ? "OK\n" : "not found\n");

    if (info.ms10_attempted) {
        serial_puts("[USB DURABILITY] MODE SENSE(10) page 0x08: ");
        serial_puts(info.ms10_ok ? "OK\n" : "not found\n");
    }

    if (info.ms6_ok || info.ms10_ok) {
        serial_puts("[USB DURABILITY]   WCE=");
        serial_puts(info.wce ? "1" : "0");
        serial_puts(" RCD=");
        serial_puts(info.rcd ? "1" : "0");
        serial_puts(info.write_protect ? " WRITE_PROTECT=1" : "");
        serial_puts("\n");
    }

    serial_puts("[USB DURABILITY] SYNCHRONIZE CACHE test: ");
    serial_puts(!info.sync_attempted ? "not attempted (transport unavailable)\n" :
                info.sync_ok ? "OK\n" : "failed/unsupported\n");
    if (bot_rings->transport_failed || bot_rings->latched_offline) {
        /* Probe runs in unlocked thread context, after all polling has returned. */
        serial_puts("[USB DURABILITY] Transport failure opcode=");
        serial_print_hex(bot_rings->last_error.opcode);
        serial_puts(" phase(1=CBW,2=data,3=CSW)=");
        serial_print_hex(bot_rings->last_error.phase);
        serial_puts(" completion=");
        serial_print_hex(bot_rings->last_error.completion_code);
        serial_puts(bot_rings->latched_offline ? " recovery failed; offline\n" : " offline\n");
    }

    const char *mode_str;
    const char *eligible_str;
    switch (bot_rings->durability_mode) {
        case USB_DURABILITY_SYNC_BACKED:
            mode_str = "SYNC_BACKED";
            eligible_str = "RW eligible (every flush must execute SYNCHRONIZE CACHE)";
            break;
        case USB_DURABILITY_WRITE_THROUGH:
            mode_str = "WRITE_THROUGH";
            eligible_str = "RW eligible (barrier without cache command after successful writes)";
            break;
        case USB_DURABILITY_ASSUMED_WRITE_THROUGH:
            mode_str = "ASSUMED_WRITE_THROUGH";
            eligible_str = "RW eligible (assumed write-through)";
            break;
        case USB_DURABILITY_READ_ONLY:
            mode_str = "READ_ONLY";
            eligible_str = "RW NOT eligible";
            break;
        default:
            mode_str = "UNKNOWN";
            eligible_str = "RW NOT eligible (unknown cache policy)";
            break;
    }
    serial_puts("[USB DURABILITY] Classification: ");
    serial_puts(mode_str);
    serial_puts("\n");

    if (bot_rings->durability_mode == USB_DURABILITY_ASSUMED_WRITE_THROUGH) {
        serial_puts("[USB DURABILITY] Note: device does not report cache policy. Assuming write-through,\n"
                    "                 matching Linux and Windows behavior. Clean shutdown is assumed\n"
                    "                 durable. Power-loss during writes may lose data.\n");
    }

    serial_puts("[USB DURABILITY] Mount eligibility: ");
    serial_puts(eligible_str);
    serial_puts("\n");
}

bool xhci_bot_flush_barrier(const xhci_rings_io_t *io,
                            xhci_dma_buffers_t *ring_dma,
                            const xhci_dev_dma_t *dev_dma,
                            xhci_bot_rings_t *bot_rings) {
    if (!bot_rings) return false;
    if (bot_rings->transport_failed || bot_rings->latched_offline) return false;

    switch (bot_rings->durability_mode) {
        case USB_DURABILITY_SYNC_BACKED:
            /* Must execute and verify SYNCHRONIZE CACHE */
            if (!xhci_scsi_sync_cache(io, ring_dma, dev_dma, bot_rings)) {
                /* Latch to read-only: the device could not durably flush */
                bot_rings->durability_mode = USB_DURABILITY_READ_ONLY;
                return false;
            }
            return true;

        case USB_DURABILITY_WRITE_THROUGH:
            /* Device-reported write-through: barrier is satisfied if transport is healthy.
             * No cache command needed. */
            if (bot_rings->transport_failed || bot_rings->latched_offline) {
                bot_rings->durability_mode = USB_DURABILITY_READ_ONLY;
                return false;
            }
            return true;

        case USB_DURABILITY_ASSUMED_WRITE_THROUGH:
            /* Try anyway; some devices honor SYNC even without advertising it.
             * But don't fail the barrier if the device rejects. */
            if (bot_rings->transport_failed || bot_rings->latched_offline) {
                bot_rings->durability_mode = USB_DURABILITY_READ_ONLY;
                return false;
            }
            (void)xhci_scsi_sync_cache(io, ring_dma, dev_dma, bot_rings);
            if (bot_rings->transport_failed || bot_rings->latched_offline) {
                bot_rings->durability_mode = USB_DURABILITY_READ_ONLY;
                return false;
            }
            return true;

        default:
            /* UNKNOWN or READ_ONLY: cannot provide a durability guarantee */
            return false;
    }
}
