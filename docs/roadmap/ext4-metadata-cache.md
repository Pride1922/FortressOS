# EXT4 bounded clean metadata cache — 2026-10-04

Implemented from the clean checkpoint with user approval. Scope is EXT4 and
its tests/docs; no USB, DMA, scheduler, network or flush-policy change.

Dell profiling before this change showed 1024 file-write callbacks, 4107
bitmap reads, 3078 inode-table reads, 1026 superblock reads and 1025 descriptor
reads during a 16 MiB download. Write callbacks occupied approximately 6.09 s;
checksum work occupied only 0.18 s within that total. Lock acquisition contention
was negligible. Dell physical acceptance passed on 2026-10-04 (user-confirmed).

## Ownership and coherence

Each mount has eight fixed 4096-byte slots in its heap object (32 KiB payload,
plus tags), guarded by the existing EXT4 lock. Enable only for production RW
mounts after geometry/profile admission. RO and exclusive allocation/recovery
workbenches remain uncached. Phase 8.1's exclusive transaction workbench tests
the same clean-cache publication helpers under single-caller ownership, without
VFS exposure. No production hot-path allocations or stack buffers.

Cache only the main superblock, group descriptors, block/inode bitmaps and
inode tables. Ordinary data, directories and extent traversal remain uncached.
A miss reads one complete filesystem block; sub-block requests copy the desired
slice. Full-block fill must succeed before the entry becomes valid. Replacement
uses vacant slots first, then a bounded round-robin cursor. No dirty cache or
writeback worker is introduced.

Checksum, structure, allocation-bit and inode validation remain at their
existing call sites and run on cache hits. Cached bytes are read snapshots;
they do not bypass validation. Exclusive device write ownership is required:
raw writes/media changes outside EXT4 require remount. The production driver
already serializes filesystem writers; future journal integration must ensure
replay/external write paths invalidate or disable this cache before use.

Every EXT4 write invalidates overlapping cached blocks before the first device
submission, including partial writes. After the entire commit and all existing
NEW/ALLOC/REFERENCE/free barriers succeed, descriptor state is updated and the
final committed metadata images may populate the clean cache. Staged or
intermediate images never populate it. Failure cannot reach that publication.

Any byte-reader I/O failure, commit taint, refresh/create taint, sync-flush failure
or shutdown-finish failure clears every entry and disables caching for that
mount. Subsequent permitted reads use the backing device. Tainted writes still
fail without new device work. Cache admission does not change dirty-volume
policy, clean-flag ordering, DMA quarantine or no-retry bulk-write behavior.

## Evidence

User-supplied same-stick/server baseline: FortressOS/Mint 1 MiB stream
approximately 1.45/0.129 s, 16 MiB stream 3.5/0.753 s, 1 MiB to disk
1.45/0.22 s, and 16 MiB to disk 10.15/0.70 s. FortressOS's 16 MiB disk
path adds approximately 6.65 s versus its stream; this includes storage's
effect on network scheduling, not isolated device time. Mint's difference
is within measurement noise, so no storage-overhead ratio can be inferred.
The 16 KiB USB runs are already implemented: Dell write commands fell from
approximately 8200 to 4109, while elapsed time remained 10.19 s. Cache
performance on the Dell is recorded below.

## Dell physical acceptance — PASS (2026-10-04)

Same Dell Latitude 5590, SanDisk stick, server and 16 MiB payload:

| Metric | Before | After |
| --- | --- | --- |
| Download | 10.15 s | 7.06 s (30% less elapsed time) |
| USB read commands during download | approximately 9241 | 7 |
| USB write commands | approximately 4110 | 4110 |
| Flush commands | approximately 3077 | 3077 |
| Cache hits/misses during download | not applicable | 9234 / 2 |

No reported I/O failures or clock anomalies. User confirmed SHA-256 after
download, after clean shutdown/reboot, and independently in Linux Mint:
`71b83c780eeb1a2665d5aabf71229490ae069450ef9b88ca91d1229fe17f45c6`.
Unmounted Mint `e2fsck -fn` completed all five passes, exit status 0:
`FORTRESS_E4_TEST: 18/512 files (0.0% non-contiguous), 4153/16384 blocks`.
No filesystem errors detected; this read-only check performed no repairs.

Remaining measured USB writes approximately 1.48 s and flushes approximately
1.97 s. Journal transaction batching is the planned next improvement; its
performance benefit remains unmeasured. The disk-versus-stream difference
includes scheduling interaction and is not isolated device time.

Host write ASan/UBSan six geometries PASS: cache hit slices, checksum validation
on hits, eight-slot eviction, partial-read failure, invalidation, accepted-prefix
write/flush faults and taint/cache-disable containment, namespace/append/gap,
1/16 MiB exact bytes and Linux fsck. Evidence build/ext4-phase4/host-6j8tf_74.
Allocation and maximum-map host suite PASS: build/ext4-phase3/run-m8d85jr8.
Read-only/format host sanitizer suites PASS, including malformed metadata,
checksum, I/O/OOM and zero-write rejection coverage.
Delivered-image BIOS/UEFI smoke 2/2 PASS: sync, later mutation, clean shutdown
and independent Linux bytes/fsck; build/ext4-usb/run-yjscv2_j.
Production USB BIOS/UEFI SMP=1/4 suite: 4/4 cases, 12 boots PASS, including
true AP concurrent append, namespace/persistence, hashes, Linux fsck and
immutable RO/degraded-GPT fixtures; build/ext4-usb/run-9ntodqft.
Build/image PASS without new warnings. The 1 MiB host write workload now records
0-1 metadata read runs, retaining 192 barriers and prior write-run counts.
This is a host workload observation, not a predicted Dell elapsed time.

Dell image bin/fortress-ext4-test.img timestamp 2026-10-04 11:25:02,
SHA-256 9f81ca555f6693e24683d46f1a7389ce2fd5c689d6920fb0f10b92310663cb12.
Use sync; dmesg -n 40; download /mnt http://192.168.0.153:8000/data-16m.bin;
sync; dmesg -n 40; sha256sum /mnt/data-16m.bin. Snapshot before hashing or
saving extra logs. Capture the download and record elapsed time. A new cache
hits/misses line is included in the existing on-demand sync snapshot.
User-confirmed physical cache acceptance: 16 MiB download 7.06 s, hashes pass
after reboot and independently on Mint; unmounted `e2fsck -fn` exits 0.
This acceptance does not cover journaled operation or crash recovery.
