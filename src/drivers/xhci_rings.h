#ifndef FORTRESS_XHCI_RINGS_H
#define FORTRESS_XHCI_RINGS_H

#include "xhci_trb.h"
#include "spinlock.h"

#define XHCI_RING_TRB_COUNT 256u
#define XHCI_MAX_SLOTS      32u
#define XHCI_MAX_DCIS       32u
#define XHCI_MBOX_DEPTH     32u
#define XHCI_HID_FIFO_CAP   32u
#define XHCI_PUMP_EVENT_BUDGET 32u

typedef struct xhci_rings_io {
    void *mmio_ctx;
    uint32_t mmio_size;
    uint32_t (*read32)(void *ctx, uint32_t offset);
    void (*write32)(void *ctx, uint32_t offset, uint32_t value);
    bool (*delay_ms)(void *ctx);
    /* Optional bounded micro-delay; no sleeps/IRQ enables under FS locks. */
    bool (*delay_us)(void *ctx,unsigned microseconds);
} xhci_rings_io_t;

typedef struct xhci_dma_buffers {
    /* Virtual pointers to 4 KiB buffers */
    xhci_trb_t *cmd_ring_virt;
    xhci_trb_t *event_ring_virt;
    xhci_erst_entry_t *erst_virt;

    /* Physical addresses of 4 KiB buffers */
    uint64_t cmd_ring_phys;
    uint64_t event_ring_phys;
    uint64_t erst_phys;

    /* Ring state tracking */
    uint32_t cmd_enqueue_idx;
    uint32_t cmd_cycle;
    uint32_t event_dequeue_idx;
    uint32_t event_cycle;
} xhci_dma_buffers_t;

typedef struct {
    uint32_t comp_code;
    uint32_t residual;
    uint64_t trb_param;
    uint8_t  slot_id;
    uint8_t  dci;
    bool     event_data;
    bool     valid;
} xhci_event_t;

typedef struct {
    xhci_event_t events[XHCI_MBOX_DEPTH];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint32_t drops;
} xhci_mailbox_t;

typedef struct {
    uint8_t  reports[XHCI_HID_FIFO_CAP][8];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint32_t drops;
} xhci_hid_report_fifo_t;

/* Serializes xHCI Interrupter 0 event ring draining across CPUs (Rank 2 leaf lock) */
extern spinlock_t g_xhci_ring_lock;

/* Serializes xHCI Command Ring submissions across callers (Rank 2 leaf lock) */
extern spinlock_t g_xhci_cmd_lock;

/*
 * Executes Phase 9G.1c ring verification:
 * Sets up Command Ring & Event Ring, configures CRCR & Primary Interrupter,
 * starts the controller, executes a No-Op Command, validates Command Completion Event,
 * and advances the ring indices.
 */
bool xhci_verify_rings(const xhci_rings_io_t *io,
                       xhci_dma_buffers_t *dma,
                       xhci_dump_record_t *dump);

/*
 * Unified Event Ring Pump:
 * Dequeues all pending events from the xHCI event ring, advances ERDP,
 * and routes events into per-(slot, dci) mailboxes, command mailbox, or HID report FIFO.
 * Does NOT call sched_wake_all or submit commands. Leaf lock only.
 */
void xhci_event_ring_pump(const xhci_rings_io_t *io, xhci_dma_buffers_t *dma);

/*
 * Waits for a Transfer Event in mailbox [slot_id][dci] with bounded timeout.
 */
bool xhci_mbox_wait_transfer(const xhci_rings_io_t *io,
                             xhci_dma_buffers_t *dma,
                             uint8_t slot_id,
                             uint8_t dci,
                             uintptr_t submitted_phys,
                             uint32_t *out_residual,
                             uint8_t *out_code,
                             unsigned timeout_us);

/*
 * Waits for a Command Completion Event on the command mailbox with bounded timeout.
 */
bool xhci_mbox_wait_command(const xhci_rings_io_t *io,
                            xhci_dma_buffers_t *dma,
                            uintptr_t submitted_phys,
                            xhci_trb_t *out_event,
                            unsigned timeout_us);

/*
 * Registers an active HID keyboard Interrupt-IN endpoint so the pump can route reports.
 */
void xhci_hid_register_endpoint(uint8_t slot_id, uint8_t dci, uintptr_t report_buf_phys, const uint8_t *report_buf_virt);

/*
 * Pops an 8-byte boot keyboard report from the lock-free FIFO. Returns true on success.
 */
bool xhci_hid_fifo_pop(uint8_t out_report[8]);

/*
 * Atomic report pending counter.
 */
uint32_t xhci_hid_reports_pending(void);

/*
 * Cumulative port-status-change event counter.
 */
uint32_t xhci_port_change_count(void);

/*
 * Resets all mailbox states, FIFOs, and drop counters (for test and controller init).
 */
void xhci_rings_reset_mailboxes(void);

/*
 * Diagnostics / metrics: drops by endpoint and FIFO.
 */
uint32_t xhci_mbox_drop_count(uint8_t slot_id, uint8_t dci);
uint32_t xhci_cmd_mbox_drop_count(void);
uint32_t xhci_hid_fifo_drop_count(void);

#endif /* FORTRESS_XHCI_RINGS_H */
