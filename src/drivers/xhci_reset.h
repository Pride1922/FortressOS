#ifndef FORTRESS_XHCI_RESET_H
#define FORTRESS_XHCI_RESET_H
#include "types.h"

/* Internal register transport: host tests execute the same reset state machine
 * against a labelled mock. No callbacks allocate, log, sleep or enable IRQs. */
typedef struct {
    void *context;
    uint32_t size;
    uint32_t (*read)(void *, uint32_t);
    void (*write)(void *, uint32_t, uint32_t);
    bool (*delay_ms)(void *);
} xhci_reset_io_t;

typedef struct {
    uint32_t cap, hcs1, hcs2, hcc1, dboff, rtsoff;
    uint32_t command, status, pagesize, legacy;
    uint32_t failed_offset, last_value;
    const char *error;
} xhci_reset_result_t;

/* All offsets validated against the sized aperture before access. On failure
 * only returns captured data; caller owns reporting and PCI containment. */
bool xhci_reset_controller(const xhci_reset_io_t *io, xhci_reset_result_t *result);
#endif
