# Phase 9G.2 — Read-Only USB Mass Storage Block Device

Status: COMPLETE (2026-09-19). See `AGENTS.md` status table for current
summary; this file holds the detailed implementation notes and evidence.

Completed Milestone 9G.2 (Bulk-Only Transport, SCSI engine, uniform block
device registration, and GPT partition discovery) across QEMU (BIOS and
UEFI) and physical bare-metal Dell Latitude 5590 hardware:

## Implementation

- **Bulk Transfer Rings**: Configured xHCI transfer rings for Bulk-In
  (Endpoint ID / DCI 3) and Bulk-Out (DCI 4) via `Configure Endpoint`
  command (Type 12) with Input Context slot indexing
  `(dci + 1) * ctx_dwords`.
- **Bulk-Only Transport (BOT)**:
  - 31-byte CBW (`0x43425355` "USBC") submission on Bulk-Out.
  - Data transfer stage on Bulk-In/Bulk-Out with cacheline flushing.
  - 13-byte CSW (`0x53425355` "USBS") reading on Bulk-In with signature, tag
    matching, and status validation.
- **SCSI Engine**:
  - `INQUIRY` (0x12): Reported Product "USB DISK 2.0".
  - `TEST UNIT READY` (0x00): Automatic `REQUEST SENSE` (0x03) recovery for
    initial Unit Attention.
  - `READ CAPACITY (10)` (0x25): Dell Kingston USB drive reported
    30,320,640 sectors (16 GB / 14.46 GiB), 512 bytes/sector.
  - `READ (10)` (0x28): Verified logical sector reads.
- **Uniform Block Device & GPT Discovery**:
  - Registered block device `sda` via `block_register_usb()`.
  - Sector 0 read verified with Protective MBR signature `0xAA55`.
  - GPT partition table parsed: published `sdap1` (ESP FAT32, 64 MiB) and
    `sdap2` (Linux FS ext2, 64 MiB).
  - xHCI controller and DMA rings remain active at runtime for block I/O.

## Scope

9G.2 commands: `INQUIRY` (0x12), `TEST UNIT READY` (0x00),
`READ CAPACITY(10)` (0x25), `READ(10)` (0x28), `REQUEST SENSE` (0x03).
Bound retries and implement BOT stall/reset recovery. Initially support
LUN 0 only; document GET MAX LUN handling and reject unsupported
configurations. Reject UAS explicitly. Defer `READ(12/16)`,
`MODE SENSE(6/10)`, `REPORT LUNS` and larger-capacity command sets; reject
`READ CAPACITY(10)`'s overflow sentinel and unrepresentable LBAs.
`WRITE(10)` (0x2a) and `SYNCHRONIZE CACHE(10)` (0x35) belong to 9G.4.

Bulk completion polls the event ring with a bounded timeout: no USB
completion IRQ dependency, sleeps, re-enabling IF or waiting on another
thread. This follows ext2's IRQ-save lock contract (L1). A timeout
propagates an I/O error and initiates bounded quiescence or DMA quarantine;
it does not permit immediate reuse/free of active buffers.

9G.2 owns 512/4096-byte sector integration tests against GPT and ext2;
reject other sizes explicitly. The current boot image is laid out in
512-byte LBAs: do not reinterpret it as a 4096-byte-sector image. Use
separately generated matching-geometry fixtures for 4096-byte tests.

## Verification

`make test-usb-block` passed 100% (8 host ASan/UBSan unit tests +
BIOS/UEFI QEMU absent/present matrix). Bare-metal Dell Latitude 5590 photo
confirmed `sda`, `sdap1`, and `sdap2` registration and interactive Ring 3
shell reached.
