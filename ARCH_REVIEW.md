# FortressOS - Architectural Review and Technical Debt

This file holds cross-cutting audit findings, deferred work, and technical
debt — things that span or outlive individual phases. Phase-specific
implementation narrative and verification evidence now live in
`docs/roadmap/`; where a topic below has a phase-specific home, this file
points there rather than restating it, to avoid two copies drifting apart.

## Implemented: allocation-set and mapping audits

`pmm_snapshot()` copies the actual PMM allocation bitmap into caller-owned
storage under the PMM lock. The ext2 acceptance suite establishes its
baseline after mounting, populating the inode cache and warming a process
lifecycle. It compares exact allocation sets after ten further Ring 3
process cycles, alongside heap usage, allocated page-table counts and heap
integrity.

An explicit negative test allocates a different frame while freeing the
first: free-page counters match, but the bitmap comparison detects the
changed set. A changed bit means an unexpected allocation-state change, not
automatically a leak or unauthorized free. Matching bitmaps do not prove
frame ownership.

`vmm_kernel_mapping_fingerprint()` walks the master higher-half page tables
and ignores hardware-managed accessed/dirty bits. This is a
non-cryptographic 64-bit FNV-style diagnostic fingerprint, not an exact
mapping proof or security hash. Expected retained tables and cached nodes
must exist before the baseline. Snapshots must be compared at a quiescent
test boundary. Future concurrent workloads need an explicit audit barrier
and ownership instrumentation — see `SMP_DESIGN.md` Piece 3.

## Implemented: lock discipline

The current diagnostic build tracks actual held lock identities in a
bounded bootstrap-CPU stack. IRQs are disabled before checking or updating
tracking. Acquisition requires an increasing rank; recursion, equal-rank
nesting and inversions panic through raw UART before spinning. Releases
must be LIFO. The 64-bit saved RFLAGS token belongs to each acquisition's
caller.

Order: scheduler or ext2 (1), heap (2), VMM (3), PMM (4), console (5).
Scheduler and ext2 locks cannot nest with one another. The ext2 lock
serializes synchronous reads and node-cache publication; it is not held
across a yield. `spin_unlock_noirq()` releases and updates tracking while
retaining IRQ masking; both scheduler switch sites assert that no locks
remain held. The boot self-test checks the recursion/inversion predicates
and nested IF restoration. Real lock use is exercised throughout the
BIOS/UEFI suites.

NMI/fatal diagnostics remain lockless. Piece 2 gives CPUs private current
thread/stack state but keeps APs out of subsystem locks. Lock-debug tracking
remains BSP-only; its storage migration and cross-CPU re-verification belong
to Piece 3 (SM10/SM11). No cross-CPU lock/deadlock-detection claim follows
from Piece 2's per-CPU scheduler storage.

The USB storage driver (Phase 9G) adds no new locks to this hierarchy. Its
synchronous BOT transfers execute under the ext2 lock when called from the
filesystem and perform no allocations, sleeps, or IRQ-enabling waits in
that path. USB transfer completion is bounded polling, not IRQ-driven,
precisely to preserve this contract. See
`docs/roadmap/phase-9g2-usb-block.md`.

## Per-CPU syscall entry; deferred SMP address-space lifetime

Piece 6A (2026-09-24) supplies the missing documented boot allocation ceiling
and CR3-gated high-memory unlock, plus AP readiness checks. Implementation is
written; build/host/QEMU/Dell verification is pending user execution. See
[the 6A handoff](docs/roadmap/smp-piece6-memory.md). Concurrent allocator,
shootdown-progress and address-space lifetime work remains in 6B–6D; this
boot-order fix does not resolve the teardown issue described below.

Piece 2 implements GS-local syscall scratch/RSP0, current thread, TSS and
IST stacks. Entry uses SWAPGS; interrupt entry checks actual GS base because
saved CS alone cannot distinguish the NMI windows around SWAPGS. Seven
exact syscall boundaries are exercised by the BSP-only NMI runner. See
[Piece 2 evidence](docs/roadmap/smp-piece2-percpu.md) for AP fault/NMI tests
and the remaining physical-acceptance boundary.

Address-space teardown must prevent new scheduling into the dying space and
wait until every CPU has stopped using it. Permission/unmap changes need
invalidation on CPUs that actually cache the address space, with
acknowledgement before reclaiming frames. A scheduling affinity mask and a
local CR3 inequality are insufficient. TLB shootdown alone does not prevent
re-entry into freed tables.

**This entire section is the design problem `SMP_DESIGN.md` addresses.**
Per-CPU syscall entry is Piece 2; TLB shootdown with real acknowledgment is
Piece 5, which explicitly cites the "CR3 inequality is insufficient" point
above as its reason for existing. Read the design doc before starting this
work, not just this summary.

## Implemented: Phase 9C.2 read-only ext2

See `docs/roadmap/phase-9c2-readonly-ext2.md` for implementation detail and
verification evidence. Superseded/extended by Phase 9D
(`docs/roadmap/phase-9d-writable-ext2.md`) for writable support.

## GPT conformance

Supported partition-entry sizes are 128, 256 and 512 bytes. This is a
deliberately bounded subset of the specification's power-of-two multiples
of 128 bytes. GPT and ext2 boot acceptance run in both BIOS and UEFI.

GPT partition-array validation is bounded to the image's declared layout.
For a raw image flashed to larger media, the backup GPT remains at the
image's last sector, not the device's. Phase 9G.3
(`docs/roadmap/phase-9g3-usb-mount.md`) accepts the primary header with a
validated array in this case and logs the discrepancy as a possible
raw-copy size mismatch. Phase 9G.4
(`docs/roadmap/phase-9g4-usb-durability.md`) records that writable
eligibility on such media is not yet defined — writes to a larger
as-flashed device are deferred pending an explicit policy. Automated repair
or relocation of GPT is never performed.

This note is kept here, once, because it's used identically by Phase 9C.2,
9G.3 and 9G.4 — duplicating it into all three phase files would drift.

## Implemented: Phase 9D bounded writable ext2

See `docs/roadmap/phase-9d-writable-ext2.md` for the full write/truncation
ordering, prefix-durability rules, superblock clean/dirty lifecycle, and
verification evidence.

## ext2 write cap

The ext2 write path supports **direct and single-indirect block mappings only**.
Double-indirect and triple-indirect blocks are not implemented. Files that
would require them are rejected at inode validation, not silently truncated.

### What "rejected" means

Two distinct rejection points exist in the code:

- **At inode open / validate (`src/fs/ext2.c:547`):** An inode whose block
  array contains a double-indirect or triple-indirect pointer is rejected
  with an unsupported-structure error.
- **At truncate / block collection (`src/fs/ext2.c:856`):** A truncate
  operation that would need to walk past the single-indirect boundary
  is rejected before any mutation is attempted.

Both rejections are **explicit and fail-closed**: they do not silently
truncate, silently zero, or silently accept a partial write.

### Supported block sizes

The mount path accepts the three standard ext2 block sizes and rejects
anything larger (`src/fs/ext2.c:1603`):

```c
if (... || u32(sb + 24) > 2 || ...) return false;
```

`s_log_block_size > 2` is rejected at mount time.

### Maximum file size

The cap depends on the filesystem's block size:

| Block size | Max file size | Formula |
| --- | --- | --- |
| 1 KiB | 268 KiB | (12 + 256) × 1024 = 274,432 |
| 2 KiB | 1 MiB | (12 + 512) × 2048 = 1,073,152 |
| 4 KiB | 4 MiB | (12 + 1024) × 4096 = 4,243,456 |

The general formula is:

```text
max_size = (12 + block_size / 4) × block_size
```

where the 12 is the direct block array and `block_size / 4` is the number
of 4-byte block pointers that fit in one indirect block.

The first block is also block-size dependent (`src/fs/ext2.c:1641`):
1 KiB filesystems start data at block 1; larger block sizes start at block 0.
This is enforced at mount time.

### Why this limit exists

The ext2 write path exists for three purposes:

1. Reading and writing disposable QEMU test fixtures. The E4-A
   non-journaled path uses ext2 as its substrate.
2. Compatibility with existing ext2 images that a user might attach
   or that tests might construct.
3. Boot-time persistence for the raw disk image's data partition
   (before the ext4 journal is production-enabled).

It is not the target for new filesystem features. The README states
"ext2 writes support direct and single-indirect blocks, with explicit
rejection of unsupported structures" — that's this limit.

### Per-operation cap (separate)

There is also a per-read and per-write cap of 64 KiB:

```c
#define EXT2_MAX_READ  (64U * 1024U)   /* src/fs/ext2.c:12 */
#define EXT2_MAX_WRITE (64U * 1024U)   /* src/fs/ext2.c:13 */
```

These bound a single read or write call, not file size. A file can be
up to the block-mapping cap above; individual I/O operations against it
are limited to 64 KiB. Callers loop for larger transfers.

### Feature-flag policy

The mount path accepts only the features the implementation understands
(`src/fs/ext2.c:1614–1624`):

- **Compat (`s_feature_compat`):** bits 3, 4, 5 only.
- **Incompat (`s_feature_incompat`):** bit 1 (FILETYPE) only.
- **Read-only compat (`s_feature_ro_compat`):** bits 0, 1 (SPARSE_SUPER,
  LARGE_FILE) only.

Anything else → mount rejected. This is how unsupported inode flags
(extents, compression, indexed directories) are caught: they require
incompat flags the mount path doesn't accept.

### Writable-mount restrictions

Writable mounts are stricter than read-only mounts (`src/fs/ext2.c:1625–1638`):

- `s_feature_incompat` must be exactly FILETYPE (bit 1).
- `s_feature_ro_compat` must be exactly SPARSE_SUPER or LARGE_FILE.

A filesystem not marked clean (`s_state != EXT2_VALID_FS`) warns but
proceeds, matching Linux's ext2 driver. A stronger recovery path
(fsck, journal) is deferred.

### Where new filesystem work lives

New filesystem capabilities target the ext4 path:

- Journaling (JBD2 transactions).
- Crash-consistency guarantees.
- Larger files (extent trees, not indirect block chains).
- Atomic metadata updates.

See:

- `docs/plans/EXT4_PLAN.md` — the ext4 target and phases.
- `docs/roadmap/ext4-phase8-4.md` — truncate and orphan recovery.
- `docs/roadmap/ext4-phase8-6.md` — integration audit.
- `docs/plans/EXT4_PHASE9_PLAN.md` — crash campaign and E4-B acceptance.

### Non-goals

Not planned for ext2:

- Double-indirect or triple-indirect block support (`ext2.c:547`, `:856`).
- Ext2 crash-consistency or journaling.
- Ext2 extent trees.
- Ext2 indexed directories.
- Ext2 compression.
- Ext2 external extended attributes.
- Ext2 format conversion in place.

The default boot image remains ext2 (non-journaled). Accepted E4-A USB
media remain ext2 (non-journaled). ext4 is the target for production
journaled RW, currently disabled pending Phase 9 acceptance.

### Related limits

**LARGE_FILE feature.** `src/fs/ext2.c:194` notes the LARGE_FILE feature
is honored only when the file's high size word is zero. Files whose
`i_size_high` field is non-zero (nominally ≥ 4 GiB) are treated as if
the high word were zero. The block-mapping cap already limits files to
a few MiB, so this is not the binding constraint — but the size-field
handling is deliberately narrower than ext2's own spec.

### Verification

The cap is verified by:

- `make test-ext2-write` — bounded read/write against disposable images.
- `make test-storage` — GPT/ext2 parsing and user-space reads.
- `make test-ext2` — ext2/VFS code under ASan/UBSan, including
  malformed data and injected failures.

See `docs/roadmap/` for retained evidence per phase.

### Cross-references

- `src/fs/ext2.c:12-13` — 64 KiB per-operation read/write caps.
- `src/fs/ext2.c:193` — inode flag rejection.
- `src/fs/ext2.c:194` — LARGE_FILE narrowness.
- `src/fs/ext2.c:547` — double/triple indirect rejection at validation.
- `src/fs/ext2.c:856` — double/triple indirect guard at truncate.
- `src/fs/ext2.c:1603` — block-size upper bound at mount.
- `src/fs/ext2.c:1614-1624` — feature-flag audit.
- `src/fs/ext2.c:1625-1638` — writable-mount restrictions.
- `src/fs/ext2.c:1641` — first-block consistency check.
- `README.md` "Storage" section — top-level summary.
- `docs/plans/EXT4_PLAN.md` — where new work lives.
- `docs/roadmap/ext4-phase8-6.md` — the most recent journal acceptance.


## Implemented: exact-boundary NMI delivery verification

See `docs/roadmap/subsystems.md` ("NMI transition and physical-boot
diagnostics") for the full `make test-nmi` methodology, the masked-LINT1
finding, and evidence locations. Note for SMP work: this verification is
BSP-only for syscall transitions. Piece 2 adds actual hardware-NMI delivery
on parked APs and checks CPU-local IST2 records; this does not establish
syscall-boundary coverage on APs, which do not execute user tasks yet.

## Physical boot: visible early diagnostics

See `docs/roadmap/subsystems.md` ("Physical boot: visible early
diagnostics") for the early-framebuffer-logging fix and its QEMU/hardware
evidence.

## Boot-console performance

See `docs/roadmap/subsystems.md` ("Boot-console scrolling") for the static
RAM text cache implementation and `make test-console` coverage.

## Interactive shell checkpoint (S0–S2 delivered)

See `docs/roadmap/shell-s0-s2.md` for the Shell S0–S2 foundation, raw/timed
input (`SYS_INPUT_READ`), terminal control (`SYS_TERMCTL`), line editing,
AltGr support, and RAM history/search. Historical input/scheduler contracts
and early test coverage live in `docs/roadmap/subsystems.md` ("Interactive
input and shell"). S3+ (cwd, quoting, redirection, pipes, jobs) remain planned
in `docs/plans/SHELL_DESIGN.md`.

## Dell manual hardware acceptance (2026-09-16 & 2026-09-18)

See `docs/roadmap/subsystems.md` ("Dell Latitude 5590 physical acceptance")
for the general acceptance narrative, and
`docs/roadmap/phase-9e-exec-and-files.md` for the AZERTY/ABI-specific
verification done in that same session.

## System V AMD64 ELF User Stack & Argument Passing ABI

See `docs/roadmap/phase-9e-exec-and-files.md` for the full stack-layout,
register-initialization, and verification detail.

## Saved File Management (Phase 9E) & Bug H4 Resolution

See `docs/roadmap/phase-9e-exec-and-files.md` for directory operations
(`mkdir`/`unlink`/`rename`), on-disk reclamation, and the AZERTY scancode
fix.

## Implemented: Phase 9G USB storage

Self-contained driver set under `src/drivers/xhci*` and
`src/fs/usb_mount.c`, targeting xHCI controllers only (EHCI/UHCI/OHCI out of
scope). Full implementation and verification detail is split across:

- `docs/roadmap/phase-9g1-xhci-enumeration.md` — controller discovery,
  MMIO/reset, rings, ports, device addressing & descriptors.
- `docs/roadmap/phase-9g2-usb-block.md` — Bulk-Only Transport, SCSI engine,
  block device registration.
- `docs/roadmap/phase-9g3-usb-mount.md` — production `/mnt` mount,
  PARTUUID selection.
- `docs/roadmap/phase-9g4-usb-durability.md` — writable mount, durability
  classification.
- `docs/roadmap/phase-9g5-superspeed.md` — multi-controller enumeration,
  SuperSpeed/USB 3.x.

Scope limits (verified by construction, not aspiration): USB 2.0 and
SuperSpeed direct-attached devices only (no external hubs); attached-at-boot
only, no hot-plug; no isochronous/interrupt/control-only classes;
root-port only.

Physical acceptance spans two devices on two device classes: Kingston USB
DISK 2.0 (USB 2.0, `ASSUMED_WRITE_THROUGH`) and SanDisk USB 3.2 Gen 1
(SuperSpeed, `SYNC_BACKED`) — see the phase files above for per-device
detail. Internal NVMe is excluded from USB mount selection by
parent-device provenance in all cases.

## Implemented: USB durability classification

Four-tier state machine, gating writable mount on what a device's SCSI
responses actually report:

- `SYNC_BACKED` — accepts `SYNCHRONIZE CACHE(10)`. Strongest guarantee.
- `WRITE_THROUGH` — reports `WCE=0` explicitly.
- `ASSUMED_WRITE_THROUGH` — no cache policy reported, transport healthy;
  write-through assumed (matches Linux/Windows behavior); three-line
  power-loss disclosure printed at boot.
- `READ_ONLY` — reports `WCE=1` and rejects sync, or is in an
  error/latched-offline state.
- `UNKNOWN` — probe incomplete; treated as `READ_ONLY` for mount
  eligibility.

Full classification rules, SCSI probe sequence, and per-device hardware
results: `docs/roadmap/phase-9g4-usb-durability.md`. Do not whitelist by
VID:PID; do not assume removable flash has no volatile cache — durability
is established by what the device reports, not by vendor identity.

## Next milestones

1. Phase 9G USB storage (through 9G.5b) and Phase 9H (32 GiB RAM) are
   complete — see `docs/roadmap/README.md` for current status per phase.
2. Open USB sub-projects: 9G.5d (persistence on the SanDisk), 9G.5e (hubs,
   deferred). See `docs/roadmap/phase-9g5-superspeed.md`.
3. **SMP (multi-core) support is COMPLETE and hardware-verified** (Pieces 1–5,
   6A, 6B, 6C, 6D on Dell Latitude 5590). Full architectural specification:
   `docs/plans/SMP_DESIGN.md`; evidence and implementation logs:
   `docs/roadmap/smp-piece*.md`.
   Current open items following SMP:
   - Shell progression: S0–S2 implemented (automated checks PASS, Dell verification checklist pending; see `docs/roadmap/shell-s0-s2.md`); S3–S4 (working directories `cd`/`pwd`, consistent quoting, completion, persistent history) are planned next in `docs/plans/SHELL_DESIGN.md`.
   - System introspection syscalls/tools (`sysinfo`/`top`/`ps`).
   - Persistent rootfs (`/paradise`).
   - MicroPython.
4. Following SMP: user accounts, identity/permission enforcement, and
   installation target selection on storage partitions.
5. Do not add flatfs or automatic formatting on first write. Keep
   formatting an explicit operation on a selected disposable image or
   user-selected partition.

## Known behavior: unclean shutdown leaves the USB stick unmountable

If the stick is removed or the system is powered off without a clean
shutdown, the ext2 dirty marker is set. On the next boot, both
`ext2_mount_rw` and `ext2_mount` fail, leaving `/mnt` unmounted with a
generic error. Recovery requires `e2fsck` from a Linux host or reflashing.
The failure diagnostic does not surface the dirty-marker reason. A
read-only fallback for dirty-but-valid filesystems is deferred.

This is open technical debt, not tied to a single phase — worth prioritizing
over further USB topology work given how directly it affects anyone testing
on real hardware (USB sticks get unplugged; that's the point of USB).
