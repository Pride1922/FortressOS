# Phase 9G.3 — Production `/mnt` Mount & Command Line Partition Selection

Status: COMPLETE (2026-09-19). See `AGENTS.md` status table for current
summary; this file holds the detailed implementation notes and evidence.

Phase 9G.3 delivers production storage initialization independent of QEMU
acceptance fixtures, mounting the persistent ext2 data partition (`sdap2`)
read-only at `/mnt` using explicit GPT partition GUID (`PARTUUID`) matching
and USB device provenance verification.

## Implementation

- **Limine Command Line Capture**:
  - Instantiated `struct limine_kernel_file_request kernel_file_request`
    under Base Revision 3 protocol.
  - Deep-copied `kernel_file->cmdline` into kernel-owned storage in
    `boot_info_t` (`boot_info.cmdline`, up to 512 bytes, null-terminated).
- **Bounded Command Line & GUID Parser** (`src/fs/usb_mount.h`,
  `src/fs/usb_mount.c`):
  - `gpt_str_to_guid()`: converts standard 36-char mixed-endian UUID string
    `XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX` into `gpt_guid_t`, functioning as
    exact inverse to `gpt_guid_to_str()`.
  - `usb_mount_parse_cmdline()`: parses whitespace-delimited tokens for
    `usb_data=PARTUUID=<guid>` and `usb_data_mode=ro|rw` (default `ro`).
    Non-PARTUUID selection (e.g. filesystem labels or raw disk names) is
    strictly rejected as malformed.
- **Storage Initialization & Provenance Verification**:
  - Hooked directly into boot sequence in `kmain()` after
    `xhci_boot_probe()`.
  - Checks controller initialization state (`usb_is_initialized()`); if
    absent, cleanly logs diagnostic and leaves `/mnt` unmounted.
  - Checks if `/mnt` is already mounted (preventing conflicts with test
    fixtures).
  - Filters candidates by USB BOT parent device provenance
    (`part->parent->name == "sda"`), strictly excluding internal NVMe
    devices (`nvme0n1`).
  - Resolves candidates against target GUID:
    - 0 matches: logs
      `Partition PARTUUID=... not found on supported USB storage; /mnt left unmounted`.
    - > 1 matches: logs
      `Ambiguous candidates: multiple partitions matched PARTUUID=...; /mnt left unmounted`.
    - 1 match: verifies GPT policy (rejects invalid/ambiguity, accepts
      consistent primary, degraded primary, or backup fallback).
  - Enforces read-only policy for Phase 9G.3 (if `mode=rw` is requested,
    notes that writable persistence is deferred to 9G.4 and proceeds with
    read-only mount).
  - Reports device provenance:
    `[USB 9G.3] Selected USB device: sda, partition: sdap2 (PARTUUID=...)`.
  - Mounts ext2 partition read-only via `ext2_mount(&part->block_dev, "/mnt")`
    and logs `[USB 9G.3] PASS: Mounted sdap2 read-only at /mnt`.
- **Dual-Boot Image Builder Update** (`scripts/create_boot_img.py`):
  - Generates and prints `[IMG] Data partition PARTUUID: <UUID>`.
  - Generates image-specific `limine.conf` with:
    - Default entry: `/FortressOS (UEFI x86_64)` with
      `kernel_cmdline: usb_data=PARTUUID=<UUID> usb_data_mode=ro`.
    - Writable entry:
      `/FortressOS (Persistent Storage - Writable: PARTUUID=<UUID>)` with
      `kernel_cmdline: usb_data=PARTUUID=<UUID> usb_data_mode=rw`.
  - Deploys to all 4 standard ESP configuration locations.

## Verification Evidence

- **Host ASan/UBSan Unit Test Suite** (`scripts/test_usb_mount_host.py`):
  - `PASS`: `gpt_str_to_guid` and `gpt_guid_to_str` bidirectional roundtrip
    test.
  - `PASS`: `usb_mount_parse_cmdline` with valid RO/RW, default modes, extra
    arguments, malformed targets, and invalid syntax.
  - `PASS`: `usb_mount_production_storage` candidate selection: 0 matches,
    non-USB parent rejection, ambiguous clones rejection, GPT policy
    rejection, degraded primary acceptance, and RW fallback to RO.
- **QEMU Full Matrix Integration Suite** (`make test-usb-mount`):
  - `PASS usb-mount-bios-absent`: boots cleanly without xHCI, shell prompt
    reached and responsive to keyboard echo.
  - `PASS usb-mount-bios-present`: boots raw disk image `bin/fortress.img`
    as an emulated USB flash drive under legacy BIOS. Discovers xHCI,
    addresses BOT device on Slot 1 Port 1, registers `sda`, parses GPT
    (`sdap1`, `sdap2`), reads Limine cmdline, mounts `sdap2` read-only at
    `/mnt`, drops to interactive shell, verifies `ls /mnt` lists
    `README.txt`, and `cat /mnt/README.txt` prints persistent storage
    banner.
  - `PASS usb-mount-uefi-absent`: paired OVMF 4M UEFI firmware boots
    cleanly without xHCI, shell prompt reached and responsive.
  - `PASS usb-mount-uefi-present`: boots raw disk image `bin/fortress.img`
    as an emulated USB flash drive under UEFI firmware. Fully mounts
    `sdap2` at `/mnt`, and verifies `ls /mnt` and `cat /mnt/README.txt` via
    QMP.
- **Bare-Metal Dell Latitude 5590 Hardware Acceptance (2026-09-19)**:
  - Booted from physical USB flash drive flashed with Rufus.
  - Discovered xHCI controller 8086:9D2F, enumerated Kingston/Phison flash
    drive on Port 9, registered `sda` (30,320,640 sectors).
  - Verified Sector 0 MBR signature `0xAA55`.
  - GPT parsed with both Primary and Backup valid and consistent; published
    `sdap1` (ESP FAT32) and `sdap2` (ext2 data).
  - Kernel command line read from Limine
    (`usb_data=PARTUUID=79C710... usb_data_mode=ro`).
  - Matched `sdap2` against target PARTUUID with USB BOT parent provenance
    (`sda`).
  - Successfully mounted `sdap2` read-only at `/mnt`
    (`[USB 9G.3] PASS: Mounted sdap2 read-only at /mnt`).
  - Interactive Ring 3 shell prompt reached and responsive. Photographic
    evidence confirmed.

## Policy notes (from AGENTS.md, kept with the implementation they gate)

"User-selected test USB" means the user deliberately chooses that
configured target and writable entry. Require exactly one matching
partition on a supported USB BOT device. A matching filesystem label
(`FORTRESS_DATA`), GPT name (`Fortress Persistent Data`) or marker file
alone is never write authorization. No first-disk or first-matching-label
fallback. Duplicate GUIDs (including two clones of the same image),
missing/malformed selection, unsupported media or failed eligibility checks
must never produce a writable mount. Without a valid unique target, leave
`/mnt` unmounted and explain why; with a selected target but failed RW
eligibility, allow only the documented read-only fallback.

Test the 130 MiB image on a larger disposable device as well as at exact
image size. The GPT backup remains at the image boundary after a raw copy,
while the current parser probes the device's last sector. 9G.3 owns this
policy: accept a fully validated primary header AND partition array in
degraded read-only mode when the end-of-device backup is absent/invalid;
log the declared backup LBA and actual last LBA as a possible raw-copy size
mismatch. Do not label an unverified mismatch definitively a raw copy.
Preserve rejection of two valid but conflicting GPTs and the existing
validated read-only backup fallback. Reject when neither copy validates.
Never silently repair/resize disks.
