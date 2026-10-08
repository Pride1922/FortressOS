#include "usb_mount.h"
#include "string.h"
#include "serial.h"
#include "xhci.h"
#include "ext2.h"
#include "ext4.h"
#include "vfs.h"

/* Cached mounted block device for normal mid-session sync. Set only when a
 * writable mount succeeds; cleared on failure. Never freed or reallocated. */
static block_dev_t *s_mounted_rw_dev = NULL;
static ext4_mount_t *s_ext4_mount;

/* Probe only the selected partition. Full feature/geometry validation remains
 * the filesystem's admission responsibility; extents never fall back to ext2. */
static int usb_mount_format(block_dev_t *dev) {
    uint8_t sector[4096];
    if (!dev || (dev->sector_size != 512 && dev->sector_size != 4096)) return -1;
    uint64_t lba = (1024 + 56) / dev->sector_size;
    if (!block_read_sector(dev, lba, sector)) return -1;
    size_t base = 1024 % dev->sector_size;
    /* The required superblock prefix fits one sector for both geometries. */
    if (sector[base+56] != 0x53 || sector[base+57] != 0xef) return -1;
    uint32_t incompat = (uint32_t)sector[base+96] |
        ((uint32_t)sector[base+97]<<8) | ((uint32_t)sector[base+98]<<16) |
        ((uint32_t)sector[base+99]<<24);
    if (incompat & 0x40) {
        uint32_t compat=(uint32_t)sector[base+92] |
            ((uint32_t)sector[base+93]<<8) | ((uint32_t)sector[base+94]<<16) |
            ((uint32_t)sector[base+95]<<24);
        return (compat & 4u) ? 5 : 4; /* E4-B journal vs accepted E4-A. */
    }
    uint32_t compat = (uint32_t)sector[base+92] |
        ((uint32_t)sector[base+93]<<8) | ((uint32_t)sector[base+94]<<16) |
        ((uint32_t)sector[base+95]<<24);
    uint32_t ro = (uint32_t)sector[base+100] |
        ((uint32_t)sector[base+101]<<8) | ((uint32_t)sector[base+102]<<16) |
        ((uint32_t)sector[base+103]<<24);
    /* Same legacy profile as ext2 RO admission. In particular an ext3 journal
     * must not enter the legacy RW API, which does not check compat features. */
    return (compat & ~0x38u) || (incompat & ~2u) || (ro & ~3u) ? -1 : 2;
}

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

    int format = usb_mount_format(&matched_part->block_dev);
    if (format < 0) {
        serial_puts("[USB 9G.3] Invalid filesystem or probe I/O failure; /mnt left unmounted\n");
        return false;
    }
    serial_puts(format == 5 ? "[USB E4-B] Selected filesystem: ext4 journaled\n" :
                format == 4 ? "[USB E4-A] Selected filesystem: ext4\n" :
                             "[USB 9G.3] Selected filesystem: ext2\n");

    /* Determine RW eligibility using the durability state machine (9G.4).
     * The durability mode was probed during boot by xhci_bot_probe_durability(). */
    bool rw_eligible = false;
    if (cfg.mode == USB_MOUNT_MODE_RW) {
        usb_durability_mode_t dur = usb_get_durability_mode();

        /* GPT must be strictly consistent for RW (degraded = backup GPT invalid) */
        if (pol != GPT_POLICY_PRIMARY_CONSISTENT) {
            serial_puts("[USB 9G.4] GPT policy not strictly consistent; RW not eligible\n");
        } else if (!matched_part->block_dev.write_sector || !matched_part->block_dev.flush) {
            serial_puts("[USB 9G.4] Device missing write or flush capability; RW not eligible\n");
        } else if (dur == USB_DURABILITY_UNKNOWN) {
            serial_puts("[USB 9G.4] Cache durability unknown; RW not eligible (fallback to RO)\n");
        } else if (dur == USB_DURABILITY_READ_ONLY) {
            serial_puts("[USB 9G.4] Device classified read-only by durability probe; RW not eligible\n");
        } else {
            /* SYNC_BACKED, WRITE_THROUGH, or ASSUMED_WRITE_THROUGH: run flush preflight
             * to verify the barrier path works before any filesystem writes are permitted. */
            if (!block_flush(&matched_part->block_dev)) {
                serial_puts("[USB 9G.4] Flush preflight failed before filesystem writes; RW not eligible\n");
                usb_report_flush_failure();
            } else {
                serial_puts("[USB 9G.4] Flush preflight passed\n");
                rw_eligible = true;
            }
        }

        /* Log selected identity and durability mode */
        serial_puts("[USB 9G.4] USB device: ");
        serial_puts(matched_part->parent->name);
        serial_puts(", PARTUUID=");
        serial_puts(target_str);
        serial_puts(", durability=");
        switch (dur) {
            case USB_DURABILITY_SYNC_BACKED:           serial_puts("sync-backed"); break;
            case USB_DURABILITY_WRITE_THROUGH:         serial_puts("write-through"); break;
            case USB_DURABILITY_ASSUMED_WRITE_THROUGH: serial_puts("assumed-write-through"); break;
            case USB_DURABILITY_READ_ONLY:             serial_puts("read-only"); break;
            default:                                   serial_puts("unknown"); break;
        }
        serial_puts("\n");
    }

    if (rw_eligible) {
        bool mounted = format == 5 ?
            ext4_mount_journal_rw(&matched_part->block_dev,"/mnt",&s_ext4_mount) == 0 : format == 4 ?
            ext4_mount_rw(&matched_part->block_dev, "/mnt", &s_ext4_mount) == 0 :
            ext2_mount_rw(&matched_part->block_dev, "/mnt");
        if (mounted) {
            s_mounted_rw_dev = &matched_part->block_dev;
            serial_puts("[USB 9G.4] Mount mode: read-write\n");
            serial_puts("[USB 9G.4] PASS: Mounted ");
            serial_puts(matched_part->block_dev.name);
            serial_puts(" read-write at /mnt\n");
            return true;
        } else {
            if (format == 5) {
                serial_puts("[USB E4-B] Journal recovery/mount failed; /mnt left unmounted\n");
                return false; /* Recovery may have written: never hide failure as RO. */
            }
            usb_report_flush_failure();
            serial_puts(format == 4 ? "[USB E4-A] FAIL: ext4 writable mount failed on " :
                                      "[USB 9G.4] FAIL: ext2 writable mount failed on ");
            serial_puts(matched_part->block_dev.name);
            serial_puts("; attempting read-only fallback\n");
        }
    }

    serial_puts("[USB 9G.3] Mount mode: read-only\n");

    bool mounted = format >= 4 ?
        ext4_mount_ro(&matched_part->block_dev, "/mnt", &s_ext4_mount) == 0 :
        ext2_mount(&matched_part->block_dev, "/mnt");
    if (mounted) {
        serial_puts("[USB 9G.3] PASS: Mounted ");
        serial_puts(matched_part->block_dev.name);
        serial_puts(" read-only at /mnt\n");
        return true;
    } else {
        serial_puts(format == 4 ? "[USB E4-A] FAIL: ext4 mount failed on " :
                                  "[USB 9G.3] FAIL: ext2 mount failed on ");
        serial_puts(matched_part->block_dev.name);
        serial_puts("; /mnt left unmounted\n");
        return false;
    }
}

bool usb_mount_sync(void) {
    /* Normal mid-session sync: flush the mounted block device via the durability
     * barrier WITHOUT marking the filesystem clean or freezing writes.
     * This is explicitly distinct from ext2_sync_all() (shutdown-only clean close).
     * Returns true if the barrier succeeded; false on any error. */
    if (!s_mounted_rw_dev) return ext4_sync_journal_fixture()==0;
    return s_ext4_mount ? ext4_sync(s_ext4_mount) == 0 : block_flush(s_mounted_rw_dev);
}


bool usb_mount_freeze_and_sync(void) {
    /* Call each filesystem separately: never nest rank-1 filesystem locks. */
    bool ext4_ok = s_ext4_mount ? ext4_freeze_and_sync(s_ext4_mount)==0 : ext4_freeze_journal_fixture()==0;
    bool ext2_ok = ext2_sync_all();
    return ext4_ok && ext2_ok;
}
