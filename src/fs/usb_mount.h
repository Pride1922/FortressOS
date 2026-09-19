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

#endif /* FORTRESS_USB_MOUNT_H */
