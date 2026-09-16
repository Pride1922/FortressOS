# FortressOS - Architectural Review and Technical Debt

## Implemented: allocation-set and mapping audits

`pmm_snapshot()` copies the actual PMM allocation bitmap into caller-owned
storage under the PMM lock. The ext2 acceptance suite establishes its baseline
after mounting, populating the inode cache and warming a process lifecycle.
It compares exact allocation sets after ten further Ring 3 process cycles,
alongside heap usage, allocated page-table counts and heap integrity.

An explicit negative test allocates a different frame while freeing the first:
free-page counters match, but the bitmap comparison detects the changed set.
A changed bit means an unexpected allocation-state change, not automatically
a leak or unauthorized free. Matching bitmaps do not prove frame ownership.

`vmm_kernel_mapping_fingerprint()` walks the master higher-half page tables and
ignores hardware-managed accessed/dirty bits. This is a non-cryptographic 64-bit
FNV-style diagnostic fingerprint, not an exact mapping proof or security hash.
Expected retained tables and cached nodes must exist before the baseline.
Snapshots must be compared at a quiescent test boundary. Future concurrent
workloads need an explicit audit barrier and ownership instrumentation.

## Implemented: lock discipline

The current diagnostic build tracks actual held lock identities in a bounded
bootstrap-CPU stack. IRQs are disabled before checking or updating tracking.
Acquisition requires an increasing rank; recursion, equal-rank nesting and
inversions panic through raw UART before spinning. Releases must be LIFO.
The 64-bit saved RFLAGS token belongs to each acquisition's caller.

Order: scheduler or ext2 (1), heap (2), VMM (3), PMM (4), console (5).
Scheduler and ext2 locks cannot nest with one another. The ext2 lock serializes
synchronous reads and node-cache publication; it is not held across a yield.
`spin_unlock_noirq()` releases and updates tracking while retaining IRQ masking;
both scheduler switch sites assert that no locks remain held.
The boot self-test checks the recursion/inversion predicates and nested IF
restoration. Real lock use is exercised throughout the BIOS/UEFI suites.

NMI/fatal diagnostics must remain lockless. Before AP startup, migrate tracking
and current-thread/stack state to per-CPU storage. This checker does not claim
to detect every possible deadlock, and its global state is single-CPU only.

## Deferred: per-CPU syscall entry and SMP address-space lifetime

Keep the existing single-CPU syscall stack design until SMP is introduced.
Per-CPU TSS, current thread, syscall scratch and GS state require a complete
entry/exit design, including NMI arrival around both SWAPGS transitions.
An illustrative SWAPGS sequence alone is not a safe implementation.

Address-space teardown must prevent new scheduling into the dying space and
wait until every CPU has stopped using it. Permission/unmap changes need
invalidation on CPUs that actually cache the address space, with acknowledgement
before reclaiming frames. A scheduling affinity mask and a local CR3 inequality
are insufficient. TLB shootdown alone does not prevent re-entry into freed tables.

## Implemented: Phase 9C.2 read-only ext2

- Mount a preformatted, clean ext2 partition at `/mnt` during boot; root USTAR
  remains accessible. The partition device must live for the entire mount.
- Revisions 0/1, 1/2/4 KiB filesystem blocks, 512/4096-byte device sectors,
  power-of-two inode sizes from 128 bytes through filesystem block size.
- Regular files and linear directories; classic direct, single-, double- and
  triple-indirect block lookup. Indirection depth decreases explicitly, so
  repeated pointers cannot cause unbounded recursion. Zero pointers read as holes.
- Iterative path lookup, lazy immutable inode snapshots and node caching.
  Limits: 4096 block groups, 1024 cached nodes per mount, 1 MiB per directory,
  63-byte component names and 255-byte paths. Each read transfers at most 64 KiB;
  larger callers receive a short read and can continue.
- Validate superblock geometry/features, table ranges, inode numbers, block
  references and directory record lengths/names. Reject out-of-partition reads,
  dirty filesystems and unsupported formats; mount publication is transactional.
- Supported compat bits: ext_attr, resize_inode and dir_index, but actual indexed
  directory inodes are rejected. Supported incompat bit: filetype. Supported
  ro_compat bits: sparse_super and large_file, but nonzero high file-size words
  are rejected. Other feature bits and nonzero inode flags are rejected.
- No symlink/device support, journal replay, extents, writes, unmount or cache
  eviction. Unsupported nodes return errors when reached, not a full upfront
  certification of every inode on disk. Cached mount metadata is intentionally
  retained and accounted for; it is not classified as a leak.
- Current polling I/O executes under IRQ masking on one CPU. This bounds work
  but can delay scheduling during disk reads. Sleepable locks, asynchronous I/O,
  device-wide concurrency and mount lifetime management precede broader use.

The fixture generator requires e2fsprogs, creates real file data with mke2fs,
and runs e2fsck. It no longer substitutes an ext2 signature on failure or writes
raw patterns into filesystem sectors. GPT boundary tests compare translated
partition reads directly with corresponding parent reads.

### Verification

- `make test-ext2`: actual ext2/VFS sources, host adapters, ASan/UBSan and leak
  detection. 1/2/4 KiB blocks, 512/4096-byte sectors, 128/256-byte inodes;
  malformed geometry/features/table pointers/directory records; allocation and
  I/O failure; sparse reads; indirect pointer bounds and fixed-depth traversal.
- `make test-storage`: strict kernel build, BIOS and paired-pflash UEFI boot.
  Includes previous suites, all GPT negative cases, nested lookup/enumeration,
  independent offsets, direct/single/double-indirect fixture reads and sparse data.
  Ring 3 opens `/mnt/hello.txt`, checks exact contents, prints, checks EOF, closes,
  exits, and repeats under the allocation-set/mapping/heap audits.
- Host tests exercise triple-indirect traversal with synthetic references; a
  full large on-disk triple-indirect file is not part of the QEMU fixture.
- QEMU tests use snapshot disk writes. Logs: `build/storage-bios.log` and
  `build/storage-uefi.log`. Physical Latitude storage remains unverified.

## GPT conformance

Supported partition-entry sizes are 128, 256 and 512 bytes. This is a deliberately
bounded subset of the specification's power-of-two multiples of 128 bytes.
GPT and ext2 boot acceptance now run in both BIOS and UEFI.

## Next milestones

1. Keyboard/serial input and a blocking wait queue, with I/O APIC delivery tests.
2. Ring 3 shell and small editor using input/output syscalls. Editing and command
   parsing belong in user space; the kernel supplies input and terminal services.
3. Controlled ext2 writes, creation and allocation, then reboot persistence tests.
4. User/account permissions and installation once writable storage is reliable.

Do not add flatfs or automatic formatting on first write. Keep formatting an
explicit operation on a selected disposable image or user-selected partition.
SMP and SWAPGS work remain separate milestones.

## Implemented: exact-boundary NMI delivery verification

`make test-nmi` injects real external NMIs through QMP at these five debugger
hardware-breakpoint locations: before saving user RSP, after saving it, after
switching to the kernel stack, before restoring user RSP, and immediately before
SYSRET with user RSP already active. Labels add no instructions or delays.
The harness checks the hardware frame's exact interrupted RIP/CS/RFLAGS/RSP/SS,
checks execution on IST2, verifies all GPRs and scratch values are preserved,
compares 128 bytes below user RSP, executes IRET and compares restored state.
The exit probe also checks successful SYSRET to CPL3. Four rounds per firmware
provide 40 verified injections total, followed by full-suite completion.
JSON evidence and serial logs are in `build/nmi-bios.*` and `build/nmi-uefi.*`.

Testing exposed masked LINT1 routing: QEMU's external NMI delivery respects that
mask. The kernel now parses MADT Local APIC NMI records and applies the declared
bootstrap-CPU routes, rather than assuming both LINT pins can always stay masked.
Invalid pin/flag encodings are tested; conflicting applicable routes are rejected.
The harness requires this normal boot configuration; it performs no guest memory
or register writes. References: QEMU GDB documentation and QEMU hw/intc/apic.c.

This closes the QEMU transition-window delivery gap. It does not establish
physical-hardware delivery, all nested-fault cases, SWAPGS correctness or SMP safety.

## Physical boot: visible early diagnostics

The framebuffer console previously started after PMM/VMM/heap tests. The PMM
also audited a fixed 2 GiB capacity after sizing itself for all installed RAM,
so a larger machine could halt before any screen output. Initialization now
starts screen logging early and explicitly limits managed RAM to the supported
low 2 GiB. Higher RAM remains unavailable to allocation. UART writes also stop
waiting after a bounded poll or failed loopback test.

A QEMU UEFI regression with 8 GiB and COM1 absent reaches PCI discovery after
all memory, process and initramfs tests; its framebuffer screenshot is saved.
Laptop root cause still requires a new boot/photo; emulator evidence alone does
not identify the exact failure on the Latitude. Physical disks are excluded from
QEMU-specific storage fixture tests using the controller vendor/device identity.

## Boot-console performance

Replaced uncached framebuffer-to-framebuffer scrolling with a static RAM text
cache. Rendering compares character and colours and skips unchanged cells;
blank cells ignore irrelevant foreground-colour differences. Scrolling advances
up to eight rows per batch, reducing screen movement frequency during logs.
The fixed 1.5 MiB cache requires no early allocator and supports a 512 x 256 text
viewport. Framebuffer writes remain uncached; this is not GPU acceleration or
write-combining support. Actual Latitude performance awaits the rebuilt ISO test.

Host ASan/UBSan tests verify rendered pixels, padding bounds, preserved colours,
batched scroll frequency, no blank-cell redraw, and exact-width newline handling.
BIOS and UEFI acceptance suites pass with the updated cursor semantics.
