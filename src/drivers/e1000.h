#ifndef FORTRESS_E1000_H
#define FORTRESS_E1000_H

#include "types.h"
#include "pci.h"
#include "net.h"

/* e1000 / e1000e Supported PCI IDs */
#define E1000_VENDOR_INTEL          0x8086
#define E1000_DEV_82540EM           0x100E  /* QEMU -device e1000 */
#define E1000_DEV_82574L            0x10D3  /* QEMU -device e1000e */
#define E1000_DEV_I219_LM           0x15B7  /* Dell Latitude 5590 physical */
#define E1000_DEV_I219_LM_ALT       0x156F  /* Dell variant */
#define E1000_DEV_I219_LM_15D7      0x15D7  /* Dell Latitude 5590 I219-LM (observed) */
#define E1000_DEV_I219_LM_15BD      0x15BD  /* Dell Latitude 5500 I219-LM */
#define E1000_DEV_I219_LM_15BB      0x15BB  /* I219-LM variant */

/* Dedicated higher-half kernel MMIO virtual address window */
#define E1000_MMIO_VIRT             0xFFFFFFFFE2000000ULL
#define E1000_DEFAULT_APERTURE      (128 * 1024) /* 128 KiB */

/* Register Offsets (Intel 8254x / 8257x family) */
#define E1000_REG_CTRL              0x0000  /* Device Control */
#define E1000_REG_STATUS            0x0008  /* Device Status */
#define E1000_REG_EECD              0x0010  /* EEPROM/Flash Control */
#define E1000_REG_EERD              0x0014  /* EEPROM Read */
#define E1000_REG_ICR               0x00C0  /* Interrupt Cause Read */
#define E1000_REG_IMS               0x00D0  /* Interrupt Mask Set */
#define E1000_REG_IMC               0x00D8  /* Interrupt Mask Clear */
#define E1000_REG_RCTL              0x0100  /* Receive Control */
#define E1000_REG_TCTL              0x0400  /* Transmit Control */
#define E1000_REG_RAL0              0x5400  /* Receive Address Low 0 */
#define E1000_REG_RAH0              0x5404  /* Receive Address High 0 */

/* STATUS Register Bits */
#define E1000_STATUS_FD             (1U << 0)   /* Full Duplex */
#define E1000_STATUS_LU             (1U << 1)   /* Link Up */
#define E1000_STATUS_SPEED_MASK     (3U << 6)   /* Link Speed */
#define E1000_STATUS_SPEED_10       (0U << 6)   /* 10 Mb/s */
#define E1000_STATUS_SPEED_100      (1U << 6)   /* 100 Mb/s */
#define E1000_STATUS_SPEED_1000     (2U << 6)   /* 1000 Mb/s (or 3) */

/* RAH0 Register Bits */
#define E1000_RAH_AV                (1U << 31)  /* Address Valid */

/* EERD Register Bits */
#define E1000_EERD_START            (1U << 0)   /* Start Read */
#define E1000_EERD_DONE             (1U << 4)   /* Read Done */
#define E1000_EERD_ADDR_SHIFT       8
#define E1000_EERD_DATA_SHIFT       16

/* e1000 Device Representation */
typedef struct {
    pci_device_t pci;
    uintptr_t    bar0_phys;
    uintptr_t    mmio_virt;
    uint32_t     aperture_size;
    uint8_t      mac_addr[6];
    uint32_t     status;
    bool         link_up;
    bool         initialized;
} e1000_device_t;

/* Public API */
bool e1000_boot_probe(void);
bool net_boot_probe(void);
const e1000_device_t *e1000_get_active_device(void);
net_dev_t *e1000_get_net_device(void);
int e1000_send_raw(net_dev_t *dev, const void *buf, size_t len);
pbuf_t *e1000_poll_rx(net_dev_t *dev);
void e1000_recycle_rx(net_dev_t *dev, pbuf_t *packet);
/* Explicit test hook, never invoked on a normal boot. */
void e1000_raw_selftest(const char *cmdline);
/* Fatal stop: permanently retains all DMA allocations, even after reset. */
bool e1000_quiesce(void);

#endif /* FORTRESS_E1000_H */
