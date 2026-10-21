#include "xhci_dev.h"
#include <string.h>
#include "serial.h"

#ifndef XHCI_DEV_HOST_TEST
#include "pmm.h"
#include "vmm.h"
#endif

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

static uint32_t s_ep0_idx[XHCI_MAX_SLOTS];
static uint32_t s_ep0_cycle[XHCI_MAX_SLOTS];
static bool     s_ep0_tracking_init = false;

static inline void init_ep0_tracking(void) {
    if (!s_ep0_tracking_init) {
        for (uint32_t i = 0; i < XHCI_MAX_SLOTS; i++) {
            s_ep0_idx[i] = 0;
            s_ep0_cycle[i] = 1;
        }
        s_ep0_tracking_init = true;
    }
}

static inline uintptr_t get_output_ctx_phys(const xhci_dev_dma_t *dma, uint8_t slot_id) {
    if (slot_id < XHCI_MAX_SLOTS && dma->slots_output_ctx_phys[slot_id]) {
        return dma->slots_output_ctx_phys[slot_id];
    }
    return dma->output_ctx_phys;
}

static inline uint32_t *get_output_ctx_virt(const xhci_dev_dma_t *dma, uint8_t slot_id) {
    if (slot_id < XHCI_MAX_SLOTS && dma->slots_output_ctx_virt[slot_id]) {
        return dma->slots_output_ctx_virt[slot_id];
    }
    return dma->output_ctx_virt;
}

static inline uintptr_t get_ep0_ring_phys(const xhci_dev_dma_t *dma, uint8_t slot_id) {
    if (slot_id < XHCI_MAX_SLOTS && dma->slots_ep0_ring_phys[slot_id]) {
        return dma->slots_ep0_ring_phys[slot_id];
    }
    return dma->ep0_ring_phys;
}

static inline xhci_trb_t *get_ep0_ring_virt(const xhci_dev_dma_t *dma, uint8_t slot_id) {
    if (slot_id < XHCI_MAX_SLOTS && dma->slots_ep0_ring_virt[slot_id]) {
        return dma->slots_ep0_ring_virt[slot_id];
    }
    return dma->ep0_ring_virt;
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

/* Submit command to Command Ring and wait for Command Completion Event */
static bool send_command(const xhci_rings_io_t *io,
                         xhci_dma_buffers_t *dma,
                         xhci_trb_t trb,
                         xhci_trb_t *out_event) {
    uint32_t dboff = io->read32(io->mmio_ctx, 0x14);

    uint64_t cmd_flags = spin_lock_irqsave(&g_xhci_cmd_lock);
    uint32_t idx = dma->cmd_enqueue_idx;
    trb.control = (trb.control & ~XHCI_TRB_C) | (dma->cmd_cycle ? XHCI_TRB_C : 0);
    dma->cmd_ring_virt[idx] = trb;
    xhci_clflush_range(&dma->cmd_ring_virt[idx], sizeof(xhci_trb_t));
    __asm__ volatile("mfence" ::: "memory");

    uint64_t submitted_cmd_phys = dma->cmd_ring_phys + idx * sizeof(xhci_trb_t);

    /* Advance enqueue index (Link TRB at index 255) */
    dma->cmd_enqueue_idx++;
    if (dma->cmd_enqueue_idx == XHCI_RING_TRB_COUNT - 1) {
        dma->cmd_ring_virt[XHCI_RING_TRB_COUNT - 1].control =
            (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_TC | (dma->cmd_cycle ? XHCI_TRB_C : 0);
        xhci_clflush_range(&dma->cmd_ring_virt[XHCI_RING_TRB_COUNT - 1], sizeof(xhci_trb_t));
        dma->cmd_enqueue_idx = 0;
        dma->cmd_cycle ^= 1;
    }

    __asm__ volatile("mfence" ::: "memory");
    io->write32(io->mmio_ctx, dboff + 0, 0);
    spin_unlock_irqrestore(&g_xhci_cmd_lock, cmd_flags);

    return xhci_mbox_wait_command(io, dma, submitted_cmd_phys, out_event, 500000);
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

    /* Copy outgoing data if OUT transfer; pre-fill sentinel 0xAA if IN transfer */
    if (!dir_in && length > 0 && data_buf) {
        memcpy(dev_dma->bounce_buf_virt, data_buf, length);
        xhci_clflush_range(dev_dma->bounce_buf_virt, length);
    } else if (dir_in && length > 0) {
        memset(dev_dma->bounce_buf_virt, 0xaa, length);
        xhci_clflush_range(dev_dma->bounce_buf_virt, length);
    }

    init_ep0_tracking();
    uint8_t slot_idx = (slot_id < XHCI_MAX_SLOTS) ? slot_id : 0;
    uintptr_t ep0_phys = get_ep0_ring_phys(dev_dma, slot_id);
    xhci_trb_t *ep0_virt = get_ep0_ring_virt(dev_dma, slot_id);

    if (s_ep0_idx[slot_idx] >= XHCI_RING_TRB_COUNT - 4) {
        s_ep0_idx[slot_idx] = 0;
        s_ep0_cycle[slot_idx] ^= 1;
    }

    /* 1. Setup Stage TRB: TD Size = 1 if Data Stage follows, 0 if no data */
    uint32_t trt = (length > 0) ? (dir_in ? 3 : 2) : 0;
    xhci_trb_t setup_trb = {0};
    memcpy(&setup_trb.parameter_low, &setup, 8);
    setup_trb.status = 8 | ((length > 0 ? 1u : 0u) << 17);
    /* TRT is at bits 17:16 of control; bit 6 is IDT (Immediate Data); bit 5 (IOC) is 0 */
    setup_trb.control = (XHCI_TRB_TYPE_SETUP_STAGE << 10) | (trt << 16) | (1u << 6) /* IDT */ | (s_ep0_cycle[slot_idx] ? 1u : 0);
    ep0_virt[s_ep0_idx[slot_idx]++] = setup_trb;

    /* 2. Data Stage TRB (if length > 0) */
    if (length > 0) {
        xhci_trb_t data_trb = {0};
        data_trb.parameter_low = (uint32_t)dev_dma->bounce_buf_phys;
        data_trb.parameter_high = (uint32_t)(dev_dma->bounce_buf_phys >> 32);
        data_trb.status = length;
        data_trb.control = (XHCI_TRB_TYPE_DATA_STAGE << 10) | (dir_in ? (1u << 16) : 0) | (s_ep0_cycle[slot_idx] ? 1u : 0);
        ep0_virt[s_ep0_idx[slot_idx]++] = data_trb;
    }

    /* 3. Status Stage TRB */
    uintptr_t status_trb_phys = ep0_phys + (s_ep0_idx[slot_idx] * sizeof(xhci_trb_t));
    if (device) {
        device->expected_status_trb=status_trb_phys;
        device->last_comp_code=0;device->last_residual=0;device->last_trb_param=0;
        device->control_wait_ms=0;device->control_failure=1;
    }
    xhci_trb_t status_trb = {0};
    status_trb.control = (XHCI_TRB_TYPE_STATUS_STAGE << 10) | (dir_in ? 0 : (1u << 16)) | (1u << 5) /* IOC */ | (s_ep0_cycle[slot_idx] ? 1u : 0);
    ep0_virt[s_ep0_idx[slot_idx]++] = status_trb;

    /* Flush newly written TRBs from CPU cache */
    uint32_t num_trbs = (length > 0) ? 3 : 2;
    xhci_clflush_range(&ep0_virt[s_ep0_idx[slot_idx] - num_trbs], num_trbs * sizeof(xhci_trb_t));

    __asm__ volatile("mfence" ::: "memory");

    /* Ring Doorbell for Slot ID, Target = 1 */
    io->write32(io->mmio_ctx, dboff + slot_id * 4, 1);

    uint32_t residual = 0;
    uint8_t comp_code = 0;
    bool ok = xhci_mbox_wait_transfer(io, ring_dma, slot_id, 1, status_trb_phys, &residual, &comp_code, 500000);
    if (device) {
        device->last_comp_code = comp_code;
        device->last_residual = residual;
        device->last_trb_param = status_trb_phys;
        device->control_failure = ok ? 0 : 3;
    }
    if (!ok) return false;

    if (dir_in && length > 0 && data_buf) {
        xhci_clflush_range(dev_dma->bounce_buf_virt, length);
        __asm__ volatile("mfence" ::: "memory");
        memcpy(data_buf, dev_dma->bounce_buf_virt, length);
    }
    return true;
}

bool xhci_dev_send_command(const xhci_rings_io_t *io,
                           xhci_dma_buffers_t *dma,
                           xhci_trb_t trb,
                           xhci_trb_t *out_event) {
    return send_command(io, dma, trb, out_event);
}

bool xhci_dev_control_transfer(const xhci_rings_io_t *io,
                               xhci_dma_buffers_t *ring_dma,
                               const xhci_dev_dma_t *dev_dma,
                               uint8_t slot_id,
                               usb_setup_pkt_t setup,
                               void *data_buf,
                               uint16_t length,
                               bool dir_in,
                               xhci_bot_device_t *device) {
    return control_transfer(io, ring_dma, dev_dma, slot_id, setup, data_buf, length, dir_in, device);
}

bool xhci_enumerate_device_topo(const xhci_rings_io_t *io,
                                xhci_dma_buffers_t *ring_dma,
                                const xhci_dev_dma_t *dev_dma,
                                const xhci_device_topology_t *topo,
                                xhci_bot_device_t *device) {
    if (!device) return false;
    *device = (xhci_bot_device_t){0};
    uint8_t port_num = topo ? topo->root_port : 0;
    uint8_t port_speed = topo ? topo->speed : 0;
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
    if (!slot_id || slot_id > max_slots || slot_id >= XHCI_MAX_SLOTS) {
        device->error_msg = "Invalid Slot ID returned";
        return false;
    }
    device->slot_id = slot_id;

    init_ep0_tracking();

#ifndef XHCI_DEV_HOST_TEST
    if (slot_id > 1 && !dev_dma->slots_output_ctx_phys[slot_id]) {
        uintptr_t out_phys = pmm_alloc_page();
        uintptr_t ep0_phys = pmm_alloc_page();
        if (!out_phys || !ep0_phys) {
            if (out_phys) pmm_free_page(out_phys);
            if (ep0_phys) pmm_free_page(ep0_phys);
            device->error_msg = "OOM allocating slot contexts";
            return false;
        }
        ((xhci_dev_dma_t *)dev_dma)->slots_output_ctx_phys[slot_id] = out_phys;
        ((xhci_dev_dma_t *)dev_dma)->slots_output_ctx_virt[slot_id] = (uint32_t *)vmm_phys_to_virt(out_phys);
        ((xhci_dev_dma_t *)dev_dma)->slots_ep0_ring_phys[slot_id] = ep0_phys;
        ((xhci_dev_dma_t *)dev_dma)->slots_ep0_ring_virt[slot_id] = (xhci_trb_t *)vmm_phys_to_virt(ep0_phys);
    }
#endif

    uintptr_t slot_out_phys = get_output_ctx_phys(dev_dma, slot_id);
    uint32_t *slot_out_virt = get_output_ctx_virt(dev_dma, slot_id);
    uintptr_t slot_ep0_phys = get_ep0_ring_phys(dev_dma, slot_id);
    xhci_trb_t *slot_ep0_virt = get_ep0_ring_virt(dev_dma, slot_id);

    s_ep0_idx[slot_id] = 0;
    s_ep0_cycle[slot_id] = 1;

    /* Setup EP0 Ring Link TRB at index 255 */
    memset(slot_ep0_virt, 0, 4096);
    slot_ep0_virt[XHCI_RING_TRB_COUNT - 1].parameter_low = (uint32_t)slot_ep0_phys;
    slot_ep0_virt[XHCI_RING_TRB_COUNT - 1].parameter_high = (uint32_t)(slot_ep0_phys >> 32);
    slot_ep0_virt[XHCI_RING_TRB_COUNT - 1].control =
        (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_TC | XHCI_TRB_C;

    /* Register Output Context in DCBAA for this slot */
    memset(slot_out_virt, 0, 4096);
    dev_dma->dcbaa_virt[slot_id] = slot_out_phys;
    __asm__ volatile("" ::: "memory");

    /* 2. Setup Input Context for Address Device */
    device->step = 2;
    memset(dev_dma->input_ctx_virt, 0, 4096);
    uint32_t *ctrl_ctx = &dev_dma->input_ctx_virt[0];
    uint32_t *slot_ctx = &dev_dma->input_ctx_virt[1 * ctx_dwords];
    uint32_t *ep0_ctx  = &dev_dma->input_ctx_virt[2 * ctx_dwords];

    ctrl_ctx[1] = (1u << 0) | (1u << 1); /* Add Slot Context & EP0 Context */

    /* Slot Context: Context Entries = 1, Speed, Route String, TT fields */
    if (topo) {
        xhci_hub_populate_slot_context(slot_ctx, topo, 1, 0);
    } else {
        slot_ctx[0] = ((uint32_t)port_speed << 20) | (1u << 27);
        slot_ctx[1] = (uint32_t)port_num << 16;
    }

    /* EP0 Context: Control Endpoint, CErr = 3, MaxPacketSize based on speed. */
    uint16_t initial_max;
    if (port_speed == XHCI_SPEED_SUPER || port_speed == XHCI_SPEED_SUPER_PLUS) {
        initial_max = 512;
    } else if (port_speed == XHCI_SPEED_HIGH) {
        initial_max = 64;
    } else {
        initial_max = 8;
    }
    ep0_ctx[1] = (3u << 1) /* CErr */ | (4u << 3) /* Control */ | ((uint32_t)initial_max << 16);
    ep0_ctx[2] = ((uint32_t)slot_ep0_phys & ~0x3fu) | 1u; /* DCS = 1 */
    ep0_ctx[3] = (uint32_t)(slot_ep0_phys >> 32);
    ep0_ctx[4] = 8; /* Average TRB length */

    xhci_clflush_range(dev_dma->input_ctx_virt, 4096);
    __asm__ volatile("mfence" ::: "memory");

    /* Issue Address Device Command */
    xhci_trb_t addr_cmd = {0};
    addr_cmd.parameter_low = (uint32_t)dev_dma->input_ctx_phys;
    addr_cmd.parameter_high = (uint32_t)(dev_dma->input_ctx_phys >> 32);
    addr_cmd.control = (XHCI_TRB_TYPE_ADDRESS_DEVICE_CMD << 10) | ((uint32_t)slot_id << 24);

    if (!send_command(io, ring_dma, addr_cmd, &comp_event)) {
        serial_puts("[USB DEV] Address Device failed: code=");
        serial_print_hex((comp_event.status >> 24) & 0xff);
        serial_puts(" slot_id=");
        serial_print_hex(slot_id);
        serial_puts(" DW0=");
        serial_print_hex(slot_ctx[0]);
        serial_puts(" DW1=");
        serial_print_hex(slot_ctx[1]);
        serial_puts(" DW2=");
        serial_print_hex(slot_ctx[2]);
        serial_puts("\n");
        device->error_msg = "Address Device failed";
        return false;
    }

    /* USB 2.0 SetAddress recovery time (TRSTRCY >= 2 ms, wait 20 ms) */
    for (int i = 0; i < 20; ++i) io->delay_ms(io->mmio_ctx);

    /* Read Slot State and EP0 State from Output Context */
    device->slot_state = (uint8_t)((slot_out_virt[3] >> 27) & 0x1f);
    device->ep0_state = (uint8_t)(slot_out_virt[1 * ctx_dwords] & 0x7);

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
    if (!cfg_ok) {
        /* Completion is uncertain: keep the DMA quarantine contract. A larger
         * descriptor request is not recovery for an unfinished EP0 transfer. */
        device->error_msg = "Read Configuration Descriptor header transfer failed";
        return false;
    }
    memcpy(device->raw_cfg_hdr, cfg_hdr, 9);
    uint16_t total_len = cfg_hdr[2] | ((uint16_t)cfg_hdr[3] << 8);
    device->total_cfg_len = total_len;

#define XHCI_MAX_CFG_DESC_SIZE 2048u

    uint8_t full_cfg[XHCI_MAX_CFG_DESC_SIZE] = {0};

    if (total_len >= 9 && total_len <= XHCI_MAX_CFG_DESC_SIZE && cfg_hdr[1] == USB_DESC_CONFIGURATION) {
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
            uint8_t if_num      = full_cfg[off + 2];
            uint8_t if_class    = full_cfg[off + 5];
            uint8_t if_subclass = full_cfg[off + 6];
            uint8_t if_proto    = full_cfg[off + 7];
            device->if_class    = if_class;
            device->if_subclass = if_subclass;
            device->if_proto    = if_proto;

            found_bot_if = (if_class == USB_CLASS_MASS_STORAGE &&
                            if_subclass == USB_SUBCLASS_SCSI &&
                            if_proto == USB_PROTOCOL_BOT);
            if (if_class == 0x09) { /* Hub */
                device->is_hub = true;
            }
            if (if_class == 0x03 && if_subclass == 0x01 && if_proto == 0x01) { /* HID Boot Keyboard */
                device->is_hid_keyboard = true;
                device->hid_if_num = if_num;
            }
        } else if (found_bot_if && type == USB_DESC_ENDPOINT && len >= 7) {
            uint8_t ep_addr   = full_cfg[off + 2];
            uint8_t ep_attr   = full_cfg[off + 3] & 0x03;
            uint16_t max_packet = full_cfg[off + 4] | ((uint16_t)full_cfg[off + 5] << 8);

            if (ep_attr == 2) {
                if (ep_addr & 0x80) {
                    device->bulk_in_ep = ep_addr;
                    device->bulk_in_max_packet = max_packet;
                } else {
                    device->bulk_out_ep = ep_addr;
                    device->bulk_out_max_packet = max_packet;
                }
            }

            if ((device->speed == XHCI_SPEED_SUPER ||
                 device->speed == XHCI_SPEED_SUPER_PLUS) &&
                off + len + 6 <= total_len &&
                full_cfg[off + len] == 6 && full_cfg[off + len + 1] == 0x30) {
                uint8_t max_burst = full_cfg[off + len + 2] & 0x1F;
                if (ep_addr & 0x80) {
                    device->bulk_in_max_burst = max_burst;
                } else {
                    device->bulk_out_max_burst = max_burst;
                }
            }
        } else if (device->is_hid_keyboard && type == USB_DESC_ENDPOINT && len >= 7) {
            uint8_t ep_addr   = full_cfg[off + 2];
            uint8_t ep_attr   = full_cfg[off + 3] & 0x03;
            uint16_t max_packet = full_cfg[off + 4] | ((uint16_t)full_cfg[off + 5] << 8);
            if (ep_attr == 3 && (ep_addr & 0x80)) { /* Interrupt IN */
                device->int_in_ep = ep_addr;
                device->int_in_max_packet = max_packet;
                device->int_in_interval = full_cfg[off + 6];
            }
        }
        /* Every descriptor advances the walk, including configuration,
         * non-storage interfaces and descriptors we do not recognize. */
        off += len;
    }

    /* Provenance dump: re-walk the config and show what each endpoint did. */
    serial_puts("[USB 9G.1e] Descriptor provenance:\n");
    off = 0;
    while (off + 2 <= total_len) {
        uint8_t len  = full_cfg[off];
        uint8_t type = full_cfg[off + 1];
        if (!len || off + len > total_len) break;

        if (type == USB_DESC_INTERFACE && len >= 9) {
            serial_puts("  iface num=");
            serial_print_hex(full_cfg[off + 2]);
            serial_puts(" alt=");
            serial_print_hex(full_cfg[off + 3]);
            serial_puts(" cls=");
            serial_print_hex(full_cfg[off + 5]);
            serial_puts(" sub=");
            serial_print_hex(full_cfg[off + 6]);
            serial_puts(" proto=");
            serial_print_hex(full_cfg[off + 7]);
            serial_puts("\n");
        } else if (type == USB_DESC_ENDPOINT && len >= 7) {
            serial_puts("    ep addr=");
            serial_print_hex(full_cfg[off + 2]);
            serial_puts(" attr=");
            serial_print_hex(full_cfg[off + 3] & 0x03);
            serial_puts(" maxpacket=");
            serial_print_hex(full_cfg[off + 4] | ((uint16_t)full_cfg[off + 5] << 8));
            serial_puts(" => ");
            serial_puts(full_cfg[off + 2] == device->bulk_in_ep  ? "OVERWROTE IN"  :
                        full_cfg[off + 2] == device->bulk_out_ep ? "OVERWROTE OUT" :
                        "not selected");
            serial_puts("\n");
        }
        off += len;
    }

    if (!device->is_hub && !device->is_hid_keyboard && (!device->bulk_in_ep || !device->bulk_out_ep)) {
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

    if (device->bulk_in_ep && device->bulk_out_ep) {
        device->is_valid_bot_storage = true;
    }
    device->ep0_enqueue_idx = s_ep0_idx[slot_id];
    device->ep0_cycle = (uint8_t)s_ep0_cycle[slot_id];
    return true;
}

bool xhci_enumerate_device(const xhci_rings_io_t *io,
                           xhci_dma_buffers_t *ring_dma,
                           const xhci_dev_dma_t *dev_dma,
                           uint8_t port_num,
                           uint8_t port_speed,
                           xhci_bot_device_t *device) {
    xhci_device_topology_t root_topo = {
        .root_port = port_num,
        .speed = port_speed,
        .tier = 0,
        .route_string = 0,
    };
    return xhci_enumerate_device_topo(io, ring_dma, dev_dma, &root_topo, device);
}
