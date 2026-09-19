#ifndef FORTRESS_USB_MOUNT_H
#define FORTRESS_USB_MOUNT_H

#include "types.h"
#include "boot_info.h"
#include "gpt.h"

typedef enum {
    USB_MOUNT_MODE_RO = 0,
    USB_MOUNT_MODE_RW = 1
} usb_mount_mode_t;

typedef struct {
    bool             has_target;
    bool             malformed;
    gpt_guid_t       target_guid;
    usb_mount_mode_t mode;
} usb_mount_config_t;

void usb_mount_parse_cmdline(const char *cmdline, usb_mount_config_t *out_cfg);
bool usb_mount_production_storage(const boot_info_t *boot_info);

/* Mid-session durability barrier flush.
 * Flushes the block device backing a writable /mnt mount via the durability
 * barrier (SYNCHRONIZE CACHE for sync-backed, barrier check for write-through).
 * Does NOT mark the filesystem clean; does NOT freeze writes.
 * Returns true on success; false if no writable mount is active or barrier fails. */
bool usb_mount_sync(void);

#endif /* FORTRESS_USB_MOUNT_H */
