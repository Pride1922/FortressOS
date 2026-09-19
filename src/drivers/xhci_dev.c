#include "xhci_dev.h"
#include <string.h>

#define XHCI_TRB_TYPE_SETUP_STAGE          2u
#define XHCI_TRB_TYPE_DATA_STAGE           3u
#define XHCI_TRB_TYPE_STATUS_STAGE         4u
#define XHCI_TRB_TYPE_ENABLE_SLOT_CMD      9u
#define XHCI_TRB_TYPE_ADDRESS_DEVICE_CMD   11u
#define XHCI_TRB_TYPE_EVAL_CONTEXT_CMD     13u
#define XHCI_TRB_TYPE_TRANSFER_EVENT       32u
#define XHCI_TRB_TYPE_CMD_COMPLETION_EVENT 33u
#define XHCI_TRB_TYPE_PORT_STATUS_EVENT    34u

static bool fits(const xhci_rings_io_t *io, uint32_t off, uint32_t len) {
    return !(off & 3) && off <= io->mmio_size && len <= io->mmio_size - off;
}

static uint32_t s_ep0_idx = 0;
static uint32_t s_ep0_cycle = 1;

/* Submit command to Command Ring and wait for Command Completion Event */
static bool send_command(const xhci_rings_io_t *io,
                         xhci_dma_buffers_t *dma,
                         xhci_trb_t trb,
                         xhci_trb_t *out_event) {
    uint32_t dboff = io->read32(io->mmio_ctx, 0x14);
    uint32_t rtsoff = io->read32(io->mmio_ctx, 0x18);
    uint32_t intr0 = rtsoff + 0x20;

    /* Write command TRB */
    uint32_t idx = dma->cmd_enqueue_idx;
    trb.control = (trb.control & ~XHCI_TRB_C) | (dma->cmd_cycle ? XHCI_TRB_C : 0);
    dma->cmd_ring_virt[idx] = trb;
    __asm__ volatile("" ::: "memory");

    uint64_t submitted_cmd_phys = dma->cmd_ring_phys + idx * sizeof(xhci_trb_t);

    /* Advance enqueue index (Link TRB at index 255) */
    dma->cmd_enqueue_idx++;
    if (dma->cmd_enqueue_idx == XHCI_RING_TRB_COUNT - 1) {
        dma->cmd_enqueue_idx = 0;
        dma->cmd_cycle ^= 1;
    }

    /* Ring Doorbell 0 */
    io->write32(io->mmio_ctx, dboff + 0, 0);

    /* Poll Event Ring */
    for (unsigned ms = 0; ms <= 500; ++ms) {
        while (1) {
            volatile const xhci_trb_t *event = &dma->event_ring_virt[dma->event_dequeue_idx];
            __asm__ volatile("" ::: "memory");
            uint32_t c_bit = event->control & XHCI_TRB_C;
            if (c_bit != (dma->event_cycle ? XHCI_TRB_C : 0)) {
                break;
            }

            uint32_t trb_type = (event->control & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT;
            uint32_t comp_code = (event->status >> 24) & 0xff;
            uint64_t cmd_ptr = ((uint64_t)event->parameter_high << 32) | event->parameter_low;

            /* Advance dequeue pointer and acknowledge to hardware via ERDP */
            dma->event_dequeue_idx = (dma->event_dequeue_idx + 1) % XHCI_RING_TRB_COUNT;
            if (dma->event_dequeue_idx == 0) dma->event_cycle ^= 1;
            uint64_t new_erdp = dma->event_ring_phys + (dma->event_dequeue_idx * sizeof(xhci_trb_t));
            io->write32(io->mmio_ctx, intr0 + 0x18, ((uint32_t)new_erdp & ~0xfu) | (1u << 3));
            io->write32(io->mmio_ctx, intr0 + 0x1c, (uint32_t)(new_erdp >> 32));

            if (trb_type == XHCI_TRB_TYPE_CMD_COMPLETION_EVENT) {
                if (out_event) *out_event = *(const xhci_trb_t *)event;
                return (cmd_ptr == submitted_cmd_phys && comp_code == XHCI_COMP_SUCCESS);
            } else if (trb_type == XHCI_TRB_TYPE_PORT_STATUS_EVENT) {
                continue;
            }
        }
        io->delay_ms(io->mmio_ctx);
    }
    return false;
}

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

/* Execute USB Control Transfer on Endpoint 0 */
static bool control_transfer(const xhci_rings_io_t *io,
                             xhci_dma_buffers_t *ring_dma,
                             const xhci_dev_dma_t *dev_dma,
                             uint8_t slot_id,
                             usb_setup_pkt_t setup,
                             void *data_buf,
                             uint16_t length,
                             bool dir_in,
                             xhci_bot_device_t *device) {
    uint32_t dboff = io->read32(io->mmio_ctx, 0x14);
    uint32_t rtsoff = io->read32(io->mmio_ctx, 0x18);
    uint32_t intr0 = rtsoff + 0x20;

    /* Copy outgoing data if OUT transfer; pre-fill sentinel 0xAA if IN transfer */
    if (!dir_in && length > 0 && data_buf) {
        memcpy(dev_dma->bounce_buf_virt, data_buf, length);
        xhci_clflush_range(dev_dma->bounce_buf_virt, length);
    } else if (dir_in && length > 0) {
        memset(dev_dma->bounce_buf_virt, 0xaa, length);
        xhci_clflush_range(dev_dma->bounce_buf_virt, length);
    }

    if (s_ep0_idx >= XHCI_RING_TRB_COUNT - 4) {
        s_ep0_idx = 0;
        s_ep0_cycle ^= 1;
    }

    /* 1. Setup Stage TRB: TD Size = 1 if Data Stage follows, 0 if no data */
    uint32_t trt = (length > 0) ? (dir_in ? 3 : 2) : 0;
    xhci_trb_t setup_trb = {0};
    memcpy(&setup_trb.parameter_low, &setup, 8);
    setup_trb.status = 8 | ((length > 0 ? 1u : 0u) << 17);
    /* TRT is at bits 17:16 of control; bit 6 is IDT (Immediate Data); bit 5 (IOC) is 0 */
    setup_trb.control = (XHCI_TRB_TYPE_SETUP_STAGE << 10) | (trt << 16) | (1u << 6) /* IDT */ | (s_ep0_cycle ? 1u : 0);
    dev_dma->ep0_ring_virt[s_ep0_idx++] = setup_trb;

    /* 2. Data Stage TRB (if length > 0) */
    if (length > 0) {
        xhci_trb_t data_trb = {0};
        data_trb.parameter_low = (uint32_t)dev_dma->bounce_buf_phys;
        data_trb.parameter_high = (uint32_t)(dev_dma->bounce_buf_phys >> 32);
        data_trb.status = length;
        data_trb.control = (XHCI_TRB_TYPE_DATA_STAGE << 10) | (dir_in ? (1u << 16) : 0) | (s_ep0_cycle ? 1u : 0);
        dev_dma->ep0_ring_virt[s_ep0_idx++] = data_trb;
    }

    /* 3. Status Stage TRB */
    uintptr_t status_trb_phys = dev_dma->ep0_ring_phys + (s_ep0_idx * sizeof(xhci_trb_t));
    xhci_trb_t status_trb = {0};
    status_trb.control = (XHCI_TRB_TYPE_STATUS_STAGE << 10) | (dir_in ? 0 : (1u << 16)) | (1u << 5) /* IOC */ | (s_ep0_cycle ? 1u : 0);
    dev_dma->ep0_ring_virt[s_ep0_idx++] = status_trb;

    /* Flush newly written TRBs from CPU cache */
    uint32_t num_trbs = (length > 0) ? 3 : 2;
    xhci_clflush_range(&dev_dma->ep0_ring_virt[s_ep0_idx - num_trbs], num_trbs * sizeof(xhci_trb_t));

    __asm__ volatile("mfence" ::: "memory");

    /* Ring Doorbell for Slot ID, Target = 1 */
    io->write32(io->mmio_ctx, dboff + slot_id * 4, 1);

    /* Poll Event Ring for Transfer Event */
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

            if (trb_type == XHCI_TRB_TYPE_TRANSFER_EVENT) {
                uint64_t trb_ptr = ((uint64_t)event->parameter_high << 32) | event->parameter_low;
                if (device) {
                    device->last_comp_code = comp_code;
                    device->last_residual = event->status & 0xffffff;
                    device->last_trb_param = trb_ptr;
                }
                if (comp_code != XHCI_COMP_SUCCESS && comp_code != XHCI_COMP_SHORT_PACKET) {
                    return false;
                }
                /* If this transfer event was for an earlier TRB (e.g. Setup Stage), keep polling for Status Stage */
                if (trb_ptr != 0 && trb_ptr != status_trb_phys) {
                    continue;
                }
                if (dir_in && length > 0 && data_buf) {
                    xhci_clflush_range(dev_dma->bounce_buf_virt, length);
                    __asm__ volatile("mfence" ::: "memory");
                    memcpy(data_buf, dev_dma->bounce_buf_virt, length);
                }
                return true;
            } else if (trb_type == XHCI_TRB_TYPE_PORT_STATUS_EVENT) {
                continue;
            }
        }
        io->delay_ms(io->mmio_ctx);
    }
    return false;
}

bool xhci_enumerate_device(const xhci_rings_io_t *io,
                           xhci_dma_buffers_t *ring_dma,
                           const xhci_dev_dma_t *dev_dma,
                           uint8_t port_num,
                           uint8_t port_speed,
                           xhci_bot_device_t *device) {
    if (!device) return false;
    *device = (xhci_bot_device_t){0};
    device->port_num = port_num;
    device->speed = port_speed;

    if (!fits(io, 0, 0x40)) {
        device->error_msg = "Aperture too short";
        return false;
    }

    uint32_t hcs1 = io->read32(io->mmio_ctx, 0x04);
    uint32_t hcc1 = io->read32(io->mmio_ctx, 0x10);
    uint32_t max_slots = hcs1 & 0xff;

    uint32_t ctx_shift = (hcc1 & (1 << 2)) ? 6 : 5; /* 64-byte vs 32-byte contexts */
    uint32_t ctx_dwords = 1u << (ctx_shift - 2);

    /* Reset EP0 ring local tracking */
    s_ep0_idx = 0;
    s_ep0_cycle = 1;

    /* Setup EP0 Ring Link TRB at index 255 */
    memset(dev_dma->ep0_ring_virt, 0, 4096);
    dev_dma->ep0_ring_virt[XHCI_RING_TRB_COUNT - 1].parameter_low = (uint32_t)dev_dma->ep0_ring_phys;
    dev_dma->ep0_ring_virt[XHCI_RING_TRB_COUNT - 1].parameter_high = (uint32_t)(dev_dma->ep0_ring_phys >> 32);
    dev_dma->ep0_ring_virt[XHCI_RING_TRB_COUNT - 1].control =
        (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_TC | XHCI_TRB_C;

    /* 1. Enable Slot Command */
    device->step = 1;
    xhci_trb_t enable_cmd = {0};
    enable_cmd.control = (XHCI_TRB_TYPE_ENABLE_SLOT_CMD << 10);
    xhci_trb_t comp_event = {0};
    if (!send_command(io, ring_dma, enable_cmd, &comp_event)) {
        uint32_t comp_code = (comp_event.status >> 24) & 0xff;
        if (comp_code == 0) {
            device->error_msg = "Enable Slot timeout (no completion event)";
        } else if (comp_code == 9) {
            device->error_msg = "Enable Slot: No Slots Available (code 9)";
        } else {
            device->error_msg = "Enable Slot failed with completion code";
        }
        return false;
    }
    uint8_t slot_id = (comp_event.control >> 24) & 0xff;
    if (!slot_id || slot_id > max_slots) {
        device->error_msg = "Invalid Slot ID returned";
        return false;
    }
    device->slot_id = slot_id;

    /* Register Output Context in DCBAA for this slot */
    memset(dev_dma->output_ctx_virt, 0, 4096);
    dev_dma->dcbaa_virt[slot_id] = dev_dma->output_ctx_phys;
    __asm__ volatile("" ::: "memory");

    /* 2. Setup Input Context for Address Device */
    device->step = 2;
    memset(dev_dma->input_ctx_virt, 0, 4096);
    uint32_t *ctrl_ctx = &dev_dma->input_ctx_virt[0];
    uint32_t *slot_ctx = &dev_dma->input_ctx_virt[1 * ctx_dwords];
    uint32_t *ep0_ctx  = &dev_dma->input_ctx_virt[2 * ctx_dwords];

    ctrl_ctx[1] = (1u << 0) | (1u << 1); /* Add Slot Context & EP0 Context */

    /* Slot Context: Context Entries = 1, Speed = port_speed */
    slot_ctx[0] = ((uint32_t)port_speed << 20) | (1u << 27);
    slot_ctx[1] = (uint32_t)port_num << 16;

    /* EP0 Context: Control Endpoint, CErr = 3, MaxPacketSize based on speed.
    * SuperSpeed always uses 512; USB 2.0 High-Speed uses 64; Full/Low-Speed
    * use 8 (the initial packet size, later raised via Evaluate Context if
    * the device reports a larger bMaxPacketSize0). */
    uint16_t initial_max;
    if (port_speed == XHCI_SPEED_SUPER || port_speed == XHCI_SPEED_SUPER_PLUS) {
        initial_max = 512;
    } else if (port_speed == XHCI_SPEED_HIGH) {
        initial_max = 64;
    } else {
        initial_max = 8;
    }
ep0_ctx[1] = (3u << 1) /* CErr */ | (4u << 3) /* Control */ | ((uint32_t)initial_max << 16);
ep0_ctx[2] = ((uint32_t)dev_dma->ep0_ring_phys & ~0x3fu) | 1u; /* DCS = 1 */
ep0_ctx[3] = (uint32_t)(dev_dma->ep0_ring_phys >> 32);
ep0_ctx[4] = 8; /* Average TRB length */

    /* Issue Address Device Command */
    xhci_trb_t addr_cmd = {0};
    addr_cmd.parameter_low = (uint32_t)dev_dma->input_ctx_phys;
    addr_cmd.parameter_high = (uint32_t)(dev_dma->input_ctx_phys >> 32);
    addr_cmd.control = (XHCI_TRB_TYPE_ADDRESS_DEVICE_CMD << 10) | ((uint32_t)slot_id << 24);

    if (!send_command(io, ring_dma, addr_cmd, &comp_event)) {
        device->error_msg = "Address Device failed";
        return false;
    }

    /* USB 2.0 SetAddress recovery time (TRSTRCY >= 2 ms, wait 20 ms) */
    for (int i = 0; i < 20; ++i) io->delay_ms(io->mmio_ctx);

    /* Read Slot State and EP0 State from Output Context */
    device->slot_state = (uint8_t)((dev_dma->output_ctx_virt[3] >> 27) & 0x1f);
    device->ep0_state = (uint8_t)(dev_dma->output_ctx_virt[1 * ctx_dwords] & 0x7);

    /* 3. Read Device Descriptor (18 bytes for High-Speed, or first 8 bytes if Full-Speed) */
    device->step = 3;
    uint16_t req_len = (port_speed == XHCI_SPEED_FULL ||
                    port_speed == XHCI_SPEED_LOW) ? 8 : 18;
    usb_setup_pkt_t get_dev_desc = {
        .bmRequestType = 0x80, /* IN, Standard, Device */
        .bRequest = USB_REQ_GET_DESCRIPTOR,
        .wValue = (USB_DESC_DEVICE << 8),
        .wIndex = 0,
        .wLength = req_len,
    };

    usb_device_descriptor_t dev_desc = {0};
    if (!control_transfer(io, ring_dma, dev_dma, slot_id, get_dev_desc, &dev_desc, req_len, true, device)) {
        device->error_msg = "Read Device Descriptor failed";
        return false;
    }
    memcpy(device->raw_desc_hdr, &dev_desc, 8);

    if (dev_desc.bLength < 8 || dev_desc.bDescriptorType != USB_DESC_DEVICE) {
        device->error_msg = "Invalid Device Descriptor header";
        return false;
    }

    /* SuperSpeed devices report bMaxPacketSize0 as an exponent (spec
 * requires 9, meaning 2^9 = 512 bytes). USB 2.0 devices report the
 * literal value (8, 16, 32, or 64). Normalize to the actual byte
 * count so downstream code has one interpretation. */
uint8_t raw_max0 = dev_desc.bMaxPacketSize0;
if (port_speed == XHCI_SPEED_SUPER || port_speed == XHCI_SPEED_SUPER_PLUS) {
    if (raw_max0 != 9) {
        device->error_msg = "Invalid bMaxPacketSize0 (SuperSpeed expects 9)";
        return false;
    }
    device->ep0_max_packet = 512;
} else {
    if (raw_max0 != 8 && raw_max0 != 16 && raw_max0 != 32 && raw_max0 != 64) {
        device->error_msg = "Invalid bMaxPacketSize0 in descriptor";
        return false;
    }
    device->ep0_max_packet = raw_max0;   /* 8, 16, 32, or 64 */
}

    /* 4. Evaluate Context if initial packet size was 8 but descriptor declares 64 */
    device->step = 4;
    if (initial_max != device->ep0_max_packet) {
        memset(dev_dma->input_ctx_virt, 0, 4096);
        ctrl_ctx[1] = (1u << 1); /* Add EP0 Context */
        ep0_ctx[1] = (3u << 1) | (4u << 3) | ((uint32_t)device->ep0_max_packet << 16);
        xhci_trb_t eval_cmd = {0};
        eval_cmd.parameter_low = (uint32_t)dev_dma->input_ctx_phys;
        eval_cmd.parameter_high = (uint32_t)(dev_dma->input_ctx_phys >> 32);
        eval_cmd.control = (XHCI_TRB_TYPE_EVAL_CONTEXT_CMD << 10) | ((uint32_t)slot_id << 24);
        send_command(io, ring_dma, eval_cmd, &comp_event);
    }

    if (req_len < 18) {
        get_dev_desc.wLength = 18;
        if (!control_transfer(io, ring_dma, dev_dma, slot_id, get_dev_desc, &dev_desc, 18, true, device)) {
            device->error_msg = "Read full Device Descriptor failed";
            return false;
        }
        if (dev_desc.bLength != 18 || dev_desc.bDescriptorType != USB_DESC_DEVICE) {
            device->error_msg = "Malformed Device Descriptor";
            return false;
        }
    }

    device->vendor_id = dev_desc.idVendor;
    device->product_id = dev_desc.idProduct;

    /* 5. Read Configuration Descriptor */
    device->step = 5;
    usb_setup_pkt_t get_cfg_desc = {
        .bmRequestType = 0x80,
        .bRequest = USB_REQ_GET_DESCRIPTOR,
        .wValue = (USB_DESC_CONFIGURATION << 8),
        .wIndex = 0,
        .wLength = 9,
    };

    uint8_t cfg_hdr[9] = {0};
    bool cfg_ok = control_transfer(io, ring_dma, dev_dma, slot_id, get_cfg_desc, cfg_hdr, 9, true, device);
    memcpy(device->raw_cfg_hdr, cfg_hdr, 9);
    uint16_t total_len = cfg_hdr[2] | ((uint16_t)cfg_hdr[3] << 8);
    device->total_cfg_len = total_len;

#define XHCI_MAX_CFG_DESC_SIZE 2048u

    uint8_t full_cfg[XHCI_MAX_CFG_DESC_SIZE] = {0};

    if (cfg_ok && total_len >= 9 && total_len <= XHCI_MAX_CFG_DESC_SIZE && cfg_hdr[1] == USB_DESC_CONFIGURATION) {
        /* Read full configuration descriptors using the reported length */
        device->step = 6;
        get_cfg_desc.wLength = total_len;
        if (!control_transfer(io, ring_dma, dev_dma, slot_id, get_cfg_desc, full_cfg, total_len, true, device)) {
            device->error_msg = "Read full Configuration Descriptor failed";
            return false;
        }
    } else {
        /* Fallback: Windows-style direct 255-byte configuration descriptor request */
        device->step = 6;
        get_cfg_desc.wLength = 255;
        if (!control_transfer(io, ring_dma, dev_dma, slot_id, get_cfg_desc, full_cfg, 255, true, device)) {
            device->error_msg = "Read Configuration Descriptor (255-byte fallback) failed";
            return false;
        }
        memcpy(device->raw_cfg_hdr, full_cfg, 9);
        total_len = full_cfg[2] | ((uint16_t)full_cfg[3] << 8);
        device->total_cfg_len = total_len;
        if (total_len < 9 || total_len > XHCI_MAX_CFG_DESC_SIZE || full_cfg[1] != USB_DESC_CONFIGURATION) {
            device->error_msg = "Invalid Configuration Descriptor in 255-byte read";
            return false;
        }
    }

    /* 7. Parse interfaces and endpoints */
    device->step = 7;
    bool found_bot_if = false;
    uint16_t off = 0;
    while (off + 2 <= total_len) {
        uint8_t len = full_cfg[off];
        uint8_t type = full_cfg[off + 1];
        if (!len || off + len > total_len) break;

        if (type == USB_DESC_INTERFACE && len >= 9) {
            uint8_t if_class = full_cfg[off + 5];
            uint8_t if_subclass = full_cfg[off + 6];
            uint8_t if_proto = full_cfg[off + 7];
            device->if_class = if_class;
            device->if_subclass = if_subclass;
            device->if_proto = if_proto;
            if (if_class == USB_CLASS_MASS_STORAGE &&
                if_subclass == USB_SUBCLASS_SCSI &&
                if_proto == USB_PROTOCOL_BOT) {
                found_bot_if = true;
            } else if (!device->bulk_in_ep || !device->bulk_out_ep) {
                found_bot_if = false;
            }
        } else if (found_bot_if && type == USB_DESC_ENDPOINT && len >= 7) {
            uint8_t ep_addr = full_cfg[off + 2];
            uint8_t ep_attr = full_cfg[off + 3] & 0x03;
            uint16_t max_packet = full_cfg[off + 4] | ((uint16_t)full_cfg[off + 5] << 8);

            if (ep_attr == 2) { /* Bulk endpoint */
                if (ep_addr & 0x80) {
                    device->bulk_in_ep = ep_addr;
                    device->bulk_in_max_packet = max_packet;
                } else {
                    device->bulk_out_ep = ep_addr;
                    device->bulk_out_max_packet = max_packet;
                }
            }
        }
        off += len;
    }

    if (!device->bulk_in_ep || !device->bulk_out_ep) {
        device->error_msg = "Device is not a supported BOT Mass Storage device";
        xhci_trb_t dis_cmd = {0};
        dis_cmd.control = (XHCI_TRB_TYPE_DISABLE_SLOT_CMD << 10) | ((uint32_t)slot_id << 24);
        send_command(io, ring_dma, dis_cmd, &comp_event);
        return false;
    }

    /* 8. Send SET_CONFIGURATION(1) */
    device->step = 8;
    usb_setup_pkt_t set_cfg = {
        .bmRequestType = 0x00,
        .bRequest = USB_REQ_SET_CONFIGURATION,
        .wValue = 1,
        .wIndex = 0,
        .wLength = 0,
    };

    if (!control_transfer(io, ring_dma, dev_dma, slot_id, set_cfg, NULL, 0, false, device)) {
        device->error_msg = "SET_CONFIGURATION(1) failed";
        xhci_trb_t dis_cmd = {0};
        dis_cmd.control = (XHCI_TRB_TYPE_DISABLE_SLOT_CMD << 10) | ((uint32_t)slot_id << 24);
        send_command(io, ring_dma, dis_cmd, &comp_event);
        return false;
    }

    device->is_valid_bot_storage = true;
    return true;
}
