#include "nvme.h"
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "serial.h"
#include "string.h"
#include "apic.h"
#include "../../user/tools/digest.h"

#define QUEUE_SIZE 32   /* 32 entries: compact, fits in 1 page, easily wraps in tests */

typedef struct {
    bool g_initialized;
    bool g_controller_fatal;
    bool g_dma_quarantined;
    uint16_t g_pci_seg;
    uint8_t g_pci_bus;
    uint8_t g_pci_dev;
    uint8_t g_pci_fn;
    uintptr_t g_bar0_phys;
    uintptr_t g_mmio_virt;
    uint8_t g_dstrd;
    uint32_t g_active_nsid;
    bool g_unique_namespace;
    bool g_root_geometry;
    uint64_t g_sector_count;
    uint32_t g_sector_size;
    bool g_installer_deadlines;
    bool g_installer_inventory_admitted;
    uint32_t g_controller_id;
    uintptr_t g_asq_phys;
    uintptr_t g_acq_phys;
    uintptr_t g_iosq_phys;
    uintptr_t g_iocq_phys;
    uintptr_t g_dma_buf_phys;
    uint16_t g_admin_sq_tail;
    uint16_t g_admin_cq_head;
    uint8_t g_admin_cq_phase;
    uint16_t g_io_sq_tail;
    uint16_t g_io_cq_head;
    uint8_t g_io_cq_phase;
    uint16_t g_cid_counter;
    nvme_sq_entry_t *g_asq_virt;
    nvme_cq_entry_t *g_acq_virt;
    nvme_sq_entry_t *g_iosq_virt;
    nvme_cq_entry_t *g_iocq_virt;
    uint8_t *g_dma_buf_virt;
} nvme_state_t;
static nvme_state_t states[8]={{.g_active_nsid=1,.g_sector_size=512,.g_admin_cq_phase=1,.g_io_cq_phase=1,.g_cid_counter=1}};
static nvme_state_t *active_state=&states[0];
#define g_initialized (active_state->g_initialized)
#define g_controller_fatal (active_state->g_controller_fatal)
#define g_dma_quarantined (active_state->g_dma_quarantined)
#define g_pci_seg (active_state->g_pci_seg)
#define g_pci_bus (active_state->g_pci_bus)
#define g_pci_dev (active_state->g_pci_dev)
#define g_pci_fn (active_state->g_pci_fn)
#define g_bar0_phys (active_state->g_bar0_phys)
#define g_mmio_virt (active_state->g_mmio_virt)
#define g_dstrd (active_state->g_dstrd)
#define g_active_nsid (active_state->g_active_nsid)
#define g_unique_namespace (active_state->g_unique_namespace)
#define g_root_geometry (active_state->g_root_geometry)
#define g_sector_count (active_state->g_sector_count)
#define g_sector_size (active_state->g_sector_size)
#define g_installer_deadlines (active_state->g_installer_deadlines)
#define g_installer_inventory_admitted (active_state->g_installer_inventory_admitted)
#define g_controller_id (active_state->g_controller_id)
#define g_asq_phys (active_state->g_asq_phys)
#define g_acq_phys (active_state->g_acq_phys)
#define g_iosq_phys (active_state->g_iosq_phys)
#define g_iocq_phys (active_state->g_iocq_phys)
#define g_dma_buf_phys (active_state->g_dma_buf_phys)
#define g_admin_sq_tail (active_state->g_admin_sq_tail)
#define g_admin_cq_head (active_state->g_admin_cq_head)
#define g_admin_cq_phase (active_state->g_admin_cq_phase)
#define g_io_sq_tail (active_state->g_io_sq_tail)
#define g_io_cq_head (active_state->g_io_cq_head)
#define g_io_cq_phase (active_state->g_io_cq_phase)
#define g_cid_counter (active_state->g_cid_counter)

#define g_asq_virt (active_state->g_asq_virt)
#define g_acq_virt (active_state->g_acq_virt)
#define g_iosq_virt (active_state->g_iosq_virt)
#define g_iocq_virt (active_state->g_iocq_virt)
#define g_dma_buf_virt (active_state->g_dma_buf_virt)
static pci_device_t inventory[8];
static size_t inventory_count;
static bool inventory_pinned;
static const pci_device_t *initial_device;
/* Controller State and Geometry */

static inline uint64_t nvme_read64(uint32_t offset);
/* Existing iteration caps remain a clock-fault backstop. Installer commands
 * also stop at ten monotonic seconds; CAP.TO transition deadline caps at 30s.
 * Normal live/installed driver policy is unchanged. No IRQ/PIT dependency. */
static uint64_t installer_deadline(bool transition) {
    if (!g_installer_deadlines) return UINT64_MAX;
    uint64_t hz=apic_poll_clock_hz(),now=apic_poll_clock_read();
    uint64_t half_seconds=transition ? (uint64_t)NVME_CAP_TO(nvme_read64(NVME_REG_CAP)) : 20;
    if (!half_seconds) half_seconds=1;
    if (half_seconds>60) half_seconds=60;
    uint64_t delta=hz*half_seconds/2;
    return now>UINT64_MAX-delta ? UINT64_MAX : now+delta;
}
static bool installer_expired(uint64_t deadline) {
    return g_installer_deadlines && apic_poll_clock_read()>=deadline;
}

/* DMA Queues & Bounce Buffer (Phys & Virt) */

/* Queue Pointers & Phases */

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
    uint64_t deadline=installer_deadline(true);
    while ((nvme_read32(NVME_REG_CSTS) & NVME_CSTS_RDY) != 0) {
        if (--timeout == 0 || installer_expired(deadline)) {
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
    uint64_t deadline=installer_deadline(false);
    while (true) {
        uint16_t status_word = g_acq_virt[g_admin_cq_head].status;
        uint8_t phase_bit = status_word & 0x01;

        if (phase_bit == g_admin_cq_phase) {
            break; /* Entry is ready */
        }

        /* Installer identity refreshes use this queue for every transfer.
         * Match the I/O queue's bounded status cadence rather than forcing an
         * MMIO access on every spin; ordinary boot polling stays unchanged. */
        if ((!g_installer_deadlines || (poll_count & 0x3ff)==0) &&
            (nvme_read32(NVME_REG_CSTS) & NVME_CSTS_CFS)) {
            serial_puts("[FAIL] NVMe Controller Fatal Status (CFS) detected during Admin poll!\n");
            g_controller_fatal = true;
            g_initialized = false;
            if (!nvme_quiesce()) {
                g_dma_quarantined = true;
            }
            return false;
        }

        if (--poll_count == 0 || ((poll_count & 0x3ff)==0 && installer_expired(deadline))) {
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
        /* PAUSE can trap under nested virtualization. Installer polling uses
         * memory completion checks between bounded relaxation points. */
        if (!g_installer_deadlines || (poll_count & 0x3ff)==0) __asm__ volatile("pause");
    }

    /* Validate Command ID */
    if (g_acq_virt[g_admin_cq_head].cid != expected_cid) {
        serial_puts("[FAIL] NVMe Admin CQ CID mismatch! Expected: ");
        serial_print_dec(expected_cid);
        serial_puts(", Got: ");
        serial_print_dec(g_acq_virt[g_admin_cq_head].cid);
        serial_puts("\n");
        if (g_installer_deadlines) {g_controller_fatal=true;g_initialized=false;if (!nvme_quiesce()) g_dma_quarantined=true;}
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
    uint64_t deadline=installer_deadline(false);
    while (true) {
        uint16_t status_word = g_iocq_virt[g_io_cq_head].status;
        uint8_t phase_bit = status_word & 0x01;

        if (phase_bit == g_io_cq_phase) {
            break; /* Entry is ready */
        }

        if ((poll_count & 0x3FF) == 0 && (nvme_read32(NVME_REG_CSTS) & NVME_CSTS_CFS)) {
            serial_puts("[FAIL] NVMe Controller Fatal Status (CFS) detected during I/O poll!\n");
            g_controller_fatal = true;
            g_initialized = false;
            if (!nvme_quiesce()) {
                g_dma_quarantined = true;
            }
            return false;
        }

        if (--poll_count == 0 || ((poll_count & 0x3ff)==0 && installer_expired(deadline))) {
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
        if (!g_installer_deadlines || (poll_count & 0x3ff)==0) __asm__ volatile("pause");
    }

    /* Validate Command ID */
    if (g_iocq_virt[g_io_cq_head].cid != expected_cid) {
        serial_puts("[FAIL] NVMe I/O CQ CID mismatch! Expected: ");
        serial_print_dec(expected_cid);
        serial_puts(", Got: ");
        serial_print_dec(g_iocq_virt[g_io_cq_head].cid);
        serial_puts("\n");
        if (g_installer_deadlines) {g_controller_fatal=true;g_initialized=false;if (!nvme_quiesce()) g_dma_quarantined=true;}
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
    if (initial_device) pci_dev=*initial_device;
    else if (!pci_find_device(PCI_CLASS_STORAGE, PCI_SUBCLASS_STORAGE_NVME, PCI_PROGIF_STORAGE_NVME, &pci_dev)) {
        serial_puts("[FAIL] No NVMe storage controller found on PCI bus!\n");
        return false;
    }

    g_pci_seg   = pci_dev.segment;
    g_controller_id=(uint32_t)pci_dev.vendor_id | (uint32_t)pci_dev.device_id<<16;
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
    uint64_t deadline=installer_deadline(true);
    while ((nvme_read32(NVME_REG_CSTS) & NVME_CSTS_RDY) == 0) {
        if (nvme_read32(NVME_REG_CSTS) & NVME_CSTS_CFS) {
            serial_puts("[FAIL] NVMe Controller Fatal Status during enable!\n");
            nvme_quiesce();
            return false;
        }
        if (--wait_timeout == 0 || installer_expired(deadline)) {
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

    g_unique_namespace=false;
    if (!nvme_submit_admin_cmd(&cmd, NULL)) {
        serial_puts("[WARN] Active Namespace List query unsupported, assuming NSID 1\n");
        g_active_nsid = 1;
    } else {
        uint32_t *active_list = (uint32_t *)g_dma_buf_virt;
        g_unique_namespace=active_list[0]!=0;
        for (unsigned i=1;i<PAGE_SIZE/sizeof(uint32_t);i++)
            if (active_list[i]) g_unique_namespace=false;
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

    if (flbas_idx>id_ns->nlbaf || (id_ns->flbas & 0xf0) ||
        (id_ns->lbaf[flbas_idx].lbads!=9 && id_ns->lbaf[flbas_idx].lbads!=12)) {
        serial_puts("[FAIL] Namespace uses unsupported LBA geometry!\n");
        nvme_quiesce();return false;
    }

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

    g_sector_size = 1u << id_ns->lbaf[flbas_idx].lbads;
    if (g_sector_count>UINT64_MAX/g_sector_size) {nvme_quiesce();return false;}
    g_root_geometry=g_sector_size==512 && id_ns->ncap==id_ns->nsze && !(id_ns->nsfeat & 1);

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

/* Boot-only inventory. Each controller keeps distinct DMA queues and failure
 * state. Switching is permitted only before storage publication, on the BSP.
 * No hotplug enrollment or DMA allocation reuse after failure. */
bool nvme_inventory_valid(void) {
    if (!inventory_pinned || !pci_inventory_complete()) return false;
    /* Complete discovery is pinned once, as with the legacy singleton. Never
     * rescan all firmware bus ranges in the block-I/O path or enroll hotplug.
     * Refresh every pinned BDF's controller ID, class and BAR mapping. */
    for (size_t i=0;i<inventory_count;i++) {
        const pci_device_t *d=&inventory[i];
        if (pci_read_config32(d->segment,d->bus,d->device,d->function,PCI_REG_VENDOR_ID)!=
            ((uint32_t)d->vendor_id | (uint32_t)d->device_id<<16)) return false;
        uint32_t class=pci_read_config32(d->segment,d->bus,d->device,d->function,8);
        if ((class>>8)!=((uint32_t)PCI_CLASS_STORAGE<<16 | (uint32_t)PCI_SUBCLASS_STORAGE_NVME<<8 | PCI_PROGIF_STORAGE_NVME)) return false;
        uint32_t low=pci_read_config32(d->segment,d->bus,d->device,d->function,PCI_REG_BAR0);
        uint64_t bar=low & PCI_BAR_MEM_ADDR_MASK;
        if (low & PCI_BAR_IO_SPACE) return false;
        if ((low & PCI_BAR_MEM_TYPE_MASK)==PCI_BAR_MEM_TYPE_64)
            bar|=(uint64_t)pci_read_config32(d->segment,d->bus,d->device,d->function,PCI_REG_BAR1)<<32;
        if (bar!=d->bar[0]) return false;
    }
    return true;
}
size_t nvme_inventory_boot(void) {
    if (inventory_pinned) return nvme_inventory_valid() ? inventory_count : 0;
    if (!pci_inventory_complete() || !apic_poll_clock_hz() || apic_poll_clock_hz()>UINT64_MAX/60) return 0;
    pci_device_t devices[9];
    size_t n=pci_find_all_devices(PCI_CLASS_STORAGE,PCI_SUBCLASS_STORAGE_NVME,PCI_PROGIF_STORAGE_NVME,devices,9);
    if (!n || n>8 || !pci_inventory_complete()) return 0;
    /* Existing singleton state may already have DMA ownership. Never overwrite it. */
    if (g_mmio_virt || g_initialized || g_controller_fatal || g_dma_quarantined) return 0;
    memcpy(inventory,devices,n*sizeof(*devices));inventory_count=n;inventory_pinned=true;
    for (size_t i=0;i<n;i++) {
        active_state=&states[i];g_active_nsid=1;g_sector_size=512;
        g_admin_cq_phase=g_io_cq_phase=1;g_cid_counter=1;g_installer_deadlines=true;
        initial_device=&inventory[i];
        if (!nvme_init()) { g_controller_fatal=true; /* Retain all DMA, no retry. */ }
    }
    initial_device=NULL;active_state=&states[0];
    return nvme_inventory_valid() ? n : 0;
}
bool nvme_inventory_select(unsigned index) {
    if (!nvme_inventory_valid() || index>=inventory_count) return false;
    active_state=&states[index];
    return g_initialized && !g_controller_fatal && !g_dma_quarantined;
}

bool nvme_installer_identity(install_identity_v1_t *id) {
    if (!id || !apic_poll_clock_hz() || apic_poll_clock_hz()>UINT64_MAX/60) return false;
    if (!pci_inventory_complete()) return false;
    if (!g_installer_inventory_admitted && !inventory_pinned) {
        pci_device_t controllers[2];
        if (pci_find_all_devices(PCI_CLASS_STORAGE,PCI_SUBCLASS_STORAGE_NVME,
            PCI_PROGIF_STORAGE_NVME,controllers,2)!=1 || !pci_inventory_complete()) return false;
        if (!nvme_init() || controllers[0].segment!=g_pci_seg || controllers[0].bus!=g_pci_bus ||
            controllers[0].device!=g_pci_dev || controllers[0].function!=g_pci_fn) return false;
        /* Pin this complete singleton admission for boot lifetime. There is no
         * hotplug enrollment: every later operation refreshes only the selected
         * BDF/controller and its complete namespace identity below. */
        g_installer_inventory_admitted=true;
    }
    g_installer_deadlines=true;
    if (inventory_pinned && !nvme_inventory_valid()) return false;
    if (!nvme_init() || !g_unique_namespace || !g_root_geometry || g_sector_size!=512 ||
        pci_read_config32(g_pci_seg,g_pci_bus,g_pci_dev,g_pci_fn,PCI_REG_VENDOR_ID)!=g_controller_id) return false;
    install_identity_v1_t next={.size=sizeof(next),.version=1,.generation=1,
        .sector_count=g_sector_count,.sector_size=g_sector_size,.nsid=g_active_nsid,
        .segment=g_pci_seg,.bus=g_pci_bus,.device=g_pci_dev,.function=g_pci_fn,.flags=INSTALL_ID_ELIGIBLE};
    nvme_sq_entry_t cmd={.opcode=NVME_ADMIN_OP_IDENTIFY,.prp1=g_dma_buf_phys,.cdw10=NVME_IDENTIFY_CNS_CTRL};
    memset(g_dma_buf_virt,0,PAGE_SIZE);
    if (!nvme_submit_admin_cmd(&cmd,NULL)) return false;
    memcpy(next.serial,g_dma_buf_virt+4,20);memcpy(next.model,g_dma_buf_virt+24,40);
    /* Refresh complete active list and conventional geometry every operation.
     * A namespace replacement or unsupported list never inherits write access. */
    cmd=(nvme_sq_entry_t){.opcode=NVME_ADMIN_OP_IDENTIFY,.prp1=g_dma_buf_phys,.cdw10=NVME_IDENTIFY_CNS_ACTIVE_NS};
    memset(g_dma_buf_virt,0,PAGE_SIZE);
    if (!nvme_submit_admin_cmd(&cmd,NULL)) return false;
    uint32_t *active=(void *)g_dma_buf_virt;
    if (active[0]!=g_active_nsid) return false;
    for (unsigned i=1;i<PAGE_SIZE/4;i++) if (active[i]) return false;
    cmd=(nvme_sq_entry_t){.opcode=NVME_ADMIN_OP_IDENTIFY,.nsid=g_active_nsid,.prp1=g_dma_buf_phys,.cdw10=NVME_IDENTIFY_CNS_NS};
    memset(g_dma_buf_virt,0,PAGE_SIZE);
    if (!nvme_submit_admin_cmd(&cmd,NULL)) return false;
    const nvme_id_namespace_t *ns=(void *)g_dma_buf_virt;
    unsigned index=ns->flbas&15;
    if (index>ns->nlbaf || (ns->flbas&0xf0) || ns->dps || ns->lbaf[index].ms ||
        ns->lbaf[index].lbads!=9 || ns->nsze!=g_sector_count || ns->ncap!=ns->nsze || (ns->nsfeat&1)) return false;
    memcpy(next.nguid,ns->nguid,16);memcpy(next.eui64,ns->eui64,8);
    digest_t d;digest_init(&d,DIGEST_SHA256);(void)digest_update(&d,&next,128);digest_final(&d,next.digest);
    *id=next;return true;
}
bool nvme_is_initialized(void) {
    return g_initialized;
}

uint32_t nvme_get_active_nsid(void) {
    return g_active_nsid;
}
bool nvme_has_unique_namespace(void) { return g_initialized && g_unique_namespace; }
bool nvme_root_geometry_supported(void) {return g_initialized && g_root_geometry;}

uint64_t nvme_get_sector_count(void) {
    return g_sector_count;
}

uint32_t nvme_get_sector_size(void) {
    return g_sector_size;
}

/* Synchronous Read of a Single Sector via I/O Queue 1 */
bool nvme_read_sectors(uint64_t lba, uint32_t count, void *buf) {
    if (!g_initialized || !buf || !count || g_controller_fatal || g_dma_quarantined) {
        return false;
    }

    uint32_t max_sectors = PAGE_SIZE / g_sector_size;
    uint8_t *dest = (uint8_t *)buf;
    while (count > 0) {
        uint32_t n = count > max_sectors ? max_sectors : count;
        memset(g_dma_buf_virt, 0, n * g_sector_size);

        nvme_sq_entry_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.opcode = NVME_NVM_OP_READ;
        cmd.nsid   = g_active_nsid;
        cmd.prp1   = g_dma_buf_phys;
        cmd.cdw10  = (uint32_t)(lba & 0xFFFFFFFF);
        cmd.cdw11  = (uint32_t)(lba >> 32);
        cmd.cdw12  = (uint32_t)(n - 1); /* 0-based */

        if (!nvme_submit_io_cmd(&cmd, NULL)) {
            return false;
        }

        memcpy(dest, g_dma_buf_virt, n * g_sector_size);
        dest += n * g_sector_size;
        lba += n;
        count -= n;
    }
    return true;
}

bool nvme_read_sector(uint64_t lba, void *buf) {
    return nvme_read_sectors(lba, 1, buf);
}

/* Synchronous Write of Multiple Sectors via I/O Queue 1 */
bool nvme_write_sectors(uint64_t lba, uint32_t count, const void *buf) {
    if (!g_initialized || !buf || !count || g_controller_fatal || g_dma_quarantined) {
        return false;
    }

    uint32_t max_sectors = PAGE_SIZE / g_sector_size;
    const uint8_t *src = (const uint8_t *)buf;
    while (count > 0) {
        uint32_t n = count > max_sectors ? max_sectors : count;
        memcpy(g_dma_buf_virt, src, n * g_sector_size);

        nvme_sq_entry_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.opcode = NVME_NVM_OP_WRITE;
        cmd.nsid   = g_active_nsid;
        cmd.prp1   = g_dma_buf_phys;
        cmd.cdw10  = (uint32_t)(lba & 0xFFFFFFFF);
        cmd.cdw11  = (uint32_t)(lba >> 32);
        cmd.cdw12  = (uint32_t)(n - 1); /* 0-based */

        if (!nvme_submit_io_cmd(&cmd, NULL)) {
            return false;
        }

        src += n * g_sector_size;
        lba += n;
        count -= n;
    }
    return true;
}

bool nvme_write_sector(uint64_t lba, const void *buf) {
    return nvme_write_sectors(lba, 1, buf);
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

