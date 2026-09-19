#ifndef FORTRESS_XHCI_BOT_H
#define FORTRESS_XHCI_BOT_H

#include "xhci_dev.h"
#include "block.h"

/* USB Mass Storage Bulk-Only Transport (BOT) Signatures */
#define USB_BOT_CBW_SIGNATURE   0x43425355u /* "USBC" */
#define USB_BOT_CSW_SIGNATURE   0x53425355u /* "USBS" */

/* BOT CBW Flags */
#define USB_BOT_CBW_FLAG_IN     0x80u
#define USB_BOT_CBW_FLAG_OUT    0x00u

/* BOT CSW Status */
#define USB_BOT_CSW_STATUS_PASSED      0x00u
#define USB_BOT_CSW_STATUS_FAILED      0x01u
#define USB_BOT_CSW_STATUS_PHASE_ERROR 0x02u

/* Command Block Wrapper (CBW) - 31 bytes */
typedef struct {
    uint32_t dCBWSignature;          /* 0x43425355 ("USBC") */
    uint32_t dCBWTag;                /* Unique per-command tag */
    uint32_t dCBWDataTransferLength; /* Number of bytes to transfer */
    uint8_t  bmCBWFlags;             /* 0x80: Data-In, 0x00: Data-Out */
    uint8_t  bCBWLUN;                /* Bits 3:0 LUN (0) */
    uint8_t  bCBWCBLength;           /* Length of CDB (6, 10, 12, 16) */
    uint8_t  CBWCB[16];              /* Command Descriptor Block */
} __attribute__((packed)) usb_bot_cbw_t;

/* Command Status Wrapper (CSW) - 13 bytes */
typedef struct {
    uint32_t dCSWSignature;          /* 0x53425355 ("USBS") */
    uint32_t dCSWTag;                /* Must match dCBWTag */
    uint32_t dCSWDataResidue;        /* Difference between expected and actual data */
    uint8_t  bCSWStatus;             /* 0: Passed, 1: Failed, 2: Phase Error */
} __attribute__((packed)) usb_bot_csw_t;

/* SCSI Command Opcodes */
#define SCSI_CMD_TEST_UNIT_READY       0x00u
#define SCSI_CMD_REQUEST_SENSE         0x03u
#define SCSI_CMD_INQUIRY               0x12u
#define SCSI_CMD_MODE_SENSE_6          0x1Au
#define SCSI_CMD_READ_CAPACITY_10      0x25u
#define SCSI_CMD_READ_10               0x28u
#define SCSI_CMD_WRITE_10              0x2Au
#define SCSI_CMD_SYNCHRONIZE_CACHE_10  0x35u
#define SCSI_CMD_MODE_SENSE_10         0x5Au

/* USB Feature Selectors (for CLEAR_FEATURE) */
#define USB_FEATURE_ENDPOINT_HALT      0x00u
/* bmRequestType: endpoint, host-to-device */
#define USB_RT_ENDPOINT_OUT            0x02u

/* Standard SCSI INQUIRY Response (36 bytes) */
typedef struct {
    uint8_t  pdt;                    /* Peripheral Device Type (bits 4:0: 0x00 = disk) */
    uint8_t  removable;              /* Bit 7: RMB (Removable Media Bit) */
    uint8_t  version;                /* ANSI version */
    uint8_t  response_format;        /* Bits 3:0 response format */
    uint8_t  additional_length;      /* n - 4 (typically 31) */
    uint8_t  sccs;                   /* Embedded storage array flags */
    uint8_t  flags1;
    uint8_t  flags2;
    char     vendor[8];              /* T10 Vendor ID (ASCII, space padded) */
    char     product[16];            /* Product ID (ASCII, space padded) */
    char     revision[4];            /* Product Revision (ASCII) */
} __attribute__((packed)) scsi_inquiry_data_t;

/* Standard SCSI READ CAPACITY (10) Response (8 bytes) */
typedef struct {
    uint32_t last_lba_be;            /* Big-endian: Last Logical Block Address */
    uint32_t block_size_be;          /* Big-endian: Block size in bytes */
} __attribute__((packed)) scsi_read_capacity_data_t;

/* Standard SCSI REQUEST SENSE Response (18 bytes) */
typedef struct {
    uint8_t  response_code;          /* 0x70 or 0x71 */
    uint8_t  obsolete;
    uint8_t  sense_key;              /* Bits 3:0: Sense Key */
    uint32_t information;
    uint8_t  additional_sense_len;
    uint32_t cmd_specific;
    uint8_t  asc;                    /* Additional Sense Code */
    uint8_t  ascq;                   /* Additional Sense Code Qualifier */
    uint8_t  fruc;
    uint8_t  sense_key_specific[3];
} __attribute__((packed)) scsi_sense_data_t;

/* USB Durability Mode — device write durability classification */
typedef enum {
    USB_DURABILITY_UNKNOWN = 0,           /* Not yet probed */
    USB_DURABILITY_ASSUMED_WRITE_THROUGH, /* No caching page and sync failed; assumed write-through */
    USB_DURABILITY_WRITE_THROUGH,         /* WCE=0 in MODE SENSE caching page; barrier without cache command */
    USB_DURABILITY_SYNC_BACKED,           /* SYNCHRONIZE CACHE succeeded; every flush must complete */
    USB_DURABILITY_READ_ONLY              /* WCE=1+sync unavailable, transport error, or write-protect */
} usb_durability_mode_t;

/* Cache policy discovered via MODE SENSE caching page 0x08 */
typedef struct {
    bool     probed;              /* true if at least one MODE SENSE command completed */
    bool     wce;                 /* Write Cache Enable bit (WCE) */
    bool     rcd;                 /* Read Cache Disable bit */
    bool     write_protect;       /* Write Protect bit from mode parameter header */
    bool     sync_ok;             /* SYNCHRONIZE CACHE succeeded during probe */
    bool     ms6_attempted;       /* MODE SENSE(6) was attempted */
    bool     ms6_ok;              /* MODE SENSE(6) returned a valid caching page */
    bool     ms10_attempted;      /* MODE SENSE(10) was attempted */
    bool     ms10_ok;             /* MODE SENSE(10) returned a valid caching page */
    uint8_t  raw_ms6[28];         /* Raw MODE SENSE(6) response (up to 28 bytes) */
    uint8_t  raw_ms10[32];        /* Raw MODE SENSE(10) response (up to 32 bytes) */
    uint8_t  ms6_len;             /* Actual bytes captured for MODE SENSE(6) */
    uint8_t  ms10_len;            /* Actual bytes captured for MODE SENSE(10) */
    usb_durability_mode_t policy; /* Resulting classification */
} scsi_durability_info_t;

/* Bulk Endpoint Transfer Ring & State */
typedef struct {
    uint8_t opcode;
    uint8_t csw_status;
    bool command_failed; /* Valid CSW, status FAILED; REQUEST SENSE is safe. */
    bool transport_failed;
    bool sense_valid;
    uint8_t sense_response;
    uint8_t sense_key;
    uint8_t asc;
    uint8_t ascq;
} xhci_bot_error_t;

typedef struct {
    uintptr_t  bulk_in_ring_phys;
    xhci_trb_t *bulk_in_ring_virt;
    uint32_t   in_idx;
    uint8_t    in_cycle;
    uint8_t    in_dci;
    uint16_t   in_max_packet;

    uintptr_t  bulk_out_ring_phys;
    xhci_trb_t *bulk_out_ring_virt;
    uint32_t   out_idx;
    uint8_t    out_cycle;
    uint8_t    out_dci;
    uint16_t   out_max_packet;

    uint8_t    slot_id;
    uint32_t   tag;
    uint32_t   sector_size;
    uint64_t   sector_count;
    char       vendor[9];
    char       product[17];
    /* No reuse of these rings/bounce page after uncertain DMA completion.
     * Buffers remain allocated until reboot; runtime recovery is not supplied. */
    bool       transport_failed;
    bool       latched_offline;   /* Endpoint latched offline after unrecoverable stall */
    xhci_bot_error_t last_error;
    uint32_t   data_transferred;
    /* Durability state — set once by xhci_bot_probe_durability(), never changed except
     * on transport_failed/latched_offline latch. */
    usb_durability_mode_t durability_mode;
    scsi_durability_info_t durability_info;
} xhci_bot_rings_t;

/* Configures Bulk-In and Bulk-Out transfer rings on the controller via Configure Endpoint */
bool xhci_configure_bulk_endpoints(const xhci_rings_io_t *io,
                                   xhci_dma_buffers_t *ring_dma,
                                   const xhci_dev_dma_t *dev_dma,
                                   const xhci_bot_device_t *device,
                                   xhci_bot_rings_t *bot_rings);

/* Executes a synchronous BOT transaction: CBW -> Data -> CSW */
bool xhci_bot_transfer(const xhci_rings_io_t *io,
                       xhci_dma_buffers_t *ring_dma,
                       const xhci_dev_dma_t *dev_dma,
                       xhci_bot_rings_t *bot_rings,
                       const void *cdb,
                       uint8_t cdb_len,
                       void *data,
                       uint32_t data_len,
                       bool dir_in);

/* SCSI high-level commands */
bool xhci_scsi_inquiry(const xhci_rings_io_t *io,
                       xhci_dma_buffers_t *ring_dma,
                       const xhci_dev_dma_t *dev_dma,
                       xhci_bot_rings_t *bot_rings,
                       scsi_inquiry_data_t *inq);

bool xhci_scsi_test_unit_ready(const xhci_rings_io_t *io,
                               xhci_dma_buffers_t *ring_dma,
                               const xhci_dev_dma_t *dev_dma,
                               xhci_bot_rings_t *bot_rings);

bool xhci_scsi_read_capacity(const xhci_rings_io_t *io,
                             xhci_dma_buffers_t *ring_dma,
                             const xhci_dev_dma_t *dev_dma,
                             xhci_bot_rings_t *bot_rings,
                             uint64_t *out_sectors,
                             uint32_t *out_sector_size);

bool xhci_scsi_read_sector(const xhci_rings_io_t *io,
                           xhci_dma_buffers_t *ring_dma,
                           const xhci_dev_dma_t *dev_dma,
                           xhci_bot_rings_t *bot_rings,
                           uint64_t lba,
                           void *buf);

bool xhci_scsi_write_sector(const xhci_rings_io_t *io,
                            xhci_dma_buffers_t *ring_dma,
                            const xhci_dev_dma_t *dev_dma,
                            xhci_bot_rings_t *bot_rings,
                            uint64_t lba,
                            const void *buf);

bool xhci_scsi_sync_cache(const xhci_rings_io_t *io,
                          xhci_dma_buffers_t *ring_dma,
                          const xhci_dev_dma_t *dev_dma,
                          xhci_bot_rings_t *bot_rings);

/* MODE SENSE caching page discovery (Commit 2).
 * Attempts MODE SENSE(6) then MODE SENSE(10) for page 0x08 (current values).
 * Separately probes SYNCHRONIZE CACHE(10) with IMMED=0.
 * Populates *info and returns true if at least one form succeeded.
 * Never changes device settings; never uses MODE SELECT. */
bool xhci_scsi_probe_cache_policy(const xhci_rings_io_t *io,
                                  xhci_dma_buffers_t *ring_dma,
                                  const xhci_dev_dma_t *dev_dma,
                                  xhci_bot_rings_t *bot_rings,
                                  scsi_durability_info_t *info);

/* Durability state machine (Commit 3).
 * Calls probe_cache_policy(), classifies the device into one of the
 * USB_DURABILITY_* modes, and stores the result in bot_rings->durability_mode.
 * Must be called once after block device registration, before any write.
 * Prints the [USB DURABILITY] diagnostic. */
void xhci_bot_probe_durability(const xhci_rings_io_t *io,
                               xhci_dma_buffers_t *ring_dma,
                               const xhci_dev_dma_t *dev_dma,
                               xhci_bot_rings_t *bot_rings);

/* Durability-mode-aware flush barrier.
 * SYNC_BACKED: executes and verifies SYNCHRONIZE CACHE.
 * WRITE_THROUGH: succeeds immediately if transport is healthy (no cache command).
 * ASSUMED_WRITE_THROUGH: attempts SYNCHRONIZE CACHE; succeeds even if command is
 *                         rejected by the device, fails only on transport failure.
 * All other modes: fails immediately.
 * Latches the mode to READ_ONLY on failure; never silently ignores errors. */
bool xhci_bot_flush_barrier(const xhci_rings_io_t *io,
                            xhci_dma_buffers_t *ring_dma,
                            const xhci_dev_dma_t *dev_dma,
                            xhci_bot_rings_t *bot_rings);

/* Returns current durability mode without probing. */
usb_durability_mode_t xhci_bot_get_durability_mode(const xhci_bot_rings_t *bot_rings);

/* Bounded BOT endpoint stall recovery (Commit 1b).
 * Issues Stop Endpoint, Reset Endpoint, Set Dequeue Pointer, and USB CLEAR_FEATURE(HALT).
 * Sets latched_offline and returns false if any step times out or fails.
 * Must NOT be called after transport_failed is set. */
bool xhci_bot_endpoint_reset(const xhci_rings_io_t *io,
                             xhci_dma_buffers_t *ring_dma,
                             const xhci_dev_dma_t *dev_dma,
                             xhci_bot_rings_t *bot_rings,
                             uint8_t dci);

#endif /* FORTRESS_XHCI_BOT_H */
