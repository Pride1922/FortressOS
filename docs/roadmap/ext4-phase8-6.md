# EXT4 Phase 8.6 — integration audit

The Phase-8 integration gate covers the bounded journal profile on explicitly
admitted disposable fixtures. Production journaled mount eligibility remains
disabled. The default image is ext2; accepted E4-A USB media remain non-journaled.
Phase 9 owns the exhaustive crash campaign and E4-B acceptance.

## Metadata coverage audit

The audit follows callbacks and their indirect callees, rather than treating
a search for write calls as proof of transaction coverage.

| Mutation | Mounted entry and planner | Durable publication |
| --- | --- | --- |
| File write, append, allocation, unwritten conversion and extent rebuild | `e4_jwrite` → `ext4_engine_file_write` → `e4_file_plan` | `e4_semantic_commit` → `e4_transaction_commit`; exposed data is ordered, metadata is checkpointed |
| Create and mkdir, inode reserve, parent links and directory growth | `e4_jcreate` → `ext4_engine_namespace_create`; shared allocator/directory checksum helpers | One semantic transaction; VFS inode/parent publication follows durable checkpoint |
| Regular-file rename without replacement | `e4_jrename` → `ext4_engine_namespace_rename` | Destination insertion and old-name retirement share one transaction |
| Unlink and empty rmdir | `e4_junlink` → namespace remove → `ext4_engine_orphan_unlink` | Name/link/orphan intent share one transaction; bounded reclamation follows |
| Truncate, partial tail and open-unlink shrink | `e4_jtruncate` → `e4_orphan_pinned_truncate` | Size/orphan intent and partial-tail zeroing are journaled together, then restartable cleanup |
| Last close and orphan recovery | `e4_jclose`, mount recovery, sync/freeze → `e4_orphan_cleanup` | Right-edge frees, tree edits, inode bitmap and orphan detach use semantic transactions and revokes |
| Block/inode bitmaps, counters, group descriptors and checksums | `e4_stage_bitmap`, `e4_account`, `e4_descriptor`, `e4_seal` | Staged images only; descriptors/cache republish after all checkpoint barriers |
| Inode/root/external extent checksums and directory tails | `e4_inode_sum`, tree sealing, `e4_dir_sum` | Sealed before writer staging; new metadata is never classified as ordered file data |
| Activation and clean-state markers | `e4_journal_mark` | Whole superblock-containing image through JBD2; clean follows drain and barriers |
| Recovery writes and journal retirement | validated read-only preview → `jbd2_replay` | Committed, unrevoked images only; no RW VFS publication until recovery and ownership validation succeed |
| Journal descriptors, payloads, revokes, commit and tail | JBD2 writer | Existing ordered-data/log/activation/commit/checkpoint/tail barriers |

All actual filesystem device writes reach either E4-A `e4_write_bytes` or
JBD2 `j_io`. A journal engine cannot use `e4_write_bytes`, legacy
`e4_commit_unlocked`, or `ext4_engine_finish`. Legacy plan entry rejects a writer
unless a semantic planner explicitly owns it. Mounted callbacks are selected
by `e4_setup`; they never dispatch to E4-A `e4_publish`. Unsupported directory
moves, replacement rename, hard links and broader features remain rejected.
Backups remain read-only identity sources; no backup-update path was added.

The host device adapter additionally audits every changed home-block slice,
including adjacent sub-blocks in a 4096-byte sector. Ordered home data must
match a staged writer data image during COMMITTING; changed home metadata must
match a staged metadata image during CHECKPOINTING. Journal writes require
the corresponding writer phase. These runtime checks cover mounted operations;
pre-publication recovery is covered by the static trace and Phase-8.5 recovery
gate. A runtime audit is evidence for exercised paths, not a general C proof.

The transitive I/O trace is JBD2 `j_io` → bounded block run/sector adapters →
GPT partition translation → NVMe sector/flush submission (or USB BOT adapter
and bounded xHCI event polling for the retained E4-A path). Neither path sleeps,
enables IF or acquires another rank-1 lock under EXT4 exclusion. Heap work
retains rank-2 ordering. Driver timeout/fatal ownership still quiesces or
quarantines DMA rather than reusing it. SYS_SYNC dispatches the filesystem
drain; shutdown dispatches freeze/drain before power operations. No lower-layer
barrier, durability admission or DMA contract was changed.

## Integration fixes

VFS previously copied `file_t.offset` before entering the filesystem lock and
published the result after return. Two writers sharing one non-append handle
could therefore supply the same offset despite correct EXT4 exclusion.
`serializes_write_offset` now explicitly lets EXT4 own the real shared offset
and cached size under its existing rank-1 lock. Other filesystems retain their
existing callback behavior; streams keep their separate path. There is no
new VFS lock, lock nesting, scheduling or IRQ transition.

The regression pairs two actual VFS callbacks at a barrier, forcing concurrent
entry. It verifies 64 unique intact records and the shared final offset;
independent append handles receive a separate check. An isolated source copy
with the old offset behavior fails the record-size assertion, establishing
that this test detects the original race. Concurrent VFS reads are not claimed
to gain a new shared-offset serialization contract from this write fix.

Truncate can durably publish a smaller size and then fail a later cleanup
staging allocation. The mounted wrapper now refreshes the authoritative inode
when a healthy registered orphan proves that intent was published, even if
cleanup returns an error. Uncertain I/O still taints and requires restart.
Failed cache publication after known commit taints; it is never reported as
rollback. A focused pre-fix test reproduces the stale size; the final staging
matrix checks immediate cache agreement after every healthy truncate failure.

## Verification and retained evidence

Commands use disposable regular files under the ignored workspace directory
`.codex-remote-attachments/ext4-phase8-6`, outside `make clean`. The existing
Phase-8.5 host source fixtures are read-only inputs, with recorded hashes.

```sh
make test-ext4-integration-host EXT4_INTEGRATION_FIXTURES=.codex-remote-attachments/ext4-phase8-5/host-frz8r6nu
make test-ext4-integration-staging-host EXT4_INTEGRATION_FIXTURES=.codex-remote-attachments/ext4-phase8-5/host-frz8r6nu
make test-ext4-integration EXT4_INTEGRATION_FIXTURES=.codex-remote-attachments/ext4-phase8-5/host-frz8r6nu
```

The host matrix is 1/2/4 KiB blocks × 512/4096-byte sectors × normal/wrapped
journal placement. Thirteen actual mounted operations cover create, mkdir,
write, append, rename, truncate, unlink, open-unlink, last close, rmdir,
post-reclamation allocation, sync and freeze. Focused cuts cover writer-phase
transitions, every flush and first/last event, both before/after and cached/
write-through persistence. Every cut destroys runtime state, drops volatile
cache and performs fresh mount recovery with ownership/namespace/byte checks.
Failures permanently taint; further sync/freeze must issue no I/O.

Staging tests inject each baseline read and allocation failure for each
operation. Sub-sector RMW read failure after entering commit is uncertain I/O,
even before the first write callback; the initial test expectation was corrected
to preserve that accepted taint behavior. A one-slot in-memory ring-budget shim
checks semantic credit exhaustion before publication, with unchanged offsets,
zero writes, no taint and a successful drain after restoring the budget.
This is reservation fault injection, not an on-disk journal geometry claim.

Independent Linux copies undergo journal replay where needed, unmounted
`e2fsck -fn` with exit 0, primary/JBD2 clean/checksum checks and exact byte or
record-set audits. No automatic fsck repair supplies acceptance evidence.

QEMU uses immutable ISO copies, explicit GPT/NVMe fixtures, BIOS/UEFI,
1/2/4 KiB blocks and SMP=1/4, two boots per case. It combines recovery of a
preexisting log/orphan, namespace changes, partial truncate, dup/independent
open-unlink pins, last close, reallocation, Ring-3 SYS_SYNC, later writes,
shutdown and reboot. SMP=4 also runs both independent and shared append
workers on APs 1 and 2, and independently verifies all 200 records per file on
Linux after each stopped guest. Exact argv preflight rejects added disks,
snapshots and host-device arguments. Every QEMU PID is bounded and reaped.

Final gates PASS, 2026-10-04. Paths below are relative to the ignored evidence
root. These are final runs after both implementation fixes; earlier probe,
failed-test and repeated runs remain retained and are excluded from totals.

| Evidence | Final result |
| --- | --- |
| `host-cbplw4v9` | 12/12 configurations; 6,624 focused write/flush cuts and fresh recoveries; 180 independent Linux operation/record copies |
| `staging-m04e2qbd` | 12/12 configurations; 6,712 read/allocation/credit injections; immediate healthy truncate cache checks; 24 independent Linux record copies |
| `guest-smp1-nk82pe7i`, `guest-smp4-jhdnc75z` | BIOS/UEFI × 1/2/4 KiB × SMP=1/4, 12/12 cases and 24 boots; exact bytes, namespace, clean/checksum/fsck; true AP append at SMP=4 |
| `vfs-regressions.log`, `regressions/host-*` | E4-A host writes/cache at six geometries, ext2, pipe, USB mount policy and BOT sanitizer regressions PASS |
| `regressions/usb-run-4om82b93` | E4-A production USB BIOS/UEFI × SMP=1/4, 4/4 cases and 12 persistence boots, plus four immutable RO/degraded cases PASS |
| `negative-offset.log`, `negative-offset-result.json` | Isolated old shared-offset implementation rejected by the forced-overlap regression |
| `truncate-cache-before.log`, `truncate-cache-after.log` | Pre-fix stale cache reproduced; corrected focused truncate-staging gate PASS |
| `final-build.log` | Strict freestanding build and default ext2 image verification PASS |

There are **204 independent Linux host copies**, separately from 24 guest
offline audits. Linux block maps also confirm actual reuse of reclaimed target
blocks in all twelve host configurations. A final targeted BIOS/SMP=4 two-boot
check verifies the additional paired fixture-key/mount guard; it is recorded
separately and is not added to the matrix totals. The final `verification.json`
retains source/binary hashes, exact artifact directories and source snapshot.
No unrelated grep or later stream-tool work is changed by this implementation.

## Remaining limits

This is bounded Phase-8 integration acceptance, not Phase-9 exhaustive crash or
physical acceptance. Sector tears, arbitrary durable corruption, disconnects,
recovery/checkpoint campaigns and sacrificial physical power cuts retain their
separate evidence requirements. Ordered mode does not make overwritten data or
application multi-file updates atomic. Journaled USB production admission and
physical journal durability are not enabled or claimed. E4-A's Dell 5590
7.06-second download, reboot/Mint hashes and offline fsck acceptance remain
separate physical evidence. No root switch, conversion or installer is included.
