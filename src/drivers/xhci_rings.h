#ifndef FORTRESS_XHCI_RINGS_H
#define FORTRESS_XHCI_RINGS_H

#include "xhci_trb.h"

#define XHCI_RING_TRB_COUNT 256u

typedef struct {
    void *mmio_ctx;
    uint32_t mmio_size;
    uint32_t (*read32)(void *ctx, uint32_t offset);
    void (*write32)(void *ctx, uint32_t offset, uint32_t value);
    bool (*delay_ms)(void *ctx);
} xhci_rings_io_t;

typedef struct {
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

/* Executes Phase 9G.1c ring verification:
 * Sets up Command Ring & Event Ring, configures CRCR & Primary Interrupter,
 * starts the controller, executes a No-Op Command, validates Command Completion Event,
 * and advances the ring indices.
 * Leaves the controller running for subsequent port reset & device addressing.
 * Fills 'dump' with captured state for thread-context diagnostics.
 * Returns true on success, false on failure or timeout. */
bool xhci_verify_rings(const xhci_rings_io_t *io,
                       xhci_dma_buffers_t *dma,
                       xhci_dump_record_t *dump);

#endif
