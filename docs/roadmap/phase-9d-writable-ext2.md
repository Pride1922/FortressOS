# Phase 9D — Bounded Writable ext2 Filesystem

Status: COMPLETE. See `AGENTS.md` status table for current summary; this file
holds the detailed implementation notes and evidence.

## Phase 9D review follow-up (2026-09-17)

Fixed shutdown synchronization without a writable mount, removed failed-mount
placeholder pointers, and reserved detached mount nodes before dirtying disk.
Truncation now reserves its bitmap scratch space before detachment and checks
all reclamation errors. Inode-initialization flush failure taints immediately,
without rollback writes. Metadata mapping rejection returns EIO; reserved GDT
expansion blocks are excluded from file data. Shutdown synchronization returns
failure on taint or I/O failure and freezes writes after a clean marker.

`wsl -d Ubuntu-24.04 -- make test-ext2` passed all eight sanitizer geometries,
including 14 fresh-image regression scenarios covering mount OOM/write/flush
failure, read-only/no-mount shutdown, allocation ownership, truncation OOM and
reclamation failure, tainted no-I/O behavior, and shutdown freeze/flush failure.
`make test-ext2-write` passed BIOS/UEFI three-boot persistence and both offline
`e2fsck -fn` audits per firmware on disposable clones. These tests do not claim
crash atomicity, torn-sector recovery, or physical writable-disk acceptance.

## Phase 9D — Bounded Writable ext2 Filesystem Verification (2026-09-17)

Phase 9D delivers bounded write support on the ext2 block layer, enabling file
creation, truncation, block reclamation, and persistence to NVMe storage.

### Implementation Details

- Direct block and single-indirect block allocation and writes (`vfs_write`).
- Directory entry insertion (`vfs_create`).
- File truncation (`vfs_truncate`) with a 3-stage contract: scratch
  pre-allocation, Stage 2 detachment (inode size/pointers zeroed and flushed
  first), and Stage 3 block reclamation with per-block error validation and
  taint marking.
- Unsupported structures (double/triple indirect blocks, non-regular files)
  are explicitly pre-rejected with `-EFBIG` / `-EOPNOTSUPP`.
- Atomic mount staging: `/mnt` VFS node is linked to the hierarchy only after
  the dirty marker is persisted and flushed to disk; mount failure unwinds
  cleanly.
- Clean shutdown lifecycle: `ext2_sync_all()` returns `bool`. If the
  filesystem is tainted, it refuses to mark the filesystem clean; on success,
  it sets `s_state = EXT2_VALID_FS` and freezes further writes
  (`fs->read_only = true`).
- Line editor in `user/shell.c` expanded to 8 KiB with save-protection guards
  (`editor_save_disabled`) against truncated reads.
- Explicit write opt-in: writes are disabled by default; enabled only when
  `-fw_cfg name=opt/fortress/write_test,string=1` is provided (or
  `WRITE_TEST=1`).

### Write ordering (from ARCH_REVIEW.md)

3-stage ordered allocation with intermediate NVMe flushes:

1. Allocation reservation: mark bits in block/inode bitmap, decrement free
   counters in group descriptors and superblock, write to disk, issue
   `block_flush`.
2. Data initialization: zero-fill newly allocated block on disk, issue
   `block_flush`.
3. Reference publication: write block pointer to inode or single-indirect
   table, or publish directory entry, write inode/directory block to disk,
   issue `block_flush`.

Allocation reservation precedes pointer publication. Interrupted operations
can leave unreferenced allocations; this ordering is not a guarantee against
torn writes or pre-existing cross-file corruption.

### Truncation ordering

3-stage ordered truncation with pre-validation:

1. Pre-validation: traverse and collect all direct and single-indirect block
   pointers into an allocated array, verifying partition bounds, metadata
   exclusion (superblock, group descriptors, reserved GDT expansion blocks,
   bitmaps, inode tables, including sparse-super backup blocks), and absence
   of duplicate pointers. Files with double/triple indirection return
   `-EFBIG` (`-27`); files with external extended attribute blocks
   (`file_acl != 0`) return `-EOPNOTSUPP` (`-95`). Even files with recorded
   size 0 are pre-validated if pointers exist. Scratch space for bitmap
   reclamation is reserved together with the block list, before detachment.
2. Inode detachment: zero block pointers, size, and `i_blocks` in memory and
   on disk, write inode to disk, issue `block_flush`.
3. Block reclamation: clear bits in block bitmap, update free block counters
   in group descriptors and superblock, write to disk, issue `block_flush`.

Reclamation follows a successful detachment flush. Interrupted operations can
leak blocks; crash-atomic updates and torn-write recovery are not provided.

### Prefix durability & error reporting

`vfs_write` returns a positive byte count only for a prefix whose data
blocks, inode metadata (size/i_blocks), and flushes have fully succeeded. If
a subsequent block allocation fails (e.g. `ENOSPC`), the flushed prefix count
is returned. If a metadata write or flush fails, the mount is marked tainted
(`fs->tainted = true`) and `-VFS_EIO` (`-5`) is returned, translated to
`SYSCALL_EIO` (`-9`) at the syscall boundary to avoid colliding with
`SYSCALL_ENOENT` (`-5`).

### State separation

Read-only mount policy returns `-VFS_EROFS` (`-30`, translated to
`SYSCALL_EROFS` `-11`) on write/truncate attempts (`fs->read_only == true`).
Mounts halted by I/O or flush errors return `-VFS_EIO` (`-5`, translated to
`SYSCALL_EIO` `-9`) (`fs->tainted == true`). Refusing further writes limits
disk corruption.

### Superblock clean/dirty lifecycle

On writable mount, `s_state` clears `EXT2_VALID_FS` (`s_state = 0`, actively
mounted) and flushes. On clean shutdown or reboot via `sys_reboot`,
`ext2_sync_all()` writes `s_state = 1` (`EXT2_VALID_FS`, clean) and flushes
only if the mount is untainted; if tainted, it issues no writes or flushes
and reports failure. Shutdown synchronization freezes further mutations; the
reboot syscall returns `SYSCALL_EIO` if synchronization fails. Unclean
filesystems (`s_state != 1`) reject writable mount.

### Resource pre-reservation

`vfs_open_ext` allocates the `file_t` descriptor before executing destructive
`vfs_truncate(node, 0)` on `O_TRUNC` or node creation, ensuring allocation
failure leaves file data untouched. `sys_open` verifies descriptor table
availability before invoking `vfs_open_ext`.

### Ring 3 Editor (`edit`)

Safe saving (`w` command) with path length limits, pre-open validation, and
unmodified status preservation on save failures.

## Verification Environment & Evidence

- **Environment**: WSL2 `Ubuntu-24.04` on Windows 11 host (x86_64, Linux 6.6
  kernel). QEMU `q35`, 2 GiB RAM, PCIe NVMe controller (`serial=fortress0`),
  GPT with a 1024-byte-block ext2 partition. These prior results were
  supplied by the user; they are not new test runs by the implementation
  agent.
- **Automated 3-Boot Persistence Suite (`make test-ext2-write`)**:
  - Ran disposable GPT NVMe fixtures across both legacy BIOS and UEFI
    (paired with OVMF 4M firmware).
  - Boot 1: created `/mnt/written.txt` via Ring 3 shell, saved, verified
    NVMe flush, and clean ACPI S5 shutdown (QEMU exit code 0). Offline
    `e2fsck -fn` passed with 0 errors.
  - Boot 2: verified persisted multi-line content, truncated and overwrote
    with shorter content, clean shutdown. Offline `e2fsck -fn` passed with 0
    errors.
  - Boot 3: verified cross-boot persistence of truncated state with zero
    stale lines.
- **Interactive Manual Verification**:
  - Booted via `make run-bios WRITE_TEST=1` in WSL Ubuntu-24.04 (from
    PowerShell).
  - Verified kernel log: `[ext2] Writable mount complete at /mnt`.
  - Created `/mnt/test.txt` via `edit /mnt/test.txt`, entered multi-line text
    in append mode, saved via `w`
    (`[EDIT] Saved 58 bytes (2 lines) to /mnt/test.txt`), quit with `q`, and
    verified contents via `cat /mnt/test.txt`.
  - Executed clean shutdown via `poweroff`.
  - User reports persistence on reload. The clean-marker/freeze behavior is
    established by code and automated offline checks, not solely by
    shutdown output.
- **Host Fault-Injection Matrix (`make test-ext2`)**:
  - 14 deterministic regression scenarios in `tests/ext2_host.c` under
    ASan/UBSan, covering exact write failure counts, OOM allocations, and
    shutdown freeze invariants.

## Later note (from Phase 9G work)

Physical NVMe write/mount was not established by this phase — Phase 9D's
persistence claim is QEMU-only against disposable NVMe GPT fixtures. Physical
storage persistence on real hardware was subsequently achieved via the USB
path in Phase 9G.4, not the NVMe path this phase covers. Real NVMe RW
persistence on hardware remains untested — see current status in `AGENTS.md`.
