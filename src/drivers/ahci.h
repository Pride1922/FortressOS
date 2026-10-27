#ifndef FORTRESS_AHCI_H
#define FORTRESS_AHCI_H
#include "block.h"
#include "install_abi.h"

#define AHCI_MAX_DISKS 8u
/* Boot-only, pinned inventory of ATA disks behind PCI SATA/AHCI controllers.
 * Polling, one outstanding command per port; no NCQ, ATAPI, IDE or RAID/RST.
 * Published disks live for the boot. Uncertain DMA ownership is terminal and
 * retains all descriptor/bounce frames. No hotplug or automatic retry. */
size_t ahci_inventory_boot(void);
bool ahci_inventory_valid(void);
block_dev_t *ahci_get_disk(unsigned index);
/* Fresh IDENTIFY + pinned BDF/BAR/link/geometry validation. Port+1 is the
 * logical-device identifier in the existing install identity's nsid field. */
bool ahci_identity(unsigned index, install_identity_v1_t *identity);
bool ahci_quiesce(void);

/* ATA IDENTIFY parser, also exercised by the actual-driver host tests. */
typedef struct {
    uint64_t sectors;
    uint32_t sector_size;
    bool flush_ext;
    char serial[20], model[40];
    uint8_t wwn[8];
} ahci_geometry_t;
bool ahci_decode_identify(const uint16_t words[256], ahci_geometry_t *out);
#endif
