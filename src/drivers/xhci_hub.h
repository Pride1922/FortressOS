#ifndef FORTRESS_XHCI_HUB_H
#define FORTRESS_XHCI_HUB_H

#include "types.h"
#include "xhci_rings.h"

/* USB Hub Class */
#define USB_CLASS_HUB                   0x09
#define USB_SUBCLASS_HUB                0x00
#define USB_PROTO_HUB_FULLSPEED         0x00
#define USB_PROTO_HUB_HIGHSPEED_SINGLE  0x01
#define USB_PROTO_HUB_HIGHSPEED_MULTI   0x02
#define USB_PROTO_HUB_SUPERSPEED        0x03

/* Hub Class Requests */
#define USB_REQ_GET_STATUS              0x00
#define USB_REQ_CLEAR_FEATURE           0x01
#define USB_REQ_SET_FEATURE             0x03
#define USB_REQ_GET_DESCRIPTOR          0x06
#define USB_REQ_SET_HUB_DEPTH           12

/* Hub Descriptor Types */
#define USB_DESC_TYPE_HUB               0x29
#define USB_DESC_TYPE_SS_HUB            0x2A

/* Hub Port Feature Selectors */
#define HUB_PORT_FEAT_CONNECTION        0
#define HUB_PORT_FEAT_ENABLE            1
#define HUB_PORT_FEAT_SUSPEND           2
#define HUB_PORT_FEAT_OVER_CURRENT      3
#define HUB_PORT_FEAT_RESET             4
#define HUB_PORT_FEAT_POWER             8
#define HUB_PORT_FEAT_LOW_SPEED         9
#define HUB_PORT_FEAT_C_CONNECTION      16
#define HUB_PORT_FEAT_C_ENABLE          17
#define HUB_PORT_FEAT_C_SUSPEND         18
#define HUB_PORT_FEAT_C_OVER_CURRENT    19
#define HUB_PORT_FEAT_C_RESET           20

/* Port Status Bits (from GET_PORT_STATUS) */
#define HUB_PORT_STAT_CONNECTION        (1u << 0)
#define HUB_PORT_STAT_ENABLE            (1u << 1)
#define HUB_PORT_STAT_SUSPEND           (1u << 2)
#define HUB_PORT_STAT_OVER_CURRENT      (1u << 3)
#define HUB_PORT_STAT_RESET             (1u << 4)
#define HUB_PORT_STAT_POWER             (1u << 8)
#define HUB_PORT_STAT_LOW_SPEED         (1u << 9)
#define HUB_PORT_STAT_HIGH_SPEED        (1u << 10)

/* SuperSpeed Port Status Bits */
#define HUB_SS_PORT_STAT_CONNECTION     (1u << 0)
#define HUB_SS_PORT_STAT_ENABLE         (1u << 1)
#define HUB_SS_PORT_STAT_OVER_CURRENT   (1u << 3)
#define HUB_SS_PORT_STAT_RESET          (1u << 4)
#define HUB_SS_PORT_STAT_LINK_STATE_SHIFT 5
#define HUB_SS_PORT_STAT_POWER          (1u << 9)
#define HUB_SS_PORT_STAT_SPEED_5GBPS    (0u << 10)

/* USB 2.0 Hub Descriptor (variable length up to 71 bytes for 255 ports) */
typedef struct {
    uint8_t  bDescLength;
    uint8_t  bDescriptorType;
    uint8_t  bNbrPorts;
    uint16_t wHubCharacteristics;
    uint8_t  bPwrOn2PwrGood;
    uint8_t  bHubContrCurrent;
    uint8_t  deviceRemovable[8];
} __attribute__((packed)) usb_hub_descriptor_t;

/* SuperSpeed Hub Descriptor (12 bytes) */
typedef struct {
    uint8_t  bDescLength;
    uint8_t  bDescriptorType;
    uint8_t  bNbrPorts;
    uint16_t wHubCharacteristics;
    uint8_t  bPwrOn2PwrGood;
    uint8_t  bHubContrCurrent;
    uint8_t  bHubHdrDecLat;
    uint16_t wHubDelay;
    uint16_t DeviceRemovable;
} __attribute__((packed)) usb_ss_hub_descriptor_t;

/*
 * Computes the 20-bit Route String for a device behind an xHCI hub tier.
 * tier: 1..5 (tier 1 = directly plugged into root hub, tier 2 = plugged into tier 1 hub, etc.)
 * port: downstream port number on that hub (1..15; ports > 14 clamp to 0xF per xHCI spec 4.3).
 */
static inline uint32_t xhci_hub_calc_route_string(uint32_t parent_route, uint8_t tier, uint8_t port) {
    if (tier < 1 || tier > 5 || port == 0) return parent_route;
    uint32_t nibble = (port > 14) ? 0xFu : ((uint32_t)port & 0xFu);
    uint32_t shift = (tier - 1) * 4;
    return (parent_route & ~(0xFu << shift)) | (nibble << shift);
}

/*
 * Hub topology node tracked in software for TT derivation and routing.
 */
typedef struct {
    uint8_t  slot_id;
    uint8_t  root_port;
    uint8_t  speed;
    bool     is_hub;
    bool     is_hs_hub;
    uint8_t  tier;
    uint32_t route_string;
    uint8_t  parent_slot_id;
    uint8_t  parent_port_num;
    uint8_t  tt_slot_id;
    uint8_t  tt_port_num;
    uint8_t  ttt; /* TT Think Time */
} xhci_device_topology_t;

/*
 * Derives child topology attributes given a parent hub and downstream port.
 */
static inline void xhci_hub_derive_child_topology(const xhci_device_topology_t *parent_hub,
                                                  uint8_t downstream_port,
                                                  uint8_t child_speed,
                                                  xhci_device_topology_t *child) {
    if (!parent_hub || !child) return;
    child->root_port = parent_hub->root_port;
    child->parent_slot_id = parent_hub->slot_id;
    child->parent_port_num = downstream_port;
    child->tier = parent_hub->tier + 1;
    child->route_string = xhci_hub_calc_route_string(parent_hub->route_string, child->tier, downstream_port);
    child->speed = child_speed;
    child->ttt = 0;

    /* TT inheritance rules (xHCI spec 4.3.3):
     * TT fields MUST be programmed in Slot Context DW2 ONLY for LS/FS devices behind a HS hub.
     * 1. If child is Low-Speed (2) or Full-Speed (1):
     *    - If parent hub is High-Speed, this parent hub provides the TT!
     *    - If parent hub already has a TT (e.g. nested downstream hub), inherit it!
     * 2. If child is High-Speed (3) or SuperSpeed (4/5): no TT needed. */
    if (child_speed == 1 || child_speed == 2) { /* Full or Low speed */
        if (parent_hub->tt_slot_id != 0) {
            child->tt_slot_id = parent_hub->tt_slot_id;
            child->tt_port_num = parent_hub->tt_port_num;
            child->ttt = parent_hub->ttt;
        } else if (parent_hub->is_hs_hub) {
            child->tt_slot_id = parent_hub->slot_id;
            child->tt_port_num = downstream_port;
            child->ttt = parent_hub->ttt;
        } else {
            child->tt_slot_id = 0;
            child->tt_port_num = 0;
        }
    } else {
        child->tt_slot_id = 0;
        child->tt_port_num = 0;
    }
}

/*
 * Populates xHCI Slot Context DW0..DW3 with verified hardware fields.
 * NOTE (xHCI spec 6.2.2):
 * DW0: Route String (19:0), Speed (23:20), MTT (25 = 0), Hub (26), Context Entries (31:27)
 * DW1: Root Port Number (23:16), Number of Ports (31:24)
 * DW2: TT Hub Slot ID (7:0), TT Port Number (15:8), TTT (17:16), Interrupter (31:22)
 * DW3: Device Address (7:0), Slot State (31:27)
 */
static inline void xhci_hub_populate_slot_context(uint32_t *slot_ctx,
                                                 const xhci_device_topology_t *topo,
                                                 uint8_t context_entries,
                                                 uint8_t num_hub_ports) {
    if (!slot_ctx || !topo) return;

    /* DW0: Route String, Speed, Hub flag, Context Entries. MTT is 0 by default. */
    slot_ctx[0] = (topo->route_string & 0x000FFFFFu) |
                  (((uint32_t)topo->speed & 0xFu) << 20) |
                  (topo->is_hub ? (1u << 26) : 0u) |
                  (((uint32_t)context_entries & 0x1Fu) << 27);

    /* DW1: Root Hub Port Number, Number of Downstream Ports (for hubs) */
    slot_ctx[1] = (((uint32_t)topo->root_port & 0xFFu) << 16) |
                  (topo->is_hub ? (((uint32_t)num_hub_ports & 0xFFu) << 24) : 0u);

    /* DW2: TT Hub Slot ID (bits 7:0), TT Port Number (bits 15:8), TTT (bits 17:16) */
    slot_ctx[2] = ((uint32_t)topo->tt_slot_id & 0xFFu) |
                  (((uint32_t)topo->tt_port_num & 0xFFu) << 8) |
                  (((uint32_t)topo->ttt & 0x3u) << 16);

    /* DW3: Initialized to 0 (hardware writes Device Address and Slot State) */
    slot_ctx[3] = 0;
}

struct xhci_dev_dma;
typedef struct xhci_dev_dma xhci_dev_dma_t;
struct xhci_bot_device;
typedef struct xhci_bot_device xhci_bot_device_t;

bool xhci_hub_configure_and_enumerate(const xhci_rings_io_t *io,
                                      xhci_dma_buffers_t *ring_dma,
                                      const xhci_dev_dma_t *dev_dma,
                                      const xhci_device_topology_t *hub_topo,
                                      uint8_t slot_id,
                                      xhci_bot_device_t *out_bot_storage,
                                      bool *out_found_storage);

#endif /* FORTRESS_XHCI_HUB_H */
