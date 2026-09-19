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
#define SCSI_CMD_READ_CAPACITY_10      0x25u
#define SCSI_CMD_READ_10               0x28u

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

/* Bulk Endpoint Transfer Ring & State */
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

#endif /* FORTRESS_XHCI_BOT_H */
