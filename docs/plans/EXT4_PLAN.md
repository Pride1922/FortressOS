# FortressOS EXT4 implementation plan

Status: Phases 0-4 delivered (RO reads, bounded mutation engine and non-journaled RW VFS); Phase 5 E4-A production USB integration COMPLETE: automated gates and Dell 5590 physical acceptance PASS (2026-10-03); Phases 6-7 recovery/writer workbenches delivered and host-verified; Phase 8 bounded disposable journal integration COMPLETE (2026-10-04); Phases 9-10 pending. Production journaled RW remains disabled. Physical evidence is limited to the reported Phase-5 checks; journaling hardware acceptance is not claimed. See [Phase-0 baseline](EXT4_PHASE0.md), [Phase-1 evidence](../roadmap/ext4-phase1.md), [Phase-2 evidence](../roadmap/ext4-phase2.md), [Phase-3 evidence](../roadmap/ext4-phase3.md), [Phase-4 evidence](../roadmap/ext4-phase4.md), [Phase-5 handoff](../roadmap/ext4-phase5.md), [Phase-6 evidence](../roadmap/ext4-phase6.md) and [Phase-7 evidence](../roadmap/ext4-phase7.md).
Date: 2026-10-03. Based on the supplied notes, the current block/VFS/ext2/USB contracts, and Linux's ext4 format documentation.
Phase 8.6 update (2026-10-04): transaction, file-write/allocation, bounded
namespace, restartable orphan, mounted lifecycle and combined integration
gates delivered on explicit disposable fixtures. Production journaled RW
remains disabled pending Phase-9 acceptance and rollout. See [8.2 evidence and limits](../roadmap/ext4-phase8-2.md)
and [8.3 operation boundaries/evidence](../roadmap/ext4-phase8-3.md), plus
[8.4 recovery, lifetime and verification boundaries](../roadmap/ext4-phase8-4.md).

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

Gate implemented: `test-jbd2-replay-host` passes 12 geometry/source cases and 48 zero-write rejections, revokes/reuse, escapes, circular/sequence wrap, incomplete tails, replay interruption and idempotence. The experimental debugfs producer's zero tag UUIDs are normalized and descriptor CRCs independently resealed; raw output is retained and Linux replay is the independent oracle. Read-only analysis changes nothing; replay requires explicit writable admission. Production mounts still reject journals; filesystem RECOVER/clean/orphan transitions belong to Phase 8. See [scope and limitations](../roadmap/ext4-phase6.md).

### Phase 7 — Ordered transaction writer and checkpoints

Define transaction credit accounting and bounded block images; preflight space for descriptors, metadata, revokes and commit. One serialized transaction initially. Write and flush newly exposed file data before metadata commit; persist journal contents before its commit record; make commit durable before home checkpoint. Advance reusable journal space only after checkpoint barriers complete. Never mutate metadata home blocks before durable commit.

Track transaction state and failure semantics. Uncertain commit means taint/abort, not fictitious rollback. Handle sequence wrap and journal-full admission without sleeping under a spinlock. Existing overwritten data is not promised atomic by ordered metadata journaling.

Gate implemented: `test-jbd2-write-host` passes 12 geometry/placement cases with ordered-data/log/activation/commit/checkpoint/tail barriers, 3,568 atomic cut/restart cases, sector tears, credit limits, read/OOM failures and reuse. Linux replays unmodified FortressOS-written committed journals and checks bytes/mode/fsck (36 copies). Phase-6 regression checks reverse interoperability with its documented debugfs UUID normalization. One exclusive transaction is staged/committed/checkpointed at a time; production VFS transactions remain Phase 8. See [writer evidence/limits](../roadmap/ext4-phase7.md).

### Phase 8 — Journal all mutations, orphan recovery and sync

Route every allocation, free, bitmap/counter/checksum, inode/tree and directory update through transactions. Include rename and truncate/delete restartability. Use a supported traditional orphan mechanism where needed; orphan_file stays excluded. Do not reuse freed blocks while older replay can overwrite them; implement revokes correctly.

Connect `SYS_SYNC` to commit/checkpoint/barrier; define durability on successful return. Shutdown freezes new work, drains transactions, flushes, then records clean state. Read-write mount performs validated recovery before VFS publication. Mid-session sync does not freeze the filesystem.

Gate: transaction coverage audit finds no direct metadata write bypass. Fault injection covers each operation, orphan cleanup, open-unlink, shared handles and append. Recovery restores consistent ownership and namespace within declared operation boundaries.

#### Phase 8 implementation parts — agreed 2026-10-03

Implement sequentially as six separately reviewable and committable parts,
each with a focused verification gate. Part **8.1** implements the exclusive
block-image transaction foundation; operation integration starts at **8.2**.
See [8.1 inventory, ownership and evidence](../roadmap/ext4-phase8-1.md).
Part 8.2 is implemented as an exclusive file workbench: 12 host cases,
20,256 crash cuts and 48 Linux oracle copies PASS; production mount integration
remains disabled. See [8.2 ownership and evidence](../roadmap/ext4-phase8-2.md).
Part 8.3 is implemented in the same exclusive workbench: bounded create/mkdir,
regular-file rename without replacement, closed-file unlink and empty rmdir.
12 configurations, 48,288 crash cuts and 324 Linux oracle copies PASS.
Part 8.4 adds bounded traditional orphan recovery, shrink intents, in-place
restartable extent reclamation/revokes and exclusive shared/independent handle
lifetimes for open-unlink. Twelve configurations, 145,720 interruption/restart
checks (including focused depth-2 cuts) and 291 Linux oracle copies PASS.
See [8.4 boundaries and evidence](../roadmap/ext4-phase8-4.md).
Part 8.5 provides explicit disposable journaled VFS mounts, validated recovery
before publication, real descriptor/orphan lifetimes, SYS_SYNC, shutdown
freeze/drain and journaled clean-state markers. Twelve host configurations,
16,480 lifecycle interruption/restart checks, 84 independent Linux host copies
and BIOS/UEFI 6/6 guest cases (12 boots) PASS. See
[8.5 scope and retained evidence](../roadmap/ext4-phase8-5.md).
Part 8.6 completes the mutation-coverage audit and combined mounted gate:
12 host configurations, 6,624 focused crash cuts, 6,712 staging/credit injections,
204 independent Linux host copies and BIOS/UEFI SMP=1/4 12/12 guest cases
(24 boots) PASS. It also fixes shared write-offset serialization and cache
publication after durable truncate intent. See [8.6 audit and limits](../roadmap/ext4-phase8-6.md).
Production journaled RW remains disabled pending Phase-9 acceptance and rollout.

| Part | Implementation scope | Verification gate |
| --- | --- | --- |
| **8.1 — Transaction foundation** | Inventory every metadata write path, define bounded credits and transaction ownership, and connect the journal writer to the mutation engine. Preserve the non-journaled E4-A path. | Enabled journal mutation paths have no direct metadata-home write bypass; credit exhaustion and staging failures occur before publication, with existing taint semantics preserved. |
| **8.2 — File writes and allocation** | Journal allocation, extents, inode sizes, bitmaps, counters and checksums. Flush newly exposed data before committing references; preserve ordered-mode limits for overwrites. | Write/append fault and recovery tests pass, including shared handles, allocation ownership and stale-byte exclusion. |
| **8.3 — Namespace operations** | Integrate create, mkdir, rename, unlink and rmdir within the supported profile. Define operation transaction boundaries before enabling each path. | Interrupted operations recover consistent names, directory checksums, reference counts and ownership. |
| **8.4 — Truncate and orphans** | Make truncation and reclamation restartable; implement supported traditional orphan tracking, open-unlink lifetime and required revokes. Keep orphan_file excluded. | Recovery resumes interrupted cleanup without leaked allocations, double frees or replay overwriting reused blocks. |
| **8.5 — Mount, sync and shutdown** | Validate and recover before VFS publication, subject to explicit writable/recovery admission. Connect SYS_SYNC, shutdown freeze/drain, barriers and clean-state transitions. | Successful sync satisfies the declared durability contract without freezing; failed recovery never admits RW; clean state follows successful drain and barriers only. |
| **8.6 — Integration audit** | Audit every mutation for transaction coverage and run combined host/QEMU operation, lifecycle and recovery tests. | No metadata bypass remains; complete Phase-8 integration gate above passes across the supported configurations. |

Each part includes focused write/flush fault and crash tests. Phase 9 retains
the exhaustive crash campaign and E4-B acceptance. Production journaled RW
stays disabled until complete integration passes; intermediate verification
uses explicit disposable test fixtures. The default image stays ext2 and the
accepted optional E4-A image stays non-journaled until a separately verified
journaled test image is provided.

Retain current clean-state rejection for dirty non-journaled external volumes:
they require an offline filesystem checker, not automatic flag clearing.
Journaled recovery validates and replays supported transactions, completes
required orphan cleanup, and flushes before a clean-state claim. Neither
journaling nor a readable volume proves arbitrary corruption can be repaired.

### Phase 9 — Crash campaign and E4-B acceptance

Proposed six-part execution sequence: [Phase 9 detailed plan](EXT4_PHASE9.md).

Part 9.1 is host-verified: calibrated persistence model, 156 mounted operation
inventories, 228 omitted-commit-flush controls and independent Linux integrity/
byte/namespace checks. See [specification](EXT4_PHASE9_1.md) and
[evidence and limits](../roadmap/ext4-phase9-1.md). The 333,424 initial 9.2 fault
cases are planned, not executed. E4-B acceptance remains pending.

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

Next implementation unit: **Phase 9 crash campaign and E4-B acceptance**. Phases 8.1-8.4 provide transaction, file-allocation, bounded namespace and restartable orphan workbenches; Phase 8.5 connects explicit disposable journal mounts to VFS, sync and shutdown; Phase 8.6 completes the combined metadata-coverage and mounted integration gate. Production journaled mount eligibility remains disabled; passing integration does not substitute for Phase-9 crash/physical acceptance or authorize production rollout. Phase-5 [Dell acceptance](../roadmap/ext4-phase5-dell.md) includes the user-confirmed metadata-cache run: 16 MiB in 7.06 seconds, hashes after reboot and independently on Mint, and unmounted e2fsck -fn exit 0, separately from journal acceptance. The default image remains ext2; the optional EXT4 image remains non-journaled. This plan authorizes no format conversion, root switch, real-disk installation or protected synchronization change.

E4-A [complete physical acceptance record](../roadmap/ext4-phase5-acceptance.md): all seven checklist items PASS. Flush timing attribution and proposed count reductions remain estimates, not measured guarantees.
