#include "gpt.h"
#include "crc32.h"
#include "heap.h"
#include "string.h"
#include "serial.h"

/* Linux Filesystem Data GUID: 0FC63DAF-8483-4772-8E79-3D69D8477DE4 */
const gpt_guid_t GPT_GUID_LINUX_FS = {
    .bytes = { 0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4 }
};

/* EFI System Partition (ESP) GUID: C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
const gpt_guid_t GPT_GUID_ESP = {
    .bytes = { 0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B }
};

static gpt_partition_t    g_partitions[GPT_MAX_PARTITIONS];
static size_t             g_partition_count = 0;
static gpt_policy_result_t g_last_policy = GPT_POLICY_REJECT_INVALID;

bool gpt_guid_equal(const gpt_guid_t *a, const gpt_guid_t *b) {
    if (!a || !b) return false;
    return (memcmp(a->bytes, b->bytes, 16) == 0);
}

static bool gpt_guid_is_zero(const gpt_guid_t *guid) {
    if (!guid) return true;
    for (int i = 0; i < 16; i++) {
        if (guid->bytes[i] != 0) return false;
    }
    return true;
}

static char hex_digit(uint8_t val) {
    val &= 0x0F;
    return (val < 10) ? ('0' + val) : ('A' + (val - 10));
}

void gpt_guid_to_str(const gpt_guid_t *guid, char *out_str) {
    if (!guid || !out_str) return;

    /* Formatted as mixed-endian 8-4-4-4-12 UUID */
    const uint8_t *b = guid->bytes;
    int idx = 0;

    /* Data1 (4 bytes, little-endian) */
    for (int i = 3; i >= 0; i--) {
        out_str[idx++] = hex_digit(b[i] >> 4);
        out_str[idx++] = hex_digit(b[i]);
    }
    out_str[idx++] = '-';

    /* Data2 (2 bytes, little-endian) */
    for (int i = 5; i >= 4; i--) {
        out_str[idx++] = hex_digit(b[i] >> 4);
        out_str[idx++] = hex_digit(b[i]);
    }
    out_str[idx++] = '-';

    /* Data3 (2 bytes, little-endian) */
    for (int i = 7; i >= 6; i--) {
        out_str[idx++] = hex_digit(b[i] >> 4);
        out_str[idx++] = hex_digit(b[i]);
    }
    out_str[idx++] = '-';

    /* Data4 (2 bytes, big-endian) */
    for (int i = 8; i <= 9; i++) {
        out_str[idx++] = hex_digit(b[i] >> 4);
        out_str[idx++] = hex_digit(b[i]);
    }
    out_str[idx++] = '-';

    /* Data4 remaining (6 bytes, big-endian) */
    for (int i = 10; i < 16; i++) {
        out_str[idx++] = hex_digit(b[i] >> 4);
        out_str[idx++] = hex_digit(b[i]);
    }
    out_str[idx] = '\0';
}

/* Bounded Partition Device Read Handler */
static bool gpt_partition_read_sector(block_dev_t *dev, uint64_t lba, void *buf) {
    if (!dev || !buf) return false;
    gpt_partition_t *part = (gpt_partition_t *)dev->priv;
    if (!part || !part->parent || !part->parent->read_sector) return false;

    /* Strict boundary enforcement: reject any read at or beyond partition capacity */
    if (lba >= part->sector_count) {
        serial_puts("[WARN] Partition read out of bounds rejected! LBA: ");
        serial_print_dec(lba);
        serial_puts(", Capacity: ");
        serial_print_dec(part->sector_count);
        serial_puts("\n");
        return false;
    }

    /* Overflow-safe arithmetic for parent LBA translation */
    if (part->starting_lba + lba < part->starting_lba) {
        serial_puts("[FAIL] Partition LBA calculation arithmetic overflow!\n");
        return false;
    }

    uint64_t parent_lba = part->starting_lba + lba;
    if (parent_lba > part->ending_lba) {
        serial_puts("[FAIL] Partition LBA exceeds ending LBA!\n");
        return false;
    }

    return part->parent->read_sector(part->parent, parent_lba, buf);
}

/* Header Structural and Placement Validation */
static bool gpt_validate_header(const gpt_header_t *hdr, uint64_t expected_lba, uint64_t disk_sectors, uint32_t sector_size, bool is_primary) {
    if (!hdr) return false;

    /* 1. Signature Check ("EFI PART") */
    if (hdr->signature != GPT_SIGNATURE_MAGIC) {
        return false;
    }

    /* 2. Revision Check (1.0) */
    if (hdr->revision != GPT_REVISION_1_0) {
        serial_puts("[GPT] Unsupported revision: ");
        serial_print_hex(hdr->revision);
        serial_puts("\n");
        return false;
    }

    /* 3. Header Size Bounds */
    if (hdr->header_size < GPT_MIN_HEADER_SIZE || hdr->header_size > sector_size) {
        serial_puts("[GPT] Invalid header size: ");
        serial_print_dec(hdr->header_size);
        serial_puts("\n");
        return false;
    }

    /* 4. Current LBA Validation */
    if (hdr->current_lba != expected_lba) {
        serial_puts("[GPT] Header current_lba mismatch! Expected: ");
        serial_print_dec(expected_lba);
        serial_puts(", Got: ");
        serial_print_dec(hdr->current_lba);
        serial_puts("\n");
        return false;
    }

    /* 5. Usable LBA Range Check with Overflow Protection */
    if (hdr->first_usable_lba > hdr->last_usable_lba || hdr->last_usable_lba >= disk_sectors) {
        serial_puts("[GPT] Usable LBA range invalid!\n");
        return false;
    }
    if (hdr->first_usable_lba <= 1) {
        serial_puts("[GPT] First usable LBA overlaps MBR or Primary Header!\n");
        return false;
    }

    /* 6. Partition Entry Size & Count Bounds with Overflow Safety */
    if (hdr->sizeof_partition_entry != 128 &&
        hdr->sizeof_partition_entry != 256 &&
        hdr->sizeof_partition_entry != 512) {
        serial_puts("[GPT] Invalid sizeof_partition_entry (must be 128, 256, or 512): ");
        serial_print_dec(hdr->sizeof_partition_entry);
        serial_puts("\n");
        return false;
    }
    if (hdr->num_partition_entries < 1 || hdr->num_partition_entries > GPT_MAX_SUPPORTED_ENTRIES) {
        serial_puts("[GPT] Invalid num_partition_entries: ");
        serial_print_dec(hdr->num_partition_entries);
        serial_puts("\n");
        return false;
    }

    uint64_t array_bytes = (uint64_t)hdr->num_partition_entries * hdr->sizeof_partition_entry;
    if (array_bytes > GPT_MAX_ARRAY_BYTES) {
        serial_puts("[GPT] Partition entry array byte size exceeds maximum limit!\n");
        return false;
    }

    uint64_t array_sectors = (array_bytes + sector_size - 1) / sector_size;
    if (hdr->partition_entry_lba + array_sectors < hdr->partition_entry_lba ||
        hdr->partition_entry_lba + array_sectors > disk_sectors) {
        serial_puts("[GPT] Partition entry array bounds exceed disk capacity!\n");
        return false;
    }

    /* 7. Non-overlap validation: array must not overlap headers or usable space */
    if (is_primary) {
        if (hdr->partition_entry_lba < 2) {
            serial_puts("[GPT] Primary partition array overlaps MBR or Primary Header!\n");
            return false;
        }
        if (hdr->partition_entry_lba + array_sectors > hdr->first_usable_lba) {
            serial_puts("[GPT] Primary partition array overlaps usable partition space!\n");
            return false;
        }
    } else {
        if (hdr->partition_entry_lba <= hdr->last_usable_lba) {
            serial_puts("[GPT] Backup partition array overlaps usable partition space!\n");
            return false;
        }
        if (hdr->partition_entry_lba + array_sectors > hdr->current_lba) {
            serial_puts("[GPT] Backup partition array overlaps Backup Header!\n");
            return false;
        }
    }

    /* 8. Header CRC32 Checksum Validation */
    uint8_t temp_hdr[512];
    if (hdr->header_size > sizeof(temp_hdr)) return false;
    memcpy(temp_hdr, hdr, hdr->header_size);
    gpt_header_t *temp_copy = (gpt_header_t *)temp_hdr;
    temp_copy->header_crc32 = 0; /* Zero CRC field during calculation */

    uint32_t computed_crc = crc32(0, temp_copy, hdr->header_size);
    if (computed_crc != hdr->header_crc32) {
        return false;
    }

    return true;
}

/* Consistency Check Between Primary and Backup Headers */
static bool gpt_headers_consistent(const gpt_header_t *prim, const gpt_header_t *back) {
    if (!prim || !back) return false;

    if (memcmp(&prim->disk_guid, &back->disk_guid, sizeof(gpt_guid_t)) != 0) {
        serial_puts("[GPT] Inconsistency: Disk GUID mismatch between Primary and Backup!\n");
        return false;
    }
    if (prim->first_usable_lba != back->first_usable_lba ||
        prim->last_usable_lba != back->last_usable_lba) {
        serial_puts("[GPT] Inconsistency: Usable LBA range mismatch between Primary and Backup!\n");
        return false;
    }
    if (prim->num_partition_entries != back->num_partition_entries ||
        prim->sizeof_partition_entry != back->sizeof_partition_entry) {
        serial_puts("[GPT] Inconsistency: Partition entry geometry mismatch between Primary and Backup!\n");
        return false;
    }
    if (prim->partition_entry_array_crc32 != back->partition_entry_array_crc32) {
        serial_puts("[GPT] Inconsistency: Partition array CRC mismatch between Primary and Backup!\n");
        return false;
    }
    if (prim->current_lba != back->backup_lba || prim->backup_lba != back->current_lba) {
        serial_puts("[GPT] Inconsistency: Cross-referencing current/backup LBA pointer mismatch!\n");
        return false;
    }

    return true;
}

/* Read and Validate Partition Entry Array Buffer */
static uint8_t *gpt_read_and_verify_array(block_dev_t *dev, const gpt_header_t *hdr) {
    size_t array_bytes = (size_t)hdr->num_partition_entries * hdr->sizeof_partition_entry;
    size_t array_sectors = (array_bytes + dev->sector_size - 1) / dev->sector_size;

    uint8_t *buf = (uint8_t *)kmalloc(array_sectors * dev->sector_size);
    if (!buf) {
        serial_puts("[GPT] Out of memory allocating partition array buffer!\n");
        return NULL;
    }

    for (size_t s = 0; s < array_sectors; s++) {
        uint64_t entry_lba = hdr->partition_entry_lba + s;
        if (!dev->read_sector(dev, entry_lba, buf + s * dev->sector_size)) {
            serial_puts("[GPT] Failed reading sector LBA: ");
            serial_print_dec(entry_lba);
            serial_puts("\n");
            kfree(buf);
            return NULL;
        }
    }

    /* Strict CRC calculation over EXACT entry bytes (excluding sector padding) */
    uint32_t computed_crc = crc32(0, buf, array_bytes);
    if (computed_crc != hdr->partition_entry_array_crc32) {
        serial_puts("[GPT] Partition array CRC mismatch! Expected: ");
        serial_print_hex(hdr->partition_entry_array_crc32);
        serial_puts(", Computed: ");
        serial_print_hex(computed_crc);
        serial_puts("\n");
        kfree(buf);
        return NULL;
    }

    return buf;
}

void gpt_reset(void) {
    for (size_t i = 0; i < g_partition_count; i++) {
        block_unregister_dev(&g_partitions[i].block_dev);
    }
    memset(g_partitions, 0, sizeof(g_partitions));
    g_partition_count = 0;
    g_last_policy = GPT_POLICY_REJECT_INVALID;
}

gpt_policy_result_t gpt_get_last_policy(void) {
    return g_last_policy;
}

/* Parse GPT on Block Device with Extended Policy Reporting */
bool gpt_parse_ex(block_dev_t *dev, gpt_policy_result_t *out_policy) {
    gpt_reset();

    if (!dev || !dev->read_sector || dev->sector_size < 512 || dev->sector_count < 68) {
        serial_puts("[GPT] Device invalid or too small for GPT!\n");
        g_last_policy = GPT_POLICY_REJECT_INVALID;
        if (out_policy) *out_policy = g_last_policy;
        return false;
    }

    static uint8_t sector_buf[4096];
    if (dev->sector_size > sizeof(sector_buf)) {
        serial_puts("[GPT] Device sector size exceeds sector buffer!\n");
        g_last_policy = GPT_POLICY_REJECT_INVALID;
        if (out_policy) *out_policy = g_last_policy;
        return false;
    }

    /* =====================================================================
     * Step 1: Validate Protective MBR (LBA 0)
     * ===================================================================== */
    if (!dev->read_sector(dev, 0, sector_buf)) {
        serial_puts("[GPT] Failed to read LBA 0 (MBR)!\n");
        g_last_policy = GPT_POLICY_REJECT_INVALID;
        if (out_policy) *out_policy = g_last_policy;
        return false;
    }

    gpt_protective_mbr_t *mbr = (gpt_protective_mbr_t *)sector_buf;
    if (mbr->signature != MBR_SIGNATURE_MAGIC) {
        serial_puts("[GPT] MBR boot signature (0xAA55) missing! Got: ");
        serial_print_hex(mbr->signature);
        serial_puts("\n");
        g_last_policy = GPT_POLICY_REJECT_INVALID;
        if (out_policy) *out_policy = g_last_policy;
        return false;
    }

    bool has_gpt_protective = false;
    for (int i = 0; i < 4; i++) {
        if (mbr->entries[i].os_type == MBR_PARTITION_TYPE_GPT) {
            has_gpt_protective = true;
            break;
        }
    }

    if (!has_gpt_protective) {
        serial_puts("[GPT] No Protective MBR partition (type 0xEE) found at LBA 0!\n");
        g_last_policy = GPT_POLICY_REJECT_INVALID;
        if (out_policy) *out_policy = g_last_policy;
        return false;
    }

    /* =====================================================================
     * Step 2: Read & Evaluate Primary and Backup Headers
     * ===================================================================== */
    gpt_header_t primary_hdr;
    bool primary_hdr_valid = false;
    if (dev->read_sector(dev, 1, sector_buf)) {
        memcpy(&primary_hdr, sector_buf, sizeof(gpt_header_t));
        primary_hdr_valid = gpt_validate_header(&primary_hdr, 1, dev->sector_count, dev->sector_size, true);
    }

    uint64_t backup_lba = dev->sector_count - 1;
    gpt_header_t backup_hdr;
    bool backup_hdr_valid = false;
    if (dev->read_sector(dev, backup_lba, sector_buf)) {
        memcpy(&backup_hdr, sector_buf, sizeof(gpt_header_t));
        backup_hdr_valid = gpt_validate_header(&backup_hdr, backup_lba, dev->sector_count, dev->sector_size, false);
    }

    /* =====================================================================
     * Step 3: Resolve Deterministic Backup Policy Matrix
     * ===================================================================== */
    const gpt_header_t *chosen_hdr = NULL;
    uint8_t *chosen_array_buf = NULL;
    gpt_policy_result_t resolved_policy = GPT_POLICY_REJECT_INVALID;

    if (primary_hdr_valid && backup_hdr_valid) {
        if (!gpt_headers_consistent(&primary_hdr, &backup_hdr)) {
            serial_puts("[GPT] AMBIGUITY DETECTED: Primary and Backup headers valid but inconsistent! Rejecting disk.\n");
            resolved_policy = GPT_POLICY_REJECT_AMBIGUITY;
            goto policy_done;
        }

        /* Both valid and consistent: test primary array first */
        chosen_array_buf = gpt_read_and_verify_array(dev, &primary_hdr);
        if (chosen_array_buf) {
            chosen_hdr = &primary_hdr;
            resolved_policy = GPT_POLICY_PRIMARY_CONSISTENT;
            serial_puts("[GPT] Policy: Primary and Backup valid and consistent. Using Primary.\n");
        } else {
            /* Primary array corrupt: attempt read-only fallback to backup array */
            serial_puts("[GPT] Primary array CRC failed. Attempting read-only fallback to Backup...\n");
            chosen_array_buf = gpt_read_and_verify_array(dev, &backup_hdr);
            if (chosen_array_buf) {
                chosen_hdr = &backup_hdr;
                resolved_policy = GPT_POLICY_BACKUP_FALLBACK;
                serial_puts("[GPT] Policy: Primary array invalid, Backup valid. Read-only fallback in memory.\n");
            } else {
                serial_puts("[GPT] Both Primary and Backup partition arrays invalid! Rejecting disk.\n");
                resolved_policy = GPT_POLICY_REJECT_INVALID;
            }
        }
    } else if (primary_hdr_valid && !backup_hdr_valid) {
        /* Degraded mode: Primary valid, Backup invalid */
        chosen_array_buf = gpt_read_and_verify_array(dev, &primary_hdr);
        if (chosen_array_buf) {
            chosen_hdr = &primary_hdr;
            resolved_policy = GPT_POLICY_DEGRADED_PRIMARY;
            serial_puts("[GPT] Policy: Degraded mode. Primary valid, Backup header corrupted.\n");
        } else {
            serial_puts("[GPT] Primary valid but array corrupted, Backup invalid! Rejecting disk.\n");
            resolved_policy = GPT_POLICY_REJECT_INVALID;
        }
    } else if (!primary_hdr_valid && backup_hdr_valid) {
        /* Read-only fallback: Primary invalid, Backup valid */
        chosen_array_buf = gpt_read_and_verify_array(dev, &backup_hdr);
        if (chosen_array_buf) {
            chosen_hdr = &backup_hdr;
            resolved_policy = GPT_POLICY_BACKUP_FALLBACK;
            serial_puts("[GPT] Policy: Primary invalid, Backup valid. Read-only fallback in memory.\n");
        } else {
            serial_puts("[GPT] Backup header valid but backup array corrupted! Rejecting disk.\n");
            resolved_policy = GPT_POLICY_REJECT_INVALID;
        }
    } else {
        serial_puts("[GPT] Policy: Both Primary and Backup headers invalid! Rejecting disk.\n");
        resolved_policy = GPT_POLICY_REJECT_INVALID;
    }

policy_done:
    g_last_policy = resolved_policy;
    if (out_policy) *out_policy = resolved_policy;

    if (!chosen_hdr || !chosen_array_buf) {
        if (chosen_array_buf) kfree(chosen_array_buf);
        return false;
    }

    /* =====================================================================
     * Step 4: Validate Entire Table Into Staging Buffer (All-or-Nothing)
     * ===================================================================== */
    gpt_partition_t staged_parts[GPT_MAX_PARTITIONS];
    size_t staged_count = 0;
    memset(staged_parts, 0, sizeof(staged_parts));

    for (uint32_t i = 0; i < chosen_hdr->num_partition_entries; i++) {
        gpt_entry_t *entry = (gpt_entry_t *)(chosen_array_buf + i * chosen_hdr->sizeof_partition_entry);

        if (gpt_guid_is_zero(&entry->type_guid)) {
            continue; /* Unused entry slot */
        }

        /* 4A: Boundary Checks with overflow safety */
        if (entry->starting_lba < chosen_hdr->first_usable_lba ||
            entry->ending_lba > chosen_hdr->last_usable_lba ||
            entry->starting_lba > entry->ending_lba) {
            serial_puts("[GPT] Entry ");
            serial_print_dec(i + 1);
            serial_puts(" out of usable range! Aborting table.\n");
            kfree(chosen_array_buf);
            return false;
        }

        uint64_t count = (entry->ending_lba - entry->starting_lba) + 1;
        if (count == 0) {
            serial_puts("[GPT] Entry ");
            serial_print_dec(i + 1);
            serial_puts(" has zero sector count! Aborting table.\n");
            kfree(chosen_array_buf);
            return false;
        }

        /* 4B: Pairwise Overlap Check against all previously staged partitions */
        for (size_t p = 0; p < staged_count; p++) {
            if (entry->starting_lba <= staged_parts[p].ending_lba &&
                staged_parts[p].starting_lba <= entry->ending_lba) {
                serial_puts("[GPT] Partition overlap detected between entry ");
                serial_print_dec(i + 1);
                serial_puts(" and staged partition ");
                serial_print_dec(staged_parts[p].part_index);
                serial_puts("! Aborting table.\n");
                kfree(chosen_array_buf);
                return false;
            }
        }

        if (staged_count >= GPT_MAX_PARTITIONS) {
            serial_puts("[GPT] Exceeded maximum supported partitions (");
            serial_print_dec(GPT_MAX_PARTITIONS);
            serial_puts(")! Aborting table.\n");
            kfree(chosen_array_buf);
            return false;
        }

        /* Stage valid partition */
        gpt_partition_t *part = &staged_parts[staged_count++];
        part->parent       = dev;
        part->part_index   = i + 1;
        part->starting_lba = entry->starting_lba;
        part->ending_lba   = entry->ending_lba;
        part->sector_count = count;
        part->type_guid    = entry->type_guid;
        part->unique_guid  = entry->unique_partition_guid;
    }

    /* All entries validated successfully. Release temporary allocation. */
    kfree(chosen_array_buf);

    /* =====================================================================
     * Step 5: Publish Validated Partitions into Registry
     * ===================================================================== */
    g_partition_count = staged_count;
    for (size_t i = 0; i < staged_count; i++) {
        g_partitions[i] = staged_parts[i];
        gpt_partition_t *part = &g_partitions[i];

        /* Setup bounded block device */
        memset(&part->block_dev, 0, sizeof(block_dev_t));
        size_t parent_len = strlen(dev->name);
        if (parent_len > sizeof(part->block_dev.name) - 5) {
            parent_len = sizeof(part->block_dev.name) - 5;
        }
        memcpy(part->block_dev.name, dev->name, parent_len);
        part->block_dev.name[parent_len] = 'p';
        part->block_dev.name[parent_len + 1] = '0' + (char)part->part_index;
        part->block_dev.name[parent_len + 2] = '\0';

        part->block_dev.sector_size  = dev->sector_size;
        part->block_dev.sector_count = part->sector_count;
        part->block_dev.read_sector  = gpt_partition_read_sector;
        part->block_dev.write_sector = NULL; /* Read-only partition device for Phase 9C.1 */
        part->block_dev.flush        = NULL;
        part->block_dev.priv         = part;

        block_register_dev(&part->block_dev);

        char guid_buf[40];
        gpt_guid_to_str(&part->type_guid, guid_buf);
        serial_puts("[GPT] Registered partition: ");
        serial_puts(part->block_dev.name);
        serial_puts(" [Type: ");
        serial_puts(guid_buf);
        serial_puts("]\n      LBA ");
        serial_print_dec(part->starting_lba);
        serial_puts("..");
        serial_print_dec(part->ending_lba);
        serial_puts(" (");
        serial_print_dec(part->sector_count);
        serial_puts(" sectors)\n");
    }

    serial_puts("[GPT] Completed. Published ");
    serial_print_dec(g_partition_count);
    serial_puts(" partition(s).\n");

    return true;
}

bool gpt_parse(block_dev_t *dev) {
    return gpt_parse_ex(dev, NULL);
}

size_t gpt_get_partition_count(void) {
    return g_partition_count;
}

gpt_partition_t *gpt_get_partition(size_t index) {
    if (index >= g_partition_count) {
        return NULL;
    }
    return &g_partitions[index];
}

gpt_partition_t *gpt_find_by_type(const gpt_guid_t *type_guid) {
    if (!type_guid) return NULL;
    for (size_t i = 0; i < g_partition_count; i++) {
        if (gpt_guid_equal(&g_partitions[i].type_guid, type_guid)) {
            return &g_partitions[i];
        }
    }
    return NULL;
}
