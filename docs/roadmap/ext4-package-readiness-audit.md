# EXT4 readiness review for installed systems and packages

2026-10-19. **Source/evidence review, not a new runtime acceptance campaign.**
No filesystem implementation, image, target disk or protected synchronization
contract was changed. Findings below distinguish code defects, missing APIs,
intentional profile limits and unmeasured performance/physical questions.

Follow-up: creation/write timestamp encoding is now corrected and verified in
[EH-1A](ext4-timestamps.md); additive one-file replacement is implemented with
[EH-2 automated evidence](ext4-atomic-replace.md). The findings below retain the original pre-fix
assessment; broader timestamp policy and later prerequisites remain open.

## What should be retained

The bounded journaled implementation already has ordered commits/checkpoints,
checksum and ownership validation, traditional orphan recovery, retained VFS
nodes, coherent executable snapshots, write-failure taint and extensive host/
firmware fault-injection evidence. Phase 9.6 additionally records three controlled
physical interruption cases on one identified USB device. It is not necessary
to replace this foundation or enable every Linux ext4 feature to build packages.

Keep admission fail-closed for unsupported features, exclusive device ownership,
cache invalidation/publication discipline, durability classification, taint and
DMA quarantine. More features must not weaken these contracts.

## Priority 1: fix and test timestamp encoding

**Concrete source defect:** `e4_reserve_inode_unlocked` clears the new 256-byte
inode, then writes epoch seconds at byte **148 / 0x94**:
[`ext4_mutate.inc`](../../src/fs/ext4_mutate.inc:487).
The ext4 layout places creation seconds at **144 / 0x90**, while 148 contains
creation nanoseconds/epoch extension bits. Thus new inodes do not receive the
intended creation timestamp, and the extra field is encoded incorrectly.
The journaled namespace create path calls this allocator at
[`ext4_namespace.inc`](../../src/fs/ext4_namespace.inc:59).

This is an on-disk metadata correctness issue, not a demonstrated loss of file
contents or evidence that journal replay is broken. Checksummed wrong timestamps
can still pass structural consistency checks; test their meaning independently.

**Related source gap:** time updates cast UTC seconds to uint32 and write base
mtime/ctime without updating their epoch/nanosecond extension fields
([write/rebuild](../../src/fs/ext4_mutate.inc:263)). This needs a common ext4 time
codec, especially now that the clock API permits dates through 2100. Existing
extra bits from Linux-created inodes must not be left inconsistent when a base
seconds field is rewritten. Audit chmod/chown, create, write, truncate, unlink
and rename timestamp policies together; do not implement an atime write on every
read by accident.

Acceptance: actual mounted file/directory creation and mutation; independent
Linux `debugfs stat` plus raw-field decoding for current time, 2038 boundaries,
2100 and inherited nonzero extra bits. Preserve inode checksums and data bytes.
Only after the codec is correct, add a versioned metadata ABI for `ls -l` and
`touch`: current stat layouts contain no timestamps and must not change in place.

Layout reference: [Linux ext4 inode documentation](https://www.kernel.org/doc/html/latest/filesystems/ext4/inodes.html).

## Priority 2: atomic regular-file replacement

**Missing intentional operation:** mounted ext4 advertises no-replace rename;
the namespace engine rejects an existing destination with EEXIST and rejects
directory moves ([implementation](../../src/fs/ext4_namespace.inc:149)).

Add a separately reviewed regular-file replacement operation. It must combine
directory edits, link counts, overwritten-inode orphan handling and cache/node
publication without changing the existing no-replace API. Open descriptors keep
their original object; new lookup/spawn sees a complete admitted old/new image.

Prove old-or-new destination bytes/name after interruption at every relevant
journal write/flush, commit, checkpoint and orphan cleanup boundary. Include
same inode, cross-directory same-filesystem publication, protected account paths,
allocation/credit exhaustion and concurrent spawn/open/unlink.

This is a package-manager correctness gate and also benefits safe editor saves,
configuration updates and verified download publication. Never emulate it with
in-place overwrite or runtime unlink-then-rename.

## Priority 3: make durability and error contracts usable by applications

**Existing functionality:** `SYS_SYNC` invokes `storage_sync_all()`; the ABI's
older `/mnt`-only comment is stale. Synchronous ext4 mutation already performs
commit/checkpoint barriers. Absence of `fsync` does not mean writes currently
have no durability protection.

**Missing interface:** there is no descriptor-level `fsync`/`fdatasync` contract
or explicit directory-sync interface. Define what a successful file sync and
directory sync guarantee before adding them. An initial implementation may use
the existing coarse barrier if it honestly documents that cost and propagates
backend errors. Do not invent Linux delayed-write semantics that do not exist.

Audit propagation of commit/checkpoint/flush failures through write, close,
sync, shutdown and recovery. The close callback has no error return; existing
orphan cleanup relies on later sync/shutdown reporting failures. Expose mount
health/taint clearly to users rather than relying on a generic I/O diagnostic.
`ext4_get_mount_info` currently computes its RW flag from engine/frozen state,
not a separate health status; a writable mount flag is not proof of health.

Acceptance: successful barriers followed by reboot, injected late errors,
unchanged failure outputs and no false clean/success report. Atomic file update
and metadata journaling are different properties: existing ordered mode does
not promise old-or-new contents after arbitrary in-place overwrites.

## Priority 4: disk-full behavior, headroom and package-scale limits

**Already present:** `SYS_MOUNTINFO`/`ext4_get_mount_info` report free blocks and
free inodes; `df` consumes them. A new statfs-style syscall is not automatically
required just to display free space.

**Remaining need:** an observed free-space count is not a reservation. Design
package staging/recovery headroom and verify that log growth or another writer
cannot consume the bytes/inodes needed to finish or recover a transaction.
Preserve the existing account-specific headroom policy. Recovery material must
not be evicted simply to let a new install begin.

Current bounded profile includes 1024 cached nodes (not a total on-disk file
limit), a 1 MiB directory bound, 4096 extent/traversal budgets, extent depth <=2,
64-image mutation budgets and bounded orphan state. Understand and report each
limit distinctly; do not just increase constants.

Acceptance: realistic application inventories; many small files; block-full and
inode-full separately; fragmentation; directory growth; all nodes pinned;
repeated install/remove cycles and interrupted reclamation. Prove exact free
counts, no leaks/double allocations, preserved committed data and useful
ENOSPC/EFBIG/ENOMEM diagnostics. Keep package hard limits inside the supported
filesystem/loader limits.

## Priority 5: measure storage latency and contention before optimizing

The mounted writer holds the global rank-1 IRQ-save `e4_lock` while calling the
synchronous engine ([write wrapper](../../src/fs/ext4_mount_journal.inc:133)).
The transaction layer checkpoints synchronously before returning
([transaction path](../../src/fs/ext4_transaction.inc:120)). The clean metadata
cache has eight blocks ([mount state](../../src/fs/ext4.c:60)); directory lookup
and some allocation/accounting paths scan bounded structures.

These are observable architectural costs, **not measured proof** that storage
causes any particular SSH pause. Record lock hold/wait maxima, flush latency,
checkpoint time, directory scan work, cache hit rates and concurrent SSH/logging
responsiveness on installed NVMe and USB separately.

Then choose a small optimization: allocation hints, fewer repeated scans,
cache tuning or bounded I/O batching. Do not drop flushes, add delayed allocation,
make checkpoints asynchronous or split locks as an unreviewed speed fix. A change
to lock ownership/scheduling needs the protected-contract review and a fresh
crash/lifetime argument, not only a throughput benchmark.

## Priority 6: expand application filesystem semantics selectively

- Directory rename/move: presently rejected. Requires correct `..`, parent link
  counts, cycle rejection, retained descendants and cached path behavior.
- Truncate extension: the journaled path supports shrinking but rejects
  `size > current size` ([implementation](../../src/fs/ext4_orphan.inc:324)).
  Extension needs explicit sparse/zero-fill and crash semantics; do not describe
  truncate as entirely missing.
- Symlinks/hard links, xattrs/ACLs and broader Linux-created filesystem profiles:
  explicitly outside current acceptance. Add them only for an identified
  application need, with path traversal, lifetime and security tests first.
- General large-volume, indexed-directory, flex_bg and 64bit feature support:
  keep current rejection until each feature's on-disk and recovery semantics
  have a dedicated reviewed implementation and independent Linux oracle.

Atomic regular-file replacement is more urgent for v1 packages than these broader
compatibility features. The initial package class deliberately avoids links and
multi-file runtime activation.

## Verification and documentation work across all priorities

Phase 9.6 physical acceptance covers an identified USB device, a 32 MiB filesystem,
4 KiB filesystem blocks and 512-byte device sectors. It does not establish
arbitrary consumer-NVMe power-loss behavior, large-volume performance, 4Kn physical
behavior or every package workload. Installed Dell persistence acceptance and
USB interruption evidence are separate claims.

Extend host fault campaigns and disk-free/regular-file BIOS/UEFI installed-root
fixtures first. Use Linux fsck, replay, debugfs metadata decoding and exact byte/
ownership audits. A later physical NVMe interruption procedure needs an explicitly
disposable target and separately agreed authorization; no existing gate is relaxed.

Reconcile present-status documentation: the storage annex contains older rows
saying journaled production RW is disabled and internal NVMe cannot be mounted,
alongside later accepted USB/root/installer work. Preserve historical reports,
but make the current support matrix distinguish live USB, installed NVMe,
disposable fixtures, supported features and exact physical acceptance.

## Recommended execution order

1. Timestamp codec regression and focused correction.
2. Atomic replacement design plus actual-code crash/lifetime tests.
3. Durability/error API review and package recovery headroom tests.
4. Package-scale filesystem campaign, then measured latency work.
5. Selective compatibility features driven by real applications.

Review only: timestamp findings are source-confirmed; no new disk-level
reproduction or fix is claimed here. Existing acceptance is not invalidated by
unimplemented features, and this review is not a complete corruption/security audit.
