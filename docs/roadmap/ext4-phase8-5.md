# EXT4 Phase 8.5 — Mount, sync and shutdown

Phase 8.5 connects the Phase 8.1–8.4 transaction engine to a real mounted VFS
on explicitly admitted disposable journal fixtures. Production journaled RW
remains disabled. The ordinary E4-A mounts, selected USB PARTUUID/durability
policy, default ext2 image and accepted non-journaled Dell image remain intact.

## Admission and recovery

`ext4_mount_journal_fixture` requires all three explicit admissions:
disposable fixture, writable media and recovery writes, plus write and flush
callbacks. It reserves the exclusive engine owner until either publication or
complete failure cleanup. Ordinary mount constructors still reject journals.

The JBD2 reader first validates and snapshots committed records. A read-only
sector preview overlays unrevoked records in replay order and the resulting
empty journal superblock. The full allocation, inode/extent ownership,
namespace, traditional orphan and maximum cleanup-credit checks run against
that preview before any recovery write. A checksummed committed journal whose
result aliases another inode's blocks therefore rejects with zero writes.
Replay is followed by reconstruction and validation against actual media.
Allocation/read/I/O failure publishes no mount and releases private resources;
failure after admitted replay can leave a valid recoverable prefix.

Activation durably sets dirty + RECOVER using a bounded JBD2 transaction before
orphan cleanup or VFS publication. Activation and the final clean transition
stage the full superblock-containing block, preserve its checksum and use the
normal commit/checkpoint/tail barriers. Neither transition directly writes the
metadata home block. A narrowly scoped guard permits these lifecycle state
changes while preserving journal identity and all other feature bits.

The explicit fixture constructor analyzes the journal even when RECOVER is
clear, allowing restart of interrupted activation or final-clean records. An
activation interrupted before the filesystem RECOVER home checkpoint has not
changed allocation or namespace metadata. Linux ignoring that activation-only
log does not lose a filesystem mutation. This fixture policy is not a general
production mount policy or arbitrary-corruption repair mechanism.

## Ownership and VFS lifetime

Mounted mutation callbacks hold the existing rank-1 IRQ-save EXT4 lock.
JBD2 writer mutation calls assert their owner's exclusion; ordinary standalone
workbench calls retain their unlocked-context assertion. Storage callbacks
remain bounded/polled and never sleep, schedule or enable interrupts. No
scheduler, assembly, lock-rank, DMA ownership or quarantine contract changes.

Create, mkdir, file writes/append, bounded rename without replacement,
unlink/rmdir and truncate use the existing transactional operations. Mounted
reads and directory queries refresh authoritative inode state after changes.
Parent inode/extent and VFS size caches refresh after successful namespace
transactions, including directory growth during rename. A failed cache
publication after a known commit taints; it cannot claim rollback. The E4-A
cache and its validation/invalidation behavior remain intact.

Real VFS `file_t` references now pin open-unlinked inodes. Duplicated descriptors
share one open file and offset; independent opens have separate offsets and
pins. Final close reclaims an unlinked inode if mutations are still permitted.
Failed cleanup leaves the durable registered intent for sync/recovery. Frozen
close only releases the pin and performs no media writes. Tombstones remain
allocated for the mount's lifetime, as in E4-A.
Mounted pins use VFS open-file references; the exclusive workbench's 16-token
handle pool is not used for mounted descriptors. Existing VFS resource bounds
and the profile's orphan/node/extent limits still apply.

## Durability and failure semantics

Existing `SYS_SYNC` and shutdown routing use the journal fixture only when no
production USB mount owns that route. Each operation synchronously commits,
checkpoints and retires its transaction. Mid-session sync drains reclaimable
orphans and issues the device barrier; it retains dirty + RECOVER and accepts
later mutations. Ordered journaling does not make overwritten file data atomic.

Shutdown first freezes mutations, drains unpinned orphan work, requires an
empty writer and orphan chain, and completes a barrier before its final clean
transaction. Success means clean state, RECOVER clear and an empty journal.
A pinned deleted inode returns EAGAIN with the mount frozen, dirty and
untainted. A subsequent frozen close performs no I/O; a later shutdown retry
can reclaim and finish. Staging read/OOM failure is write-free and retryable.
Uncertain write/flush or publication RMW-read failure taints permanently; further
sync/freeze calls return EIO without I/O. A durable prefix may already contain
the clean marker after a failed final barrier, but that failed call never
reports success or permits further mutations. Fresh validated recovery is
required.

## Verification

The host gate `make test-ext4-mount-host` uses actual EXT4/VFS/JBD2 code under
ASan/UBSan with a sector-atomic volatile/stable-media adapter. It covers all
1/2/4 KiB blocks × 512/4096-byte sectors × normal/circular-sequence-wrap
placements. Every activation, pending replay/orphan recovery and clean-transition
write/flush boundary is interrupted before and after with four persistence
profiles. Admission read/OOM failures, real open-unlink/dup/independent lifetimes,
sync followed by writes, frozen pins, permanent taint and eight pthread
write/freeze races per configuration are checked. The pthread adapter proves
host exclusion, not real IRQ/AP scheduling or hardware timing.

Six retained positive images per configuration are copied for independent
Linux journal/orphan replay, then unmounted `e2fsck -fn` exit 0 and exact
namespace/byte checks. The valid-journal/invalid-ownership negative fixture is
retained separately and never presented as Linux acceptance.

`make test-ext4-journal-mount` consumes completed host disposable fixtures,
copies the ISO and OVMF variables, creates isolated GPT/NVMe disks and rejects
extra storage arguments. BIOS/UEFI at each filesystem block size exercise two
boots: pending-journal/orphan recovery, actual VFS operations, Ring 3 SYS_SYNC,
later writes, SHA-256 and orderly shutdown, then persisted-byte verification.
Each stopped guest is independently checked for clean/RECOVER/orphan state,
unmounted fsck and exact Linux dumps. Every QEMU PID is reaped and logs/argv/
images are retained on failure. This is SMP=1 NVMe evidence, not journaled USB
or physical acceptance.

All focused gates PASS:

| Gate | Retained evidence / result |
| --- | --- |
| Mounted VFS host fault matrix | `host-frz8r6nu`: 12/12 configurations, **16,480 write/flush interruptions and restarts**, all mount/freeze read/OOM points, 96 pthread races. |
| Independent Linux host oracle | Same directory: **72 copies**, journal/orphan recovery followed by `e2fsck -fn` exit 0; exact namespace/bytes and original clean/RECOVER/orphan/JBD2-empty checks in `byte-audit-manifest.json`. |
| Final VFS coherence/lifetime smoke | `latest-vfs-nsoupoi6`: 12/12 lifecycle, recovery, admission, pin/freeze and concurrency cases under ASan/UBSan, plus forced directory growth during rename and **12 independent Linux copies**. |
| Real journaled VFS/SYS_SYNC/shutdown | `guest-2m64yyhp`: BIOS/UEFI × 1/2/4 KiB, **6/6 cases, 12 boots**, exact SHA-256 and 12 independent stopped-image Linux fsck/dump audits. |
| E4-A USB regression | `regressions/usb-run-18pfr4wh`: BIOS/UEFI × SMP=1/4, 4/4 cases, 12 persistence boots plus four immutable RO/degraded cases. |
| E4-A host writes/cache | `regressions/ext4-phase4/host-cykshea3`: six geometries, namespace/failure cases, Linux fsck and exact 1/16 MiB bytes. |
| JBD2 reader/writer regressions | `regressions/jbd2-replay/run-5pkndu79` and `regressions/jbd2-write/run-6vncf0bm`: 12/12 each, including writer credit exhaustion and 3,568 atomic cuts. |
| Transaction regression | `regressions/ext4-phase8-1/run-n5__b5d7`: 12/12 credit/ownership/sticky-stage/cache/taint/recovery foundation cases. |
| Other relevant checks | `legacy-host-regressions.log`: `test-ext4-read-host`, `test-ext2`, `test-usb-mount-host`, `test-xhci-bot-host` PASS; strict kernel/image build and scoped whitespace checks PASS. |

All paths in this table are relative to the retained, ignored workspace
directory `.codex-remote-attachments/ext4-phase8-5/`, outside `make clean`.
There are **84 independent Linux host copies** (72 lifecycle/crash + 12
directory-coherence), plus 12 stopped-guest fsck/dump audits. The original
pre-clean deep-orphan regression also passed 12 zero-write credit admissions,
337-extent depth-2 reclaim, 24 cuts and three Linux copies; those older build
artifacts were removed by concurrent cleanup and are not included in the
retained Phase-8.5 artifact totals.

| Block / sector | Activation + recovery + freeze cuts per placement |
| --- | ---: |
| 1 KiB / 512 B | 1,040 |
| 1 KiB / 4 KiB | 640 |
| 2 KiB / 512 B | 1,840 |
| 2 KiB / 4 KiB | 640 |
| 4 KiB / 512 B | 3,440 |
| 4 KiB / 4 KiB | 640 |

Each row runs both normal and circular/sequence-wrap placement. The host
manifest retains exact argv and source hashes. Linux audit invocation:
`python3 scripts/test_ext4_mount_host.py --audit .codex-remote-attachments/ext4-phase8-5/host-frz8r6nu`.
Latest-VFS smoke invocation:
`make test-ext4-mount-smoke-host EXT4_MOUNT_FIXTURES=.codex-remote-attachments/ext4-phase8-5/host-frz8r6nu`.
Guest invocation: `python3 scripts/test_ext4_journal_mount.py`, after the selected
disposable host profiles completed. Each guest manifest retains its ISO hash.

The retained exhaustive matrix includes the concurrent in-memory `/tmp` VFS
initialization. Legacy ext2/E4-A host fixtures now measure the actual root
allocation baseline and release unrelated empty root nodes, preserving exact
leak/failure checks. Final parent-cache refreshes were added afterward: the
focused 12-case coherence/lifetime matrix and complete guest rerun verify
those reads; activation/replay/freeze transaction code and fault boundaries
are unchanged. No exhaustive rerun of the unrelated `/tmp` feature is claimed.

Earlier successful artifacts in shared `build` and native `/tmp` disappeared
before retention. The final matrix and guest runs were regenerated directly in
the retained workspace directory; regressions were copied there before exit.
The original attempts are not added to these totals. No unrelated VFS/network/
shell changes were overwritten or committed by this task.

## Remaining limits

Phase 8.6 still owns the combined integration/metadata-coverage audit before
production journaled RW can be considered. Phase 9 owns the complete crash
campaign and E4-B physical acceptance. The bounded profile still excludes
orphan_file, hard links, directory rename, replacement rename and broader ext4
features. No root-filesystem switch, installer, format conversion or real-disk
write is authorized. Dell's accepted E4-A cache measurements and Mint/reboot
hashes remain separate physical evidence.
