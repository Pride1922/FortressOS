# FortressOS EXT4 implementation plan

Status: Phases 0-4 delivered (RO reads, bounded mutation engine and non-journaled RW VFS); Phases 5-10 pending. Production RW ext4 and hardware acceptance are not claimed. See [Phase-0 baseline](EXT4_PHASE0.md), [Phase-1 evidence](../roadmap/ext4-phase1.md), [Phase-2 evidence](../roadmap/ext4-phase2.md) [Phase-3 evidence](../roadmap/ext4-phase3.md) and [Phase-4 evidence](../roadmap/ext4-phase4.md).
Date: 2026-10-03. Based on the supplied notes, the current block/VFS/ext2/USB contracts, and Linux's ext4 format documentation.

## 1. Decision and corrections

Implement a separate ext4 driver in two milestones: **E4-A: bounded, non-journaled ext4 data storage**, then **E4-B: JBD2 ordered journaling and recovery**. Keep ext2 available throughout. Neither milestone means complete support for every ext4 feature.

The immediate motivation is confirmed physical evidence: a 1 MiB wget download stopped at 274,432 bytes on the Dell 5590. The image generator uses 1 KiB ext2 blocks; the write mapper implements 12 direct blocks and 256 single-indirect entries. Its limit is `(12 + block_size / 4) * block_size`, not a universal ext2 limit. With double-indirect support the 1 KiB case would reach 67,383,296 bytes (about 64.26 MiB). Extending ext2 remains a valid independent interim fix, not a prerequisite for this plan.

Corrections to the source proposal:

- Large files require a more capable block mapper, not necessarily ext4. Extents are this plan's chosen mapper.
- A package manager needs persistent storage plus durable application update protocols. It does not intrinsically require ext4. An installer likewise does not mathematically require journaling; FortressOS should require demonstrated crash recovery before advertising unattended persistent installation.
- Journaling protects metadata consistency within transaction boundaries. It does not make a multi-file package upgrade atomic, protect against arbitrary media corruption, or guarantee the last overwritten file contents survive power loss.
- Checksums are a correctness subsystem, not polish: allocation, inode, directory and extent changes must maintain them.
- HTree is optional for the initial controlled profile, but becomes a compatibility requirement if indexed directories are accepted. Never edit indexed directories as ordinary linear ones.
- A filesystem without a journal is still an ext4-format volume with a restricted feature set. It cannot be described as safe read/write support for arbitrary real ext4 filesystems.
- Committed but uncheckpointed journal transactions must be replayed, subject to revocations. Incomplete transactions are not applied. The supplied recovery wording reverses this.
- Read journal identity from the superblock. Inode 8 is conventional, not an unconditional driver constant.
- Large-file limits depend on format, block size, implementation bounds and VFS behavior. Do not promise 16 TiB just because extents exist.
- F2FS, JFFS2 and littlefs are not interchangeable journaling replacements; media model, ecosystem and recovery behavior need separate evaluation.
- The suggested durations are estimates without a measured basis. Estimate after each gate; do not commit to weeks or months before the design and failure matrix are sized.

Format references: [ext4 overview](https://www.kernel.org/doc/html/latest/filesystems/ext4/overview.html), [extent mapping](https://www.kernel.org/doc/html/latest/filesystems/ext4/ifork.html), [metadata checksums](https://www.kernel.org/doc/html/latest/filesystems/ext4/checksums.html), [JBD2](https://www.kernel.org/doc/html/latest/filesystems/ext4/journal.html). These describe the format; the phases below are FortressOS design proposals.

## 2. Contracts and integration boundaries

Read PROTECTED.md and AGENTS.md sections 4, 7.3–7.5 and 9 before implementation. Preserve existing ext2 behavior, PARTUUID selection, degraded-GPT read-only policy, internal physical NVMe exclusion, USB cache policy and DMA quarantine.

Proposed files: `src/fs/ext4.h`, `ext4.c`, `ext4_disk.h`, `ext4_extent.c`, `ext4_csum.c`, then `jbd2.h`/`jbd2.c`. Split further only when interfaces justify it. Decode bytes explicitly; do not cast disk buffers to native packed structs. Use freestanding scalar C CRC32C; existing IEEE CRC32 is not a substitute and SIMD is unavailable.

Expose mount-RO, explicit mount-RW, mid-session sync and shutdown-freeze APIs. Mount accepts a partition block device, never raw physical storage. Publish VFS callbacks only after all validation succeeds. Keep ext2 and ext4 dispatch unambiguous despite their common magic: feature classification must prevent retrying an unsupported ext4 volume through ext2.

Use a separate rank-1 filesystem lock with existing rank ordering. It cannot nest with ext2, process, scheduler or another rank-1 lock. Trace all block/USB calls transitively. No sleep, scheduler wait, IRQ enabling or context switch while holding the filesystem lock. Start with bounded synchronous operations and preallocated workspace; journal-space waits must happen unlocked and revalidate state. If acceptable transaction bounds cannot fit this model, produce an explicit synchronization redesign before implementing a worker. Preserve append EOF serialization and descriptor lifetime semantics.

Current `SYS_SYNC` reaches `usb_mount_sync()`, a device barrier; shutdown calls `ext2_sync_all()`. Ext4 integration must dispatch a filesystem commit/checkpoint operation before the barrier, and freeze/drain before shutdown marks clean. A device flush alone cannot commit a journal. Do not repurpose ext2's shutdown-only API for normal sync.

Every failed write/flush or uncertain metadata publication taints the mount and stops further mutations. Do not mark it clean on shutdown. Return EROFS for policy denial, EIO for taint/I/O, ENOSPC for allocation exhaustion, EFBIG for declared implementation limits, and ENOMEM for workspace exhaustion. Preserve successfully completed short-write prefixes only when their metadata state is valid.

## 3. Explicit feature profiles

Phase 0 must freeze a numeric feature-mask manifest and tested geometry limits. Proposed initial E4-A profile:

| Item | Initial policy |
| --- | --- |
| Filesystem blocks | 1, 2 and 4 KiB; geometry must agree with 512/4096-byte sector backend |
| Inodes | 256-byte profile, with validated extra-size fields |
| Mapping | Extents for accepted regular files and directories; bounded tree depth and work count |
| Descriptors | Conventional group layout, 32-byte descriptors; 64bit feature off |
| Metadata integrity | metadata_csum supported; checksum seed derived from UUID; csum_seed feature initially off |
| Allocation | Future writes use immediate allocation and verified initialized bitmaps; RO recognizes unused UNINIT groups without accessing undefined bitmap contents; no delayed allocation |
| Directories | Linear filetype records, checksum tails; dir_index off |
| Journal | has_journal off for E4-A writable mounts |
| Optional layout features | flex_bg, meta_bg, bigalloc, resize_inode and sparse_super2 off initially |
| Advanced features | Inline data, encryption, verity, casefold, quotas, EA inode, orphan_file, MMP, fast commit and external journal excluded |
| Inode capabilities | Regular files/directories initially; unsupported inode flags/types rejected before mutation |

Support sparse_super and large_file only with validated interpretation. Phase 0 must verify that this exact profile is creatable by the installed e2fsprogs version; adjust the manifest explicitly if dependencies differ. Never rely on `mke2fs -t ext4` defaults. Generate disposable fixtures with explicit feature selection and eager inode/journal initialization; record tool version, argv, `dumpe2fs -h`, UUID, geometry and digest.

Unknown incompat bits reject mount. Unknown ro_compat bits disallow RW; RO is permitted only where the implemented parser can safely interpret the remaining structures. Conservatively deny RW for unreviewed compat bits too. Unsupported per-inode features must not be mistaken for empty files. Volumes requiring recovery are rejected without writes until E4-B; a read-only request never silently replays onto the source device. Corruption yields a precise rejection, not automatic repair or ext2 fallback.

E4-B adds a **separately specified internal-journal profile**: ordered data, synchronous commit, revoke support, a chosen JBD2 checksum/tag variant and explicit 32/64-bit journal addressing policy. Async commit and fast commit remain excluded. Compatibility expansion is separate from E4-B completion.

## 4. On-disk implementation checklist

Offsets are relative to their containing structure; the filesystem superblock starts at byte 1024. Bounds and feature checks precede decoding optional fields.

| Structure | Fields to decode |
| --- | --- |
| Superblock | magic 0x38; block-size exponent 0x18; inode size 0x58; compat/incompat/ro_compat 0x5c/0x60/0x64; UUID 0x68; journal inode 0xe0; descriptor size 0xfe; block-count high 0x150; checksum seed 0x270; checksum 0x3fc |
| Group descriptor | bitmap addresses 0x00/0x04; inode-table address 0x08; flags 0x12; checksum 0x1e; high fields only for a supported extended descriptor profile |
| Inode | size low 0x04; flags 0x20; 60-byte i_block area 0x28; generation 0x64; size high 0x6c; checksum low 0x7c; extra-size 0x80 and checksum high 0x82 when present |
| Extent header | 12 bytes: magic 0x00 (0xf30a), entry count 0x02, capacity 0x04, depth 0x06, generation 0x08 |
| Extent leaf | 12 bytes: logical block 0x00, encoded length 0x04, physical high 0x06, physical low 0x08 |
| Extent index | 12 bytes: logical key 0x00, child low 0x04, child high 0x08; validate reserved bytes |
| JBD2 common header | 12 bytes, big-endian: magic 0x00 (0xc03b3998), block type 0x04, sequence 0x08 |

Verify offsets against [superblock](https://www.kernel.org/doc/html/latest/filesystems/ext4/super.html), [group descriptors](https://www.kernel.org/doc/html/latest/filesystems/ext4/group_descr.html), [inodes](https://www.kernel.org/doc/html/latest/filesystems/ext4/inodes.html) and [directory entries](https://www.kernel.org/doc/html/latest/filesystems/ext4/directory.html). Ext4 fields are little-endian; JBD2 fields are big-endian. Root extent capacity is four entries. Unwritten extent length encoding has boundary cases, including initialized length 32768: never mask a high bit without interpreting those rules.

Checksum definitions must specify seed, excluded checksum fields, inode number/generation inclusion, truncation and tail placement for each structure. Verify independent Linux-produced vectors for superblock, descriptors, bitmaps, inodes, external extent nodes and directory blocks. Recompute affected checksums in the same metadata operation; do not update only extent checksums.

## 5. Phase-by-phase delivery

### Phase 0 — Design freeze, inventory and fixtures

Deliver numeric feature masks, filesystem/file/tree/transaction/cache bounds, lock call graph, supported inode semantics, mount error table, mkfs recipe, test fixture manifest and a write-ordering design. Inventory VFS open-unlink lifetime, 64-bit size/offset arithmetic, executable loader behavior and current sync paths. Define proposed API headers before coding.

Gate: every accepted feature has a planned parser, mutation and checksum owner; unsupported cases have rejection-fixture recipes. Deliver API declarations, lock/lifetime inventory and reproducible Linux-validated profile fixtures. Instrumented zero-write admission checks and durability fault tests require actual driver code in Phases 1/3; do not claim them from fixture generation. No installer, root switch or normal image-format change. Delivered baseline: [EXT4_PHASE0.md](EXT4_PHASE0.md), including concrete crash-injection points and API/callback signatures.

### Phase 1 — Geometry, admission and read-only metadata

Implement mount validation, groups, inode lookup, linear readdir and checksum codecs. Validate counts, offsets, descriptor spans, reserved metadata ownership and partition bounds with overflow-safe arithmetic. Detect duplicate/overlapping metadata reservations. No allocation or write callbacks yet.

Gate: proposed `test-ext4-format-host` passes ASan/UBSan with independent vectors, malformed bounds and bit-flip fixtures. Every rejected/RO fixture records zero writes and zero flush-triggered metadata changes. OOM leaves no published mount or leaked resources.

### Phase 2 — Extent read path

Walk root and external index/leaf blocks with bounded depth and traversal, increasing keys, nonoverlapping logical ranges, physical bounds and cycle/alias checks. Holes and unwritten extents return zeros. Support read ranges across EOF and extent/block boundaries. Mixed legacy-mapped inodes are explicitly rejected until a mapper is added.

Gate: proposed `test-ext4-read` passes BIOS/UEFI disposable fixtures with Linux-built sparse, fragmented, multi-level and greater-than-268-KiB files. Compare full bytes and independent digests; include logical offsets above 4 GiB in sparse fixtures. Extent parsing host tests reject hostile trees without out-of-range I/O.

### Phase 3 — Allocation and extent mutation engine

Implement block/inode allocation, reservation exclusion, counters/checksums, zero-before-visible initialized blocks, contiguous growth, merges, leaf splits, root promotion and index propagation. Implement deterministic ENOSPC/EFBIG bounds. Plan deletion and subtree reclamation now, rather than after allocation.

Gate: actual mutation engine runs against fake block storage under ASan/UBSan. Force fragmented allocation and every tree transition; inject OOM/sector failure at each publication boundary. Audit allocation sets and reserved ranges, not only totals. No production RW mounting yet.

### Phase 4 — Non-journaled VFS write and namespace operations

Add read/write/create, directory growth, mkdir, unlink/rmdir, supported same-filesystem rename and truncate-to-zero through the existing VFS contract. Keep append atomic. Define rename replacement behavior and open-unlink handling explicitly; either implement safe deferred reclamation or reject unsupported active-file deletion. Handle EOF-gap zeroing and unwritten conversion without exposing old media data.

Use a documented persistence order: make initialized allocations durable before inode/tree links, and remove durable references before freeing blocks. This reduces risk but is not transactional crash consistency. Partial namespace updates on I/O failure must taint the volume. Bound operations exceeding a single workspace; do not publish half an extent split.

Gate: proposed `test-ext4-write` passes BIOS/UEFI three-boot persistence and Linux interoperability. Download/save/reopen/hash 1 MiB and 16 MiB files; test fragmented growth, overwrite, append, truncate, directory growth, rename and reclamation. Linux `e2fsck -fn` must be clean after orderly shutdown. Unclean E4-A volumes reject RW until host repair; crash tests document possible inconsistency rather than asserting journaling guarantees.

### Phase 5 — E4-A integration and physical acceptance

Wire feature-based mount dispatch into existing selected USB partition policy. Add ext4 sync/freeze dispatch without changing ext2 APIs. Keep the default production image ext2 until compatibility and regression gates pass; create a separately labelled opt-in ext4 image. Do not convert an existing volume in place or reflash over user data without a migration procedure.

Gate: ext2/storage/USB regressions, mount eligibility tests and SMP append pass at the supported CPU counts. Dell acceptance uses a designated test USB, records durability class, verifies 1/16 MiB downloads and overwrite/delete/reboot persistence, then independently audits Linux hashes and offline fsck. BIOS/UEFI QEMU NVMe and USB evidence are distinct from physical evidence.

**E4-A complete:** controlled non-journaled profile supports large files. Package staging experiments may begin, but no crash-safe installer/rootfs claim.

### Phase 6 — JBD2 recovery reader before writer

Implement journal identity/geometry/feature validation, descriptor/tag decoding, escape handling, sequence wrap, checksum validation and revoke processing. Analyze recovery entirely before modifying the target. Replay complete committed transactions that need checkpointing; disregard incomplete tails. Reject corrupt or unsupported journals without guessing transaction boundaries.

Gate: proposed `test-jbd2-replay-host` uses independently generated Linux journals and deliberate corruptions. Revoked block reuse, circular wrap, truncated descriptors, wrong UUID and replay interruption are covered. Repeated recovery is idempotent. A read-only source is never changed; recovery requires explicit writable admission or runs on a disposable copy.

### Phase 7 — Ordered transaction writer and checkpoints

Define transaction credit accounting and bounded block images; preflight space for descriptors, metadata, revokes and commit. One serialized transaction initially. Write and flush newly exposed file data before metadata commit; persist journal contents before its commit record; make commit durable before home checkpoint. Advance reusable journal space only after checkpoint barriers complete. Never mutate metadata home blocks before durable commit.

Track transaction state and failure semantics. Uncertain commit means taint/abort, not fictitious rollback. Handle sequence wrap and journal-full admission without sleeping under a spinlock. Existing overwritten data is not promised atomic by ordered metadata journaling.

Gate: proposed `test-jbd2-write-host` checks actual ordered writes/barriers and interrupted commits against an independent durability model; Linux can replay FortressOS-written transactions. FortressOS can replay Linux-written transactions for the declared profile.

### Phase 8 — Journal all mutations, orphan recovery and sync

Route every allocation, free, bitmap/counter/checksum, inode/tree and directory update through transactions. Include rename and truncate/delete restartability. Use a supported traditional orphan mechanism where needed; orphan_file stays excluded. Do not reuse freed blocks while older replay can overwrite them; implement revokes correctly.

Connect `SYS_SYNC` to commit/checkpoint/barrier; define durability on successful return. Shutdown freezes new work, drains transactions, flushes, then records clean state. Read-write mount performs validated recovery before VFS publication. Mid-session sync does not freeze the filesystem.

Gate: transaction coverage audit finds no direct metadata write bypass. Fault injection covers each operation, orphan cleanup, open-unlink, shared handles and append. Recovery restores consistent ownership and namespace within declared operation boundaries.

### Phase 9 — Crash campaign and E4-B acceptance

Enumerate every write/flush cut point in representative transactions. The fake device separates volatile cache from stable media; model cache loss, reordered unflushed writes, torn sectors, failed flushes and disconnects. Corrupt durable journal content must be detected; do not promise reconstruction of arbitrary corruption.

For supported crash scenarios: crash, recover using FortressOS, check independent namespace/data oracle and run offline `e2fsck -fn`. Reverse interoperability: replay FortressOS images on Linux copies, and Linux images on FortressOS. Model journal-full and crash-during-recovery/checkpoint cases. Extend BIOS/UEFI QEMU power-cut runs and preserve exact argv, image before/after, logs and seed. Deliberate physical power-cut tests require a separate sacrificial-media procedure; clean physical reboots alone prove no crash-recovery property.

Gate: no unexplained fsck repairs, leaked allocations, double allocation, stale-data exposure or silent checksum acceptance in supported cases. Durability claims require a device with verified write-through or working synchronous flush. ASSUMED_WRITE_THROUGH retains its existing policy but cannot establish a power-loss guarantee. Record this distinction without silently changing USB policy.

**E4-B complete:** bounded journaling profile accepted; not universal ext4 support.

### Phase 10 — Persistent root, package manager and installer handoff

Separate design/approval: root mount and `/paradise` layout, boot failure fallback, ELF/backing lifetime, symlinks and permissions required by packages, staged package extraction, atomic replacement and package-database recovery. Add fsync/directory durability APIs if required; global sync is not a substitute for an unspecified update protocol. Multi-file upgrades need an application recovery record even with journaling.

Installer partition selection and real-disk writes need explicit authorization and preserve the current internal-NVMe exclusion until its separate contract change is approved. No installer should automatically infer its target from whichever block device appears first.

Gate: independent boot/install/package update acceptance, including power loss at application update boundaries. Filesystem completion alone does not complete these capabilities.

## 6. Tests, rollout and claims

Targets through Phase 3 exist and their results are recorded in the phase evidence; later test names remain proposed. Host tests exercise actual code with clearly labelled adapters. Guest runners use disposable images, preflight final argv, pair OVMF code/vars, bound waits and reap QEMU. Include 512/4096-sector geometry cases, 1/2/4-KiB filesystem blocks, fragmentation, hostile feature masks, allocation failure, short I/O and taint behavior.

For each phase, retain the exact commands, fixture feature manifest, tool versions, output and limitations in `docs/roadmap/ext4-phaseN.md`. Update subsystem status only after evidence exists. Run relevant existing `test-ext2`, storage, USB durability/persistence and SMP append gates when shared integration changes. Runner prompt recognition must support the new cwd prompt before interpreting a timeout as a filesystem failure.

Linux validation: inspect via `dumpe2fs`/`debugfs`, run `e2fsck -fn` only on unmounted images, and compare independent hashes. Recovery/repair experiments operate on copies and record every change. Never use an automatic fsck repair as proof the original output was consistent.

Compatibility extensions after E4-B: HTree, flex_bg/64bit, additional inode mappings/types and xattrs are separate phases with feature-specific gates. Performance caching and delayed allocation come after correctness; no full-ext4 label or terabyte capacity promise without matching validation.

Next implementation unit: **Phase 5 E4-A integration and physical acceptance**. Phase 4 bounded RW VFS passes host and BIOS/UEFI persistence gates; production mounting remains disabled. Kernel fixtures record raw TSC callback costs; physical latency remains to be measured before accepting RW integration. This plan authorizes no format conversion, root switch, real-disk installation or protected synchronization change.
