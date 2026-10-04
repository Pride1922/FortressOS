# EXT4 Phase 8.2 — file writes and allocation

Implemented from clean local `main` checkpoint `321ed6f` (Phase 8.1),
2026-10-04. Production journaled RW remains disabled. This is an explicit,
exclusive disposable-media workbench for existing singly linked regular files.

## Implementation and ownership

`ext4_engine_open_journal_files` requires explicit durability admission, an
empty supported internal journal, a pre-existing RECOVER bit, a valid primary
superblock, checksummed allocation bitmaps and consistent free counters.
Journal data and external journal extent nodes have immutable protected ranges;
ordinary file traversal cannot replace their identity or make them allocatable.
Recovery-bootstrap tolerance is disabled before semantic file operations.

`ext4_engine_file_write` uses the same write-free planner as E4-A. Allocation,
unwritten conversion, sparse extension, extent rebuilding, inode size/block
counts, bitmap initialization/freeing, group/superblock counters and checksums
are sealed metadata snapshots. Data images are explicitly classified separately
from new extent metadata, although both retain E4-A's existing NEW role.

Every exposed data image passes the ordered-data barrier before journaled
references commit. Partial initialized writes preserve untouched bytes; new
and unwritten blocks are zeroed before exposure. Initialized preallocation
between EOF and a distant write is cleared with bounded credits. Extent roots,
new leaves/indexes and freed old tree nodes publish in one transaction; old
tree nodes are revoked and the log is checkpointed/retired before reuse.

The caller exclusively serializes every operation and close, in unlocked
thread context. Append reads authoritative EOF. Shared offset pointers advance
only after commit, checkpoint and tail barriers; independent pointers retain
their own positions. This adapter is not a journaled VFS shared-fd or AP test.
No protected lock/scheduler, durability policy or DMA ownership contract changes.

Each call accepts at most 32 KiB and stages at most 64 combined data/metadata
images plus 64 old-tree revokes. Planning, credits and writer snapshot errors
occur before any write or flush. Offsets remain unchanged on failure. Uncertain
I/O permanently taints the engine and disables its metadata cache; abort never
clears taint. Metadata cache publication follows successful checkpoint/tail
barriers, and logical mappings are invalidated after each operation.

Raw image transactions are disabled on file contexts. Legacy grow/trim/inode
reserve/release/finish remain forbidden. The workbench never enters E4-A's
direct metadata-home publication path. E4-A retains its existing lock,
publication ordering, cache, bounded USB runs and mount policy.

## Verification

Focused target: `make test-ext4-journal-file-host`, actual EXT4/JBD2 code under
ASan/UBSan and `-Werror`, disposable regular images only. Evidence:
`build/ext4-phase8-2/run-vgutiquq`: 12/12 cases, 20,256 crash cuts and
48 Linux replay/fsck/byte/mode oracle copies PASS.

The matrix covers 1/2/4 KiB blocks, 512/4096-byte sector adapters and normal/
wrapped journal placement/sequence. Four profiles exercise sparse extension,
append, cross-block overwrite and conversion of a Linux-generated depth-2
unwritten tree. Free/unwritten storage contains hostile A5 bytes. Tests also
exercise root promotion, shared/independent offset adapters, ENOSPC and image
credit exhaustion with zero device events, admission/read/allocation unwind,
every operation read/snapshot failure, every write/flush cut before/after the
event under four persistence patterns, taint, replay and idempotence.

Each restart checks all transaction metadata as exactly old or new, validates
allocation ownership/extent checksums and reads the complete file through the
actual reader. A device callback asserts that metadata homes cannot change
before checkpoint. Linux independently replays unmodified committed journals,
runs fsck and checks bytes/mode on committed, checkpointed, restarted and
depth-2 conversion copies (48 copies for the complete matrix).

Regression evidence from this implementation session:

- E4-A write/cache host six geometries, exact 1/16 MiB bytes, faults and Linux
  fsck: `build/ext4-phase4/host-6k1bvd83`, PASS.
- Phase 8.1 transaction host: `build/ext4-phase8-1/run-y357mg7n`, PASS.
- Phase 6 recovery host: `build/jbd2-replay/run-x1q8f_ho`, PASS.
- Phase 7 writer host: `build/jbd2-write/run-zc5s65xn`, 12 cases,
  3,568 crash cuts and 36 Linux oracle copies, PASS.
- Phase 3 allocation host: `build/ext4-phase3/run-1ng9vhtz`, PASS.
- Production E4-A USB BIOS/UEFI at SMP=1/4: 4 cases, 12 boots plus immutable
  RO/degraded cases, hashes and offline fsck, PASS:
  `build/ext4-usb/run-ot1q_yiy`.
- EXT4 read/format host six adapters, ext2 host, USB mount-policy and BOT/SCSI
  host targets, including 16 KiB runs and failed transport ownership, PASS.
- Strict kernel build `make bin/fortress.elf`, PASS.

Earlier failed harness attempts are retained. One assumed fixture permissions
0644 although its source was 0777; the oracle now verifies preservation of
the actual source mode. Another exceeded the old 120-second case limit;
file fault cases now have a finite 600-second limit. Neither is acceptance
evidence for the completed matrix.

## Remaining limits

Ordered overwrites may leave old/new data mixtures after a crash; atomicity
applies to metadata and newly exposed references. No namespace operations,
truncate/unlink/orphans, production journal mount/recovery, clean shutdown,
Ring 3 shared-fd/AP journal path or physical journal durability is claimed.
These remain parts 8.3–8.6. The accepted Dell E4-A cache evidence is separate
and unchanged. No disk conversion, commit or push is included.
