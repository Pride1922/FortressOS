#ifndef FORTRESS_USB_HID_H
#define FORTRESS_USB_HID_H

#include <stdint.h>
#include <stdbool.h>
#include "xhci_rings.h"
#include "xhci_dev.h"
#include "hid_kbd.h"

#define USB_HID_MAX_KEYBOARDS 4

typedef struct {
    bool active;
    uint8_t slot_id;
    uint8_t dci;
    uint8_t if_num;
    uint8_t interval;
    uint16_t max_packet;
    uintptr_t ring_phys;
    xhci_trb_t *ring_virt;
    uint32_t ring_idx;
    uint8_t ring_cycle;
    uintptr_t report_buf_phys;
    uint8_t *report_buf_virt;
    hid_boot_kbd_report_t prev_report;
} usb_hid_kbd_t;

/* Initialize HID keyboard subsystem */
void usb_hid_init(void);

/* Configure and register a discovered HID boot keyboard device */
bool usb_hid_configure_keyboard(const xhci_rings_io_t *io,
                               xhci_dma_buffers_t *ring_dma,
                               xhci_dev_dma_t *dev_dma,
                               const xhci_bot_device_t *device);

/* Poll for HID reports and pump event ring (thread-safe, callable from BSP worker or test) */
void usb_hid_poll(const xhci_rings_io_t *io, xhci_dma_buffers_t *ring_dma);

/* Start the BSP HID worker thread */
bool usb_hid_start_worker(const xhci_rings_io_t *io, xhci_dma_buffers_t *ring_dma);

/* Check if any HID device is actively configured */
bool usb_hid_is_active(void);

#endif /* FORTRESS_USB_HID_H */
