#include "usb_mount.h"
#include "string.h"
#include "serial.h"
#include "xhci.h"
#include "ext2.h"
#include "vfs.h"

void usb_mount_parse_cmdline(const char *cmdline, usb_mount_config_t *out_cfg) {
    if (!out_cfg) return;
    memset(out_cfg, 0, sizeof(*out_cfg));
    out_cfg->mode = USB_MOUNT_MODE_RO; /* default ro */

    if (!cmdline || !*cmdline) return;

    const char *p = cmdline;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;

        const char *token_start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
        size_t token_len = (size_t)(p - token_start);

        /* Check usb_data= */
        if (token_len >= 9 && memcmp(token_start, "usb_data=", 9) == 0) {
            const char *val = token_start + 9;
            size_t val_len = token_len - 9;
            if (val_len >= 9 && memcmp(val, "PARTUUID=", 9) == 0) {
                const char *guid_str = val + 9;
                size_t guid_len = val_len - 9;
                if (guid_len == 36 && gpt_str_to_guid(guid_str, &out_cfg->target_guid)) {
                    out_cfg->has_target = true;
                } else {
                    out_cfg->malformed = true;
                }
            } else {
                /* Non-PARTUUID target (labels, names, disks) is invalid */
                out_cfg->malformed = true;
            }
        }
        /* Check usb_data_mode= */
        else if (token_len >= 14 && memcmp(token_start, "usb_data_mode=", 14) == 0) {
            const char *val = token_start + 14;
            size_t val_len = token_len - 14;
            if (val_len == 2 && memcmp(val, "ro", 2) == 0) {
                out_cfg->mode = USB_MOUNT_MODE_RO;
            } else if (val_len == 2 && memcmp(val, "rw", 2) == 0) {
                out_cfg->mode = USB_MOUNT_MODE_RW;
            } else {
                out_cfg->malformed = true;
            }
        }
    }
}

bool usb_mount_production_storage(const boot_info_t *boot_info) {
    if (vfs_lookup("/mnt") != NULL) {
        serial_puts("[USB 9G.3] /mnt is already mounted; skipping USB mount\n");
        return true;
    }

    if (!usb_is_initialized()) {
        serial_puts("[USB 9G.3] No USB mass-storage controller or device available; /mnt left unmounted\n");
        return false;
    }

    usb_mount_config_t cfg;
    usb_mount_parse_cmdline(boot_info ? boot_info->cmdline : NULL, &cfg);

    if (cfg.malformed) {
        serial_puts("[USB 9G.3] Malformed usb_data parameter; /mnt left unmounted\n");
        return false;
    }

    if (!cfg.has_target) {
        serial_puts("[USB 9G.3] No usb_data target configured; /mnt left unmounted\n");
        return false;
    }

    char target_str[40];
    gpt_guid_to_str(&cfg.target_guid, target_str);

    size_t part_count = gpt_get_partition_count();
    gpt_partition_t *matched_part = NULL;
    int match_count = 0;

    for (size_t i = 0; i < part_count; i++) {
        gpt_partition_t *part = gpt_get_partition(i);
        if (!part || !part->parent) continue;

        /* Enforce USB provenance: parent device must be sda */
        if (strcmp(part->parent->name, "sda") != 0) continue;

        if (gpt_guid_equal(&part->unique_guid, &cfg.target_guid)) {
            matched_part = part;
            match_count++;
        }
    }

    if (match_count == 0) {
        serial_puts("[USB 9G.3] Partition PARTUUID=");
        serial_puts(target_str);
        serial_puts(" not found on supported USB storage; /mnt left unmounted\n");
        return false;
    }

    if (match_count > 1) {
        serial_puts("[USB 9G.3] Ambiguous candidates: multiple partitions matched PARTUUID=");
        serial_puts(target_str);
        serial_puts("; /mnt left unmounted\n");
        return false;
    }

    /* Exactly one unique match on supported USB storage */
    gpt_policy_result_t pol = gpt_get_last_policy();
    if (pol == GPT_POLICY_REJECT_INVALID || pol == GPT_POLICY_REJECT_AMBIGUITY) {
        serial_puts("[USB 9G.3] GPT policy rejected disk; /mnt left unmounted\n");
        return false;
    }

    serial_puts("[USB 9G.3] Selected USB device: ");
    serial_puts(matched_part->parent->name);
    serial_puts(", partition: ");
    serial_puts(matched_part->block_dev.name);
    serial_puts(" (PARTUUID=");
    serial_puts(target_str);
    serial_puts(")\n");

    if (cfg.mode == USB_MOUNT_MODE_RW) {
        serial_puts("[USB 9G.3] Mode 'rw' requested, but writable USB persistence deferred to 9G.4; mounting read-only\n");
    }
    serial_puts("[USB 9G.3] Mount mode: read-only\n");

    bool mounted = ext2_mount(&matched_part->block_dev, "/mnt");
    if (mounted) {
        serial_puts("[USB 9G.3] PASS: Mounted ");
        serial_puts(matched_part->block_dev.name);
        serial_puts(" read-only at /mnt\n");
        return true;
    } else {
        serial_puts("[USB 9G.3] FAIL: ext2 mount failed on ");
        serial_puts(matched_part->block_dev.name);
        serial_puts("; /mnt left unmounted\n");
        return false;
    }
}
