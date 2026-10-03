# Storage & Memory Subsystem Annex

This annex documents the current status, hardware facts, verification evidence, and scope boundaries of the FortressOS storage, filesystem, and physical/virtual memory subsystems. Binding contracts, locking rules (L1–L4, especially `ext2_lock`), memory ownership (M1–M4), and block/VFS interfaces live in [`AGENTS.md`](../../AGENTS.md) (§4 and §9).

---

## 1. Subsystem Status and Overview

| Subsystem | Status | Detail |
| --- | --- | --- |
| **Phase 9D Bounded writable ext2** | **COMPLETE** | Explicit opt-in writable mount, allocation/truncation ordering, emergency read-only remount, 3-boot BIOS/UEFI persistence. Full detail: [`docs/roadmap/phase-9d-writable-ext2.md`](../roadmap/phase-9d-writable-ext2.md). |
| **EXT4 Phases 0-4** | **BOUNDED RW VFS VERIFIED** | RO/RW host and BIOS/UEFI persistence, 1/16 MiB downloads, bounded allocation/namespace, failure injection and Linux audits. Production dispatch remains disabled; image stays ext2. [Evidence](../roadmap/ext4-phase4.md), [plan](../plans/EXT4_PLAN.md). |
| **Phase 9E Saved File Management** | **COMPLETE** | Directory ops (`mkdir`/`rename`/`unlink`), on-disk inode/block reclamation. Full detail: [`docs/roadmap/phase-9e-exec-and-files.md`](../roadmap/phase-9e-exec-and-files.md). |
| **Phase 9H RAM capacity** | **COMPLETE** (2026-09-20) | PMM extended to cover 32 GiB, two-stage PMM/VMM init to stay within Limine's HHDM coverage until the kernel PML4 is active. Verified on Dell 5590 (32 GiB) with a write-readback probe. Full detail: [`docs/roadmap/phase-9h-ram.md`](../roadmap/phase-9h-ram.md). |

---

## 2. Storage & Memory Invariant Notes

### ext2 Filesystem and VFS Boundaries

- **Read-Only Default and Explicit Opt-in**: ext2 mounts are strictly read-only by default. Writable mounts require deliberate boot-level opt-in (`usb_data_mode=rw`).
- **Atomic Append Serialization**: Append mode (`VFS_O_APPEND`) serializes authoritative EOF determination and sector writeback under `ext2_lock` (`ext2_write(..., &offset, append, ...)`), updating `*off` and `node->size` before lock release. True multi-core SMP concurrent append is verified by `make test-smp-append`.
- **Tainted Storage Assertions**: Distinct error strings (`Read-only filesystem.` and `I/O error.`). An unrecoverable I/O or flush failure taints the filesystem, immediately suppresses further writes, freezes state before backing device release, and returns `-EIO` while preserving read capability.

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
