#ifndef FORTRESS_GPT_H
#define FORTRESS_GPT_H

#include "types.h"
#include "block.h"

#define GPT_SIGNATURE_MAGIC          0x5452415020494645ULL /* "EFI PART" */
#define GPT_REVISION_1_0             0x00010000U
#define GPT_MIN_HEADER_SIZE          92

/* Strict bounds on partition entry array */
#define GPT_MIN_ENTRY_SIZE           128
#define GPT_MAX_ENTRY_SIZE           512
#define GPT_MAX_SUPPORTED_ENTRIES    128
#define GPT_MAX_ARRAY_BYTES          (GPT_MAX_SUPPORTED_ENTRIES * GPT_MAX_ENTRY_SIZE) /* 64 KiB */
#define GPT_MAX_PARTITIONS           16

#define MBR_SIGNATURE_MAGIC          0xAA55U
#define MBR_PARTITION_TYPE_GPT       0xEEU

/* Deterministic GPT Backup Policy Result */
typedef enum {
    GPT_POLICY_PRIMARY_CONSISTENT = 0, /* Primary valid, backup valid and consistent -> Use primary */
    GPT_POLICY_BACKUP_FALLBACK,        /* Primary invalid, backup valid -> Read-only fallback in memory */
    GPT_POLICY_DEGRADED_PRIMARY,        /* Primary valid, backup invalid -> Explicit degraded-mode policy */
    GPT_POLICY_REJECT_AMBIGUITY,        /* Primary valid, backup valid but inconsistent -> Reject ambiguity */
    GPT_POLICY_REJECT_INVALID           /* Both invalid -> Reject disk */
} gpt_policy_result_t;

/* 16-byte GUID representation */
typedef struct {
    uint8_t bytes[16];
} __attribute__((packed)) gpt_guid_t;

/* Standard Linux Filesystem Data GUID: 0FC63DAF-8483-4772-8E79-3D69D8477DE4 */
extern const gpt_guid_t GPT_GUID_LINUX_FS;

/* Standard EFI System Partition GUID: C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
extern const gpt_guid_t GPT_GUID_ESP;

/* Legacy / Protective MBR Partition Entry (16 bytes) */
typedef struct {
    uint8_t  boot_indicator;
    uint8_t  start_chs[3];
    uint8_t  os_type;
    uint8_t  end_chs[3];
    uint32_t starting_lba;
    uint32_t size_in_lba;
} __attribute__((packed)) mbr_entry_t;

/* Protective MBR Sector (LBA 0, 512 bytes) */
typedef struct {
    uint8_t     bootstrap[446];
    mbr_entry_t entries[4];
    uint16_t    signature;
} __attribute__((packed)) gpt_protective_mbr_t;

/* GPT Header (92 bytes minimum, resides at LBA 1 and Backup LBA) */
typedef struct {
    uint64_t   signature;                    /* "EFI PART" */
    uint32_t   revision;                     /* 0x00010000 */
    uint32_t   header_size;                  /* 92 bytes */
    uint32_t   header_crc32;                 /* CRC32 of header with this field 0 */
    uint32_t   reserved;                     /* Must be 0 */
    uint64_t   current_lba;                  /* LBA of this header */
    uint64_t   backup_lba;                   /* LBA of alternative header */
    uint64_t   first_usable_lba;             /* First usable LBA for partitions */
    uint64_t   last_usable_lba;              /* Last usable LBA for partitions */
    gpt_guid_t disk_guid;                    /* Disk Unique GUID */
    uint64_t   partition_entry_lba;          /* LBA of partition entry array */
    uint32_t   num_partition_entries;        /* Number of entries in array */
    uint32_t   sizeof_partition_entry;       /* Size of each entry (128 bytes) */
    uint32_t   partition_entry_array_crc32;  /* CRC32 of entire partition array */
} __attribute__((packed)) gpt_header_t;

/* GPT Partition Entry (128 bytes minimum) */
typedef struct {
    gpt_guid_t type_guid;                    /* Partition Type GUID */
    gpt_guid_t unique_partition_guid;        /* Unique Partition GUID */
    uint64_t   starting_lba;                 /* First LBA of partition */
    uint64_t   ending_lba;                   /* Last LBA of partition (inclusive) */
    uint64_t   attributes;                   /* Partition attributes */
    uint16_t   partition_name[36];           /* UTF-16LE partition name */
} __attribute__((packed)) gpt_entry_t;

/* FortressOS In-Memory Partition Descriptor */
typedef struct {
    block_dev_t *parent;
    uint32_t     part_index;                 /* 1-based index (e.g. 1 for p1) */
    uint64_t     starting_lba;
    uint64_t     ending_lba;
    uint64_t     sector_count;
    gpt_guid_t   type_guid;
    gpt_guid_t   unique_guid;
    block_dev_t  block_dev;                  /* Bounded block device adapter */
} gpt_partition_t;

/* GPT Parser API */
bool                 gpt_parse(block_dev_t *dev);
bool                 gpt_parse_ex(block_dev_t *dev, gpt_policy_result_t *out_policy);
void                 gpt_reset(void);
size_t               gpt_get_partition_count(void);
gpt_partition_t     *gpt_get_partition(size_t index);
gpt_partition_t     *gpt_find_by_type(const gpt_guid_t *type_guid);
gpt_policy_result_t  gpt_get_last_policy(void);

bool                 gpt_guid_equal(const gpt_guid_t *a, const gpt_guid_t *b);
void                 gpt_guid_to_str(const gpt_guid_t *guid, char *out_str);
bool                 gpt_str_to_guid(const char *str, gpt_guid_t *out_guid);

#endif /* FORTRESS_GPT_H */
