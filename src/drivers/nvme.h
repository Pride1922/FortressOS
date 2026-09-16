#ifndef FORTRESS_NVME_H
#define FORTRESS_NVME_H

#include "types.h"

/* NVMe Controller Register Offsets (Bar 0) */
#define NVME_REG_CAP    0x0000  /* Controller Capabilities (64-bit) */
#define NVME_REG_VS     0x0008  /* Version (32-bit) */
#define NVME_REG_INTMS  0x000C  /* Interrupt Mask Set (32-bit) */
#define NVME_REG_INTMC  0x0010  /* Interrupt Mask Clear (32-bit) */
#define NVME_REG_CC     0x0014  /* Controller Configuration (32-bit) */
#define NVME_REG_CSTS   0x001C  /* Controller Status (32-bit) */
#define NVME_REG_NSSR   0x0020  /* NVM Subsystem Reset (32-bit) */
#define NVME_REG_AQA    0x0024  /* Admin Queue Attributes (32-bit) */
#define NVME_REG_ASQ    0x0028  /* Admin Submission Queue Base (64-bit) */
#define NVME_REG_ACQ    0x0030  /* Admin Completion Queue Base (64-bit) */

/* Controller Capabilities (CAP) Bit Fields */
#define NVME_CAP_MQES(cap)      ((uint16_t)((cap) & 0xFFFF))
#define NVME_CAP_CQS(cap)       (((cap) >> 16) & 0x01)
#define NVME_CAP_AMS(cap)       (((cap) >> 17) & 0x03)
#define NVME_CAP_TO(cap)        ((uint8_t)(((cap) >> 24) & 0xFF))     /* 500ms units */
#define NVME_CAP_DSTRD(cap)     ((uint8_t)(((cap) >> 32) & 0x0F))     /* 2^(2+DSTRD) bytes */
#define NVME_CAP_NSSRS(cap)     (((cap) >> 36) & 0x01)
#define NVME_CAP_CSS(cap)       ((uint8_t)(((cap) >> 37) & 0xFF))
#define NVME_CAP_MPSMIN(cap)    ((uint8_t)(((cap) >> 48) & 0x0F))     /* 2^(12+MPSMIN) bytes */
#define NVME_CAP_MPSMAX(cap)    ((uint8_t)(((cap) >> 52) & 0x0F))

/* Controller Configuration (CC) Bit Fields */
#define NVME_CC_EN              (1U << 0)
#define NVME_CC_CSS_NVM         (0U << 4)
#define NVME_CC_MPS_4K          (0U << 7)
#define NVME_CC_IOSQES_64       (6U << 16)  /* 2^6 = 64 bytes */
#define NVME_CC_IOCQES_16       (4U << 20)  /* 2^4 = 16 bytes */

/* Controller Status (CSTS) Bit Fields */
#define NVME_CSTS_RDY           (1U << 0)   /* Ready */
#define NVME_CSTS_CFS           (1U << 1)   /* Controller Fatal Status */
#define NVME_CSTS_SHST_MASK     (3U << 2)

/* Admin Queue Attributes (AQA) Bit Fields */
#define NVME_AQA_ASQS(size)     ((uint32_t)((size) - 1))
#define NVME_AQA_ACQS(size)     (((uint32_t)((size) - 1)) << 16)

/* NVMe Admin Opcodes */
#define NVME_ADMIN_OP_DELETE_SQ 0x00
#define NVME_ADMIN_OP_CREATE_SQ 0x01
#define NVME_ADMIN_OP_DELETE_CQ 0x04
#define NVME_ADMIN_OP_CREATE_CQ 0x05
#define NVME_ADMIN_OP_IDENTIFY  0x06
#define NVME_ADMIN_OP_ABORT     0x08
#define NVME_ADMIN_OP_SET_FEAT  0x09
#define NVME_ADMIN_OP_GET_FEAT  0x0A

/* NVMe NVM Command Opcodes */
#define NVME_NVM_OP_FLUSH       0x00
#define NVME_NVM_OP_WRITE       0x01
#define NVME_NVM_OP_READ        0x02

/* Identify Controller / Namespace CNS values */
#define NVME_IDENTIFY_CNS_NS        0x00
#define NVME_IDENTIFY_CNS_CTRL      0x01
#define NVME_IDENTIFY_CNS_ACTIVE_NS 0x02

/* Submission Queue Entry (64 bytes) */
typedef struct {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t cid;
    uint32_t nsid;
    uint64_t reserved;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed)) nvme_sq_entry_t;

/* Completion Queue Entry (16 bytes) */
typedef struct {
    uint32_t dw0;
    uint32_t dw1;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status; /* Bit 0: Phase (P), Bits 15:1: Status Code */
} __attribute__((packed)) nvme_cq_entry_t;

/* LBA Format Data Structure in Identify Namespace */
typedef struct {
    uint16_t ms;        /* Metadata Size */
    uint8_t  lbads;     /* LBA Data Size (2^lbads) */
    uint8_t  rp;        /* Relative Performance */
} __attribute__((packed)) nvme_lbaf_t;

/* Identify Namespace Structure (Partial / Relevant Fields, 4096 bytes total) */
typedef struct {
    uint64_t nsze;          /* Namespace Size (total blocks) */
    uint64_t ncap;          /* Namespace Capacity (allocatable blocks) */
    uint64_t nuse;          /* Namespace Utilization */
    uint8_t  nsfeat;        /* Namespace Features */
    uint8_t  nlbaf;         /* Number of LBA Formats (0-based) */
    uint8_t  flbas;         /* Formatted LBA Size (bits 3:0 format index, bit 4 extended) */
    uint8_t  mc;            /* Metadata Capabilities */
    uint8_t  dpc;           /* End-to-end Data Protection Capabilities */
    uint8_t  dps;           /* End-to-end Data Protection Settings */
    uint8_t  nmic;
    uint8_t  rescap;
    uint8_t  fpi;
    uint8_t  dlfeat;
    uint16_t nawun;
    uint16_t nawupf;
    uint16_t nacwu;
    uint16_t nabsn;
    uint16_t nabo;
    uint16_t nabspf;
    uint16_t noiob;
    uint8_t  nvmcap[16];
    uint8_t  reserved1[40];
    uint8_t  nguid[16];
    uint8_t  eui64[8];
    nvme_lbaf_t lbaf[16];   /* LBA Format 0-15 */
    uint8_t  reserved2[192];
    uint8_t  vendor_specific[3712];
} __attribute__((packed)) nvme_id_namespace_t;

/* Public NVMe Driver API */
bool     nvme_init(void);
bool     nvme_is_initialized(void);
uint32_t nvme_get_active_nsid(void);
uint64_t nvme_get_sector_count(void);
uint32_t nvme_get_sector_size(void);

/* Synchronous read of a single sector (LBA) into buffer */
bool     nvme_read_sector(uint64_t lba, void *buf);

/* Synchronous write of a single sector (LBA) from buffer */
bool     nvme_write_sector(uint64_t lba, const void *buf);

/* Synchronous flush: commit volatile write cache to non-volatile media */
bool     nvme_flush(void);

/* Controller Quiesce & Reset (safe shutdown without memory corruption) */
bool     nvme_quiesce(void);
bool     nvme_is_dma_quarantined(void);
bool     nvme_is_fatal(void);

#endif /* FORTRESS_NVME_H */
