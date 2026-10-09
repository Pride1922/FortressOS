# Storage & Memory Subsystem Annex

This annex documents the current status, hardware facts, verification evidence, and scope boundaries of the FortressOS storage, filesystem, and physical/virtual memory subsystems. Binding contracts, locking rules (L1–L4, especially `ext2_lock`), memory ownership (M1–M4), and block/VFS interfaces live in [`AGENTS.md`](../../AGENTS.md) (§4 and §9).

---

## 1. Subsystem Status and Overview

| Subsystem | Status | Detail |
| --- | --- | --- |
| **EXT4 Phase 9.5** | **COMPLETE — AUTOMATED** (2026-10-07) | 144 main cases, ten admission controls, regressions and verified evidence; [report](../roadmap/ext4-phase9-5.md). Production journaled RW remains disabled. |
| **EXT4 Phase 9.6** | **COMPLETE — BOUNDED E4-B** (2026-10-08) | Dell 5590 / identified 4 GB stick: clean persistence and all three controlled interruption cases PASS; explicit production USB journaled RW enabled; [artifact/handoff](../roadmap/ext4-phase9-6.md), [procedure](../plans/EXT4_PHASE9_6.md). |
| **EXT4 Phase 7** | **WRITER WORKBENCH VERIFIED** | Bounded staging, ordered commit/checkpoint and circular reuse; host crash/restart/tear gates plus Linux replay/fsck/bytes. Production mutation coverage/mount integration remains Phase 8. [Scope/evidence](../roadmap/ext4-phase7.md). |
| **EXT4 Phase 6** | **RECOVERY WORKBENCH VERIFIED** | Bounded internal JBD2 CSUM_V3/revoke reader, explicit checkpoint admission, host crash/restart and Linux audits. Production journal mounts remain disabled; writer is Phase 7. [Scope/evidence](../roadmap/ext4-phase6.md). |
| **Phase 9D Bounded writable ext2** | **COMPLETE** | Explicit opt-in writable mount, allocation/truncation ordering, emergency read-only remount, 3-boot BIOS/UEFI persistence. Full detail: [`docs/roadmap/phase-9d-writable-ext2.md`](../roadmap/phase-9d-writable-ext2.md). |
| **EXT4 Phases 0-4** | **BOUNDED RW VFS VERIFIED** | RO/RW host and BIOS/UEFI persistence, 1/16 MiB downloads, bounded allocation/namespace, failure injection and Linux audits. Phase 5 adds selected-USB EXT4 dispatch and sync/shutdown; automated gates verified; Dell performance/hash/reboot/fsck checks PASS, all Phase-5 physical checklist items user-confirmed PASS (2026-10-03). Default image stays ext2; separate opt-in image available. [Phase-5 evidence](../roadmap/ext4-phase5.md), [VFS evidence](../roadmap/ext4-phase4.md), [plan](../plans/EXT4_PLAN.md). |
| **Phase 9E Saved File Management** | **COMPLETE** | Directory ops (`mkdir`/`rename`/`unlink`), on-disk inode/block reclamation. Full detail: [`docs/roadmap/phase-9e-exec-and-files.md`](../roadmap/phase-9e-exec-and-files.md). |
| **Phase 9H RAM capacity** | **COMPLETE** (2026-09-20) | PMM extended to cover 32 GiB, two-stage PMM/VMM init to stay within Limine's HHDM coverage until the kernel PML4 is active. Verified on Dell 5590 (32 GiB) with a write-readback probe. Full detail: [`docs/roadmap/phase-9h-ram.md`](../roadmap/phase-9h-ram.md). |

---

EXT4 physical follow-up: RW/GPT/SYNC_BACKED admission was user-confirmed, but a 1 MiB file download exceeded three minutes versus three seconds to stdout; ext2 was reported fast. After redundant-barrier and wget-batching changes, the user reports approximately **50 seconds for 1 MiB**, still unacceptable. Bounded 4 KiB USB runs, fine BOT polling and zero staging now pass host and BIOS/UEFI USB persistence gates; a host 1 MiB case uses 512 write commands instead of 4096. Durability barriers and IRQ serialization remain unchanged. Dell retest now passes: 1 MiB in 6–7 seconds, 16 MiB in 1m52s, both hashes match, clean reboot persistence and quiet background console responsiveness PASS. Linux Mint offline e2fsck -fn completed five passes with exit 0 (user screenshot); independent Mint file hashes also match (user-confirmed PASS). All seven Phase-5 checklist items are now user-confirmed PASS; background prompt redraw remains a separate issue. See [transport evidence](../roadmap/ext4-usb-performance.md) and [Phase-5 follow-up](../roadmap/ext4-phase5.md).

## 2. Storage & Memory Invariant Notes

### Default production image (2026-10-09)

Normal `make` and `scripts/create_boot_img.py` now create journaled EXT4 at
`bin/fortress.img`. Legacy ext2 remains supported only through explicit format
selection or dedicated legacy test fixtures. Earlier SMP benchmark captures
used ext2; they do not establish journaled EXT4 performance. The Phase-5
"default stays ext2" statement above describes that historical milestone.

### ext2 Filesystem and VFS Boundaries

- **Read-Only Default and Explicit Opt-in**: ext2 mounts are strictly read-only by default. Writable mounts require deliberate boot-level opt-in (`usb_data_mode=rw`).
- **Atomic Append Serialization**: Append mode (`VFS_O_APPEND`) serializes authoritative EOF determination and sector writeback under `ext2_lock` (`ext2_write(..., &offset, append, ...)`), updating `*off` and `node->size` before lock release. True multi-core SMP concurrent append is verified by `make test-smp-append`.
- **Tainted Storage Assertions**: Distinct error strings (`Read-only filesystem.` and `I/O error.`). An unrecoverable I/O or flush failure taints the filesystem, immediately suppresses further writes, freezes state before backing device release, and returns `-EIO` while preserving read capability.
- **ext2 Write Cap & Inode Boundaries**: The ext2 write path supports direct and single-indirect block mappings only. Double-indirect and triple-indirect blocks are not implemented. Files that would require them are rejected explicitly and fail-closed:
  - At inode open / validate (`src/fs/ext2.c:547`): Inodes with double/triple indirect pointers are rejected with an unsupported-structure error.
  - At truncate / block collection (`src/fs/ext2.c:856`): Truncate operations walking past the single-indirect boundary are rejected before mutation is attempted.
  - The cap depends on the filesystem block size: `max_size = (12 + block_size / 4) * block_size`. For 1 KiB block filesystems, the maximum write size is 268 KiB (274,432 bytes); for 2 KiB, 1 MiB; for 4 KiB, 4 MiB. Standard 1 KiB filesystems start data at block 1 (`src/fs/ext2.c:1641`).
  - This limit accommodates disposable QEMU test fixtures, legacy ext2 image compatibility, and boot-time `/mnt` persistence. Large files and enterprise workloads are targeted by the ext4 journaled driver (`ext4_mount_rw`).


### GPT Partitioning and Disk Layout

- **Sector Geometry**: Block drivers support 512-byte and 4096-byte logical sectors only. The boot image is laid out in 512-byte LBAs.
- **Backup GPT Policy**: When the primary GPT header and partition array validate, but the backup GPT header at the physical disk's end is missing or misaligned (e.g. raw-copy to a larger disk), the filesystem enters degraded read-only mode. Conflicting valid GPTs cause explicit rejection. Disks are never silently repaired or resized.

### Internal NVMe Exclusion

- The internal physical NVMe drive is excluded from mounting. `/mnt` is provided exclusively by the user-selected USB data partition or disposable QEMU test fixtures.

---

## 3. Hardware Facts and Verification Boundaries

| ID | Evidence / constraint |
| --- | --- |
| H2 | **Code:** NVMe doorbells use CAP.DSTRD-derived stride (`4 << DSTRD`) and dynamic mapping extent. No physical DSTRD measurement is established here; never hardcode QEMU's value. |
| H5 | **2026-09-20, revised:** Limine's HHDM on the Dell 5590 covers physical `[0, ~2.5 GiB)` only. Measured by direct read at `hhdm_offset + phys`: `0x80000000` succeeds, `0xA0000000` raises #PF at CR2=`0xFFFF8000A0000000`. FortressOS works around this with a two-stage PMM: allocation is capped at 1 GiB until `vmm_init` loads the kernel PML4 with a full 32 GiB HHDM, then `pmm_unlock_high_memory()` clears the cap. PMM bitmap is 1 MiB (32 GiB coverage); the memory map's top range ends at `0x82E7EC000` (~32.72 GiB), which is clamped and warned. Verified on the Dell: `dmesg` reports `Total Physical RAM: 32768 MiB (8388608 frames)`, `Usable Free RAM: 31873 MiB`; write-readback probe passed at 2, 4, 16, and 30 GiB; boot log preserved at `/mnt/boot.log`. |
| H6 | The internal physical NVMe filesystem is not mounted by the kernel. `/mnt` is provided by the USB data partition when a supported stick is attached and selected; see H9. Initramfs file reads prove neither physical disk I/O nor persistence. |
| H6a | **Phase 9F code + user report (2026-09-18):** the raw image includes an ext2 data partition; the user flashed it with Rufus and reported no `/mnt`. Kernel USB storage support is absent. USB boot and image verification do not establish USB partition mounting or persistence; Phase 9G supplies that missing path. |

---

## 4. Test Targets and Verification Notes

| Target | Scope / evidence |
| --- | --- |
| `make test-ext2` | Host ASan/UBSan: actual ext2/VFS, malformed images, I/O/OOM paths |
| `make test-ext2-write` | QEMU ext2 file creation, editor save, host `e2fsck -fn` integrity, and cross-boot persistence on disposable NVMe GPT fixture |
| `make test-smp-append` | True multi-core SMP concurrent append verification under QEMU (-smp 4, BIOS & UEFI): independent and shared handles, atomic EOF serialization under `ext2_lock`, 200 records intact, 0 loss/corruption, clean S5 shutdown, offline host `e2fsck -fn` audit. |
| `make test-storage` | BIOS/UEFI GPT/ext2, Ring 3 reads, allocation-set audits; `build/storage-*.log` |

---

## 5. Architectural References

- Phase 9D Writable ext2: [`docs/roadmap/phase-9d-writable-ext2.md`](../roadmap/phase-9d-writable-ext2.md)
- Phase 9E File Management: [`docs/roadmap/phase-9e-exec-and-files.md`](../roadmap/phase-9e-exec-and-files.md)
- Phase 9H 32 GiB RAM: [`docs/roadmap/phase-9h-ram.md`](../roadmap/phase-9h-ram.md)

E4-A **physically accepted on Dell 5590 (2026-10-03)**: all seven Phase-5 items user-confirmed PASS; [complete record](../roadmap/ext4-phase5-acceptance.md). Non-journaled scope and clean-state admission remain unchanged.

E4-B Phase 9.5 **automated disposable USB gate PASS (2026-10-07)**: 144 main
cases, ten admission controls, E4-A/ext2 USB, BOT/mount-policy and NVMe journal
regressions; [implementation, evidence and limits](../roadmap/ext4-phase9-5.md).
Phase 9.6 bounded physical acceptance completed 2026-10-08; explicit production USB journaled RW enabled. Other device coverage and intermittent enumeration remain open.
