/* Boot opt-in diagnostics only. Included by e1000.c after its print helpers.
 * Intel VT-d spec / Linux v6.12 intel/iommu.h and ACPI DMAR layout.
 * No writes to PCI status, VT-d registers, fault records or translation tables. */
#include "acpi.h"
#define NET_VTD_MAX 4
typedef struct { uintptr_t phys, virt; uint16_t segment; uint32_t fro, nfr; } net_vtd_t;
static net_vtd_t s_vtd[NET_VTD_MAX];
static unsigned s_vtd_count;
static const char *s_vtd_note = "not prepared";
static uint64_t diag_u64(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint16_t diag_u16(const void *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t vtd_read(net_vtd_t *u, unsigned off) {
    return *(volatile uint32_t *)(u->virt + off);
}
static uintptr_t vtd_map(uintptr_t phys, unsigned index, unsigned bytes) {
#ifdef E1000_PCH_HOST_TEST
    return e1000_mock_vtd_map(phys, index, bytes);
#elif defined(TEST_SMP_MEMORY)
    (void)phys; (void)index; (void)bytes; return 0;
#else
    uintptr_t virt = 0xffffffffe2100000ULL + index * 0x10000u;
    for (unsigned off = 0; off < bytes; off += PAGE_SIZE) {
        if (vmm_map_page(vmm_get_kernel_pml4_virt(), virt + off, phys + off,
                        PTE_PRESENT | PTE_PCD | PTE_PWT | PTE_NX) != VMM_OK)
            return 0; /* retain any partial mappings; never reuse the slot */
    }
    return virt;
#endif
}
static void net_vtd_prepare(void) {
    spin_debug_assert_unheld();
    s_vtd_count = 0;
    s_vtd_note = "DMAR absent/unavailable; does not prove translation disabled";
    acpi_sdt_header_t *table = acpi_find_table("DMAR");
    if (!table) return;
    if (table->length < 48 || table->length > MAX_ACPI_TABLE_SIZE) {
        s_vtd_note = "invalid DMAR length"; return;
    }
    const uint8_t *data = (const uint8_t *)table;
    uint8_t sum = 0;
    for (unsigned i = 0; i < table->length; ++i) sum += data[i];
    if (sum) { s_vtd_note = "invalid DMAR checksum"; return; }
    /* Validate all structure bounds before mapping anything. */
    for (unsigned off = 48; off < table->length;) {
        if (table->length - off < 4) { s_vtd_note = "truncated DMAR"; return; }
        unsigned len = diag_u16(data + off + 2);
        if (len < 4 || len > table->length - off ||
            (diag_u16(data + off) == 0 && len < 16)) {
            s_vtd_note = "malformed DMAR structure"; return;
        }
        off += len;
    }
    s_vtd_note = "read-only DRHD inventory; scope routing not resolved; at most four units";
    unsigned slot = 0;
    for (unsigned off = 48; off < table->length; off += diag_u16(data + off + 2)) {
        if (diag_u16(data + off) != 0) continue;
        if (slot == NET_VTD_MAX) { s_vtd_note = "DRHD inventory truncated at four units"; break; }
        unsigned index = slot++;
        uint64_t phys = diag_u64(data + off + 8);
        if (!phys || (phys & 4095) || phys > PTE_ADDR_MASK - 0x6000) {
            s_vtd_note = "invalid DRHD base; inventory incomplete"; continue;
        }
        net_vtd_t u = {.phys = phys, .segment = diag_u16(data + off + 6)};
        /* CAP.FRO (10 bits *16) + at most 256 fault records fits 0x6000.
         * Map header first; only map remaining pages after validating CAP. */
        u.virt = vtd_map(phys, index, PAGE_SIZE);
        if (!u.virt) { s_vtd_note = "DRHD mapping failed; inventory incomplete"; continue; }
        uint32_t ver = vtd_read(&u, 0);
        uint64_t cap = (uint64_t)vtd_read(&u, 8) | ((uint64_t)vtd_read(&u, 12) << 32);
        if (!ver || ver == UINT32_MAX || cap == UINT64_MAX) {
            s_vtd_note = "DRHD registers unavailable; inventory incomplete"; continue;
        }
        u.fro = ((cap >> 24) & 0x3ff) * 16;
        u.nfr = ((cap >> 40) & 255) + 1;
        unsigned end = u.fro + u.nfr * 16;
        if (u.fro < 0x100 || end > 0x6000) {
            s_vtd_note = "unsupported DRHD fault-register range"; continue;
        }
        /* Map only pages beyond the existing first page. */
#ifdef E1000_PCH_HOST_TEST
        if (!e1000_mock_vtd_map(phys, index, end)) continue;
#elif !defined(TEST_SMP_MEMORY)
        bool mapped = true;
        for (unsigned page = PAGE_SIZE; page < end; page += PAGE_SIZE)
            if (vmm_map_page(vmm_get_kernel_pml4_virt(), u.virt + page, phys + page,
                            PTE_PRESENT | PTE_PCD | PTE_PWT | PTE_NX) != VMM_OK) mapped = false;
        if (!mapped) { s_vtd_note = "DRHD fault mapping failed"; continue; }
#endif
        s_vtd[s_vtd_count++] = u;
    }
}
typedef struct {
    uint16_t status, pmcsr, pcie_status;
    bool pm, pcie, aer;
    uint32_t ue, ce;
    struct { uint32_t gsts, fsts, pmen, fault[4]; bool stable; } unit[NET_VTD_MAX];
} net_diag_t;
static uint16_t diag_pci16(unsigned off) {
    const pci_device_t *p = &s_e1000_dev.pci;
    return pci_read_config16(p->segment, p->bus, p->device, p->function, off);
}
static uint32_t diag_pci32(unsigned off) {
    const pci_device_t *p = &s_e1000_dev.pci;
    return pci_read_config32(p->segment, p->bus, p->device, p->function, off);
}
static void net_diag_copy(net_diag_t *r) {
    memset(r, 0, sizeof(*r));
    r->status = diag_pci16(6);
    unsigned ptr = diag_pci16(0x34) & 255;
    bool seen[64] = {false};
    for (unsigned n = 0; ptr && n < 48; ++n) {
        if (ptr < 0x40 || ptr > 0xfc || (ptr & 3) || seen[ptr / 4]) break;
        seen[ptr / 4] = true;
        uint16_t cap = diag_pci16(ptr);
        if ((cap & 255) == 1 && ptr <= 0xf8) { r->pm = true; r->pmcsr = diag_pci16(ptr + 4); }
        if ((cap & 255) == 0x10 && ptr <= 0xf4) { r->pcie = true; r->pcie_status = diag_pci16(ptr + 10); }
        ptr = cap >> 8;
    }
    /* Legacy CF8 only addresses 256 bytes: never wrap an AER read. */
    if (pci_is_mcfg_available()) {
        ptr = 0x100;
        for (unsigned n = 0; ptr && n < 64; ++n) {
            if (ptr < 0x100 || ptr > 0xffc || (ptr & 3)) break;
            uint32_t cap = diag_pci32(ptr);
            if (!cap || cap == UINT32_MAX) break;
            if ((cap & 65535) == 1 && ptr <= 0xfec) {
                r->aer = true; r->ue = diag_pci32(ptr + 4); r->ce = diag_pci32(ptr + 16); break;
            }
            unsigned next = cap >> 20;
            if (next == ptr) break;
            ptr = next;
        }
    }
    for (unsigned i = 0; i < s_vtd_count; ++i) {
        net_vtd_t *u = &s_vtd[i];
        r->unit[i].gsts = vtd_read(u, 0x1c);
        r->unit[i].fsts = vtd_read(u, 0x34);
        r->unit[i].pmen = vtd_read(u, 0x64);
        unsigned fri = (r->unit[i].fsts >> 8) & 255;
        if (!(r->unit[i].fsts & 2) || fri >= u->nfr) continue;
        unsigned f = u->fro + fri * 16;
        uint32_t before = vtd_read(u, f + 12);
        for (unsigned j = 0; j < 4; ++j) r->unit[i].fault[j] = vtd_read(u, f + j * 4);
        r->unit[i].stable = before == vtd_read(u, f + 12) && (before & (1u << 31));
    }
}
static void net_diag_report(const char *stage, const net_diag_t *r) {
    spin_debug_assert_unheld();
    pch_puts("[NET 2b] PCI/VT-d checkpoint: "); pch_puts(stage); pch_puts("\n PCI STATUS="); pch_hex(r->status);
    pch_puts(" error-mask="); pch_hex(r->status & 0xf900u);
    if (r->pm) { pch_puts(" PMCSR="); pch_hex(r->pmcsr); pch_puts(" D-state="); pch_hex(r->pmcsr & 3); }
    if (r->pcie) { pch_puts(" PCIe Device Status="); pch_hex(r->pcie_status); }
    if (r->aer) { pch_puts(" AER UE/CE="); pch_hex(r->ue); pch_puts("/"); pch_hex(r->ce); }
    else pch_puts(" AER unavailable/not found");
    pch_puts("\n VT-d: "); pch_puts(s_vtd_note); pch_puts("\n");
    for (unsigned i = 0; i < s_vtd_count; ++i) {
        pch_puts(" DRHD physical="); pch_hex(s_vtd[i].phys >> 32); pch_hex(s_vtd[i].phys);
        pch_puts(" segment="); pch_hex(s_vtd[i].segment);
        pch_puts(" GSTS/FSTS/PMEN="); pch_hex(r->unit[i].gsts); pch_puts("/");
        pch_hex(r->unit[i].fsts); pch_puts("/"); pch_hex(r->unit[i].pmen);
        pch_puts((r->unit[i].gsts & (1u << 31)) ? " translation ENABLED\n" : " translation disabled on this unit\n");
        if (r->unit[i].stable) {
            pch_puts(" pending fault raw128 (low word first):");
            for (unsigned j = 0; j < 4; ++j) { pch_puts(" "); pch_hex(r->unit[i].fault[j]); }
            uint16_t sid = r->unit[i].fault[2] & 65535;
            uint16_t nic = (s_e1000_dev.pci.bus << 8) | (s_e1000_dev.pci.device << 3) | s_e1000_dev.pci.function;
            pch_puts(" SID="); pch_hex(sid);
            pch_puts(" reason="); pch_hex(r->unit[i].fault[3] & 255);
            pch_puts(sid == nic && s_vtd[i].segment == s_e1000_dev.pci.segment ? " (matches NIC requester)\n" : " (other requester)\n");
        } else pch_puts(" no stable first pending fault captured; absence does not exclude DMA blocking\n");
    }
}
