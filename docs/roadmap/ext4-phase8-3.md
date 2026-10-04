# EXT4 Phase 8.3 — namespace transactions

Implemented 2026-10-04 from clean `main` checkpoint `7f4c80b`, after committing
and pushing the accepted Phase-8.2 implementation. Production journaled RW
remains disabled. These APIs extend the exclusive disposable-media workbench;
they publish no VFS nodes or file descriptors.

## Operation boundaries

`ext4_engine_open_journal_namespace` extends Phase-8.2's explicit admission:
valid primary superblock, existing RECOVER bit, empty supported internal
journal, checked bitmaps/counters and immutable journal identity/ranges. The
caller owns media exclusively and serializes all operations and close from
unlocked thread context. No mounted filesystem or open VFS handles may coexist.
Legacy engine mutation/finish and raw block-image transactions stay disabled.

Every enabled operation is one bounded JBD2 transaction, with at most 64
combined images and 64 revoked freed blocks. Directory data, including newly
allocated directory blocks, is journal metadata rather than ordered file data.
All images/checksums and writer snapshots are complete before device writes.
Create returns its inode only after commit/checkpoint/tail durability. Planning
and resource failures leave names, ownership and output values unchanged;
uncertain I/O permanently taints the engine, disables its metadata cache and
requires restart/recovery rather than a rollback claim.

| Operation | Metadata published together | Supported boundary |
| --- | --- | --- |
| create | Child inode and generation, inode bitmap/free counts, initialized-table flags/unused counts, parent entry/checksum, parent growth/root/tree and block accounting when needed | New regular file, single link, non-existing name |
| mkdir | Create set plus initialized dot/dotdot block, child size/block count, child links=2, parent link increment and group directory count | New directory; inherited E4-A inode construction policy |
| rename | New name and old-name removal, both directory checksums, destination growth/root/tree/bitmaps/counters and old-tree revokes when needed | Regular singly linked file only, same or different parent, no replacement; identical source/destination is write-free |
| unlink | Name removal/checksum, cleared inode/bitmap, preserved generation, free block/inode accounting, revokes for every released data/tree block | Closed singly linked regular file; entire reclamation must fit one transaction |
| rmdir | Unlink set plus parent link decrement and group directory-count decrement | Empty directory with valid dot/dotdot ownership and exactly two links |

Directory parents are fully scanned; dot/dotdot inode identities and entry
types, parent-directory type and link bounds are checked before edits. Names
are bounded to 63 bytes; empty, dot/dotdot and slash-containing names reject.
Reserved/journal inodes cannot be removed or renamed. Existing destinations,
directory moves, wrong target kinds, active cached targets and nonempty rmdir
reject without writes. More than 64 blocks of reclamation returns EFBIG before
freeing anything; restartable reclamation and open-unlink/orphans remain 8.4.

The implementation shares E4-A's existing directory edit, extent rebuild,
allocator, inode construction and checksum helpers. Its publication boundary
is always `e4_semantic_commit`, shared with 8.2. It never calls E4-A's direct
home-write publication. Checkpoint and tail barriers finish before any later
allocator can reuse revoked blocks. Temporary workbench reader maps are freed
between semantic operations; clean metadata cache publication remains after
durability. E4-A's mounted map lifetime, locks, namespace callbacks, durability
policy, DMA quarantine and bounded 16 KiB USB runs are unchanged.
The JBD2 source callback comment now matches its existing analysis and writer
staging call sites; no guard behavior or journal wire format changed.

## Verification

Focused target: `make test-ext4-namespace-host`, actual EXT4/JBD2 under
ASan/UBSan and strict warnings, fresh regular images only. Final evidence:
`build/ext4-phase8-3/run-v_21syr8`: 12/12 configurations, 108 operation
profiles, 48,288 crash cuts and 324 Linux replay/fsck copies PASS.

The matrix is 1/2/4 KiB blocks, 512/4096-byte sectors and normal/wrapped journal
placement/sequence. Nine profiles cover create, mkdir, same-block rename,
cross-directory rename, closed-file unlink, empty rmdir, directory-growth
create, directory-growth rename and unlink of a file with an external extent
leaf. Every write/flush event is cut before and after under four persistence
patterns. Every operation allocation/snapshot failure and read failure is
injected; outputs, taint, zero-write planning failures and idempotence are
asserted. A device callback rejects metadata-home changes before checkpoint.

On each restart, public journal analysis/replay restores an exactly old or new
set of transaction metadata. Tests verify names, file bytes, dot/dotdot, parent
links and directory checksums. An independent allocation-set audit assigns
every data/extent-tree block to exactly one inode and compares the complete
allocated bitmap against fixed metadata, protected journal ranges and all
ordinary inode trees, detecting leaks, overlap and double frees. Directory
entries must point to allocated inodes of the declared type, and link counts
must equal two plus the actual immediate subdirectory count.

Each geometry also performs 64 create/write/cross-directory-rename/unlink/
mkdir/rmdir cycles, checking inode generation advance/reuse, map retirement,
resource ownership and cache coherence. Negative tests cover open-target
defense, inode exhaustion, excess reclamation credits, duplicate/missing/
invalid names, directory moves/replacement, nonempty/wrong-kind removal, raw
mode exclusion, and a valid-checksum parent with an incorrectly typed dot
entry. Synthetic resource/corruption fixtures are write-free rejection tests,
not Linux-format acceptance claims.

For all nine operation profiles, unmodified durable pending, checkpointed and
restarted images are copied for independent Linux journal replay and offline
`e2fsck -fn` (324 copies for a complete matrix). Linux also checks the resulting
names, empty-file/directory shape, inherited directory mode and exact bytes
after rename. Fixture versions, generation commands/logs, exact test argv,
source hashes and per-case logs are retained. Earlier interrupted workbench
runs are retained separately and are not final acceptance evidence.

Regression evidence in this session:

- `test-ext4-write-host`: six geometries, cache/faults/namespace/append and
  exact 1/16 MiB bytes plus Linux fsck PASS, `build/ext4-phase4/host-aq_h5q_u`.
- `test-ext4-transaction-host`: 12 cases PASS,
  `build/ext4-phase8-1/run-_ud8i_f2`.
- EXT4 read/format six sector/block adapters, USB mount-policy and BOT/SCSI
  host regressions including 16 KiB runs, PASS.
- Strict kernel build `make bin/fortress.elf`, PASS.
- `test-ext4-journal-file-host`: 12 cases, 20,256 crash cuts and 48 Linux
  oracle copies PASS, `build/ext4-phase8-2/run-jgbk5tlk`.
- `test-jbd2-replay-host`: recovery/bootstrap, failure/tear/revoke/escape and
  Linux replay regression PASS, `build/jbd2-replay/run-m7nsqrw7`.
- Production E4-A USB BIOS/UEFI at SMP=1/4: all 12 boots, plus four immutable
  RO/degraded cases, sync/shutdown, hashes and offline fsck PASS;
  `build/ext4-usb/run-6jvxw680`, final runner 4/4 cases PASS.

Concurrent TCP buffer/target edits appeared during the final kernel rebuild
and temporarily broke a networking constant reference. Those files and the
separate networking Makefile hunk were left untouched. After that work's
build completed, `make bin/fortress.elf` rechecked successfully as up to date.
The filesystem host tests are isolated from networking; the USB regression
used the successful build preceding those late TCP edits.
An independent protected-contract documentation commit also advanced local
`main` during verification; the Phase-8.3 files remain uncommitted and that
concurrent work is preserved.

## Limits and next part

No journaled production/VFS mount, Ring 3/AP namespace execution, open-unlink,
directory moves, replacement rename, large restartable cleanup, orphan chain,
clean-state transition or physical journal acceptance is claimed. Ordered
file overwrites retain 8.2's possible old/new data mixture on crash. Phase 8.4
owns truncate/orphans and restartable reclamation; 8.5–8.6 own mounted lifetime,
sync/shutdown and complete integration. Accepted Dell E4-A cache evidence and
default ext2/optional non-journaled image policy remain unchanged.

The Phase-8.3 changes remain uncommitted pending user review.
