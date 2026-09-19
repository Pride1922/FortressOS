# FortressOS - Architectural Review and Technical Debt

## Implemented: allocation-set and mapping audits

`pmm_snapshot()` copies the actual PMM allocation bitmap into caller-owned storage under the PMM lock. The ext2 acceptance suite establishes its baseline after mounting, populating the inode cache and warming a process lifecycle. It compares exact allocation sets after ten further Ring 3 process cycles, alongside heap usage, allocated page-table counts and heap integrity.

An explicit negative test allocates a different frame while freeing the first: free-page counters match, but the bitmap comparison detects the changed set. A changed bit means an unexpected allocation-state change, not automatically a leak or unauthorized free. Matching bitmaps do not prove frame ownership.

`vmm_kernel_mapping_fingerprint()` walks the master higher-half page tables and ignores hardware-managed accessed/dirty bits. This is a non-cryptographic 64-bit FNV-style diagnostic fingerprint, not an exact mapping proof or security hash. Expected retained tables and cached nodes must exist before the baseline. Snapshots must be compared at a quiescent test boundary. Future concurrent workloads need an explicit audit barrier and ownership instrumentation.

## Implemented: lock discipline

The current diagnostic build tracks actual held lock identities in a bounded bootstrap-CPU stack. IRQs are disabled before checking or updating tracking. Acquisition requires an increasing rank; recursion, equal-rank nesting and inversions panic through raw UART before spinning. Releases must be LIFO. The 64-bit saved RFLAGS token belongs to each acquisition's caller.

Order: scheduler or ext2 (1), heap (2), VMM (3), PMM (4), console (5). Scheduler and ext2 locks cannot nest with one another. The ext2 lock serializes synchronous reads and node-cache publication; it is not held across a yield. `spin_unlock_noirq()` releases and updates tracking while retaining IRQ masking; both scheduler switch sites assert that no locks remain held. The boot self-test checks the recursion/inversion predicates and nested IF restoration. Real lock use is exercised throughout the BIOS/UEFI suites.

NMI/fatal diagnostics must remain lockless. Before AP startup, migrate tracking and current-thread/stack state to per-CPU storage. This checker does not claim to detect every possible deadlock, and its global state is single-CPU only.

The USB storage driver (Phase 9G) adds no new locks to this hierarchy. Its synchronous BOT transfers execute under the ext2 lock when called from the filesystem and perform no allocations, sleeps, or IRQ-enabling waits in that path. USB transfer completion is bounded polling, not IRQ-driven, precisely to preserve this contract. See §"Implemented: Phase 9G USB storage."

## Deferred: per-CPU syscall entry and SMP address-space lifetime

Keep the existing single-CPU syscall stack design until SMP is introduced. Per-CPU TSS, current thread, syscall scratch and GS state require a complete entry/exit design, including NMI arrival around both SWAPGS transitions. An illustrative SWAPGS sequence alone is not a safe implementation.

Address-space teardown must prevent new scheduling into the dying space and wait until every CPU has stopped using it. Permission/unmap changes need invalidation on CPUs that actually cache the address space, with acknowledgement before reclaiming frames. A scheduling affinity mask and a local CR3 inequality are insufficient. TLB shootdown alone does not prevent re-entry into freed tables.

## Implemented: Phase 9C.2 read-only ext2

- Mount a preformatted, clean ext2 partition at `/mnt` during boot; root USTAR remains accessible. The partition device must live for the entire mount.
- Revisions 0/1, 1/2/4 KiB filesystem blocks, 512/4096-byte device sectors, power-of-two inode sizes from 128 bytes through filesystem block size.
- Regular files and linear directories; classic direct, single-, double- and triple-indirect block lookup. Indirection depth decreases explicitly, so repeated pointers cannot cause unbounded recursion. Zero pointers read as holes.
- Iterative path lookup, lazy immutable inode snapshots and node caching. Limits: 4096 block groups, 1024 cached nodes per mount, 1 MiB per directory, 63-byte component names and 255-byte paths. Each read transfers at most 64 KiB; larger callers receive a short read and can continue.
- Validate superblock geometry/features, table ranges, inode numbers, block references and directory record lengths/names. Reject out-of-partition reads, dirty filesystems and unsupported formats; mount publication is transactional.
- Supported compat bits: ext_attr, resize_inode and dir_index, but actual indexed directory inodes are rejected. Supported incompat bit: filetype. Supported ro_compat bits: sparse_super and large_file, but nonzero high file-size words are rejected. Other feature bits and nonzero inode flags are rejected.
- No symlink/device support, journal replay, extents, writes, unmount or cache eviction. Unsupported nodes return errors when reached, not a full upfront certification of every inode on disk. Cached mount metadata is intentionally retained and accounted for; it is not classified as a leak.
- Current polling I/O executes under IRQ masking on one CPU. This bounds work but can delay scheduling during disk reads. Sleepable locks, asynchronous I/O, device-wide concurrency and mount lifetime management precede broader use.

The fixture generator requires e2fsprogs, creates real file data with mke2fs, and runs e2fsck. It no longer substitutes an ext2 signature on failure or writes raw patterns into filesystem sectors. GPT boundary tests compare translated partition reads directly with corresponding parent reads.

### Verification

- `make test-ext2`: actual ext2/VFS sources, host adapters, ASan/UBSan and leak detection. 1/2/4 KiB blocks, 512/4096-byte sectors, 128/256-byte inodes; malformed geometry/features/table pointers/directory records; allocation and I/O failure; sparse reads; indirect pointer bounds and fixed-depth traversal.
- `make test-storage`: strict kernel build, BIOS and paired-pflash UEFI boot. Includes previous suites, all GPT negative cases, nested lookup/enumeration, independent offsets, direct/single/double-indirect fixture reads and sparse data. Ring 3 opens `/mnt/hello.txt`, checks exact contents, prints, checks EOF, closes, exits, and repeats under the allocation-set/mapping/heap audits.
- Host tests exercise triple-indirect traversal with synthetic references; a full large on-disk triple-indirect file is not part of the QEMU fixture.
- QEMU tests use snapshot disk writes. Logs: `build/storage-bios.log` and `build/storage-uefi.log`. Physical Latitude storage was subsequently verified via the USB path in Phase 9G.4; see §"Implemented: Phase 9G USB storage."

## GPT conformance

Supported partition-entry sizes are 128, 256 and 512 bytes. This is a deliberately bounded subset of the specification's power-of-two multiples of 128 bytes. GPT and ext2 boot acceptance now run in both BIOS and UEFI.

GPT partition-array validation is bounded to the image's declared layout. For a raw image flashed to larger media, the backup GPT remains at the image's last sector, not the device's. Phase 9G.3 accepts the primary header with a validated array in this case and logs the discrepancy as a possible raw-copy size mismatch. Phase 9G.4 records that writable eligibility on such media is not yet defined — writes to a larger as-flashed device are deferred pending an explicit policy. Automated repair or relocation of GPT is never performed.

## Implemented: Phase 9D bounded writable ext2

- Explicit opt-in writable mount (`ext2_mount_rw`), keeping read-only defaults (`ext2_mount`).
- Write ordering: 3-stage ordered allocation with intermediate NVMe flushes:
  1. Allocation reservation: mark bits in block/inode bitmap, decrement free counters in group descriptors and superblock, write to disk, and issue `block_flush`.
  2. Data initialization: zero-fill newly allocated block on disk and issue `block_flush`.
  3. Reference publication: write block pointer to inode or single-indirect table, or publish directory entry, write inode/directory block to disk, and issue `block_flush`.
  Allocation reservation precedes pointer publication. Interrupted operations can leave unreferenced allocations; this ordering is not a guarantee against torn writes or pre-existing cross-file corruption.
- Truncation ordering: 3-stage ordered truncation with pre-validation:
  1. Pre-validation: traverse and collect all direct and single-indirect block pointers into an allocated array, verifying partition bounds, metadata exclusion (superblock, group descriptors, reserved GDT expansion blocks, bitmaps, inode tables, including sparse-super backup blocks), and absence of duplicate pointers. Files with double/triple indirection return `-EFBIG` (`-27`); files with external extended attribute blocks (`file_acl != 0`) return `-EOPNOTSUPP` (`-95`). Even files with recorded size 0 are pre-validated if pointers exist. Scratch space for bitmap reclamation is reserved together with the block list, before detachment.
  2. Inode detachment: zero block pointers, size, and `i_blocks` in memory and on disk, write inode to disk, and issue `block_flush`.
  3. Block reclamation: clear bits in block bitmap, update free block counters in group descriptors and superblock, write to disk, and issue `block_flush`.
  Reclamation follows a successful detachment flush. Interrupted operations can leak blocks; crash-atomic updates and torn-write recovery are not provided.
- Prefix durability & error reporting: `vfs_write` returns a positive byte count only for a prefix whose data blocks, inode metadata (size/i_blocks), and flushes have fully succeeded. If a subsequent block allocation fails (e.g. `ENOSPC`), the flushed prefix count is returned. If a metadata write or flush fails, the mount is marked tainted (`fs->tainted = true`) and `-VFS_EIO` (`-5`) is returned, translated to `SYSCALL_EIO` (`-9`) at the syscall boundary to avoid colliding with `SYSCALL_ENOENT` (`-5`).
- State separation: Read-only mount policy returns `-VFS_EROFS` (`-30`, translated to `SYSCALL_EROFS` `-11`) on write/truncate attempts (`fs->read_only == true`). Mounts halted by I/O or flush errors return `-VFS_EIO` (`-5`, translated to `SYSCALL_EIO` `-9`) (`fs->tainted == true`). Refusing further writes limits disk corruption.
- Superblock clean/dirty lifecycle: On writable mount, `s_state` clears `EXT2_VALID_FS` (`s_state = 0`, actively mounted) and flushes. On clean shutdown or reboot via `sys_reboot`, `ext2_sync_all()` writes `s_state = 1` (`EXT2_VALID_FS`, clean) and flushes only if the mount is untainted; if tainted, it issues no writes or flushes and reports failure. Shutdown synchronization freezes further mutations; the reboot syscall returns `SYSCALL_EIO` if synchronization fails. Unclean filesystems (`s_state != 1`) reject writable mount.
- Resource pre-reservation in VFS & Syscalls: `vfs_open_ext` allocates the `file_t` descriptor before executing destructive `vfs_truncate(node, 0)` on `O_TRUNC` or node creation, ensuring allocation failure leaves file data untouched. `sys_open` verifies descriptor table availability before invoking `vfs_open_ext`.
- Ring 3 Editor (`edit`): Safe saving (`w` command) with path length limits, pre-open validation, and unmodified status preservation on save failures.
- Verification:
  - `make test-ext2`: Host ASan/UBSan across 8 geometry combinations (1/2/4 KiB blocks, 512/4096-byte sectors), testing duplicate pointers, metadata pointers, xattr rejection, zero-size invalid pointers, clean shutdown sync, and flush/I/O failure injection.
  - `make test-ext2-write`: QEMU BIOS and UEFI 3-boot persistence on disposable NVMe GPT fixtures, verifying file creation, multi-line editor writes, truncation, block reclamation, clean ACPI S5 shutdown, and offline `e2fsck -fn` (0 errors across all boots).

## Implemented: exact-boundary NMI delivery verification

`make test-nmi` injects real external NMIs through QMP at these five debugger hardware-breakpoint locations: before saving user RSP, after saving it, after switching to the kernel stack, before restoring user RSP, and immediately before SYSRET with user RSP already active. Labels add no instructions or delays. The harness checks the hardware frame's exact interrupted RIP/CS/RFLAGS/RSP/SS, checks execution on IST2, verifies all GPRs and scratch values are preserved, compares 128 bytes below user RSP, executes IRET and compares restored state. The exit probe also checks successful SYSRET to CPL3. Four rounds per firmware provide 40 verified injections total, followed by full-suite completion. JSON evidence and serial logs are in `build/nmi-bios.*` and `build/nmi-uefi.*`.

Testing exposed masked LINT1 routing: QEMU's external NMI delivery respects that mask. The kernel now parses MADT Local APIC NMI records and applies the declared bootstrap-CPU routes, rather than assuming both LINT pins can always stay masked. Invalid pin/flag encodings are tested; conflicting applicable routes are rejected. The harness requires this normal boot configuration; it performs no guest memory or register writes. References: QEMU GDB documentation and QEMU hw/intc/apic.c.

This closes the QEMU transition-window delivery gap. It does not establish physical-hardware delivery, all nested-fault cases, SWAPGS correctness or SMP safety.

## Physical boot: visible early diagnostics

The framebuffer console previously started after PMM/VMM/heap tests. The PMM also audited a fixed 2 GiB capacity after sizing itself for all installed RAM, so a larger machine could halt before any screen output. Initialization now starts screen logging early and explicitly limits managed RAM to the supported low 2 GiB. Higher RAM remains unavailable to allocation. UART writes also stop waiting after a bounded poll or failed loopback test.

A QEMU UEFI regression with 8 GiB and COM1 absent reaches PCI discovery after all memory, process and initramfs tests; its framebuffer screenshot is saved. Subsequent user photos confirm successful diagnostics and interactive shell boot on the Latitude. They do not isolate the original black-screen root cause. Physical disks are excluded from QEMU-specific storage fixture tests using the controller vendor/device identity.

## Boot-console performance

Replaced uncached framebuffer-to-framebuffer scrolling with a static RAM text cache. Rendering compares character and colours and skips unchanged cells; blank cells ignore irrelevant foreground-colour differences. Scrolling advances up to eight rows per batch, reducing screen movement frequency during logs. The fixed 1.5 MiB cache requires no early allocator and supports a 512 x 256 text viewport. Framebuffer writes remain uncached; this is not GPU acceleration or write-combining support. After testing the rebuilt ISO on the Latitude, the user reported that it feels like Linux booting; no timing benchmark was collected.

Host ASan/UBSan tests verify rendered pixels, padding bounds, preserved colours, batched scroll frequency, no blank-cell redraw, and exact-width newline handling. BIOS and UEFI acceptance suites pass with the updated cursor semantics.

## Interactive shell checkpoint

Boot continues to `fortress>` after diagnostics. `/bin/shell` is a separate ELF loaded from initramfs with normal process isolation, syscalls and deferred exit reclamation. Commands: `help`, `ls`, `cat`, `echo`, `exit` (restart). Raw stdin uses a bounded queue and scheduler sleep/wakeup; editing and echo live in Ring 3. The existing read-only VFS is used throughout. Physical NVMe mounting and writes are not introduced here. See AGENTS.md for ABI, limits and reproducible tests.

## Dell manual hardware acceptance (2026-09-16 & 2026-09-18)

Hardware verification on the Latitude 5590 (Core i5-8350U, 32 GiB RAM, 256 GB NVMe, Intel UHD 620):

- **2026-09-16:** PS/2 keyboard interaction, shell commands (`help`, `ls`, `cat etc/motd`), and missing-file handling verified.
- **2026-09-18:**
  - **Belgian AZERTY (Bug H4):** Shift-Lock on number row with Caps Lock ON verified functional, typing `1234567890`. Accented unshifted keys confirmed emitting base ASCII approximations without uppercase distortion. European ISO `<` / `>` key (scancode 0x56) verified.
  - **System V AMD64 ABI:** Argument passing verified from Ring 3 shell (`run /bin/hello testing ...`) with 16-byte aligned stack.
  - **Visuals:** Limine graphical wallpaper and kernel boot logo / emblem verified.
  - **Power:** ACPI S5 shutdown and reset confirmed functional.

These observations supplement automated QEMU and host tests. Internal NVMe write/mount remains separated from physical testing.

## System V AMD64 ELF User Stack & Argument Passing ABI

Process spawning (`SYS_SPAWN`, nr 9) conforms to the System V AMD64 ELF ABI (Section 3.4.1):

- Initial user stack pointer `RSP` is 16-byte aligned (`RSP % 16 == 0`) at `_start` entry.
- Initial register state: `RDI = argc`, `RSI = argv`, `RDX = 0` (`rtld` cleanup function hook), with all other GPRs cleared to zero.
- Stack layout from `RSP` upward: `argc` (qword), `argv[0..argc-1]` (pointers), `NULL`, `envp NULL`, `AT_NULL` pair (`0, 0`).
- Argument strings are packed in high memory of the allocated 4 KiB user stack frame below `USER_STACK_TOP_VIRT` (`0x00007FFFF0001000ULL`), ending at `USER_STACK_TOP_VIRT - 1`. The kernel HHDM mapping `vmm_phys_to_virt(stack_phys)` is used to construct the frame without touching CR3.
- Bounded limits: `MAX_SPAWN_ARGS = 32`, `MAX_ARG_STRLEN = 256`, `MAX_TOTAL_ARGS_LEN = 2048`. Single-pass bounded copy `copy_user_string()` enforces bounds without unbounded `strlen()` scans. Exceeding any limit returns `SYSCALL_E2BIG`.
- Backward compatibility: internal kernel boot test modes (`user/init.asm` modes 0..7) maintain scalar `RDI` mode selection via `process_spawn_with_arg()`.
- Verified dynamically under BIOS/UEFI QEMU (`make test-shell`) including runtime `RSP % 16 == 0` hardware assertions, empty argument handling (`""`), argument limits (32 pass, 33 rejected), and zero-leak resource reclamation.

## Saved File Management (Phase 9E) & Bug H4 Resolution

- Directory operations: `vfs_mkdir`, `vfs_unlink`, and `vfs_rename` implemented and wired to syscalls `SYS_MKDIR` (11), `SYS_UNLINK` (12), and `SYS_RENAME` (13).
- Ext2 directory structures: Directory creation formats block 0 with `.` and `..` directory records with 12-byte and `block_size - 12` record lengths. Parent directory `links` count incremented on directory creation and decremented on removal.
- Deletion safety: Unlink on directories validates that only `.` and `..` entries exist; non-empty directories reject with `-VFS_ENOTEMPTY`.
- Rename and reparenting: Renaming across directory boundaries adjusts `..` entry to point to the new parent and updates parent links.
- On-disk reclamation: Unlinked inodes clear block allocations and release bitmap bits, clear `i_size` and block pointers, set `i_links_count = 0`, and set `i_dtime` to deletion timestamp.
- Verified: All 8 host test configurations (`make test-ext2`) pass under ASan/UBSan. Three-boot persistence (`make test-ext2-write`) passes under both BIOS and UEFI with offline `e2fsck -fn` reporting 0 errors.
- Bug H4: Belgian AZERTY layout top-row scancodes (0x02..0x0D) now use `shift ^ s->caps` as Shift-Lock for numeric digits (`1234567890`). Shift table precedence prevents accented letters `é`, `è`, `ç`, `à`, `ù` on scancodes `0x03`, `0x08`, `0x0A`, `0x0B`, `0x28` from emitting uppercase letters. ISO scancode `0x56` (`<` / `>`) added. Verified by `make test-input` and `make test-shell`.

## Implemented: Phase 9G USB storage

The USB storage stack is a self-contained driver set under `src/drivers/xhci*` and `src/fs/usb_mount.c`. It targets xHCI controllers only; EHCI/UHCI/OHCI controllers are out of scope.

- **xHCI controller (9G.1)**: PCI discovery of class 0x0C subclass 0x03 progif 0x30, BAR mapping with size validation, BIOS/OS ownership handoff, controller halt/reset, command and event rings with ERST, root port protocol mapping, bounded port reset, slot allocation, device addressing, control transfers, and descriptor parsing (device/config/interface/endpoint).
- **USB Mass Storage Bulk-Only Transport (9G.2)**: CBW/CSW exchange on bulk endpoints, SCSI command set (`INQUIRY`, `TEST UNIT READY`, `REQUEST SENSE`, `READ CAPACITY(10)`, `READ(10)`, `WRITE(10)`, `SYNCHRONIZE CACHE(10)`, `MODE SENSE(6/10)`). One LUN only (LUN 0). UAS is explicitly rejected. Block device registration through `block_dev_t`, uniform with NVMe.
- **Production mount (9G.3)**: GPT partition selection by PARTUUID, with device provenance verifying the partition is on a USB BOT parent (not the internal NVMe). Bounded command line parser rejects non-PARTUUID selectors. Read-only mount is the default.
- **Writable mount and durability (9G.4)**: `usb_data_mode=rw` opt-in, flush preflight, BOT stall recovery with bounded endpoint reset, durability classification (see below), and a normal `sync` path that preserves the ext2 dirty marker (distinct from `ext2_sync_all()`).

Scope limits (all verified by construction, not aspiration):

- USB 2.0 Full-Speed and High-Speed devices only. SuperSpeed (USB 3.x) slots, streams, and port state machines are rejected with a clear log message (Phase 9G.5 objective).
- Direct attachment only. Hubs (class 0x09) are rejected.
- Attached-at-boot only. No hot-plug or reconnection.
- No isochronous, interrupt, or control-only device classes.
- Root-port only. External hub-attached devices are unreachable.

Physical acceptance: Kingston USB DISK 2.0 (VID 0x13FE, PID 0x4200) on Dell Latitude 5590. Files written via the editor persist across power cycle; `e2fsck -fn` clean on the unmounted device. SanDisk USB 3.x stick tested and correctly skipped as SuperSpeed. Internal NVMe is excluded from USB mount selection by parent-device provenance.

## Implemented: USB durability classification

A four-tier state machine classifies each USB mass-storage device by what its SCSI responses actually report, then gates writable mount on the result.

- `SYNC_BACKED`: the device accepts `SYNCHRONIZE CACHE(10)` with `IMMED=0`. Every flush must complete. Strongest guarantee.
- `WRITE_THROUGH`: `MODE SENSE(6)` or `MODE SENSE(10)` reports caching page 0x08 with `WCE=0`. The device reports no write cache; the barrier succeeds after successful writes without a cache command.
- `ASSUMED_WRITE_THROUGH`: no caching page is reported on either MODE SENSE form, and `SYNCHRONIZE CACHE` is rejected, but the transport is healthy. Write-through is assumed, matching Linux and Windows behavior. A three-line disclosure is printed at boot: the device does not report cache policy; clean shutdown is assumed durable; power-loss during writes may lose data.
- `READ_ONLY`: the device explicitly reports `WCE=1` and rejects `SYNCHRONIZE CACHE`, or is in an error/latched-offline state. Mount refuses RW.
- `UNKNOWN`: probe did not complete. Treated as READ_ONLY for mount eligibility.

All writable mounts require explicit `usb_data_mode=rw`, a matching PARTUUID, USB parent provenance, a strictly consistent or degraded-primary GPT policy, and successful flush preflight. The default boot entry is read-only.

The classification is per-device and is not re-derived on hot events (which are out of scope). A device that latches to READ_ONLY during a runtime flush failure stays there for the boot session. Do not whitelist by VID:PID alone; do not assume removable flash has no volatile cache.

## Next milestones

1. Keyboard/serial input, blocking wait queues, Ring 3 shell, in-memory editor, and editor file save are implemented and verified in BIOS/UEFI QEMU and physical Latitude 5590.
2. Phase 9D bounded writable ext2 filesystem is complete and verified with multi-boot persistence and `e2fsck` integrity.
3. Phase 9E SysV AMD64 ABI, spawn/wait, program execution, exit status, and command chaining are complete.
4. Phase 9G USB storage and real `/mnt` persistence is complete on USB 2.0 devices. Durability classification is implemented and verified on the Kingston stick as `ASSUMED_WRITE_THROUGH`. Strong durability paths (`WRITE_THROUGH`, `SYNC_BACKED`) remain QEMU-verified only, pending USB 3.x support.
5. Next: Phase 9G.5 USB topology expansion — 9G.5a multiple xHCI controllers, 9G.5b SuperSpeed enumeration, 9G.5c strong durability on a USB 3.x device, 9G.5d persistence on a second device class, 9G.5e hubs (deferred).
6. Following: user accounts, identity/permission enforcement, and installation target selection on storage partitions.
7. Do not add flatfs or automatic formatting on first write. Keep formatting an explicit operation on a selected disposable image or user-selected partition.
8. SMP and SWAPGS work remain separate milestones.