#ifndef FORTRESS_XHCI_H
#define FORTRESS_XHCI_H
#include "boot_info.h"
#include "block.h"

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

#endif
