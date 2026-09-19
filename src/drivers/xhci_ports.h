#ifndef FORTRESS_XHCI_PORTS_H
#define FORTRESS_XHCI_PORTS_H

#include "xhci_rings.h"

#define XHCI_MAX_ROOT_PORTS 64u

/* PORTSC bit definitions */
#define XHCI_PORTSC_CCS          (1u << 0)   /* Current Connect Status */
#define XHCI_PORTSC_PED          (1u << 1)   /* Port Enabled/Disabled (RW1C to disable) */
#define XHCI_PORTSC_OCA          (1u << 3)   /* Over-Current Active */
#define XHCI_PORTSC_PR           (1u << 4)   /* Port Reset */
#define XHCI_PORTSC_PLS_SHIFT    5u
#define XHCI_PORTSC_PLS_MASK     (0xfu << XHCI_PORTSC_PLS_SHIFT)
#define XHCI_PORTSC_PP           (1u << 9)   /* Port Power */
#define XHCI_PORTSC_SPEED_SHIFT  10u
#define XHCI_PORTSC_SPEED_MASK   (0xfu << XHCI_PORTSC_SPEED_SHIFT)
#define XHCI_PORTSC_CSC          (1u << 17)  /* Connect Status Change (RW1C) */
#define XHCI_PORTSC_PEC          (1u << 18)  /* Port Enable/Disable Change (RW1C) */
#define XHCI_PORTSC_WRC          (1u << 19)  /* Warm Port Reset Change (RW1C) */
#define XHCI_PORTSC_OCC          (1u << 20)  /* Over-Current Change (RW1C) */
#define XHCI_PORTSC_PRC          (1u << 21)  /* Port Reset Change (RW1C) */
#define XHCI_PORTSC_PLC          (1u << 22)  /* Port Link State Change (RW1C) */
#define XHCI_PORTSC_CEC          (1u << 23)  /* Config Error Change (RW1C) */

/* Mask of bits to clear when writing to PORTSC so we don't accidentally clear RW1C bits or PED */
#define XHCI_PORTSC_RW1C_MASK    (0x7fu << 17)
#define XHCI_PORTSC_WRITE_MASK   ~(XHCI_PORTSC_PED | XHCI_PORTSC_RW1C_MASK)

/* Protocol Speeds */
#define XHCI_SPEED_FULL          1u  /* 12 Mb/s Full-Speed */
#define XHCI_SPEED_LOW           2u  /* 1.5 Mb/s Low-Speed */
#define XHCI_SPEED_HIGH          3u  /* 480 Mb/s High-Speed */
#define XHCI_SPEED_SUPER         4u  /* 5 Gb/s SuperSpeed */
#define XHCI_SPEED_SUPER_PLUS    5u  /* 10 Gb/s SuperSpeedPlus (USB 3.1) */

typedef struct {
    uint8_t port_num;       /* 1-based port index */
    uint8_t protocol_major; /* 2 = USB 2.0, 3 = USB 3.x */
    bool connected;
    bool enabled;
    uint8_t speed;          /* XHCI_SPEED_* */
    uint32_t raw_portsc;
} xhci_port_info_t;

typedef struct {
    uint32_t total_ports;
    uint32_t usb2_port_count;
    uint32_t usb3_port_count;
    uint32_t connected_count;
    xhci_port_info_t ports[XHCI_MAX_ROOT_PORTS];
} xhci_port_report_t;

/* Discovers protocol mapping, inspects all root ports, performs bounded reset
 * on attached USB 2.0 ports, drains port status events from Event Ring,
 * and populates report. */
bool xhci_discover_and_reset_ports(const xhci_rings_io_t *io,
                                   const xhci_dma_buffers_t *dma,
                                   xhci_port_report_t *report);

#endif
