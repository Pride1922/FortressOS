#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "types.h"
#include "block.h"
#include "vfs.h"
#include "xhci_bot.h"  /* for usb_durability_mode_t */

/* Stubs for linking with gpt.c */
uint32_t crc32(uint32_t crc, const void *buf, size_t len) { (void)crc; (void)buf; (void)len; return 0; }
void *kmalloc(size_t sz) { return malloc(sz); }
void *kcalloc(size_t n, size_t sz) { return calloc(n, sz); }
void kfree(void *p) { free(p); }
void serial_print_dec(uint64_t v) { (void)v; }
void serial_print_hex(uint64_t v) { (void)v; }
bool block_register_dev(block_dev_t *dev) { (void)dev; return true; }
bool block_unregister_dev(block_dev_t *dev) { (void)dev; return true; }

/* String helpers for freestanding compatibility */
static void append_str(char *dst, const char *src, size_t dst_max) {
    size_t dlen = strlen(dst);
    size_t slen = strlen(src);
    if (dlen + slen >= dst_max) slen = (dst_max > dlen) ? dst_max - dlen - 1 : 0;
    memcpy(dst + dlen, src, slen);
    dst[dlen + slen] = '\0';
}

static bool str_contains(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;
    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen > hlen) return false;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (memcmp(haystack + i, needle, nlen) == 0) return true;
    }
    return false;
}

static void copy_str(char *dst, const char *src, size_t dst_max) {
    size_t len = strlen(src);
    if (len >= dst_max) len = dst_max - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static bool s_usb_init = true;
static bool s_ext2_mount_called = false;
static bool s_ext2_mount_result = true;
static bool s_ext2_mount_rw_called = false;
static bool s_ext2_mount_rw_result = true;
static bool s_flush_result = true;
static unsigned s_flush_calls;
static unsigned s_write_calls;
static unsigned s_report_calls;
static char s_last_serial_log[1024];

void serial_puts(const char *s) {
    if (!s) return;
    append_str(s_last_serial_log, s, sizeof(s_last_serial_log));
}

bool usb_is_initialized(void) {
    return s_usb_init;
}

vfs_node_t *vfs_lookup(const char *path) {
    (void)path;
    return NULL; /* /mnt not yet mounted */
}

bool ext2_mount(block_dev_t *dev, const char *path) {
    (void)dev;
    (void)path;
    s_ext2_mount_called = true;
    return s_ext2_mount_result;
}

bool ext2_mount_rw(block_dev_t *dev, const char *path) {
    (void)dev;
    (void)path;
    s_ext2_mount_rw_called = true;
    assert(s_flush_calls > 0 && s_flush_result);
    return s_ext2_mount_rw_result;
}

static bool mock_write_sector(block_dev_t *dev, uint64_t lba, const void *buf) {
    (void)dev; (void)lba; (void)buf; s_write_calls++; return true;
}

static bool mock_flush(block_dev_t *dev) {
    (void)dev; s_flush_calls++; return s_flush_result;
}

bool block_flush(block_dev_t *dev) { return dev && dev->flush && dev->flush(dev); }
void usb_report_flush_failure(void) { s_report_calls++; }

/* Controllable durability mode stub for 9G.4 eligibility tests */
static usb_durability_mode_t s_durability_mode = USB_DURABILITY_SYNC_BACKED;
usb_durability_mode_t usb_get_durability_mode(void) { return s_durability_mode; }

#include "../src/fs/gpt.c"
#include "../src/fs/usb_mount.c"

static void test_guid_conversions(void) {
    printf("[HOST TEST] Testing gpt_str_to_guid and gpt_guid_to_str...\n");

    /* Test standard Linux FS GUID */
    char str_out[40];
    gpt_guid_to_str(&GPT_GUID_LINUX_FS, str_out);
    assert(strcmp(str_out, "0FC63DAF-8483-4772-8E79-3D69D8477DE4") == 0);

    gpt_guid_t roundtrip;
    assert(gpt_str_to_guid(str_out, &roundtrip));
    assert(gpt_guid_equal(&GPT_GUID_LINUX_FS, &roundtrip));

    /* Test ESP GUID */
    gpt_guid_to_str(&GPT_GUID_ESP, str_out);
    assert(strcmp(str_out, "C12A7328-F81F-11D2-BA4B-00A0C93EC93B") == 0);
    assert(gpt_str_to_guid(str_out, &roundtrip));
    assert(gpt_guid_equal(&GPT_GUID_ESP, &roundtrip));

    /* Test arbitrary GUID with mixed case */
    const char *mixed = "cb667616-2fc7-4820-9b24-c5d9de4227ad";
    assert(gpt_str_to_guid(mixed, &roundtrip));
    gpt_guid_to_str(&roundtrip, str_out);
    assert(strcmp(str_out, "CB667616-2FC7-4820-9B24-C5D9DE4227AD") == 0);

    /* Negative checks */
    gpt_guid_t dummy;
    assert(!gpt_str_to_guid(NULL, &dummy));
    assert(!gpt_str_to_guid("CB667616", &dummy));
    assert(!gpt_str_to_guid("CB667616-2FC7-4820-9B24-C5D9DE4227AX", &dummy)); /* invalid hex */
    assert(!gpt_str_to_guid("CB667616_2FC7_4820_9B24_C5D9DE4227AD", &dummy)); /* bad dashes */

    printf("  [PASS] GUID conversions verified\n");
}

static void test_cmdline_parsing(void) {
    printf("[HOST TEST] Testing usb_mount_parse_cmdline...\n");

    usb_mount_config_t cfg;

    /* 1. Empty / NULL */
    usb_mount_parse_cmdline(NULL, &cfg);
    assert(!cfg.has_target && !cfg.malformed && cfg.mode == USB_MOUNT_MODE_RO);
    usb_mount_parse_cmdline("", &cfg);
    assert(!cfg.has_target && !cfg.malformed && cfg.mode == USB_MOUNT_MODE_RO);

    /* 2. Valid default RO */
    usb_mount_parse_cmdline("usb_data=PARTUUID=CB667616-2FC7-4820-9B24-C5D9DE4227AD", &cfg);
    assert(cfg.has_target && !cfg.malformed && cfg.mode == USB_MOUNT_MODE_RO);

    /* 3. Valid explicit RW */
    usb_mount_parse_cmdline("usb_data=PARTUUID=CB667616-2FC7-4820-9B24-C5D9DE4227AD usb_data_mode=rw", &cfg);
    assert(cfg.has_target && !cfg.malformed && cfg.mode == USB_MOUNT_MODE_RW);

    /* 4. Surrounding parameters & extra spaces */
    usb_mount_parse_cmdline("  console=tty0   usb_data=PARTUUID=CB667616-2FC7-4820-9B24-C5D9DE4227AD   quiet  usb_data_mode=ro  ", &cfg);
    assert(cfg.has_target && !cfg.malformed && cfg.mode == USB_MOUNT_MODE_RO);

    /* 5. Malformed targets */
    usb_mount_parse_cmdline("usb_data=LABEL=FORTRESS_DATA", &cfg);
    assert(cfg.malformed && !cfg.has_target);

    usb_mount_parse_cmdline("usb_data=sda2", &cfg);
    assert(cfg.malformed && !cfg.has_target);

    usb_mount_parse_cmdline("usb_data=PARTUUID=too-short", &cfg);
    assert(cfg.malformed && !cfg.has_target);

    /* 6. Malformed mode */
    usb_mount_parse_cmdline("usb_data=PARTUUID=CB667616-2FC7-4820-9B24-C5D9DE4227AD usb_data_mode=fast", &cfg);
    assert(cfg.malformed);

    printf("  [PASS] cmdline parsing verified\n");
}

static void test_production_mount_selection(void) {
    printf("[HOST TEST] Testing usb_mount_production_storage selection logic...\n");

    block_dev_t usb_sda = {.name = "sda"};
    block_dev_t nvme_dev = {.name = "nvme0n1"};

    gpt_guid_t target_guid;
    assert(gpt_str_to_guid("CB667616-2FC7-4820-9B24-C5D9DE4227AD", &target_guid));

    boot_info_t bi = {0};
    copy_str(bi.cmdline, "usb_data=PARTUUID=CB667616-2FC7-4820-9B24-C5D9DE4227AD usb_data_mode=ro", sizeof(bi.cmdline));

    /* Case A: Controller not initialized */
    s_usb_init = false;
    s_last_serial_log[0] = '\0';
    assert(!usb_mount_production_storage(&bi));
    assert(str_contains(s_last_serial_log, "No USB mass-storage controller"));
    s_usb_init = true;

    /* Case B: Partition not found (0 matches) */
    g_partition_count = 0;
    s_last_serial_log[0] = '\0';
    assert(!usb_mount_production_storage(&bi));
    assert(str_contains(s_last_serial_log, "not found on supported USB storage"));

    /* Case C: Partition on NVMe device with matching GUID (provenance rejected) */
    g_partition_count = 1;
    g_partitions[0].parent = &nvme_dev;
    g_partitions[0].unique_guid = target_guid;
    copy_str(g_partitions[0].block_dev.name, "nvme0n1p2", sizeof(g_partitions[0].block_dev.name));
    s_last_serial_log[0] = '\0';
    assert(!usb_mount_production_storage(&bi));
    assert(str_contains(s_last_serial_log, "not found on supported USB storage"));

    /* Case D: Ambiguous clones (>1 matches on USB) */
    g_partition_count = 2;
    g_partitions[0].parent = &usb_sda;
    g_partitions[0].unique_guid = target_guid;
    copy_str(g_partitions[0].block_dev.name, "sdap2", sizeof(g_partitions[0].block_dev.name));
    g_partitions[1].parent = &usb_sda;
    g_partitions[1].unique_guid = target_guid;
    copy_str(g_partitions[1].block_dev.name, "sdap3", sizeof(g_partitions[1].block_dev.name));
    s_last_serial_log[0] = '\0';
    assert(!usb_mount_production_storage(&bi));
    assert(str_contains(s_last_serial_log, "Ambiguous candidates"));

    /* Case E: GPT policy rejected (corrupt/ambiguous partition table) */
    g_partition_count = 1;
    g_last_policy = GPT_POLICY_REJECT_INVALID;
    s_last_serial_log[0] = '\0';
    assert(!usb_mount_production_storage(&bi));
    assert(str_contains(s_last_serial_log, "GPT policy rejected disk"));
    g_last_policy = GPT_POLICY_PRIMARY_CONSISTENT;

    /* Case F: Degraded mode accepted read-only */
    g_last_policy = GPT_POLICY_DEGRADED_PRIMARY;
    s_ext2_mount_called = false;
    s_last_serial_log[0] = '\0';
    assert(usb_mount_production_storage(&bi));
    assert(s_ext2_mount_called);
    assert(str_contains(s_last_serial_log, "PASS: Mounted sdap2 read-only at /mnt"));

    /* Case G: Explicit RW requested with consistent GPT and valid write/flush capabilities (Phase 9G.4) */
    copy_str(bi.cmdline, "usb_data=PARTUUID=CB667616-2FC7-4820-9B24-C5D9DE4227AD usb_data_mode=rw", sizeof(bi.cmdline));
    g_last_policy = GPT_POLICY_PRIMARY_CONSISTENT;
    g_partitions[0].block_dev.write_sector = mock_write_sector;
    g_partitions[0].block_dev.flush = mock_flush;
    s_ext2_mount_called = false;
    s_ext2_mount_rw_called = false;
    s_ext2_mount_rw_result = true;
    s_last_serial_log[0] = '\0';
    assert(usb_mount_production_storage(&bi));
    assert(s_ext2_mount_rw_called);
    assert(!s_ext2_mount_called);
    assert(str_contains(s_last_serial_log, "Mount mode: read-write"));
    assert(str_contains(s_last_serial_log, "PASS: Mounted sdap2 read-write at /mnt"));

    /* Case H: Explicit RW requested, but GPT policy is degraded (fallback to RO) */
    g_last_policy = GPT_POLICY_DEGRADED_PRIMARY;
    s_ext2_mount_called = false;
    s_ext2_mount_rw_called = false;
    s_last_serial_log[0] = '\0';
    assert(usb_mount_production_storage(&bi));
    assert(!s_ext2_mount_rw_called);
    assert(s_ext2_mount_called);
    assert(str_contains(s_last_serial_log, "GPT policy not strictly consistent; RW not eligible"));
    assert(str_contains(s_last_serial_log, "PASS: Mounted sdap2 read-only at /mnt"));

    /* Case I: Explicit RW requested, but device lacks write or flush (fallback to RO) */
    g_last_policy = GPT_POLICY_PRIMARY_CONSISTENT;
    g_partitions[0].block_dev.write_sector = NULL;
    s_ext2_mount_called = false;
    s_ext2_mount_rw_called = false;
    s_last_serial_log[0] = '\0';
    assert(usb_mount_production_storage(&bi));
    assert(!s_ext2_mount_rw_called);
    assert(s_ext2_mount_called);
    assert(str_contains(s_last_serial_log, "Device missing write or flush capability; RW not eligible"));
    assert(str_contains(s_last_serial_log, "PASS: Mounted sdap2 read-only at /mnt"));
    g_partitions[0].block_dev.write_sector = mock_write_sector;

    /* Case J: Explicit RW requested, but ext2_mount_rw fails (attempting RO fallback) */
    s_ext2_mount_called = false;
    s_ext2_mount_rw_called = false;
    s_ext2_mount_rw_result = false;
    s_ext2_mount_result = true;
    s_last_serial_log[0] = '\0';
    assert(usb_mount_production_storage(&bi));
    assert(s_ext2_mount_rw_called);
    assert(s_ext2_mount_called);
    assert(str_contains(s_last_serial_log, "ext2 writable mount failed on sdap2; attempting read-only fallback"));
    assert(str_contains(s_last_serial_log, "PASS: Mounted sdap2 read-only at /mnt"));

    /* A device rejecting SYNCHRONIZE CACHE must never enter the RW mount,
     * which would dirty the superblock before discovering the flush failure. */
    s_ext2_mount_called = s_ext2_mount_rw_called = false;
    s_ext2_mount_rw_result = true;
    s_flush_result = false;
    s_flush_calls = s_write_calls = s_report_calls = 0;
    s_last_serial_log[0] = '\0';
    assert(usb_mount_production_storage(&bi));
    assert(s_flush_calls == 1 && s_write_calls == 0 && s_report_calls == 1);
    assert(!s_ext2_mount_rw_called && s_ext2_mount_called);
    assert(str_contains(s_last_serial_log, "Flush preflight failed before filesystem writes"));
    assert(!str_contains(s_last_serial_log, "Mount mode: read-write"));
    assert(str_contains(s_last_serial_log, "PASS: Mounted sdap2 read-only at /mnt"));
    /* Case K: Explicit RW requested, durability = READ_ONLY (device reported WCE=1, sync failed) */
    s_durability_mode = USB_DURABILITY_READ_ONLY;
    g_last_policy = GPT_POLICY_PRIMARY_CONSISTENT;
    g_partitions[0].block_dev.write_sector = mock_write_sector;
    g_partitions[0].block_dev.flush = mock_flush;
    s_ext2_mount_called = s_ext2_mount_rw_called = false;
    s_flush_result = true;
    s_flush_calls = s_write_calls = s_report_calls = 0;
    s_last_serial_log[0] = '\0';
    assert(usb_mount_production_storage(&bi));
    assert(!s_ext2_mount_rw_called && s_ext2_mount_called);
    assert(s_flush_calls == 0 && s_write_calls == 0);
    assert(str_contains(s_last_serial_log, "Device classified read-only by durability probe; RW not eligible"));
    assert(str_contains(s_last_serial_log, "PASS: Mounted sdap2 read-only at /mnt"));
    s_durability_mode = USB_DURABILITY_SYNC_BACKED;
    printf("  [PASS] Durability READ_ONLY correctly blocks RW mount\n");

    /* Case L: Explicit RW requested, durability = ASSUMED_WRITE_THROUGH (device uncooperative, sync failed) */
    s_durability_mode = USB_DURABILITY_ASSUMED_WRITE_THROUGH;
    g_last_policy = GPT_POLICY_PRIMARY_CONSISTENT;
    g_partitions[0].block_dev.write_sector = mock_write_sector;
    g_partitions[0].block_dev.flush = mock_flush;
    s_ext2_mount_called = s_ext2_mount_rw_called = false;
    s_flush_result = true;
    s_flush_calls = s_write_calls = s_report_calls = 0;
    s_last_serial_log[0] = '\0';
    assert(usb_mount_production_storage(&bi));
    assert(s_ext2_mount_rw_called && !s_ext2_mount_called);
    assert(s_flush_calls == 1 && s_write_calls == 0);
    assert(str_contains(s_last_serial_log, "durability=assumed-write-through"));
    assert(str_contains(s_last_serial_log, "Mount mode: read-write"));
    assert(str_contains(s_last_serial_log, "PASS: Mounted sdap2 read-write at /mnt"));
    printf("  [PASS] Durability ASSUMED_WRITE_THROUGH mounts RW with disclosure log\n");

    /* Case M: Explicit RW requested, durability = UNKNOWN */
    s_durability_mode = USB_DURABILITY_UNKNOWN;
    s_ext2_mount_called = s_ext2_mount_rw_called = false;
    s_flush_result = true;
    s_flush_calls = s_write_calls = s_report_calls = 0;
    s_last_serial_log[0] = '\0';
    assert(usb_mount_production_storage(&bi));
    assert(!s_ext2_mount_rw_called && s_ext2_mount_called);
    assert(s_flush_calls == 0 && s_write_calls == 0);
    assert(str_contains(s_last_serial_log, "Cache durability unknown; RW not eligible (fallback to RO)"));
    assert(str_contains(s_last_serial_log, "PASS: Mounted sdap2 read-only at /mnt"));
    printf("  [PASS] Durability UNKNOWN correctly blocks RW mount\n");

    /* Reset flush state for remaining tests */
    s_durability_mode = USB_DURABILITY_SYNC_BACKED;
    s_flush_result = true;
}

static void test_usb_mount_sync(void) {
    printf("[HOST TEST] Testing usb_mount_sync()...\n");

    /* usb_mount_sync() before any RW mount returns false */
    /* Note: s_mounted_rw_dev is a static in usb_mount.c; we trigger its population
     * by running a successful RW mount in a fresh state. First test the no-mount case. */
    /* (s_mounted_rw_dev is already set from the last Case G run, so flush is expected) */

    /* Re-test with a controlled flush failure */
    s_flush_result = false;
    bool sync_result = usb_mount_sync();
    /* Either it was set (and fails due to flush) or not set (and fails due to NULL). Either is false. */
    assert(!sync_result);
    s_flush_result = true;

    /* Now do a fresh RW mount to set s_mounted_rw_dev and verify sync passes */
    block_dev_t usb_sda_s = {.name = "sda"};
    gpt_guid_t tg;
    assert(gpt_str_to_guid("AB001234-2FC7-4820-9B24-C5D9DE4227AD", &tg));
    g_partition_count = 1;
    g_partitions[0].parent = &usb_sda_s;
    g_partitions[0].unique_guid = tg;
    g_partitions[0].block_dev.write_sector = mock_write_sector;
    g_partitions[0].block_dev.flush = mock_flush;
    copy_str(g_partitions[0].block_dev.name, "sdap2", sizeof(g_partitions[0].block_dev.name));
    g_last_policy = GPT_POLICY_PRIMARY_CONSISTENT;
    s_durability_mode = USB_DURABILITY_SYNC_BACKED;
    s_flush_result = true;
    s_flush_calls = 0;
    s_ext2_mount_rw_result = true;
    s_ext2_mount_rw_called = false;

    boot_info_t bi_s = {0};
    copy_str(bi_s.cmdline,
             "usb_data=PARTUUID=AB001234-2FC7-4820-9B24-C5D9DE4227AD usb_data_mode=rw",
             sizeof(bi_s.cmdline));
    assert(usb_mount_production_storage(&bi_s));
    assert(s_ext2_mount_rw_called);

    /* sync() with active RW mount: should flush once more and return true */
    unsigned pre = s_flush_calls;
    assert(usb_mount_sync());
    assert(s_flush_calls == pre + 1);

    /* sync() with flush failure: should return false */
    s_flush_result = false;
    assert(!usb_mount_sync());
    s_flush_result = true;

    printf("  [PASS] usb_mount_sync verified\n");
}

int main(void) {
    test_guid_conversions();
    test_cmdline_parsing();
    test_production_mount_selection();
    test_usb_mount_sync();
    printf("[ALL PASS] Phase 9G.4 host unit tests passed successfully!\n");
    return 0;
}
