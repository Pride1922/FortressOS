#ifndef FORTRESS_XHCI_H
#define FORTRESS_XHCI_H
#include "boot_info.h"
#include "block.h"
#include "xhci_bot.h"

#define XHCI_MAX_CONTROLLERS 4

typedef struct {
    xhci_rings_io_t    rings_io;
    xhci_dma_buffers_t dma;
    xhci_dev_dma_t     dev_dma;
    xhci_bot_rings_t   bot_rings;
    xhci_bot_error_t   flush_error;
    xhci_dump_record_t dump_record;
} xhci_controller_t;

/* One-shot 9G.1b boot probe, after PCI/input initialization, with IRQs and
 * preemption disabled and no locks held. Uses PIT channel 2 for bounded waits.
 * Sizes/restores BAR0 with decode disabled, maps UC/NX MMIO, requests firmware
 * ownership, halts/resets. Leaves bus mastering disabled and controller stopped.
 * No DMA allocation, rings, interrupts, port enumeration or USB I/O. Errors
 * report in boot thread context and return to the caller; never halt the OS. */
void xhci_boot_probe(const boot_info_t *boot_info);

/* Bounded diagnostic callable only from thread context, with no spinlock held.
 * Prints preallocated snapshot if pending or on demand. */
void usb_dump_state(void);

/* Block device operations for USB Mass Storage ("sda") */
bool usb_is_initialized(void);
uint32_t usb_get_sector_size(void);
uint64_t usb_get_sector_count(void);
bool usb_block_read(block_dev_t *dev, uint64_t lba, void *buf);
bool usb_block_write(block_dev_t *dev, uint64_t lba, const void *buf);
bool usb_block_flush(block_dev_t *dev);
/* Boot/thread context only, with no subsystem/console lock held.
 * Consumes a copied flush failure; never reads controller or DMA memory. */
void usb_report_flush_failure(void);

/* Returns current USB durability mode (USB_DURABILITY_UNKNOWN if not initialized).
 * Thread context only; no lock held. */
usb_durability_mode_t usb_get_durability_mode(void);

#endif
