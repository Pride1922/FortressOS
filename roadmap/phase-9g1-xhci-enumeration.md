# Phase 9G.1 — xHCI Controller & USB Device Enumeration

Status: COMPLETE (2026-09-19). See `AGENTS.md` status table for current
summary; this file holds the detailed implementation notes and evidence.

For the staged scope, protected contracts, and the 9G.1 checkpoint table
(required evidence + known failure modes per sub-stage), see `AGENTS.md`
§"Phase 9G implementation handoff". This file is the evidence record only.

## Handoff and evidence boundary (2026-09-18)

After flashing `fortress.img` with Rufus, the user reported no `/mnt`.
Review identified the missing runtime path: the kernel has no USB
controller or mass-storage driver, and `/mnt` is mounted from `nvme0n1p1`
only by the QEMU storage acceptance suite. The boot image's ext2 filesystem
is partition 2. The `run-img*` targets attach a separate NVMe fixture, so
their shell boot and any fixture `/mnt` do not prove access to the image's
USB data partition. Phase 9F completion covers image packaging, not
physical USB persistence. The README embedded in that partition describes
intended behavior, not proof of implemented USB access.

**Scope discipline:** 9G.1 was expected to be the largest single driver
effort since NVMe. Committed 9G.1a through 9G.1e as separate changes, each
verified in QEMU before merging.

## 9G.1a — PCI-only xHCI discovery (2026-09-18)

Implemented `pci_report_xhci()` using the existing read-only PCI lookup. It
reports the first class/subclass/interface 0x0c/0x03/0x30 match,
segment/BDF, vendor/device ID and firmware-assigned BAR0 base, width and
prefetch flag. Unsupported headers, absent/unassigned BARs, I/O BARs and
unsupported memory BAR types return with a diagnostic. No controller BAR
mapping/sizing, command register writes, firmware handoff, reset, DMA or USB
transfers are performed. BAR aperture and controller accessibility remain
unverified for 9G.1b.

The report appears immediately before shell startup through the serial
output path that also mirrors to the framebuffer. Missing NVMe now skips the
NVMe checks instead of halting, allowing PCI-only tests without a storage
fixture. The existing QEMU identity gate and physical NVMe storage exclusion
remain.

`wsl -d Ubuntu-24.04 -- make test-usb-discovery` passed all four cases:
BIOS/UEFI, each with xHCI present and absent. All reached the interactive
shell without NVMe or other data disks. The runner validates its final QEMU
arguments, uses ISO boot and disposable paired OVMF vars, bounds waits and
terminates QEMU. Evidence:
`build/usb-discovery-{bios,uefi}-{present,absent}.log` and `.stderr`. This
verifies PCI metadata and continuation, not malformed-BAR injection,
USB-device enumeration, USB I/O or physical hardware behavior.

Regression: `wsl -d Ubuntu-24.04 -- make test-shell test-storage` passed
BIOS/UEFI shell and storage checks plus keyboard-only UEFI 8 GiB without
COM1. The kernel compiled with the existing strict warning/freestanding
flags. Final `wsl -d Ubuntu-24.04 -- make test-usb-discovery img` repeated
all four discovery cases successfully and rebuilt `bin/fortress.img`; the
image builder's MBR/GPT/FAT checks and offline ext2 `e2fsck` verification
passed.

**Dell photo evidence (2026-09-18):** user-supplied `Photo 1.jpg` shows
`[USB 9G.1a] First xHCI controller: 0000:00:14.0 vendor=0x8086 device=0x9D2F`
and `BAR0=0xEF330000 memory64 prefetch=no`, followed by the
discovery-complete message, `[BOOT] Interactive shell ready.` and the
`fortress>` prompt. COM1 RX is unavailable, so this also establishes visible
framebuffer reporting on the Dell. QEMU storage fixture tests are shown as
skipped.

This confirms physical PCI discovery and boot progression to the shell
prompt. The photo does not show a typed command, so post-change keyboard
interaction is not newly verified. BAR extent, controller MMIO,
ownership handoff/reset, USB enumeration and persistence remain unverified.
These addresses/IDs are observations of this Dell, not constants for the
driver. Next: 9G.1b MMIO/reset.

## 9G.1b — MMIO and reset (Dell verification, 2026-09-18)

User confirmed that Phase 9G.1b passed on physical Dell Latitude 5590
hardware:

- Controller BAR0 sized and mapped in dedicated UC/NX virtual window.
- Capability offsets validated against the aperture.
- BIOS-to-OS ownership handoff semaphore successfully negotiated; SMIs
  disabled.
- Host controller halted and reset via HCRST; CNR cleared to 0.
- Reset readback values (USBCMD, USBSTS, PAGESIZE) validated.
- Virtual window safely unmapped, bus mastering left disabled, no firmware
  DMA leaked.
- Interactive PS/2 keyboard confirmed functional at the `fortress>` shell
  prompt.

## 9G.1c — Command and event rings (Dell verification, 2026-09-18)

User confirmed that Phase 9G.1c passed on physical Dell Latitude 5590
hardware:

- Command Ring and Event Ring DMA pages allocated and bound to `CRCR`,
  `ERSTSZ`, `ERSTBA`, and `ERDP`.
- PCI Bus Mastering enabled dynamically during transfer execution.
- Controller started (`USBCMD.RS = 1`); hardware posted a Port Status
  Change Event for attached Port 5 (`ctrl=0x8801`).
- Port status change event consumed and acknowledged via `ERDP`.
- No-Op Command TRB (type 23) executed via Doorbell 0; matching Command
  Completion Event (`XHCI_COMP_SUCCESS`, type 33) received.
- Controller cleanly halted, bus mastering disabled, DMA frames reclaimed
  safely without leaks.
- Interactive PS/2 shell confirmed functional.

## 9G.1d — Root port inspection and reset (Dell verification, 2026-09-18)

User confirmed that Phase 9G.1d passed on physical Dell Latitude 5590
hardware:

- Traversed Supported Protocol capabilities; mapped USB 2.0 and USB 3.x
  ports.
- Scanned 18 root ports (`PORTSC`), detected connected device on Port 5.
- Verified port power and executed bounded port reset on Port 5.
- Verified port enablement (`PED = 1`) and successfully decoded High-Speed
  (480 Mbps) speed.
- Selected Port 5 for subsequent device addressing; isolated non-target
  ports.
- Interactive PS/2 shell confirmed functional.

## 9G.1e — xHCI Device Addressing & Descriptors (2026-09-18/19)

Phase 9G.1e implements device slot assignment, device addressing, Default
Control Pipe (EP0) transfer ring management, USB descriptor querying and
parsing, Mass Storage BOT class validation, and device configuration
(`SET_CONFIGURATION(1)`).

### Implementation Details

- `src/drivers/xhci_dev.h`, `src/drivers/xhci_dev.c`:
  - Clean separation of memory: DCBAA table, scratchpad buffer array (up to
    128 pages based on `HCSPARAMS2`), Input Context (32-byte or 64-byte
    based on `HCCPARAMS1.CSZ`), Output Context, EP0 Transfer Ring (256 TRBs
    with Link TRB), and bounce buffer.
  - Pre-initialization: `CONFIG.MaxSlotsEn` and `DCBAAP` programmed while
    the controller is halted, following the xHCI specification Section 4.2.
  - Single running pipeline: controller starts once in
    `xhci_verify_rings()`, executes No-Op at index 0, verifies root ports in
    `xhci_discover_and_reset_ports()`, and proceeds directly to device
    enumeration without halting, preserving controller internal cycle
    states and dequeue indices.
  - Command execution via Doorbell 0: issues `ENABLE_SLOT` (acquires Slot
    ID), registers Output Context in DCBAA, sets up Input Context (Slot
    Context + EP0 Context with speed and port routing), and issues
    `ADDRESS_DEVICE`.
  - EP0 Control Transfers: Setup Stage TRB (IDT=1), Data Stage TRB
    (pointing to DMA bounce buffer), and Status Stage TRB (IOC=1). Rings
    Doorbell for Slot ID (Target=1).
  - Descriptor Parsing:
    - Reads initial 8 bytes of Device Descriptor: extracts and validates
      `bMaxPacketSize0` (8, 16, 32, 64).
    - Issues `EVALUATE_CONTEXT` if the negotiated packet size differs from
      the initial speed default.
    - Reads full 18-byte Device Descriptor: captures `idVendor`,
      `idProduct`, `bNumConfigurations`.
    - Reads 9-byte Configuration Descriptor header, validates
      `wTotalLength` (9..512), and reads full configuration descriptor.
    - Validates Interface: requires class `0x08` (Mass Storage), subclass
      `0x06` (SCSI transparent command set), protocol `0x50` (Bulk-Only
      Transport). Rejects other device classes cleanly.
    - Locates Bulk-In and Bulk-Out endpoints, verifies max packet size
      (e.g. 512 bytes for High-Speed).
    - Issues standard USB request `SET_CONFIGURATION(1)`.
  - Clean teardown and DMA quarantine: all allocated PMM pages are freed
    upon successful clean shutdown; if any command or transfer
    fails/times out, frames are quarantined to prevent DMA memory
    corruption.

### Verification Evidence

- **Host ASan/UBSan Unit Test Suite
  (`python3 scripts/test_xhci_dev_host.py`)**:
  - `PASS`: normal enumeration, descriptor reads, BOT class validation, and
    SET_CONFIGURATION(1).
  - `PASS`: Enable Slot failure handled cleanly.
  - `PASS`: Address Device failure handled cleanly.
  - `PASS`: Bad descriptor header rejected cleanly.
  - `PASS`: Malformed device descriptor rejected cleanly.
  - `PASS`: Non-mass-storage class device rejected cleanly.
  - `PASS`: Device without bulk endpoints rejected cleanly.
  - `PASS`: SET_CONFIGURATION failure handled cleanly.
- **QEMU Full Matrix Suite (`make test-usb-descriptors`)**:
  - `PASS usb-descriptors-bios-absent`: boots cleanly without xHCI, shell
    prompt reached, PS/2 echo responsive.
  - `PASS usb-descriptors-bios-present`: QEMU `qemu-xhci` with
    `usb-storage` attached to USB 2.0 port. Discovers Port 1, High-Speed
    (480 Mbps), issues Enable Slot (Slot ID 1), Address Device, reads
    descriptors (`VID=0x46F4 PID=0x0001 EP0_MAX=64 Bulk-In=0x81
    Bulk-Out=0x02`), issues `SET_CONFIGURATION(1)`, and boots to
    interactive shell with PS/2 echo.
  - `PASS usb-descriptors-uefi-absent`: paired OVMF 4M UEFI firmware boots
    cleanly without xHCI.
  - `PASS usb-descriptors-uefi-present`: full UEFI boot with `qemu-xhci`
    and `usb-storage` enumeration verified.

## Full 9G.1 completion summary (from AGENTS.md status table)

COMPLETE (2026-09-19): PCI discovery (9G.1a), MMIO/reset (9G.1b),
Command/Event rings (9G.1c), Root ports (9G.1d), and Device Addressing &
Configuration (9G.1e) verified on QEMU (BIOS & UEFI) and bare-metal Dell
Latitude 5590. Dell Kingston/Phison flash drive (VID 0x13FE, PID 0x4200)
identified on Slot 3, Port 9 with Bulk-In EP 0x81 and Bulk-Out EP 0x02.
Non-storage devices (Port 5 webcam 0x0E) safely filtered and slot released.

## Hardware facts recorded during this phase

- **H3:** ECAM segment 0 buses 0..127, base `0xF0000000` on the Dell 5590.
  Parse MCFG; never assume this address/range or apply it to another Dell.
- **H11:** Dell 5590 xHCI controller at 0000:00:14.0 (Intel Sunrise
  Point-LP, 8086:9D2F), 64 KiB BAR at 0xEF330000, 12 USB 2.0 ports and 6
  USB 3.0 ports. Legacy handoff extended capability at offset 0x846C. These
  are observations of one machine, not constants.
