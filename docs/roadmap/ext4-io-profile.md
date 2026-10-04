# EXT4 read and lock-time profiling — 2026-10-04

## Dell 16 KiB run measurement

User supplied before/after screenshots and tcpproblem.pcapng, retained as
build/tcp-dell-16k-runs.pcapng. Image PARTUUID
1500CC7A-BEDB-4E3C-A45B-F2364F605CEC matches the delivered 16 KiB image.
16 MiB file download: 10.19 s, SHA-256 matches
71b83c780eeb1a2665d5aabf71229490ae069450ef9b88ca91d1229fe17f45c6.
Capture spans 10.21198 s from SYN through final ACK, one HTTP flow, payload
16777423 bytes including 207 response-header bytes. Server capture aggregates
are accounted for; these are not on-wire oversized TCP segments.

Delta USB counts: read 9239, write 4109, flush 3078. Writes halved relative to
8200 previously; bytes remain 33584128. Download elapsed time did not improve
(previous 10.15 s). Using boot-calibrated invariant TSC 1897506174 Hz, delta
read time is about 2.981 s, write 0.852 s, flush 1.903 s. Snapshot precedes
SHA-256, so read counts exclude hash reads. Do not infer filesystem CPU cost
or scheduler causation merely by subtracting these totals from elapsed time.

## Diagnostic build

No caching, read batching, durability, IRQ policy or scheduling changes.
- Read counters classify main superblock, group descriptor table, bitmaps and
  inode tables by validated filesystem geometry. Explicit callers classify
  extent traversal, directories and ordinary file data. Remaining reads are
  labelled other; staged extent-node reads may be in this residual category.
- Calls count logical byte-read attempts; requests count block API submissions,
  not individual sectors or BOT commands. Bytes are requested logical bytes.
  Failures and backward-clock anomalies are recorded. Write RMW sector reads
  outside e4_bytes are not included; USB counters retain those operations.
- Raw fenced TSC cycles measure read operations, checksum work and write phases.
  Write total/max run from lock acquisition to immediately before unlock;
  lock-wait cycles separately measure acquisition. Preparation, commit and
  refresh are completed-phase measurements; failed early phases need not appear
  individually, but write total/failure counts still account for the attempt.
- Checksum and read timings are included in write phase totals. Do not add
  nested counters together. Convert cycles only using the admitted invariant
  clock, and reject anomalous measurements.
- Counters are cumulative across EXT4 instances for the boot. Compare snapshots
  before/after the same workload. Formatting locks EXT4 briefly, performs no
  media I/O and releases the lock before appending to dmesg.
- SYS_SYNC appends twelve EXT4 PERF lines after the existing USB snapshot.
  No per-packet/write console trace is emitted. Existing NET POLL snapshots
  provide the network-worker yield and tick-wait measurements for comparison.

Host sanitizer/VFS tests pass all six geometries, fault/taint containment,
Linux bytes/fsck and bounded snapshot/no-I/O checks: build/ext4-phase4/host-vk2ltoxe.
Strict build/image pass without new warnings. Physical profiling remains pending.
Image bin/fortress-ext4-test.img timestamp 2026-10-04 10:40:54,
SHA-256 81a88d942f157217bc1d577d1c1015997969e70223e8e0720c6d1690e9106ab1.

Dell sequence: sync; dmesg -n 30; download /mnt
http://192.168.0.153:8000/data-16m.bin; sync; dmesg -n 30;
sha256sum /mnt/data-16m.bin. Capture the download and record elapsed time.
Take the after snapshot before hashing or writing extra log files.

Exact-image BIOS/UEFI smoke PASS 2/2, including live profile fields, writes,
SYS_SYNC, later mutation, clean shutdown and Linux bytes/fsck:
build/ext4-usb/run-e4s82tfs.
