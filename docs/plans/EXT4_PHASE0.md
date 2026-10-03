# EXT4 Phase 0 — Design baseline and fixture evidence

Date: 2026-10-03. Phase-0 design and fixture tooling delivered. No ext4 driver, mount, journal, crash simulator or kernel acceptance is implemented. API declarations in `src/fs/ext4.h` are a forward contract, not callable functionality.

## Profile E4-A-v1

Exact required feature masks (do not silently expand):

| Class | Mask | Features | Owners in later phases |
| --- | --- | --- | --- |
| compat | 0x00000000 | none | admission: Phase 1 |
| incompat | 0x00000042 | FILETYPE 0x2, EXTENTS 0x40 | directories: 1/4; mapping: 2/3 |
| ro_compat | 0x00000403 | SPARSE_SUPER 0x1, LARGE_FILE 0x2, METADATA_CSUM 0x400 | layout: 1/3; size: 1/4; integrity: 1–4 |

This is an exact controlled profile for both RO and RW, deliberately narrower than generic ext4 feature negotiation. Unknown bits reject admission with EOPNOTSUPP. Dirty state, RECOVER, journal flags and nonzero legacy orphan chain reject admission without writes until their later milestone. Invalid checksums/geometry return EIO; invalid arguments EINVAL; bounds EFBIG; allocation ENOMEM; write-policy denial EROFS. RO is never a repair path.

Filesystem blocks: 1024/2048/4096 bytes. Sector adapters: 512/4096 bytes. When filesystem blocks are smaller than physical sectors, a serialized read/modify/write bounce buffer preserves adjacent blocks; Phase 1 must test this geometry rather than treating blocks as sectors. A flush failure invalidates the entire mutation, even if some sectors reached media.

Inodes: exactly 256 bytes; validate extra_isize before optional fields, accept only the documented Linux creator layout. Group descriptors: 32 bytes; high block counters must be zero without 64bit. Maximum volume: 8 GiB; maximum file logical size: 8 GiB; maximum groups: 1024; maximum inodes: 1,048,576. These are implementation caps, not ext4 limits. Sparse fixtures exercise offsets above 4 GiB independently of physical volume size.

Tree depth: at most 2 (root plus two external levels). Maximum mapping nodes inspected per operation: 4096. No unbounded recursion. User I/O chunks: at most 64 KiB per callback, valid short returns permitted. Metadata workspace: 64 block images at 4 KiB plus separate sector bounce and three traversal buffers; heap-owned at mount, not kernel stack. One mount and one serialized operation initially. No persistent dirty cache in E4-A; 16 clean read-cache blocks maximum, invalidated on matching writes/taint. No fallback allocation while mutating; mount OOM publishes nothing.

Limits are admission/operation errors, never silent truncation. Exhausting the bounded traversal budget fails the operation; mutators preflight before changing disk. Phase 3 must prove the 64-image budget covers supported operations or explicitly narrow the supported operation before release.

Only regular files and directories with extent mapping; no hard-link creation, symlinks, special files, xattrs, indexed directories or unsupported flags. Inodes with extra links/ACL blocks or incompatible semantics are not writable. Existing mkfs reserved inodes are layout reservations, not user files; do not demand ordinary extent-file semantics of every reserved inode.

Directory names remain limited by VFS_MAX_NAME=64 (63 bytes plus NUL) and paths by VFS_MAX_PATH=256. Longer existing disk names produce a visible unsupported result, not truncated lookup aliases. Reads must never disclose allocated metadata or stale contents from holes/unwritten ranges.

## API and callback mapping

Public signatures are in `src/fs/ext4.h`: mount_ro, mount_rw, sync and freeze_and_sync. `out` is set NULL on entry, publication occurs only on full success, and the mount retains a borrowed stable partition device. External USB policy supplies the same selected PARTUUID, GPT eligibility and durability preflight used today; driver admission cannot bypass those decisions.

Implementation callback sketch matching current `vfs_node_t`:

```c
static vfs_node_t *e4_lookup(vfs_node_t *dir, const char *name);
static int64_t e4_read(vfs_node_t *node, uint64_t off, void *buf, size_t len);
static int64_t e4_write(vfs_node_t *node, uint64_t *off, bool append,
                        const void *buf, size_t len);
static vfs_node_t *e4_create(vfs_node_t *dir, const char *name,
                            vfs_node_type_t type);
static int e4_unlink(vfs_node_t *dir, const char *name);
static int e4_rename(vfs_node_t *old_dir, const char *old_name,
                     vfs_node_t *new_dir, const char *new_name);
static int e4_truncate(vfs_node_t *node, uint64_t new_size);
static int e4_readdir(vfs_node_t *dir, uint64_t cookie, void *out);
static int e4_can_write(vfs_node_t *node);
static void e4_close(vfs_node_t *node);
```

RO nodes publish lookup/read/readdir and can_write denying mutation; mutation callbacks remain NULL. Phase 4 publishes mutators only for a healthy RW mount. create uses the current VFS create-error channel; close refers to final `file_t` release, not final inode reference. There is no sync slot in vfs_node_t: mount-level dispatch must call ext4_sync from usb_mount_sync, and ext4_freeze_and_sync from shutdown alongside ext2_sync_all. No VFS struct change is implemented in Phase 0.

## Current-code inventory and required work

| Boundary | Finding | Later-phase requirement |
| --- | --- | --- |
| vfs.c unlink | Frees cached target after callback; no node open count | Phase 4 must add safe VFS node references/open accounting before enabling unlink of ext4 nodes; reject active targets until deferred lifetime is verified |
| vfs.c rename | Calls vfs_unlink on existing destination before rename callback | E4-A replacement rejected before destination removal; E4-B requires a VFS replacement dispatch change so both names mutate inside one transaction |
| file_t refs | Atomic refs protect shared file objects, not independent opens of an inode | Track node/inode lifetime separately; dup shares file offset, independent opens do not |
| write offsets | VFS uses uint64_t offset/size and append callback receives offset pointer | Audit overflow in callback and wrapper; serialize authoritative EOF and updates under filesystem lock |
| ELF | Loader consumes a buffered image; spawn has MAX_ELF_FILE_SIZE | Do not equate large filesystem files with larger executable support; preserve loader limits and buffering lifetime |
| USB sync | usb_mount_sync currently invokes only block_flush | Phase 5 adds mount-kind dispatch; Phase 8 commits journal before barrier |
| power syscalls | Reboot/shutdown call ext2_sync_all | Add ext4 freeze path at integration; failure must prevent a claimed clean shutdown |

Rejecting replacement needs a check in VFS before its pre-unlink step; a filesystem callback alone is too late. These are explicit Phase-4/8 integration tasks, not changes in this phase.

## Lock and ownership call graph

Current paths reviewed: VFS callback → filesystem IRQ-save rank-1 lock → block_read/write/flush → GPT partition bounds/translation → parent block callback → USB wrappers → xhci_scsi_* → BOT command/data/status → transfer polling/delay. GPT callbacks delegate without a new filesystem lock; USB wrappers use the selected controller. BOT uses bounded polling/delay callbacks rather than scheduler sleep. Preserve failure propagation and DMA quarantine.

Ext4 owns a separate rank-1 lock and its heap mount/workspace. Heap is rank 2; VMM/PMM/console are ranks 3/4/5, but hot filesystem operations use reserved buffers and defer diagnostic output until unlocked. No nesting with ext2/process/scheduler/network rank-1 locks. Mount construction allocates outside the lock; callbacks serialize operation state and authoritative EOF. ext4 and ext2 must not concurrently drive a shared USB transport; production selects a single filesystem on a single data partition. A general shared-device multi-mount manager is excluded.

No lock is held across a scheduler wait. E4-B initially commits/checkpoints synchronously, one transaction at a time; journal-full admission checkpoints synchronously or returns a bounded failure. No new background worker or journal wait channel. Long IRQ-disabled latency is a risk to quantify in Phase 3; if bounds are unacceptable, redesign before RW delivery. This is a design audit, not a measured scheduler/IRQ claim.

## Persistence ordering and failure contract

E4-A: mark mounted filesystem unclean and flush before first mutation. Allocate/zero data and metadata blocks, persist allocation records/counters, then publish inode/tree/directory references with all affected checksums. Removal persists reference removal before freeing allocations. A crash between steps may leak blocks or produce inconsistent metadata; refuse dirty remount and require independent host recovery. There is no transaction rollback guarantee.

E4-B proposal: reserve credits, stage metadata, persist newly exposed ordered data, append journal descriptor/payload/revokes, flush, append commit, flush, checkpoint home blocks, flush, advance journal reusable tail, flush. Never overwrite metadata home blocks before durable commit. Incomplete or uncertain writes taint/abort; do not call a possibly committed transaction rolled back. Shutdown clean-state updates also participate in the proven persistence protocol.

Journal transaction cap proposed: 64 distinct metadata blocks, 64 revoke records, 128 journal blocks total reservation; reject before mutation if credits exceed the cap. Large truncate/delete operations use restartable orphan-backed chunks in Phase 8. Journal profile details require a Phase-6 addendum validating the exact Linux-generated tag/checksum variant; Phase 0 does not pretend the journal parser is specified or implemented already.

## Crash-injection specification

Implement `tests/ext4_fault_disk.*` as a host block_dev adapter linked to actual ext4/JBD2 code. Separate stable sector bytes from volatile pending writes. Enumerate event number, sector, operation, transaction ID and seed. Restart with fresh mount/runtime state using only stable bytes; never carry cache/locks across simulated power loss.

Provide cut-before/cut-after every write and flush, plus selected sector-prefix tears, pending-write permutations, dropped writes, flush failure and disconnect. A successful flush persists all preceding accepted writes under the declared model. Torn durable sectors are corruption tests: checksum rejection is acceptable; arbitrary torn storage is not promised recoverable.

Mandatory named cut points: ordered-data barrier; partial descriptor; each journal payload; revoke record; before commit; commit before flush; after durable commit before any checkpoint; each home-block checkpoint; checkpoint flush; tail advancement; clean-state write; replay and orphan-reclamation steps. Include wraparound, reuse of a revoked block, journal-full and repeated recovery.

Recovery oracle: no out-of-range I/O, no duplicate live allocation, no live reference to free/reserved metadata, no stale-data exposure, coherent inode/directory checksums and counters, complete committed transaction effects unless revoked, no effects from an incomplete transaction, and idempotent second replay. Successful sync preserves the declared durable prefix; ordinary uncommitted overwrites have no whole-file atomic guarantee. Compare namespace/data to an independent event oracle and unmounted e2fsck results. E4-A crash images are expected to require fsck; do not apply E4-B consistency assertions to them.

QEMU follow-up uses QMP quit/kill after an observable test event plus disposable images and cache configuration recorded in argv. A guest marker alone is not proof the host made a sector durable. Host cache simulation is the exhaustive oracle; QEMU is integration evidence. Build this capability in Phase 6 before implementing the writer; Linux-generated recovery fixtures test reader interoperability first.

## Fixture implementation and evidence

Run `wsl -d Ubuntu-24.04 -- python3 scripts/create_ext4_fixtures.py` (or proposed make target `ext4-fixtures`). The script accepts no device or output arguments; exclusively creates new unique directories beneath build/ext4-phase0 and never mounts or repairs anything. It records exact mkfs argv, version, UUID, feature masks, image/payload digests, dumpe2fs, debugfs and e2fsck output.

Recipe: `mke2fs -t ext4 -b BLOCK_SIZE -I 256 -O none,extent,filetype,sparse_super,large_file,metadata_csum -E lazy_itable_init=0 -m 0 -U UUID IMAGE`. Each image is a 64 MiB regular file. Metadata checksum profile is validated numerically rather than trusting printed feature names. Reproducible means a fixed recipe and recorded inputs; mkfs timestamps mean image hashes may differ between runs.

Executed 2026-10-03 on Ubuntu-24.04/e2fsprogs 1.47.0: all 1024/2048/4096-block fixtures matched masks 0/0x42/0x403, Linux debugfs round-tripped 1,048,576 bytes exactly, and e2fsck -fn returned clean. Evidence: `build/ext4-phase0/fixtures-qu53f43b/manifest.json` plus per-image text logs. This validates format feasibility and tooling only, not FortressOS reads/writes or 4096-sector behavior.

Malformed copies cover unknown incompat/ro_compat, journal, recovery-needed and checksum bit-flips. Their checksums are deliberately invalid; they cannot separately prove feature-mask rejection. Phase 1 must add recomputed-checksum unsupported-feature vectors, geometry corruption, group overlap and instrumented zero-write admission checks. Extent/tree mutation and crash oracle fixtures require actual later driver code; no fictitious test passes are recorded here.

## Gate status and estimates

Delivered: fixed E4-A profile and bounds, public API declarations, callback mapping, code inventory, lock graph, persistence ordering, crash-test design, working disposable fixture generator and independent Linux validation. Phase-1 implementation can now start. Kernel-side no-write admission and durability fault tests remain Phase-1/3 gates; the original plan's broad Phase-0 wording must not imply those already ran.

The suggested 6–8 weeks for E4-A is a provisional planning envelope, not an acceptance date. The 5–6-week E4-B estimate covers recovery plus writer only; integration, orphan handling and crash campaign (Phases 8–9) need additional estimates. “3–4 months” describes this bounded milestone at best, not full ext4. Re-estimate after Phase 2 and after sizing the transaction/fault matrix.
