# EXT4 write I/O reduction — 2026-10-04

User-approved changes preserve E4-A non-journaled durability barriers:

1. A write replacing every byte of a filesystem block stages a fresh image
   without reading its old contents, including initialized storage. Partial
   writes retain old-data reads; new/unwritten blocks retain zero-fill handling.
   This does not bypass metadata validation or device-sector read/modify/write
   when a filesystem block is smaller than the physical sector.
2. Consecutive staged images with the same role and adjacent disk block numbers
   are copied into a bounded 4096-byte engine workspace and written as one run.
   Staging order and NEW/ALLOC/REFERENCE barriers remain. Failed runs are not
   retried; the existing taint/quarantine path retains uncertain accepted writes.

The block API and USB DMA contract cap runs at 4096 bytes. Therefore batching
helps 1/2 KiB filesystem blocks; the Dell 4 KiB profile cannot combine multiple
blocks without a separately reviewed transport change. No large-run performance
claim is made. The engine gains 4096 bytes of heap workspace, not stack usage.

Host tests watch the existing file's first initialized data block: full aligned
overwrite reads no old data when filesystem block >= sector; partial overwrite
does read and preserves surrounding bytes. Exact file reads validate both.
1/2 KiB with 512-byte sector tests assert bounded transport-call reduction.
Existing six-geometry VFS tests, every fault boundary, accepted-prefix failures,
allocation/extent tests, Linux exact bytes and e2fsck remain the integrity gates.

Physical file-download measurement remains pending. Retest the same existing
16 MiB file with before/after USB snapshots and a capture, then SHA-256 and sync.
Do not interpret absence of large-run batching on Dell as failure of the bounded
implementation. Truncate/new-file allocation still requires metadata I/O;
complete overwrite optimization only applies when each block is fully replaced.

Verification: make test-ext4-write-host PASS all six geometries (ASan/UBSan,
fault matrices, namespace/append/gap, 1/16 MiB, Linux bytes/fsck), evidence
build/ext4-phase4/host-klnqt5n7. make test-ext4-alloc-host PASS six geometries
and maximum maps, evidence build/ext4-phase3/run-kgni_f7x. Strict build/image PASS.
Image bin/fortress-ext4-test.img local timestamp 2026-10-04 09:41:05,
SHA-256 0c656bc319da03b4fc4f215e8e4c2bbf5745848c8fc77242d87c4386591e0424.
New/truncated downloads already use the new-block read skip, so these changes
alone may provide little Dell speed improvement. They do not remove metadata
reads or increase the transport run limit.
Final-image BIOS/UEFI USB smoke PASS 2/2: writes, sync, clean shutdown and
Linux bytes/fsck, evidence build/ext4-usb/run-3ciidhy1.

## Follow-up: bounded 16 KiB USB writes

The separately approved transport change now raises USB runs to 16 KiB only
when its larger DMA region is successfully allocated and validated. Legacy
backends and the USB allocation fallback retain 4 KiB. GPT partitions inherit
the parent limit; the block API validates the entire range before dispatch.

Seven contiguous PMM pages contain a 16 KiB aligned usable region. Alignment
prevents a normal TRB from crossing a 64 KiB boundary. The layout rejects
address overflow and tests the full usable range against xHCI AC64 support.
The original seven-page base and count are retained in controller state.
Pre-exposure failure and proven-halt cleanup free that original allocation;
existing uncertain-ownership paths free none of it, including padding. This
cleanup coverage was inspected in source; host layout tests are not a physical
DMA or controller-halt verification claim.

BOT preserves CBW/data/CSW sequencing, completion budgets and recovery rules.
A failed data payload is never replayed. The EXT4 heap workspace is now 16 KiB;
only adjacent staged blocks of the same role are merged, within device limits.
All flush barriers remain. EXT4 read batching remains capped at 4 KiB and its
extension is deferred until Dell measurement.

Verification: BOT ASan/UBSan passes exact 16 KiB transfer/readback for 512/4096
sectors, bounce guards, capacity/boundary rejection before submission, failed
OUT payload without replay, and layout offsets/overflow/AC64 limits. EXT4 host
passes all six geometries, accepted-prefix failure without sector retry,
taint/failure matrices, Linux bytes/fsck (build/ext4-phase4/host-jdqshbta and
subsequent rerun). Allocation host and maximum-map tests pass
(build/ext4-phase3/run-d8xpdx_4). USB discovery/block BIOS/UEFI present/absent
and mount/GPT host policy tests pass. Build/image pass without new warnings.

The 4 KiB filesystem/512-byte sector host workload records 320 write runs and
192 barriers per MiB. These are mock counts, not predicted Dell results. Actual
Dell command counts, elapsed time, hash and post-shutdown fsck remain pending.

Final host rerun: build/ext4-phase4/host-f4i_kdvt. Exact delivered-image
BIOS/UEFI smoke PASS 2/2 (build/ext4-usb/run-0lebtb3a). Dell image:
bin/fortress-ext4-test.img, local timestamp 2026-10-04 10:13:17,
SHA-256 c8ffa3e312b711de7a26e8814a9c84f1d19ac95291a3b1b232db4e99f589eff0.
Boot reports `[USB 9G.2] Max run: 16384 bytes` when the larger region is active.
Production USB suite PASS 4/4 BIOS/UEFI x SMP=1/4, twelve boots, true AP append,
read-only/degraded GPT exclusions, hashes, persistence and Linux audits:
build/ext4-usb/run-4qoejyhh. No physical speed claim.
