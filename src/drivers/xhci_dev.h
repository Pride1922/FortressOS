#ifndef FORTRESS_XHCI_DEV_H
#define FORTRESS_XHCI_DEV_H

#include "xhci_ports.h"

/* USB Standard Request Types */
#define USB_REQ_GET_STATUS        0x00
#define USB_REQ_CLEAR_FEATURE     0x01
#define USB_REQ_SET_FEATURE       0x03
#define USB_REQ_SET_ADDRESS       0x05
#define USB_REQ_GET_DESCRIPTOR    0x06
#define USB_REQ_SET_DESCRIPTOR    0x07
#define USB_REQ_GET_CONFIGURATION 0x08
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_GET_INTERFACE     0x0A
#define USB_REQ_SET_INTERFACE     0x0B

/* USB Descriptor Types */
#define USB_DESC_DEVICE           0x01
#define USB_DESC_CONFIGURATION    0x02
#define USB_DESC_STRING           0x03
#define USB_DESC_INTERFACE        0x04
#define USB_DESC_ENDPOINT         0x05

/* USB Mass Storage Class/Subclass/Protocol */
#define USB_CLASS_MASS_STORAGE    0x08
#define USB_SUBCLASS_SCSI         0x06
#define USB_PROTOCOL_BOT          0x50

/* USB Standard Setup Packet (8 bytes) */
typedef struct {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed)) usb_setup_pkt_t;

/* USB Standard Device Descriptor (18 bytes) */
typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed)) usb_device_descriptor_t;

/* Validated BOT Mass Storage Device information passed to 9G.2 */
typedef struct {
    uint8_t  slot_id;
    uint8_t  port_num;
    uint8_t  speed;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t ep0_max_packet;   /* EP0 max packet size in bytes; 512 for SuperSpeed */
    uint8_t  bulk_in_ep;
    uint16_t bulk_in_max_packet;
    uint8_t  bulk_out_ep;
    uint16_t bulk_out_max_packet;
    bool     is_valid_bot_storage;
    const char *error_msg;

    /* Diagnostics for hardware bring-up and troubleshooting */
    uint8_t  raw_desc_hdr[8];
    uint8_t  raw_cfg_hdr[9];
    uint16_t total_cfg_len;
    uint8_t  step;
    uint8_t  if_class;
    uint8_t  if_subclass;
    uint8_t  if_proto;
    uint8_t  slot_state;
    uint8_t  ep0_state;
    uint32_t last_comp_code;
    uint32_t last_residual;
    uint64_t last_trb_param;
} xhci_bot_device_t;

/* Dynamic memory allocated for device management */
typedef struct {
    uintptr_t dcbaa_phys;
    uint64_t *dcbaa_virt;

    uintptr_t scratchpad_array_phys;
    uint64_t *scratchpad_array_virt;
    uintptr_t scratchpad_pages[128];
    uint32_t  scratchpad_count;

    uintptr_t input_ctx_phys;
    uint32_t *input_ctx_virt;

    uintptr_t output_ctx_phys;
    uint32_t *output_ctx_virt;

    uintptr_t ep0_ring_phys;
    xhci_trb_t *ep0_ring_virt;

    uintptr_t bounce_buf_phys;
    uint8_t  *bounce_buf_virt;
} xhci_dev_dma_t;

/* Performs Phase 9G.1e:
 * - Issues Enable Slot -> acquires Slot ID
 * - Registers Output Context in DCBAA
 * - Issues Address Device
 * - Reads initial 8 bytes of device descriptor (extracts bMaxPacketSize0)
 * - Evaluates Context if EP0 max packet size differs from initial speed default
 * - Reads full 18-byte device descriptor (records vendor & product ID)
 * - Reads configuration descriptor (validates BOT Mass Storage class 0x08 / 0x06 / 0x50)
 * - Locates Bulk IN and Bulk OUT endpoints
 * - Issues SET_CONFIGURATION(1)
 * Populates device info on success. */
bool xhci_enumerate_device(const xhci_rings_io_t *io,
                           xhci_dma_buffers_t *ring_dma,
                           const xhci_dev_dma_t *dev_dma,
                           uint8_t port_num,
                           uint8_t port_speed,
                           xhci_bot_device_t *device);

#endif
