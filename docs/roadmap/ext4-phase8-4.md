# EXT4 Phase 8.4 — truncate and traditional orphans

Implemented from clean `main` checkpoint `e2fbfbb`, 2026-10-04. The exclusive
workbench adds restartable shrink/delete and open-unlink lifetime. Production
journaled RW remains disabled; VFS publication, SYS_SYNC, freeze/drain and clean
state remain Phase 8.5. The accepted non-journaled E4-A path is unchanged.

## Admission and ownership

`ext4_engine_open_journal_orphans` extends the existing disposable-media,
explicit-durability, empty-journal workbench admission. The caller must replay
JBD2 first. Admission writes nothing and validates the complete traditional
`s_last_orphan` / inode `i_dtime` chain: at most 64 entries, allocated supported
inodes, checksums, single links or deleted orphans, range/identity bounds, no
cycles, duplicate entries or journal/reserved inode targets. `orphan_file`
remains excluded by the exact feature masks.

Admission independently reconstructs ownership of all allocated normal inodes,
data and external extent nodes and compares every block bitmap bit and free
counter. It validates namespace references against inode links, directory
types/dot ownership/parents, directory counters and root connectivity. Deleted
orphans cannot retain incoming names; multiply referenced files and directory
cycles reject before writes. Orphan extents beyond a linked truncate target's
EOF are included in ownership. A deleted directory whose dot block was already
reclaimed remains valid until its inode/chain retirement transaction.

Bounds for this intermediate workbench are 1,048,576 filesystem blocks,
65,536 inodes, 1,024 directories and 64 orphans. Rmdir supports an empty single
dot block. Larger volumes and multi-block directory deletion reject unchanged.
Temporary ownership/reference arrays and the two extent path buffers are
heap-owned and bounded; no recursive 4 KiB kernel-stack arrays are introduced.

The orphan head is mutable transaction state, excluded from the immutable
journal identity comparison. Bootstrap and replay still reject reserved,
out-of-range and journal inode heads. Journal UUID, inode, extent map/tree,
geometry and protected home ranges retain their existing guards. Ordinary
production admission and the 8.2/8.3 constructors still reject nonempty orphan
chains. A fresh 8.4 context with orphans blocks normal mutations until explicit
`ext4_engine_orphan_recover` completes.
Admission also reserves and aborts the engine's maximum 64-metadata/64-revoke
credit class without I/O. A journal too small to support bounded cleanup rejects
before any truncate/delete intent can become durable.

The traditional representation follows the [Linux inode format](https://www.kernel.org/doc/html/latest/filesystems/ext4/inodes.html)
and [orphan cleanup implementation](https://github.com/torvalds/linux/blob/master/fs/ext4/orphan.c).
This is compatibility with the restricted profile, not repair of arbitrary
filesystem corruption.

## Transaction boundaries and lifetime

| Update | Durable boundary |
| --- | --- |
| Linked file shrink | New size, orphan insertion, inode/superblock checksums and partial-tail zeroing in one transaction |
| Unlink/rmdir intent | Parent directory entry removal, target zero links, orphan insertion, checksums and parent directory link decrement together |
| One reclaim step | Remove at most 32 data blocks from the rightmost extent; remove/revoke at most two newly empty external tree nodes; bitmap/counter/inode block-count/tree checksums together |
| Final linked retirement | Remove the inode from the orphan chain and clear its next pointer |
| Final deleted retirement | Remove chain entry, free inode bitmap/accounting, decrement used-directory count if applicable, preserve inode generation and seal cleared inode together |

Reclamation edits the retained rightmost leaf/index path in place. It allocates
no new tree nodes or data blocks and never rebuilds the entire tree. Each freed
data/tree block receives a revoke. An empty freed tree node is never also staged
as a home image. Each transaction checkpoints and retires its journal tail
before a later allocator can reuse a block. The final inode free cannot precede
complete extent reclamation.

The one initialized partial tail is **journaled with the size intent**, so a
precommit crash preserves both the old size and its visible bytes. Recovery also
seals a foreign linked orphan's partial tail before removing its orphan record.
This narrow tail rule does not change ordered-mode overwrite limits for normal
file writes. Shrink only is supported; extension through truncate is excluded.

Sixteen tokenized workbench handles support independent opens, shared offset and
lifetime through dup, authoritative-EOF append, read and shrink. Unlink retains
allocations and inode identity while any handle survives. Truncating a pinned
deleted file reclaims allocations above its new EOF while preserving the orphan
registration. The final close completes deletion, including non-head chain
removal. A consumed final-close token stays invalid even when cleanup fails;
the durable orphan remains recoverable. Tokens cannot address a later reused
slot/inode within the engine. Restart has no surviving handles and reclaims
all zero-link orphans. No real VFS descriptor/node lifetime is enabled here.

All entry points retain exclusive caller serialization in unlocked thread
context. No scheduler/IRQ/lock rank, DMA ownership/quarantine or USB admission
contract changes. All enabled metadata updates use `e4_namespace_end` /
`e4_semantic_commit` and JBD2; there is no direct metadata-home write path.
Cache invalidation precedes I/O; descriptor/cache publication follows successful
checkpoint and tail barriers. Close(engine) writes nothing and never marks clean.

Initial staging, credit, allocation and read failures publish nothing. A later
cleanup staging failure may follow a durable intent/reclaimed prefix; callers
must retry recovery. Uncertain commit/checkpoint I/O permanently taints the
context, drops its metadata cache and blocks subsequent mutation. Abort never
clears taint. Failure does not promise rollback of an already committed intent.

## Verification

`make test-ext4-orphan-host` runs actual EXT4/JBD2 code under ASan/UBSan and strict
warnings on newly generated 32 MiB regular files. Three independent host
processes use immutable input fixtures and distinct writable buffers/output
prefixes. The matrix covers 1/2/4 KiB filesystem blocks, 512/4096-byte sectors,
ordinary placement and circular/UINT32 sequence wrap: 12 configurations.

Seven operation profiles cover initialized partial shrink, 65-block unwritten
shrink, large unlink, external extent-tree unlink, rmdir, shared/independent
open-unlink/append/truncate/final-close and depth-2 right-edge reclamation. Every
write/flush event is interrupted before/after under four stable/volatile sector
patterns. A separate campaign interrupts every orphan-recovery write/flush.
Fresh public JBD2 discovery/replay and orphan recovery must restore valid names,
sizes, bytes and exact allocation ownership; repeated cleanup is write-free.

The gate also injects every operation/admission read and allocation failure,
checks 13 malformed ownership/namespace/chain cases with zero writes, exhausts
64 orphan credits and 16 handles, checks shared offsets and stale token/inode
reuse, and reuses a freed data block with independently verified new bytes.
An observer rejects metadata home writes before checkpoint. Linux independently
replays committed pending copies, completes orphan cleanup and runs unmounted
`e2fsck -fn`, namespace/size and byte audits; foreign-intent, maximum-orphan and
post-reuse copies are included.

The runner permits resuming completed independent profile gates with
`EXT4_ORPHAN_ARGS="--resume build/ext4-phase8-4/run-trh2ky6h"`. Each remaining
profile starts from its immutable source, incomplete output copies are archived,
and all seven profiles still receive the complete interruption campaign. The
initial 30-minute per-case limit was too short for the largest geometry; it is
now two hours. Retained interrupted attempts and their exclusive-evidence file
collision are runner diagnostics, not acceptance results.

Regression gates PASS:

| Gate | Retained evidence / scope |
| --- | --- |
| Phase 8.1 transactions | `build/ext4-phase8-1/run-7m7x950b`, 12 configurations. |
| Phase 8.2 file transactions | `build/ext4-phase8-2/run-l_rkd3rk`, 12 configurations, 20,256 crash cuts, 48 Linux copies. |
| Phase 8.3 namespace transactions | `build/ext4-phase8-3/run-kna_8efk`, 12 configurations, 48,288 crash cuts, 324 Linux copies. |
| Phase 6 public replay | `build/jbd2-replay/run-6ekgynuo`, 12 configurations and all 48 zero-write rejection checks. |
| Phase 7 writer | `build/jbd2-write/run-7hackdl0`, 12 configurations, 3,568 atomic cuts plus sector tears, 36 Linux copies. |
| E4-A host writes/cache | `build/ext4-phase4/host-7j0cln1d`, six geometries with namespace/fault/fsck and 1/16 MiB byte checks. |
| E4-A USB | `build/ext4-usb/run-s7x3l5z5`, BIOS/UEFI × SMP=1/4, 12 boots plus four immutable RO/degraded cases. |
| Reader / USB contracts | `test-ext4-read-host`, `test-usb-mount-host`, `test-xhci-bot-host` PASS, including bounded 16 KiB BOT transfers. |
| Kernel / formatting | `make bin/fortress.elf` and scoped `git diff --check` PASS. |

The focused `test-ext4-orphan-deep-host` gate PASS is retained at
`build/ext4-phase8-4/deep-run-z2_lqwtt`: all 12 latest-source admission checks
reject insufficient cleanup journal credits without writes; full depth-2
deletion reclaims 337 sparse extents across 13,855 write/flush events, with 24
selected before/after/persistence cuts and three independent Linux replay/fsck
copies. This tests complete leaf/index collapse and multiple journal wraps.
It was run with the explicit disposable fixture directory using
`EXT4_ORPHAN_DEEP_ARGS="--fixtures build/ext4-phase8-4/run-trh2ky6h"`.

The maximum-credit reserve/abort admission check was added after the first
matrix binary was built. The focused gate above verifies that write-free
preflight across every configuration; resumed matrix profiles use the latest
source. The journal emitter and reclamation algorithm were unchanged by that
admission check. Results do not claim a full rerun of earlier matrix profiles
with the later admission-only change.

The complete Phase 8.4 matrix PASS is retained at
`build/ext4-phase8-4/run-trh2ky6h`: 12/12 configurations, 128,816 operation cuts,
16,880 recovery cuts, 156 malformed zero-write rejection checks and 288 Linux
oracle copies. Every operation/admission read and allocation failure is
injected; the maximum-orphan/handle, foreign intent and physical-block reuse
gates pass. With the separate deep gate, this is **145,720 interruption/restart
checks and 291 Linux oracle copies**. Linux journal replay/orphan cleanup is
followed by unmounted `e2fsck -fn` exit 0 and independent namespace/size/byte
audits.

| Filesystem block / sector | Operation cuts per placement | Recovery cuts per placement |
| --- | ---: | ---: |
| 1 KiB / 512 B | 8,024 | 1,040 |
| 1 KiB / 4 KiB | 4,656 | 600 |
| 2 KiB / 512 B | 14,760 | 1,920 |
| 2 KiB / 4 KiB | 4,656 | 600 |
| 4 KiB / 512 B | 27,720 | 3,680 |
| 4 KiB / 4 KiB | 4,592 | 600 |

Each row runs both ordinary and circular/sequence-wrap placement. The final
manifest records exact argv (including resumed profile boundaries) and source
hashes; raw host/Linux logs, generation commands and all image copies remain
retained. The two largest sector cases resumed after completed profiles 0–2
and 0–1 respectively; incomplete copies are preserved in `interrupted-*`
directories. No interrupted attempt is counted as a passed profile.
The full Phase-9 crash campaign, guest journal mounts, real VFS shared descriptors,
physical journal acceptance and arbitrary torn/corrupt metadata repair remain
outside this workbench. Dell's accepted E4-A metadata-cache measurements remain
separate physical evidence; they are not journaling/crash-consistency evidence.
