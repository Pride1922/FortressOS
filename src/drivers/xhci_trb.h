#ifndef FORTRESS_XHCI_TRB_H
#define FORTRESS_XHCI_TRB_H

#include "types.h"

/* xHCI Transfer Request Block (TRB) - 16 bytes */
typedef struct {
    uint32_t parameter_low;
    uint32_t parameter_high;
    uint32_t status;
    uint32_t control;
} __attribute__((packed)) xhci_trb_t;

/* TRB Types (bits 15:10 of control) */
#define XHCI_TRB_TYPE_NORMAL               1u
#define XHCI_TRB_TYPE_SETUP_STAGE          2u
#define XHCI_TRB_TYPE_DATA_STAGE           3u
#define XHCI_TRB_TYPE_STATUS_STAGE         4u
#define XHCI_TRB_TYPE_LINK                 6u
#define XHCI_TRB_TYPE_ENABLE_SLOT_CMD      9u
#define XHCI_TRB_TYPE_DISABLE_SLOT_CMD     10u
#define XHCI_TRB_TYPE_ADDRESS_DEVICE_CMD   11u
#define XHCI_TRB_TYPE_CONFIG_EP_CMD        12u
#define XHCI_TRB_TYPE_EVAL_CONTEXT_CMD     13u
#define XHCI_TRB_TYPE_RESET_EP_CMD         14u
#define XHCI_TRB_TYPE_NOOP_CMD             23u
#define XHCI_TRB_TYPE_TRANSFER_EVENT       32u
#define XHCI_TRB_TYPE_CMD_COMPLETION_EVENT 33u
#define XHCI_TRB_TYPE_PORT_STATUS_EVENT    34u

/* TRB Control bit flags */
#define XHCI_TRB_C                        (1u << 0)  /* Cycle bit */
#define XHCI_TRB_TC                       (1u << 1)  /* Toggle Cycle bit (Link TRB) */
#define XHCI_TRB_ISP                      (1u << 2)  /* Interrupt on Short Packet */
#define XHCI_TRB_CH                       (1u << 4)  /* Chain bit */
#define XHCI_TRB_IOC                      (1u << 5)  /* Interrupt on Completion */
#define XHCI_TRB_IDT                      (1u << 6)  /* Immediate Data */

#define XHCI_TRB_TYPE_SHIFT               10u
#define XHCI_TRB_TYPE_MASK                (0x3fu << XHCI_TRB_TYPE_SHIFT)

/* Completion Codes (bits 31:24 of status) */
#define XHCI_COMP_INVALID                 0u
#define XHCI_COMP_SUCCESS                 1u
#define XHCI_COMP_DATA_BUFFER_ERROR       2u
#define XHCI_COMP_BABBLE_ERROR            3u
#define XHCI_COMP_USB_TRANSACTION_ERROR   4u
#define XHCI_COMP_TRB_ERROR               5u
#define XHCI_COMP_STALL_ERROR             6u
#define XHCI_COMP_RESOURCE_ERROR          7u
#define XHCI_COMP_BANDWIDTH_ERROR         8u
#define XHCI_COMP_NO_SLOTS_ERROR          9u
#define XHCI_COMP_SHORT_PACKET            13u
#define XHCI_COMP_COMMAND_RING_STOPPED    24u
#define XHCI_COMP_COMMAND_ABORTED         25u

/* Event Ring Segment Table (ERST) Entry - 16 bytes */
typedef struct {
    uint64_t ring_segment_base_address;
    uint32_t ring_segment_size;
    uint32_t reserved;
} __attribute__((packed)) xhci_erst_entry_t;

/* Diagnostic state snapshot for thread-context reporting */
typedef struct {
    bool valid;
    uint32_t usbcmd;
    uint32_t usbsts;
    uint32_t pagesize;
    uint32_t cmd_enqueue_idx;
    uint32_t cmd_cycle_state;
    xhci_trb_t last_submitted_trb;
    uint32_t event_dequeue_idx;
    uint32_t event_cycle_state;
    xhci_trb_t last_completed_trb;
    uint32_t completion_code;
    const char *error_msg;
} xhci_dump_record_t;

#endif
