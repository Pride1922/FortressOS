# EXT4 Phase 8.1 — transaction foundation

Implemented from clean `main` checkpoint `2aa97c4`, 2026-10-04. Only part 8.1
of the agreed six-part Phase-8 plan is enabled. Production journaled RO/RW
admission remains rejected; no VFS journal callbacks, recovery-at-mount,
RECOVER/clean-state transitions, orphan processing or physical journal claim.

## Metadata write inventory

All filesystem device writes originate in `e4_write_bytes` (E4-A), JBD2
`j_io` (journal writer/replay), or their block-sector/run adapters. Reads,
admission, journal discovery and cache fill have no write callbacks. Inventory
includes indirect calls through VFS and the engine:

| Metadata / operation | Staging and checksum owner | Current publication path / Phase-8 boundary |
| --- | --- | --- |
| Block bitmap allocation, free and lazy initialization | `e4_allocate`, `e4_release`, `e4_stage_bitmap`; bitmap checksum in `e4_seal` | E4-A ALLOC role; operation integration is 8.2/8.4 |
| Inode bitmap reserve/release and initialized-table flags | `e4_reserve_inode_unlocked`, `ext4_engine_release_inode`, VFS create/unlink; `e4_stage_bitmap` | E4-A ALLOC role; 8.3/8.4 |
| Group counters, used directories, unused inode counts and descriptor checksum | `e4_account`, reserve/release/create/unlink, `e4_descriptor`, `e4_seal` | E4-A ALLOC role; 8.2-8.4 |
| Primary superblock free totals, checksum and dirty state | `e4_super`, `e4_account`, `e4_seal`; first-commit dirty marker in `e4_commit_unlocked` | E4-A direct dirty marker + ALLOC images; journal workbench blocks this path |
| Clean-state transition | `ext4_engine_finish`, called by `ext4_freeze_and_sync` | E4-A direct superblock write between barriers; journal finish returns EOPNOTSUPP; 8.5 |
| Inode identity, mode, generation, size, block count, links, deletion and extent root | `e4_inode_image`, `e4_inode_sum`, grow/trim, reserve/release, VFS write/create/unlink/rename/truncate | E4-A REFERENCE images; 8.2-8.4 |
| External extent leaves/indexes, splits, promotion and old-tree reclamation | `e4_rebuild`, `e4_old_tree`, `e4_release`; inode-seeded tree checksum | E4-A NEW images + reference/free stages; 8.2/8.4 |
| Directory blocks, records, checksum tails, dot/dotdot and directory growth | `e4_dir_edit`, `e4_dir_entry`, `e4_dir_sum`, create/mkdir/unlink/rmdir/rename helpers in `ext4_write.inc` | E4-A NEW/REFERENCE images; 8.3 |
| Deferred free publication | `e4_hold_frees`, `e4_seal` | E4-A retains allocations until reference barrier, then frees; transaction frees/revokes/orphans remain 8.4 |
| Journal descriptors, tags, payloads, revokes, commit, activation and empty tail | `jbd2_writer_*` and scalar JBD2 checksum helpers | JBD2 ordered-data/log/activation/commit/checkpoint/tail barriers; connected by 8.1 |
| Recovery home images and journal retirement | `jbd2_replay` after full `jbd2_analyze` | Explicit exclusive recovery only; production dispatch remains 8.5 |
| Superblock backups, inode tables as whole tables, orphan chain | No mutation owner enabled | Backups are recovery identity sources only; whole-table initialization and orphan updates are not implemented |

`E4_NEW` historically includes both data and new metadata. It is **not** a
journal-data classification. The new transaction boundary requires explicit
metadata versus ordered-data staging; it never infers type from E4-A roles.
E4-A's established ordering, short-write behavior, checksums and cache are
unchanged. No ordinary file/namespace operation is silently enabled on a
journal volume by this foundation.

## Ownership, bounds and failure behavior

`ext4_engine_open_journal(dev, admitted, &engine)` is an exclusive workbench.
It uses the Phase-6 filesystem discovery/identity guard and Phase-7 empty
journal admission. The global engine reservation excludes mounts and other
workbenches until close. No VFS node is published. The guard, immutable journal
tree/map discovery state and writer live until close; the writer copies its
source map. Every acquisition failure unwinds ownership and allocations.
Writable callbacks alone do not establish durability: the caller must supply
explicit external admission and exclusively own a disposable/eligible device.

The caller serializes all calls, including close, in unlocked thread context.
The existing rank-1 lock only protects ownership reservation/release. All JBD2
calls run unlocked, preserving its public contract and the existing synchronous
block/USB/DMA contracts. There is no worker, journal-space wait, lock nesting,
IRQ policy change or DMA reuse/quarantine change. Future VFS integration must
resolve its lock boundary before enabling journal operations.

Begin reserves worst-case ring space through `jbd2_writer_begin`. Limits are
64 combined full-block metadata/ordered-data engine images and 64 revokes;
each credit class also respects the writer's 64-entry limit. At 4 KiB, engine
images use at most 256 KiB; writer snapshots at most another 256 KiB. Workspaces
are heap-owned, not kernel-stack arrays. Duplicate replacements/revokes use
one credit; class overlap rejects. Ordered data cannot target reserved metadata
or journal data/tree blocks. Caller still owns file-data allocation semantics.

The transaction APIs snapshot caller bytes into the mutation engine. Metadata
must already have its filesystem checksums sealed. Before the first I/O, commit
stages **all** images/revokes through the writer's bounds/identity checks.
Any staging/credit/OOM/identity error latches until abort, issues zero writes,
and leaves the engine untainted. Empty/data-only transactions reject before I/O.
Abort discards plans only; it never removes terminal I/O taint.

Commit writes ordered data and the journal, makes commit durable, checkpoints
home metadata, and retires the tail using the Phase-7 barriers. Cache entries
are invalidated before I/O; descriptors and clean metadata cache images are
published only after successful checkpoint/tail barriers. Any commit/checkpoint
read/write/flush failure permanently taints the engine, disables/drops its cache,
and blocks later mutations/finish with EIO. Close writes nothing, leaves an
unfinished journal recoverable, and never marks EXT4 clean.

The shared direct-write helper refuses every journal engine. The legacy
locked commit helper also refuses it. Public grow/trim/reserve/release entry
points reject journal mode at `e4_plan_begin`; journal `finish` rejects too.
This makes every enabled journal mutation use the transaction boundary without
prematurely claiming the operation integration of 8.2-8.5.

## Verification

`make test-ext4-transaction-host` compiles the actual engine, filesystem
bootstrap, writer and reader with ASan/UBSan and strict warnings. Fixtures use
the explicit Phase-7 CSUM_V3/REVOKE/internal-journal generator under unique
`build/ext4-phase8-1/run-*` regular-file directories. No raw media or mounts.
They deliberately supply RECOVER and journal state; production does not yet
perform these transitions.

The gate covers 1/2/4 KiB blocks × 512/4096-byte sectors at normal and circular/
UINT32 sequence-wrap placement, 12 cases. It asserts admission/ownership,
disabled semantic entry points/direct-write helper, duplicate staging, metadata
and ordered-data credit exhaustion, maximum 64 metadata + 64 revoke credits,
ring ENOSPC, empty transactions, journal self-write/inode identity rejection,
sticky staging errors, abort, every open/snapshot allocation and open/commit
read failure, taint containment and post-checkpoint cache bytes.

Every representative write/flush event is interrupted before/after under four
stable/volatile sector patterns. Fresh public discovery/recovery must yield the
complete old or committed metadata set, ordered data when committed, unchanged
revoked bytes and idempotent replay. Successful commit/checkpoint survives a
restart. Linux independently audits checkpointed and recovered copies with
`e2fsck -fn`, exact file bytes and inode mode (24 copies). These are block-image
foundation transactions, not file allocation/namespace/crash-atomic overwrite
acceptance. The exhaustive operation/QEMU crash matrix remains Phase 9.

Commands run in WSL `Ubuntu-24.04` at `/mnt/c/Sources/FortressOS`:

| Command | Result / retained evidence |
| --- | --- |
| `make test-ext4-transaction-host` | 12/12, 3,568 atomic cuts, home-byte checkpoint/RMW guard, all-class overlap/self-write/credit rejection and 24 Linux audits; `build/ext4-phase8-1/run-ekfln4ih` |
| `make test-jbd2-write-host` | 12/12, 3,568 atomic cuts, sector tears, staging/credits/reuse/read/OOM and 36 Linux audits; `build/jbd2-write/run-uyumeko6` |
| `make test-jbd2-replay-host` | 12/12 and 48 zero-write corruption rejections; `build/jbd2-replay/run-9_9ygtkq` |
| `make test-ext4-write-host` | Six geometries, metadata cache/taint, accepted-prefix faults, namespace/append/gap, exact 1/16 MiB and Linux fsck; `build/ext4-phase4/host-2g5zytwo` |
| `make test-ext4-alloc-host` | Six geometry and six maximum-map cases, allocation/credits/fault ownership and Linux audits; `build/ext4-phase3/run-pjlldeyb` |
| `make test-ext4-read-host test-ext4-format-host` | Six geometries, checksum/malformed/read/OOM/zero-write and extent-read regressions; `build/ext4-phase0/fixtures-l9kcuiw3` |
| `make test-ext2 test-usb-mount-host test-xhci-bot-host` | PASS: ext2 geometry/fault/lifecycle, USB/EXT4 dispatch/admission, GPT bounds, BOT failure/quarantine/durability and bounded 16 KiB runs |
| `make test-ext4-usb` | Production E4-A BIOS/UEFI × SMP=1/4, 4/4 cases and 12 RW boots, plus four immutable RO/degraded-GPT cases; sync/persistence/namespace/true AP append and independent Linux hashes/fsck PASS; `build/ext4-usb/run-1lnokk2b` |
| `make bin/fortress.elf`, `make` | Strict build/link and default ext2 image/GPT/e2fsck verification PASS; no new compiler warnings |

Initial failed/superseded fixture runs remain retained. One initial test counted
fixture-construction allocations as journal-open allocations; the sweep now
resets counters after fixture discovery. The home-byte assertion also accounts
for sub-sector filesystem blocks: ordered-data sector RMW may include neighboring
metadata bytes, but must preserve those bytes exactly before checkpoint.

## Physical evidence boundary

The user confirmed the existing metadata-cache implementation on Dell 5590:
16 MiB download 7.06 s, hashes pass after reboot and independently on Mint,
unmounted `e2fsck -fn` exits 0. This is E4-A cache/persistence acceptance, not
journal performance or crash-consistency evidence. Bounded 16 KiB USB runs
were already implemented at the starting checkpoint and are preserved.
