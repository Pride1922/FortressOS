#include "nvme.h"
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "serial.h"
#include "string.h"

#define QUEUE_SIZE 32   /* 32 entries: compact, fits in 1 page, easily wraps in tests */

/* Controller State and Geometry */
static bool     g_initialized   = false;
static bool     g_controller_fatal = false;
static bool     g_dma_quarantined  = false;
static uint16_t g_pci_seg       = 0;
static uint8_t  g_pci_bus       = 0;
static uint8_t  g_pci_dev       = 0;
static uint8_t  g_pci_fn        = 0;
static uintptr_t g_bar0_phys    = 0;
static uintptr_t g_mmio_virt    = 0;

static uint8_t  g_dstrd         = 0;
static uint32_t g_active_nsid   = 1;
static uint64_t g_sector_count  = 0;
static uint32_t g_sector_size   = 512;

/* DMA Queues & Bounce Buffer (Phys & Virt) */
static uintptr_t g_asq_phys     = 0;
static uintptr_t g_acq_phys     = 0;
static uintptr_t g_iosq_phys    = 0;
static uintptr_t g_iocq_phys    = 0;
static uintptr_t g_dma_buf_phys = 0;

static nvme_sq_entry_t *g_asq_virt  = NULL;
static nvme_cq_entry_t *g_acq_virt  = NULL;
static nvme_sq_entry_t *g_iosq_virt = NULL;
static nvme_cq_entry_t *g_iocq_virt = NULL;
static uint8_t         *g_dma_buf_virt = NULL;

/* Queue Pointers & Phases */
static uint16_t g_admin_sq_tail   = 0;
static uint16_t g_admin_cq_head   = 0;
static uint8_t  g_admin_cq_phase  = 1;

static uint16_t g_io_sq_tail      = 0;
static uint16_t g_io_cq_head      = 0;
static uint8_t  g_io_cq_phase     = 1;

static uint16_t g_cid_counter     = 1;

/* MMIO Register Access Primitives */
static inline void nvme_write32(uint32_t offset, uint32_t val) {
    volatile uint32_t *reg = (volatile uint32_t *)(g_mmio_virt + offset);
    *reg = val;
}

static inline uint32_t nvme_read32(uint32_t offset) {
    volatile uint32_t *reg = (volatile uint32_t *)(g_mmio_virt + offset);
    return *reg;
}

static inline uint64_t nvme_read64(uint32_t offset) {
    volatile uint32_t *lo = (volatile uint32_t *)(g_mmio_virt + offset);
    volatile uint32_t *hi = (volatile uint32_t *)(g_mmio_virt + offset + 4);
    uint32_t lo_val = *lo;
    uint32_t hi_val = *hi;
    return ((uint64_t)hi_val << 32) | lo_val;
}

static inline void nvme_write64(uint32_t offset, uint64_t val) {
    volatile uint32_t *lo = (volatile uint32_t *)(g_mmio_virt + offset);
    volatile uint32_t *hi = (volatile uint32_t *)(g_mmio_virt + offset + 4);
    *lo = (uint32_t)(val & 0xFFFFFFFF);
    *hi = (uint32_t)(val >> 32);
}

/* Doorbell Register Offset Calculations */
static inline uint32_t nvme_sq_doorbell_offset(uint16_t qid) {
    return 0x1000 + (2 * qid) * (4 << g_dstrd);
}

static inline uint32_t nvme_cq_doorbell_offset(uint16_t qid) {
    return 0x1000 + (2 * qid + 1) * (4 << g_dstrd);
}

/* Safe Controller Quiesce & Reset */
bool nvme_quiesce(void) {
    if (g_mmio_virt == 0) return true;

    uint32_t cc = nvme_read32(NVME_REG_CC);
    if ((cc & NVME_CC_EN) == 0) {
        return true; /* Already disabled */
    }

    serial_puts("[NVMe] Initiating controller quiesce (disabling CC.EN)...\n");
    cc &= ~NVME_CC_EN;
    nvme_write32(NVME_REG_CC, cc);

    /* Bounded poll wait for CSTS.RDY == 0 */
    uint64_t timeout = 50000000;
    while ((nvme_read32(NVME_REG_CSTS) & NVME_CSTS_RDY) != 0) {
        if (--timeout == 0) {
            serial_puts("[CRITICAL] NVMe quiesce timeout! CSTS.RDY failed to clear!\n");
            serial_puts("[CRITICAL] Controller may still access memory! Quarantining DMA buffers!\n");
            g_controller_fatal = true;
            g_dma_quarantined = true;
            g_initialized = false;
            return false;
        }
        __asm__ volatile("pause");
    }

    serial_puts("[ OK ] NVMe controller quiescent and reset (CSTS.RDY = 0)\n");
    return true;
}

/* Synchronous Admin Command Execution via Bounded Polling */
static bool nvme_submit_admin_cmd(nvme_sq_entry_t *cmd, nvme_cq_entry_t *out_cqe) {
    if (g_controller_fatal || g_dma_quarantined) {
        serial_puts("[FAIL] NVMe admin submission rejected: controller in fatal/quarantined state!\n");
        return false;
    }

    /* Check for controller fatal status before submission */
    if (nvme_read32(NVME_REG_CSTS) & NVME_CSTS_CFS) {
        serial_puts("[FAIL] NVMe Controller Fatal Status (CFS) detected before Admin cmd!\n");
        g_controller_fatal = true;
        g_initialized = false;
        if (!nvme_quiesce()) {
            g_dma_quarantined = true;
        }
        return false;
    }

    cmd->cid = g_cid_counter++;
    uint16_t expected_cid = cmd->cid;

    /* Write command to Admin SQ */
    memcpy(&g_asq_virt[g_admin_sq_tail], cmd, sizeof(nvme_sq_entry_t));
    g_admin_sq_tail = (g_admin_sq_tail + 1) % QUEUE_SIZE;

    /* Ring Admin SQ Tail Doorbell */
    nvme_write32(nvme_sq_doorbell_offset(0), g_admin_sq_tail);

    /* Bounded polling wait on Admin CQ */
    uint64_t poll_count = 100000000;
    while (true) {
        uint16_t status_word = g_acq_virt[g_admin_cq_head].status;
        uint8_t phase_bit = status_word & 0x01;

        if (phase_bit == g_admin_cq_phase) {
            break; /* Entry is ready */
        }

        if (nvme_read32(NVME_REG_CSTS) & NVME_CSTS_CFS) {
            serial_puts("[FAIL] NVMe Controller Fatal Status (CFS) detected during Admin poll!\n");
            g_controller_fatal = true;
            g_initialized = false;
            if (!nvme_quiesce()) {
                g_dma_quarantined = true;
            }
            return false;
        }

        if (--poll_count == 0) {
            serial_puts("[FAIL] NVMe Admin command timeout! Opcode: 0x");
            serial_print_hex(cmd->opcode);
            serial_puts(", CID: ");
            serial_print_dec(cmd->cid);
            serial_puts("\n");
            g_initialized = false;
            if (!nvme_quiesce()) {
                g_controller_fatal = true;
                g_dma_quarantined = true;
            }
            return false;
        }
        __asm__ volatile("pause");
    }

    /* Validate Command ID */
    if (g_acq_virt[g_admin_cq_head].cid != expected_cid) {
        serial_puts("[FAIL] NVMe Admin CQ CID mismatch! Expected: ");
        serial_print_dec(expected_cid);
        serial_puts(", Got: ");
        serial_print_dec(g_acq_virt[g_admin_cq_head].cid);
        serial_puts("\n");
        return false;
    }

    if (out_cqe) {
        *out_cqe = g_acq_virt[g_admin_cq_head];
    }

    uint16_t status_code = g_acq_virt[g_admin_cq_head].status >> 1;

    /* Advance Admin CQ Head */
    g_admin_cq_head++;
    if (g_admin_cq_head == QUEUE_SIZE) {
        g_admin_cq_head = 0;
        g_admin_cq_phase ^= 1;
    }

    /* Ring Admin CQ Head Doorbell */
    nvme_write32(nvme_cq_doorbell_offset(0), g_admin_cq_head);

    return (status_code == 0);
}

/* Synchronous I/O Command Execution via Bounded Polling */
static bool nvme_submit_io_cmd(nvme_sq_entry_t *cmd, nvme_cq_entry_t *out_cqe) {
    if (g_controller_fatal || g_dma_quarantined) {
        serial_puts("[FAIL] NVMe I/O submission rejected: controller in fatal/quarantined state!\n");
        return false;
    }

    if (nvme_read32(NVME_REG_CSTS) & NVME_CSTS_CFS) {
        serial_puts("[FAIL] NVMe Controller Fatal Status (CFS) detected before I/O!\n");
        g_controller_fatal = true;
        g_initialized = false;
        if (!nvme_quiesce()) {
            g_dma_quarantined = true;
        }
        return false;
    }

    cmd->cid = g_cid_counter++;
    uint16_t expected_cid = cmd->cid;

    /* Write command to I/O SQ 1 */
    memcpy(&g_iosq_virt[g_io_sq_tail], cmd, sizeof(nvme_sq_entry_t));
    g_io_sq_tail = (g_io_sq_tail + 1) % QUEUE_SIZE;

    /* Ring I/O SQ 1 Tail Doorbell */
    nvme_write32(nvme_sq_doorbell_offset(1), g_io_sq_tail);

    /* Bounded polling wait on I/O CQ 1 */
    uint64_t poll_count = 100000000;
    while (true) {
        uint16_t status_word = g_iocq_virt[g_io_cq_head].status;
        uint8_t phase_bit = status_word & 0x01;

        if (phase_bit == g_io_cq_phase) {
            break; /* Entry is ready */
        }

        if (nvme_read32(NVME_REG_CSTS) & NVME_CSTS_CFS) {
            serial_puts("[FAIL] NVMe Controller Fatal Status (CFS) detected during I/O poll!\n");
            g_controller_fatal = true;
            g_initialized = false;
            if (!nvme_quiesce()) {
                g_dma_quarantined = true;
            }
            return false;
        }

        if (--poll_count == 0) {
            serial_puts("[FAIL] NVMe I/O command timeout! Opcode: 0x");
            serial_print_hex(cmd->opcode);
            serial_puts(", CID: ");
            serial_print_dec(cmd->cid);
            serial_puts("\n");
            g_initialized = false;
            if (!nvme_quiesce()) {
                g_controller_fatal = true;
                g_dma_quarantined = true;
            }
            return false;
        }
        __asm__ volatile("pause");
    }

    /* Validate Command ID */
    if (g_iocq_virt[g_io_cq_head].cid != expected_cid) {
        serial_puts("[FAIL] NVMe I/O CQ CID mismatch! Expected: ");
        serial_print_dec(expected_cid);
        serial_puts(", Got: ");
        serial_print_dec(g_iocq_virt[g_io_cq_head].cid);
        serial_puts("\n");
        return false;
    }

    if (out_cqe) {
        *out_cqe = g_iocq_virt[g_io_cq_head];
    }

    uint16_t status_code = g_iocq_virt[g_io_cq_head].status >> 1;

    /* Advance I/O CQ 1 Head */
    g_io_cq_head++;
    if (g_io_cq_head == QUEUE_SIZE) {
        g_io_cq_head = 0;
        g_io_cq_phase ^= 1;
    }

    /* Ring I/O CQ 1 Head Doorbell */
    nvme_write32(nvme_cq_doorbell_offset(1), g_io_cq_head);

    return (status_code == 0);
}

/* Initialize NVMe Controller & Single I/O Queue Pair */
bool nvme_init(void) {
    if (g_initialized) {
        return true;
    }
    if (g_controller_fatal || g_dma_quarantined) {
        serial_puts("[FAIL] Cannot initialize NVMe controller: controller in fatal / quarantined state!\n");
        return false;
    }

    serial_puts("[NVMe] Probing PCI bus for NVMe Storage Controller...\n");

    /* 1. Discover NVMe PCI Device */
    pci_device_t pci_dev;
    if (!pci_find_device(PCI_CLASS_STORAGE, PCI_SUBCLASS_STORAGE_NVME, PCI_PROGIF_STORAGE_NVME, &pci_dev)) {
        serial_puts("[FAIL] No NVMe storage controller found on PCI bus!\n");
        return false;
    }

    g_pci_seg   = pci_dev.segment;
    g_pci_bus   = pci_dev.bus;
    g_pci_dev   = pci_dev.device;
    g_pci_fn    = pci_dev.function;
    g_bar0_phys = pci_dev.bar[0];

    serial_puts("[NVMe] Located NVMe Controller at ");
    pci_print_bdf(g_pci_seg, g_pci_bus, g_pci_dev, g_pci_fn);
    serial_puts(" (Vendor: ");
    serial_print_hex(pci_dev.vendor_id);
    serial_puts(", Device: ");
    serial_print_hex(pci_dev.device_id);
    serial_puts(", BAR0: ");
    serial_print_hex(g_bar0_phys);
    serial_puts(")\n");

    if (g_bar0_phys == 0 || pci_dev.bar_is_io[0]) {
        serial_puts("[FAIL] NVMe BAR0 is invalid or I/O mapped!\n");
        return false;
    }

    /* 2. Enable PCI Bus Mastering & Memory Space */
    uint16_t pci_cmd = pci_read_config16(g_pci_seg, g_pci_bus, g_pci_dev, g_pci_fn, PCI_REG_COMMAND);
    pci_cmd |= (PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER);
    pci_write_config16(g_pci_seg, g_pci_bus, g_pci_dev, g_pci_fn, PCI_REG_COMMAND, pci_cmd);
    serial_puts("[ OK ] PCI Bus Mastering and Memory Space enabled in Command register\n");

    /* 3. Map MMIO Space Uncached (Initial 4 pages / 16 KiB covering base registers and early doorbells) */
    uint64_t *kernel_pml4 = vmm_get_kernel_pml4_virt();
    uint64_t hhdm_offset = vmm_get_hhdm_offset();
    uintptr_t bar_page_base = g_bar0_phys & ~(PAGE_SIZE - 1);
    g_mmio_virt = bar_page_base + hhdm_offset + (g_bar0_phys & (PAGE_SIZE - 1));

    for (uintptr_t p = bar_page_base; p < bar_page_base + 4 * PAGE_SIZE; p += PAGE_SIZE) {
        uintptr_t virt = p + hhdm_offset;
        if (!vmm_is_mapped(kernel_pml4, virt)) {
            int res = vmm_map_page(kernel_pml4, virt, p, PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT | PTE_NX);
            if (res != VMM_OK && res != VMM_ERR_ALREADY_MAPPED) {
                serial_puts("[FAIL] Failed to map NVMe MMIO page: 0x");
                serial_print_hex(p);
                serial_puts("\n");
                return false;
            }
        }
    }
    serial_puts("[ OK ] NVMe MMIO registers initially mapped (16 KiB uncached)\n");

    /* 4. Inspect Controller Capabilities (CAP) */
    uint64_t cap = nvme_read64(NVME_REG_CAP);
    uint16_t mqes   = NVME_CAP_MQES(cap);
    uint8_t  to     = NVME_CAP_TO(cap);
    g_dstrd         = NVME_CAP_DSTRD(cap);
    uint8_t  mpsmin = NVME_CAP_MPSMIN(cap);

    serial_puts("[NVMe] Controller Capabilities:\n");
    serial_puts("       - Max Queue Entries (MQES limit): ");
    serial_print_dec(mqes + 1);
    serial_puts(" (driver requires 32)\n");
    serial_puts("       - Timeout (TO): ");
    serial_print_dec(to * 500);
    serial_puts(" ms\n       - Doorbell Stride (DSTRD): ");
    serial_print_dec(g_dstrd);
    serial_puts(" (");
    serial_print_dec(4 << g_dstrd);
    serial_puts(" bytes)\n       - Min Host Page Size (MPSMIN): ");
    serial_print_dec(1 << (12 + mpsmin));
    serial_puts(" bytes\n");

    /* Validate and dynamically ensure full doorbell mapping */
    /* Highest queue doorbell used: QID 1 (IOCQ) head doorbell = 0x1000 + 3 * (4 << g_dstrd) */
    uint32_t max_db_offset = 0x1000 + 3 * (4 << g_dstrd) + 4;
    uintptr_t required_bytes = (g_bar0_phys & (PAGE_SIZE - 1)) + max_db_offset;
    size_t required_pages = (required_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    serial_puts("[NVMe] Doorbell layout verification:\n");
    serial_puts("       - Maximum Doorbell Offset: ");
    serial_print_hex(max_db_offset);
    serial_puts("\n       - Required MMIO Pages: ");
    serial_print_dec(required_pages);
    serial_puts(" (initial mapping: 4 pages)\n");

    if (required_pages > 4) {
        for (uintptr_t p = bar_page_base + 4 * PAGE_SIZE; p < bar_page_base + required_pages * PAGE_SIZE; p += PAGE_SIZE) {
            uintptr_t virt = p + hhdm_offset;
            if (!vmm_is_mapped(kernel_pml4, virt)) {
                int res = vmm_map_page(kernel_pml4, virt, p, PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT | PTE_NX);
                if (res != VMM_OK && res != VMM_ERR_ALREADY_MAPPED) {
                    serial_puts("[FAIL] Failed to map extended NVMe MMIO page: 0x");
                    serial_print_hex(p);
                    serial_puts("\n");
                    return false;
                }
            }
        }
        serial_puts("[ OK ] Extended NVMe MMIO doorbell space mapped successfully\n");
    } else {
        serial_puts("[ OK ] Required doorbells fully covered within initial 16 KiB MMIO mapping\n");
    }

    if (mpsmin > 0) {
        serial_puts("[FAIL] Controller does not support 4 KiB host pages!\n");
        return false;
    }
    if ((mqes + 1) < QUEUE_SIZE) {
        serial_puts("[FAIL] Controller MQES limit too small for driver queue size (32)!\n");
        return false;
    }

    /* 5. Controller Reset (if already enabled, quiesce and wait for RDY=0) */
    if (!nvme_quiesce()) {
        return false;
    }

    /* 6. Allocate DMA Queues & Bounce Buffer */
    g_asq_phys     = pmm_alloc_page();
    g_acq_phys     = pmm_alloc_page();
    g_iosq_phys    = pmm_alloc_page();
    g_iocq_phys    = pmm_alloc_page();
    g_dma_buf_phys = pmm_alloc_page();

    if (!g_asq_phys || !g_acq_phys || !g_iosq_phys || !g_iocq_phys || !g_dma_buf_phys) {
        serial_puts("[FAIL] Out of physical memory allocating NVMe DMA frames!\n");
        return false;
    }

    g_asq_virt     = (nvme_sq_entry_t *)vmm_phys_to_virt(g_asq_phys);
    g_acq_virt     = (nvme_cq_entry_t *)vmm_phys_to_virt(g_acq_phys);
    g_iosq_virt    = (nvme_sq_entry_t *)vmm_phys_to_virt(g_iosq_phys);
    g_iocq_virt    = (nvme_cq_entry_t *)vmm_phys_to_virt(g_iocq_phys);
    g_dma_buf_virt = (uint8_t *)vmm_phys_to_virt(g_dma_buf_phys);

    memset(g_asq_virt, 0, PAGE_SIZE);
    memset(g_acq_virt, 0, PAGE_SIZE);
    memset(g_iosq_virt, 0, PAGE_SIZE);
    memset(g_iocq_virt, 0, PAGE_SIZE);
    memset(g_dma_buf_virt, 0, PAGE_SIZE);

    g_admin_sq_tail  = 0;
    g_admin_cq_head  = 0;
    g_admin_cq_phase = 1;

    g_io_sq_tail     = 0;
    g_io_cq_head     = 0;
    g_io_cq_phase    = 1;

    /* 7. Configure Admin Queue Attributes (AQA) & Bases */
    uint32_t aqa = NVME_AQA_ASQS(QUEUE_SIZE) | NVME_AQA_ACQS(QUEUE_SIZE);
    nvme_write32(NVME_REG_AQA, aqa);
    nvme_write64(NVME_REG_ASQ, g_asq_phys);
    nvme_write64(NVME_REG_ACQ, g_acq_phys);
    serial_puts("[ OK ] Admin Queues configured (Size: 32, ASQ: ");
    serial_print_hex(g_asq_phys);
    serial_puts(", ACQ: ");
    serial_print_hex(g_acq_phys);
    serial_puts(")\n");

    /* 8. Enable Controller (CC.EN = 1) */
    uint32_t cc = NVME_CC_EN | NVME_CC_CSS_NVM | NVME_CC_MPS_4K | NVME_CC_IOSQES_64 | NVME_CC_IOCQES_16;
    nvme_write32(NVME_REG_CC, cc);

    /* Bounded poll wait for CSTS.RDY == 1 */
    uint64_t wait_timeout = 50000000;
    while ((nvme_read32(NVME_REG_CSTS) & NVME_CSTS_RDY) == 0) {
        if (nvme_read32(NVME_REG_CSTS) & NVME_CSTS_CFS) {
            serial_puts("[FAIL] NVMe Controller Fatal Status during enable!\n");
            nvme_quiesce();
            return false;
        }
        if (--wait_timeout == 0) {
            serial_puts("[FAIL] Timeout waiting for NVMe CSTS.RDY == 1!\n");
            nvme_quiesce();
            return false;
        }
        __asm__ volatile("pause");
    }
    serial_puts("[ OK ] NVMe Controller successfully enabled (CSTS.RDY = 1)\n");

    /* 9. Discover Active Namespace via Identify Command */
    nvme_sq_entry_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_OP_IDENTIFY;
    cmd.prp1   = g_dma_buf_phys;
    cmd.cdw10  = NVME_IDENTIFY_CNS_ACTIVE_NS;

    if (!nvme_submit_admin_cmd(&cmd, NULL)) {
        serial_puts("[WARN] Active Namespace List query unsupported, assuming NSID 1\n");
        g_active_nsid = 1;
    } else {
        uint32_t *active_list = (uint32_t *)g_dma_buf_virt;
        if (active_list[0] != 0) {
            g_active_nsid = active_list[0];
        } else {
            g_active_nsid = 1;
        }
    }
    serial_puts("[NVMe] Discovered Active Namespace ID: ");
    serial_print_dec(g_active_nsid);
    serial_puts("\n");

    /* 10. Query Namespace Data & Validate Geometry */
    memset(g_dma_buf_virt, 0, PAGE_SIZE);
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_OP_IDENTIFY;
    cmd.nsid   = g_active_nsid;
    cmd.prp1   = g_dma_buf_phys;
    cmd.cdw10  = NVME_IDENTIFY_CNS_NS;

    if (!nvme_submit_admin_cmd(&cmd, NULL)) {
        serial_puts("[FAIL] Identify Namespace command failed!\n");
        nvme_quiesce();
        return false;
    }

    nvme_id_namespace_t *id_ns = (nvme_id_namespace_t *)g_dma_buf_virt;
    g_sector_count = id_ns->nsze;
    uint8_t flbas_idx = id_ns->flbas & 0x0F;

    if (g_sector_count == 0) {
        serial_puts("[FAIL] Active Namespace reports 0 capacity!\n");
        nvme_quiesce();
        return false;
    }

    /* Reject non-zero metadata or data protection extensions */
    if (id_ns->lbaf[flbas_idx].ms != 0) {
        serial_puts("[FAIL] Namespace uses unsupported metadata format!\n");
        nvme_quiesce();
        return false;
    }
    if (id_ns->dps != 0) {
        serial_puts("[FAIL] Namespace uses unsupported end-to-end data protection!\n");
        nvme_quiesce();
        return false;
    }

    g_sector_size = 1 << id_ns->lbaf[flbas_idx].lbads;

    serial_puts("[NVMe] Namespace Geometry:\n");
    serial_puts("       - Total Sectors (NSZE): ");
    serial_print_dec(g_sector_count);
    serial_puts("\n       - Sector Size (LBA): ");
    serial_print_dec(g_sector_size);
    serial_puts(" bytes\n       - Total Capacity: ");
    serial_print_dec((g_sector_count * g_sector_size) / (1024 * 1024));
    serial_puts(" MiB\n");

    /* 11. Create Single I/O Completion Queue (IOCQ 1) */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_OP_CREATE_CQ;
    cmd.prp1   = g_iocq_phys;
    cmd.cdw10  = ((QUEUE_SIZE - 1) << 16) | 1; /* QSIZE = 31, QID = 1 */
    cmd.cdw11  = 0x01;                         /* IEN = 0 (polling), PC = 1 (phys contiguous) */

    if (!nvme_submit_admin_cmd(&cmd, NULL)) {
        serial_puts("[FAIL] Create I/O Completion Queue failed!\n");
        nvme_quiesce();
        return false;
    }
    serial_puts("[ OK ] I/O Completion Queue 1 created (Size: 32, Phys: ");
    serial_print_hex(g_iocq_phys);
    serial_puts(")\n");

    /* 12. Create Single I/O Submission Queue (IOSQ 1) */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_OP_CREATE_SQ;
    cmd.prp1   = g_iosq_phys;
    cmd.cdw10  = ((QUEUE_SIZE - 1) << 16) | 1; /* QSIZE = 31, QID = 1 */
    cmd.cdw11  = (1 << 16) | 0x01;             /* CQID = 1, PC = 1 */

    if (!nvme_submit_admin_cmd(&cmd, NULL)) {
        serial_puts("[FAIL] Create I/O Submission Queue failed!\n");
        nvme_quiesce();
        return false;
    }
    serial_puts("[ OK ] I/O Submission Queue 1 created (Size: 32, Phys: ");
    serial_print_hex(g_iosq_phys);
    serial_puts(")\n");

    g_initialized = true;
    serial_puts("[ OK ] NVMe Storage Driver successfully initialized and ready!\n\n");
    return true;
}

bool nvme_is_initialized(void) {
    return g_initialized;
}

uint32_t nvme_get_active_nsid(void) {
    return g_active_nsid;
}

uint64_t nvme_get_sector_count(void) {
    return g_sector_count;
}

uint32_t nvme_get_sector_size(void) {
    return g_sector_size;
}

/* Synchronous Read of a Single Sector via I/O Queue 1 */
bool nvme_read_sector(uint64_t lba, void *buf) {
    if (!g_initialized || !buf || g_controller_fatal || g_dma_quarantined) {
        return false;
    }

    memset(g_dma_buf_virt, 0, g_sector_size);

    nvme_sq_entry_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_NVM_OP_READ;
    cmd.nsid   = g_active_nsid;
    cmd.prp1   = g_dma_buf_phys;
    cmd.cdw10  = (uint32_t)(lba & 0xFFFFFFFF);
    cmd.cdw11  = (uint32_t)(lba >> 32);
    cmd.cdw12  = 0; /* 0-based: 0 indicates 1 logical block */

    if (!nvme_submit_io_cmd(&cmd, NULL)) {
        return false;
    }

    memcpy(buf, g_dma_buf_virt, g_sector_size);
    return true;
}

/* Synchronous Write of a Single Sector via I/O Queue 1 */
bool nvme_write_sector(uint64_t lba, const void *buf) {
    if (!g_initialized || !buf || g_controller_fatal || g_dma_quarantined) {
        return false;
    }

    memcpy(g_dma_buf_virt, buf, g_sector_size);

    nvme_sq_entry_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_NVM_OP_WRITE;
    cmd.nsid   = g_active_nsid;
    cmd.prp1   = g_dma_buf_phys;
    cmd.cdw10  = (uint32_t)(lba & 0xFFFFFFFF);
    cmd.cdw11  = (uint32_t)(lba >> 32);
    cmd.cdw12  = 0; /* 0-based: 0 indicates 1 logical block */

    return nvme_submit_io_cmd(&cmd, NULL);
}

/* Synchronous Flush of volatile cache to non-volatile media */
bool nvme_flush(void) {
    if (!g_initialized || g_controller_fatal || g_dma_quarantined) {
        return false;
    }

    nvme_sq_entry_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_NVM_OP_FLUSH;
    cmd.nsid   = g_active_nsid;

    return nvme_submit_io_cmd(&cmd, NULL);
}

bool nvme_is_dma_quarantined(void) {
    return g_dma_quarantined;
}

bool nvme_is_fatal(void) {
    return g_controller_fatal;
}

