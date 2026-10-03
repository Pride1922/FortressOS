# EXT4 Phase 6 — bounded JBD2 recovery reader

Implemented 2026-10-03. This is an exclusive recovery workbench, before the
Phase-7 writer and Phase-8 mount integration. Production RO/RW mounts still
reject journals. Phase-5 Dell latency/persistence acceptance remains pending.

## Interface and recovery order

`ext4_journal_analyze(partition, &plan, &report)` discovers the internal journal
inode from the superblock, validates its checksum and complete extent map,
and calls `jbd2_analyze`. Analysis performs no writes, publishes no VFS nodes,
and snapshots all committed payloads before any replay. The caller must own
the stable partition exclusively until `jbd2_release` and must separately
admit writes using the existing device/durability policy.

`jbd2_replay(plan, writable_admitted)` returns EROFS without admission. It
checkpoints committed images in log order, excludes images covered by a later
or same-transaction revoke, flushes home blocks, then clears the journal tail
and flushes the journal superblock. Transaction ordinals handle sequence wrap
and revoked-block reuse. Failure poisons the plan; restart requires fresh
analysis. Reusing a successfully applied plan writes nothing.

This API does not clear EXT4 RECOVER, mark the filesystem clean, process orphan
chains, enable journaled VFS writes, or change USB admission. Those transitions
belong to later phases. No boot command-line changes or production recovery
calls were added.

## Supported profile and rejection

- E4-A geometry/checksums/extents plus HAS_JOURNAL and optional RECOVER; internal
  journal only, no orphan chain or additional filesystem features.
- JBD2 v2 superblock with CSUM_V3 and REVOKE, CRC32C, 32-bit home blocks.
  Unsupported checksum generations, async/fast commit, 64-bit tags and unknown
  features reject. UUIDs, descriptor/revoke/commit and payload checksums,
  escaped payloads, circular boundaries and sequences are validated.
- 1/2/4 KiB filesystem blocks and 512/4096-byte sector adapters. Sub-sector
  writes preserve neighboring filesystem blocks.
- At most 32,768 journal blocks, 128 committed transactions, 1,024 payload
  images and revokes total; 64 images/revokes and 128 log blocks per transaction.
  Unsupported sizes return EFBIG before writes.
- Journal data/extent-tree self-writes, journal inode changes, geometry/identity
  changes, out-of-range home blocks and invalid group-descriptor images reject.

Bootstrap deliberately does not require consistent allocation bitmaps/free
totals: an interrupted checkpoint can leave those inconsistent. It still
validates journal identity, reserved ranges and descriptor checksums. A partial
primary-superblock checkpoint may use a checksummed group-1 backup only when
immutable identity fields agree. This read-only fallback is unavailable to
ordinary mount admission. Uncorroborated identity or damaged journal metadata
fails closed; it is not a filesystem repair facility.

An incomplete, validly parsed tail is discarded; checksum corruption is a
rejection, even in a possible tail. Future sequence gaps reject rather than
guessing that committed transactions can be skipped. Old records may remain
behind the live circular tail.

## Host power-loss gate

`make test-jbd2-replay-host` compiles the actual filesystem bootstrap and JBD2
reader under ASan/UBSan. Fixtures are fresh regular files beneath
`build/jbd2-replay`; no mounts, raw devices or user data disks are accepted.

The disk adapter keeps separate volatile/stable bytes. Every home-sector write,
home flush, journal-superblock sector write and final flush is numbered in the
retained baseline log. Each event is cut before and after under four persistence
patterns: cached, all writes durable, alternating durable sectors and their
inverse. Restart drops volatile bytes and all runtime plans. Successful flush
persists accepted writes. Selected sector-prefix tears additionally require
successful fresh recovery or a zero-write corruption rejection.

The matrix covers both e2fsprogs-produced and independent circular/sequence-wrap
journals at all six block/sector geometries (12 cases). Each journal contains
three committed transactions, five images, a revoke and subsequent block reuse;
the producer fixture also has a valid uncommitted tail. Payloads restore a
superblock checkpoint, a corrupted home inode and exact escaped file bytes.
Every analysis-read failure and allocation failure is injected. RO analysis,
ordinary journal-mount rejection, explicit admission, poisoned-plan rejection,
fresh recovery and idempotence are asserted. Linux independently replays copies;
recovered copies pass `e2fsck -fn` and exact file-byte checks.

Sixteen corruption/unsupported variants per filesystem block size (48 total)
exercise UUIDs, feature/geometry restrictions, descriptor/payload/commit CRCs,
tag UUID/flags/high words, missing LAST, journal aliases, out-of-range homes,
sequence gaps, external journals and orphan chains. They must reject with no
writes or flushes.

### Fixture provenance limitation

The installed e2fsprogs 1.47 experimental debugfs journal writer emits zero tag
UUID slots due to typed-pointer arithmetic in its descriptor writer. Raw
producer images and exact generation commands are retained. The generator
normalizes only those zero slots to the validated journal UUID and independently
reseals descriptor CRCs; payload/revoke/commit bytes remain intact. The strict
reader still rejects wrong UUIDs. Linux replay is the independent oracle; its
filesystem housekeeping fields and RECOVER clearing are distinguished from
the committed superblock payload. This is not evidence from a native Linux
kernel power-cut journal, nor a QEMU/physical crash-consistency claim.

Format references: [Linux journal format](https://www.kernel.org/doc/html/latest/filesystems/ext4/journal.html),
[Linux recovery implementation](https://github.com/torvalds/linux/blob/master/fs/jbd2/recovery.c),
[e2fsprogs debugfs journal writer](https://github.com/tytso/e2fsprogs/blob/master/debugfs/do_journal.c).

## Verification

Commands actually run and passed:

| Command | Result / retained evidence |
| --- | --- |
| `make test-jbd2-replay-host` | ASan/UBSan, 12/12 cases and 48 zero-write rejections; `build/jbd2-replay/run-tyoiqdll` |
| `make test-ext4-read-host` | Six block/sector geometries; `build/ext4-phase0/fixtures-z_b17qbh` |
| `make test-ext4-alloc-host` | Allocation/maximum-map cases, failures and Linux audits; `build/ext4-phase3/run-bmy6881u` |
| `make test-ext4-write-host` | Six VFS geometries, fault boundaries, exact 1/16 MiB and Linux audits; `build/ext4-phase4/host-r2loa2no` |
| `make test-ext4-read` | BIOS/UEFI 6/6, immutable fixtures; `build/ext4-read/run-uzzlr4xu` |
| `make` | Strict compilation/link and default ext2 image verification; no new warnings |

The recovery gate contains 1,552 atomic-sector cut/restart cases (each baseline
event under eight before/after/persistence combinations), plus sector tears,
read/allocation failure sweeps and idempotence. Failed exploratory fixture runs
remain retained but are not counted as passes.

Phase 7 implements transaction creation, durable commit and checkpoints;
Phase 8 connects recovery to mount/clean-state/orphan policy. Physical Phase-5
acceptance can proceed independently when the Dell is available.
