# Networking Phase 1a — PCI Discovery, MMIO Mapping, and MAC Read (QEMU)

**Status**: COMPLETE (2026-09-30)  
**Deliverables**: `src/drivers/e1000.[ch]`, `src/kernel/main.c`, `scripts/test_net_pci.py`, `Makefile` (`test-net-pci`).  
**Verification**:  
- `make test-net-pci`: 4/4 QEMU test cases pass (BIOS and UEFI, present and absent).  
- `make test-net-host`: 103/103 tests pass.  
- `make test-nmi`: 56/56 exact-boundary NMIs pass (regression guard on entry/exit).  
- `make test-shell`: Full shell and keyboard regression suites pass under BIOS and UEFI.  
- `make`: Clean kernel build and bootable disk image generation.

---

## 1. Overview & Architectural Placement

Networking Phase 1a is the first hardware-touching checkpoint of the FortressOS networking milestone (NET-1). Following the xHCI 9G.1a precedent:
- **Zero DMA**: No packet buffers, no descriptor rings, and no bus mastering enabled (`PCI_COMMAND_BUS_MASTER` remains disabled).
- **Zero Interrupts**: No MSI, no IOAPIC routing, and no interrupt handler registered (`PCI_COMMAND_INT_DISABLE` remains set; NET-1 is polling-only).
- **Invariant M1 Compliance**: No raw physical pointer dereferencing. The PCI BAR0 aperture is explicitly mapped via `vmm_map_page` into a dedicated higher-half virtual window (`0xFFFFFFFFE2000000ULL`) with uncached cache attributes (`PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT | PTE_NX`).
- **Post-SMP & Post-VMM Placement**: Initialized in `kmain` at line 5637, alongside the storage and xHCI probes, ensuring the kernel PML4, PMM, and higher-half direct map are active.

---

## 2. Implementation Details

### 2.1 e1000 Driver (`src/drivers/e1000.h`, `src/drivers/e1000.c`)

- **PCI Discovery**:
  - Scans all segments, buses, devices, and functions via `pci_scan_all()`.
  - Targets Intel Gigabit controllers:
    - Vendor `0x8086`, Device `0x100E` (82540EM, QEMU `-device e1000`).
    - Vendor `0x8086`, Device `0x10D3` (82574L, QEMU `-device e1000e`).
    - Vendor `0x8086`, Device `0x15B7` / `0x156F` (I219-LM, Dell Latitude 5590).
- **BAR0 Validation & Sizing**:
  - Validates that the device header is Type 0 (`PCI_HEADER_TYPE_NORMAL`).
  - Verifies BAR0 is a memory aperture (not I/O space), non-zero, and not `0xFFFFFFFF`.
  - Sizes the aperture with decode disabled by writing `0xFFFFFFFF` to BAR0, reading back the mask, and restoring the original base address (typically 128 KiB).
- **Virtual Memory Mapping**:
  - Maps aperture pages to `E1000_MMIO_VIRT` (`0xFFFFFFFFE2000000ULL`) using `vmm_map_page()` with `PTE_PCD | PTE_PWT | PTE_NX`.
  - Enables `PCI_COMMAND_MEMORY_SPACE` in the PCI command register.
- **Hardware Register Inspection**:
  - **MAC Address**: Reads RAL0 (`0x5400`) and RAH0 (`0x5404`). If `RAH0.AV` (Address Valid) is set, extracts the 6-byte MAC. If invalid or all-zeroes/all-ones, falls back to querying the SPI EEPROM via `EERD` (`0x0014`) words 0..2.
  - **Link Status**: Reads `STATUS` (`0x0008`) and checks the `LU` (Link Up, bit 1) flag.
- **Diagnostics Format**:
  Matches the xHCI 9G.1a style:
  ```
  [NET] Probing network controllers...
  [NET] e1000: 0000:00:02.0 8086:10D3 BAR0 0xFEB80000 (32-bit MMIO) -> 0xFFFFFFFFE2000000
  [NET] e1000: MAC 52:54:00:12:34:56, STATUS 0x00080283 (link up)
  ```
  Or when absent:
  ```
  [NET] Probing network controllers...
  [NET] No network controller found; networking unavailable
  ```

### 2.2 Kernel Integration (`src/kernel/main.c`)

Called via `net_boot_probe()` right after `xhci_boot_probe(&boot_info)`:
```c
pci_report_xhci();
xhci_boot_probe(&boot_info);
net_boot_probe();
boot_status("Mounting persistent storage (/mnt)...");
```

---

## 3. Test Evidence

### 3.1 QEMU Discovery Suite (`make test-net-pci`)

The automated suite `scripts/test_net_pci.py` executes 4 permutations under SMP=1 with strict argv preflight (rejecting any unauthorized storage devices):

```
========================================================
FortressOS Networking Phase 1a — PCI Discovery Suite
========================================================
--> Running test case: BIOS (e1000 present)...
  [PASS] BIOS (e1000 present): e1000 found, BAR0 mapped, MAC=52:54:00:12:34:56, link up
--> Running test case: UEFI (e1000 present)...
  [PASS] UEFI (e1000 present): e1000 found, BAR0 mapped, MAC=52:54:00:12:34:56, link up
--> Running test case: BIOS (e1000 absent)...
  [PASS] BIOS (e1000 absent): clean fallback on absent network device
--> Running test case: UEFI (e1000 absent)...
  [PASS] UEFI (e1000 absent): clean fallback on absent network device
========================================================
All 4 Phase 1a QEMU test cases passed! [100% PASS]
========================================================
```

### 3.2 Regression Verification

1. `make test-net-host`: 103/103 tests pass (RFC 1071 checksums, Ethernet II, ARP, IPv4, `pbuf_t`).
2. `make test-nmi`: 56/56 exact-boundary NMIs pass (28 syscall boundaries + 28 sigreturn boundaries under BIOS and UEFI).
3. `make test-shell`: Interactive PS/2 and UART shell operations, prompt recovery, and builtins pass on BIOS and UEFI.
4. `make`: Production ISO and raw disk images build cleanly.
