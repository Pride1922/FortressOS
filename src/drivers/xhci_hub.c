#include "xhci_hub.h"
#include "xhci_dev.h"
#include "usb_hid.h"
#include "serial.h"
#include <string.h>

#define XHCI_TRB_TYPE_CONFIG_EP_CMD   12u

bool xhci_hub_configure_and_enumerate(const xhci_rings_io_t *io,
                                      xhci_dma_buffers_t *ring_dma,
                                      const xhci_dev_dma_t *dev_dma,
                                      const xhci_device_topology_t *hub_topo,
                                      uint8_t slot_id,
                                      xhci_bot_device_t *out_bot_storage,
                                      bool *out_found_storage) {
    if (!io || !ring_dma || !dev_dma || !hub_topo || !slot_id) return false;
    if (hub_topo->tier >= 5) {
        serial_puts("[USB HUB] Max tier depth (5) reached\n");
        return false;
    }

    serial_puts("[USB HUB] Configuring Hub on Slot ");
    serial_print_hex(slot_id);
    serial_puts(" (Tier ");
    serial_print_hex(hub_topo->tier);
    serial_puts(" Speed=");
    serial_print_hex(hub_topo->speed);
    serial_puts(")\n");

    uint8_t num_ports = 0;
    uint8_t pwr_good_2ms = 10;
    uint8_t ttt = 0;

    bool is_ss = (hub_topo->speed == XHCI_SPEED_SUPER || hub_topo->speed == XHCI_SPEED_SUPER_PLUS);

    if (is_ss) {
        usb_setup_pkt_t get_desc = {
            .bmRequestType = 0xA0, /* IN, Class, Device */
            .bRequest = USB_REQ_GET_DESCRIPTOR,
            .wValue = (USB_DESC_TYPE_SS_HUB << 8),
            .wIndex = 0,
            .wLength = sizeof(usb_ss_hub_descriptor_t)
        };
        usb_ss_hub_descriptor_t ss_hub = {0};
        if (!xhci_dev_control_transfer(io, ring_dma, dev_dma, slot_id, get_desc, &ss_hub, sizeof(ss_hub), true, NULL)) {
            serial_puts("[USB HUB] Failed to read SuperSpeed Hub Descriptor\n");
            return false;
        }
        num_ports = ss_hub.bNbrPorts;
        pwr_good_2ms = ss_hub.bPwrOn2PwrGood;

        /* Issue SET_HUB_DEPTH (depth is number of tiers above it; root = 0) */
        uint16_t depth = (hub_topo->tier > 1) ? (hub_topo->tier - 1) : 0;
        usb_setup_pkt_t set_depth = {
            .bmRequestType = 0x20, /* OUT, Class, Device */
            .bRequest = USB_REQ_SET_HUB_DEPTH,
            .wValue = depth,
            .wIndex = 0,
            .wLength = 0
        };
        if (!xhci_dev_control_transfer(io, ring_dma, dev_dma, slot_id, set_depth, NULL, 0, false, NULL)) {
            serial_puts("[USB HUB] SET_HUB_DEPTH failed (continuing)\n");
        }
    } else {
        usb_setup_pkt_t get_desc = {
            .bmRequestType = 0xA0, /* IN, Class, Device */
            .bRequest = USB_REQ_GET_DESCRIPTOR,
            .wValue = (USB_DESC_TYPE_HUB << 8),
            .wIndex = 0,
            .wLength = sizeof(usb_hub_descriptor_t)
        };
        usb_hub_descriptor_t hub_desc = {0};
        if (!xhci_dev_control_transfer(io, ring_dma, dev_dma, slot_id, get_desc, &hub_desc, sizeof(hub_desc), true, NULL)) {
            serial_puts("[USB HUB] Failed to read USB 2.0 Hub Descriptor\n");
            return false;
        }
        num_ports = hub_desc.bNbrPorts;
        pwr_good_2ms = hub_desc.bPwrOn2PwrGood;
        ttt = (hub_desc.wHubCharacteristics >> 5) & 0x03;
    }

    serial_puts("[USB HUB] Ports: ");
    serial_print_hex(num_ports);
    serial_puts(" PwrGood: ");
    serial_print_hex(pwr_good_2ms);
    serial_puts(" TTT: ");
    serial_print_hex(ttt);
    serial_puts("\n");

    /* 2. Configure Endpoint: Update Hub Slot Context (Hub=1, num_ports, TTT, MTT=0) */
    uint32_t hcc1 = io->read32(io->mmio_ctx, 0x10);
    uint32_t ctx_shift = (hcc1 & (1 << 2)) ? 6 : 5;
    uint32_t ctx_dwords = 1u << (ctx_shift - 2);

    memset(dev_dma->input_ctx_virt, 0, 4096);
    uint32_t *ctrl_ctx = &dev_dma->input_ctx_virt[0];
    uint32_t *slot_ctx = &dev_dma->input_ctx_virt[1 * ctx_dwords];

    ctrl_ctx[1] = (1u << 0); /* Add Slot Context A0 */

    xhci_device_topology_t configured_topo = *hub_topo;
    configured_topo.is_hub = true;
    configured_topo.ttt = ttt;

    xhci_hub_populate_slot_context(slot_ctx, &configured_topo, 1, num_ports);

#if defined(__x86_64__) || defined(_M_X64)
    uintptr_t start = (uintptr_t)dev_dma->input_ctx_virt;
    uintptr_t end = start + 4096;
    for (; start < end; start += 64) {
        __asm__ volatile("clflush (%0)" :: "r"(start) : "memory");
    }
    __asm__ volatile("mfence" ::: "memory");
#endif

    xhci_trb_t cfg_cmd = {0};
    cfg_cmd.parameter_low = (uint32_t)dev_dma->input_ctx_phys;
    cfg_cmd.parameter_high = (uint32_t)(dev_dma->input_ctx_phys >> 32);
    cfg_cmd.control = (XHCI_TRB_TYPE_CONFIG_EP_CMD << 10) | ((uint32_t)slot_id << 24);

    xhci_trb_t comp_event = {0};
    if (!xhci_dev_send_command(io, ring_dma, cfg_cmd, &comp_event)) {
        serial_puts("[USB HUB] Configure Endpoint failed for Hub\n");
        return false;
    }

    /* 3. Power on downstream ports */
    for (uint8_t p = 1; p <= num_ports; p++) {
        usb_setup_pkt_t pwr_req = {
            .bmRequestType = 0x23, /* OUT, Class, Other/Port */
            .bRequest = USB_REQ_SET_FEATURE,
            .wValue = HUB_PORT_FEAT_POWER,
            .wIndex = p,
            .wLength = 0
        };
        (void)xhci_dev_control_transfer(io, ring_dma, dev_dma, slot_id, pwr_req, NULL, 0, false, NULL);
    }

    /* 4. Power-good stabilization delay */
    uint32_t delay_ms = (uint32_t)pwr_good_2ms * 2;
    if (delay_ms < 20) delay_ms = 20;
    for (uint32_t i = 0; i < delay_ms; i++) {
        io->delay_ms(io->mmio_ctx);
    }

    /* 5. Scan and enumerate downstream ports */
    for (uint8_t p = 1; p <= num_ports; p++) {
        usb_setup_pkt_t get_port_stat = {
            .bmRequestType = 0xA3, /* IN, Class, Other/Port */
            .bRequest = USB_REQ_GET_STATUS,
            .wValue = 0,
            .wIndex = p,
            .wLength = 4
        };
        uint16_t stat_buf[2] = {0}; /* [0] = status, [1] = change */
        if (!xhci_dev_control_transfer(io, ring_dma, dev_dma, slot_id, get_port_stat, stat_buf, 4, true, NULL)) {
            continue;
        }

        uint16_t port_stat = stat_buf[0];
        if (!(port_stat & HUB_PORT_STAT_CONNECTION)) {
            continue;
        }

        serial_puts("[USB HUB] Device connected on Port ");
        serial_print_hex(p);
        serial_puts(" status=");
        serial_print_hex(port_stat);
        serial_puts("\n");

        /* Reset the downstream port */
        usb_setup_pkt_t reset_port = {
            .bmRequestType = 0x23,
            .bRequest = USB_REQ_SET_FEATURE,
            .wValue = HUB_PORT_FEAT_RESET,
            .wIndex = p,
            .wLength = 0
        };
        (void)xhci_dev_control_transfer(io, ring_dma, dev_dma, slot_id, reset_port, NULL, 0, false, NULL);

        for (int i = 0; i < 50; i++) io->delay_ms(io->mmio_ctx);

        /* Re-read status after reset */
        memset(stat_buf, 0, sizeof(stat_buf));
        (void)xhci_dev_control_transfer(io, ring_dma, dev_dma, slot_id, get_port_stat, stat_buf, 4, true, NULL);
        port_stat = stat_buf[0];

        uint8_t child_speed = XHCI_SPEED_FULL;
        if (is_ss) {
            child_speed = XHCI_SPEED_SUPER;
        } else {
            if (port_stat & HUB_PORT_STAT_LOW_SPEED) {
                child_speed = XHCI_SPEED_LOW;
            } else if (port_stat & HUB_PORT_STAT_HIGH_SPEED) {
                child_speed = XHCI_SPEED_HIGH;
            } else {
                child_speed = XHCI_SPEED_FULL;
            }
        }

        serial_puts("[USB HUB] Port ");
        serial_print_hex(p);
        serial_puts(" reset complete, speed=");
        serial_print_hex(child_speed);
        serial_puts("\n");

        xhci_device_topology_t child_topo = {0};
        xhci_hub_derive_child_topology(&configured_topo, p, child_speed, &child_topo);

        xhci_bot_device_t child_dev = {0};
        bool child_ok = xhci_enumerate_device_topo(io, ring_dma, dev_dma, &child_topo, &child_dev);
        if (!child_ok) {
            serial_puts("[USB HUB] Failed to enumerate device on port ");
            serial_print_hex(p);
            serial_puts(": ");
            if (child_dev.error_msg) serial_puts(child_dev.error_msg);
            serial_puts("\n");
            continue;
        }

        if (child_dev.is_hid_keyboard) {
            usb_hid_configure_keyboard(io, ring_dma, (xhci_dev_dma_t *)dev_dma, &child_dev);
        } else if (child_dev.is_hub) {
            xhci_device_topology_t nested_hub = child_topo;
            nested_hub.slot_id = child_dev.slot_id;
            nested_hub.is_hub = true;
            nested_hub.is_hs_hub = (child_speed == XHCI_SPEED_HIGH);
            xhci_hub_configure_and_enumerate(io, ring_dma, dev_dma, &nested_hub, child_dev.slot_id, out_bot_storage, out_found_storage);
        } else if (child_dev.is_valid_bot_storage && out_bot_storage && out_found_storage) {
            if (!*out_found_storage) {
                *out_bot_storage = child_dev;
                *out_found_storage = true;
            }
        }
    }

    return true;
}
