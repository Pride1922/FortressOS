# Bounded synchronous staging batches — review proposal

2026-10-20. **Production API proposal; no production API number allocated.**
The user authorized a release-excluded prototype of this contract. Its bounded
implementation, verification and remaining rollout gates are tracked in the
[prototype evidence](../roadmap/ext4-stage-prototype.md). No production ABI,
deferred writeback or protected locking redesign is delivered.
Based on the [initial EH-5 baseline](../roadmap/ext4-latency.md), the
[durability/health contract](EXT4_DURABILITY_HEALTH_DESIGN.md), and the
[descriptor-sync review](EXT4_SYNC_LATENCY_REVIEW.md). EH-1B stays open.

## Recommended first change

Collect staging bytes in a bounded userspace buffer and submit one larger,
explicit synchronous batch. Commit and checkpoint that batch with the existing
JBD2 sequence before returning a positive count. Keep ordinary SYS_WRITE and
its 16 KiB limit unchanged. Do not introduce a persistent kernel dirty buffer,
leave a JBD2 transaction open across syscalls, or change lock ownership in this
first step. This is a proposed additive operation, not a global limit increase.

The baseline pays six barriers per staging write. A simple contiguous 4 KiB
filesystem could submit eight current 16 KiB chunks in one 128 KiB batch, reducing
640 transactions/3,840 barriers to 80/480 for 10 MiB. Those are conditional
counts, excluding file creation, tail handling, logging, sync and activation;
not measured performance or a general credit guarantee.

This refines the earlier suggestion of accumulating writes in the kernel.
`ext4_engine` and `jbd2_writer` each hold one shared transaction workspace.
`e4_file_plan()` reloads home inode/mapping bytes and starts a fresh plan;
simply calling it repeatedly does not accumulate a coherent transaction.
Keeping that shared writer in STAGING between user calls would deny unrelated
mutations. Dropping e4_lock would not make that state available to other writers.
Private deferred buffers would require new logical-size/read visibility, dup,
close/death, global sync/freeze, resource ownership and late-error contracts.
Avoid that complexity until evidence justifies it.

## Alternatives and tradeoffs

| Mechanism | Benefit | Limitation / decision |
| --- | --- | --- |
| SYS_FSYNC alone | Select the opened object's filesystem and report its errors | Cannot remove staging barriers already paid; separate additive API below |
| Raise global SYS_WRITE limit | Fewer transactions for all callers | Expands every backend's contract; not selected |
| Explicit synchronous staging batch | Fewer plans, commits and checkpoints without delayed-success semantics | Longer data transfer in one lock hold; selected for a disposable prototype first |
| Deferred per-file buffers | Can coalesce ordinary small calls | New memory, visibility, cancellation and lifecycle semantics; deferred |
| Shared group commit / delayed checkpoint | Could amortize barriers across files | Changes shared journal, recovery and cache-publication ownership; deferred |
| Unlock around polling I/O | Could improve foreground scheduling | Requires transaction/device ownership, sequencing and freeze redesign; separate protected review |

Fewer barriers do not guarantee shorter maximum lock holds. The 100 ms-per-flush
host fixture still implies roughly six such delays for one batch. More data per
transaction can worsen one caller's stall even while total staging accelerates.
If responsiveness fails, do not keep increasing the batch or claim fsync fixes
it: reject rollout and review ownership/IRQ exclusion separately.

## Proposed batch interface and admission

Working name: `SYS_STAGE_WRITE(fd, buffer, length, expected_eof, flags=0)`.
The name is provisional; allocate a number only after ABI review. Existing
syscalls and structures retain their numbers/layouts and behavior.

- Require a live writable regular-file descriptor, a healthy journaled ext4
  mount, and a linked, singly linked inode. Reject removed/replaced nodes,
  directories, streams, nonjournaled/other backends, and protected account
  inodes. No fallback through ordinary write, raw blocks or unlink/rename.
- Append-only: under e4_lock revalidate inode identity/generation, current EOF
  equal to expected_eof, and shared file offset equal to expected_eof. Reject
  O_APPEND for this initial explicit-offset operation. EOF/offset mismatch is
  a documented pre-I/O EAGAIN; it does not authorize blindly retrying with a
  larger offset. The package must establish exclusive staging ownership.
- This API does not prove that a filename is private, acquire the package
  writer lease, reserve package space, or make other writers impossible.
  Packages use a fresh file in their controlled staging directory; all identity,
  namespace and lease checks remain package protocol responsibilities.
- Validate 1..128 KiB input and zero flags. Validate/copy the complete supplied
  range into a bounded heap buffer before filesystem exclusion; no user pointers
  or large stack buffers reach planning/device I/O. Snapshot actor credentials
  and retain the file object through callback and reference release.
- Initial syscall error precedence: missing/out-of-range or nonwritable fd
  gives EBADF; invalid flags/length or overflowing/unrepresentable requested
  range gives EINVAL; invalid input mapping gives EFAULT; snapshot allocation
  failure gives ENOMEM; backend/type/state checks follow. Unsupported eligible
  access types/backends give EOPNOTSUPP. This additive ordering does not alter
  SYS_WRITE's existing ordering or zero-length behavior.
- Use the existing credential-aware content-mode policy, account protection,
  mount health/freeze checks, orphan fences and account block/inode floor.
  Root and capabilities cannot consume account recovery headroom through this API.

Dispatch through new explicit credential-aware backend batch and sync callbacks
or equivalently typed public adapters. Do not identify ext4 by casting arbitrary
fs_private pointers, and do not route the batch around VFS descriptor access
checks. Unsupported callbacks fail closed. These are internal kernel interface
additions; existing user ABI structures stay unchanged.

Select at most **32 filesystem data blocks** per call, capped at 128 KiB.
An unaligned EOF may consume one extra partial-block image: count actual touched
blocks and shorten the selected prefix so at most 32 are touched. Tail writes
preserve and zero bytes according to the existing planner. Remaining bytes are
not accepted. Userspace loops on a positive short count with the returned offset.

| Filesystem block size | Maximum aligned batch | Conditional 10 MiB transactions / barriers |
| --- | --- | --- |
| 1 KiB | 32 KiB | 320 / 1,920 |
| 2 KiB | 64 KiB | 160 / 960 |
| 4 KiB | 128 KiB | 80 / 480 |

The engine has **64 combined data/metadata images**, not 64 images of each kind.
Thirty-two data images leave at most 32 images for inode, extent tree, bitmap,
descriptor and superblock edits. Fragmentation, tree rebuild/revokes, group
crossing or journal credits can still reject. Plan the selected prefix once,
reserve all actual credits and JBD2 copies before I/O, or fail unchanged.
No adaptive retry with smaller plans inside this first implementation. A healthy
pre-I/O EFBIG/credit failure can end the batch attempt; a caller may explicitly
switch to ordinary bounded writes only after revalidating that no prefix was
accepted and health/identity/EOF still match. Never fall back after uncertain I/O.

## Transaction, publication and failure contract

Use one planned append, one ordered-data commit, and one checkpoint per accepted
prefix. Do not concatenate multiple existing committed write callbacks and call
the result one transaction. Do not bypass the actor-aware path or accidentally
retain the existing 32 KiB clamp in the new internal path. Keep the clamp for
all old mounted/workbench/account/handle APIs.

| Boundary | Required observable result |
| --- | --- |
| Validation, allocation, identity, planning or credit denial before I/O | Negative error, zero accepted bytes, unchanged disk/offset/namespace; destroy the private plan and release references/buffer |
| Commit/checkpoint success and coherent node refresh | Positive exact prefix count; its data/length/inode changes are checkpointed; update shared descriptor offset under existing exclusion |
| Write/flush/checkpoint uncertainty, or failed publication refresh after durable commit | EIO with existing taint/recovery-required behavior; the disk may contain a committed prefix; do not infer rollback or retry |
| Crash before completion | Recovery may expose the old durable EOF or the committed new prefix; no complete-file or multi-file atomicity claim |

Append-only admission avoids overwriting already published staging bytes through
this API. It is not a general guarantee for other descriptors or in-place writes.
Normal readers/fstat see only checkpointed inode length and bytes. There is no
uncommitted in-memory length to merge, no dirty buffer after return, and no
pending batch to flush on close. Read/exec of a partially built named file remains
possible if caller permissions allow it; keeping it private is mandatory policy.

Hold the existing e4_lock across planning, polling I/O, cache invalidation,
checkpoint and publication. Never sleep, enable IRQs or acquire another rank-1
lock there. SYS_STAGE_WRITE performs at most one transaction; it does not loop
over the complete user buffer under one exclusion window. Existing freeze/raw
ownership/taint/recovery contracts remain intact. Process death after I/O cannot
cancel or roll back a transaction; standard filesystem recovery decides the disk.

Only per-call heap snapshots survive user-copy preparation. Initially require
one in-flight snapshot per process and a bounded aggregate of four 128 KiB
snapshot credits (512 KiB maximum), acquired without nested rank-1 locks and
released on every success/error/exit path. Exhausted credits give documented
pre-I/O EAGAIN; no new wait queue or unbounded retry. This cap excludes existing
EXT4/JBD2 workspaces and transaction image copies, which need separate measured
heap accounting and OOM tests. Do not claim a 512 KiB total kernel footprint.
The active call owns its snapshot credit until I/O has finished and reference
cleanup completes. Signal/death handlers cannot free or recycle that active
snapshot; process teardown must follow the existing in-flight-call lifetime
rules. Admission and release must not nest process/filesystem rank-1 exclusion.

## Descriptor synchronization and activation

Separately propose `SYS_FSYNC(fd)`, selecting the retained object's journaled
ext4 filesystem. Permit readable or writable regular-file and read-only
directory descriptors. A borrowed fd lookup alone is insufficient across a
signal/wait/lifetime boundary; acquire/release a real operation reference using
the existing file-reference protocol, without holding process exclusion while
entering filesystem exclusion.

EBADF precedes unsupported type/backend EOPNOTSUPP; for supported mounts report
existing EIO/EAGAIN/ENOMEM/EROFS state/drain errors without suppression or
automatic retries. No user output pointer or structure is involved. Directory
support means its namespace edits are included in the selected filesystem's
drain/barrier, not an invented independent directory flush command.

The first implementation may reuse the selected mount's ext4_sync, including
eligible orphan drain, acknowledgement of saved close errors and a device
barrier. Those errors can originate in other objects on the same filesystem;
there is no per-inode historical-error ledger. Do not drain unrelated mounts.
Concurrent writers after exclusion may immediately change state. Frozen mounts
may be synchronized without unfreezing; permanently tainted ones return EIO.
If final operation-reference release triggers later orphan cleanup, its errors
follow the existing close-health contract, not retroactive fsync failure.

Package ordering: finish all batches; set final metadata; fsync the stage; read
back/hash a stable exclusively owned file and close it; revalidate ownership and
source identity; activate using EH-2 replacement or no-replace rename for a new
name. Synchronize the required parent directories as specified by the package
protocol. Current namespace operations already checkpoint their edits; fsync
does not create multi-file atomicity. Persist package intent and installed-record
ordering under the separate package recovery design. Any uncertain operation
uses the existing bounded recovery-required caller policy and stops activation.

## Prototype gates and rollout decision

Before a production ABI, implement a release-excluded disposable prototype and
collect a like-for-like baseline, including ordinary writers and readers:

1. All 1/2/4 KiB blocks x 512/4096-byte sectors, aligned/unaligned tails,
   fragmented mappings, group/tree boundaries, max credits, zero/short/oversize
   inputs, OOM/ENOSPC/inode floor, account denials and exact reclamation.
2. Retained fd/dup offsets, close/death/unlink/rename/replace/freeze races,
   shared-descriptor competing writes, actor/set-id rules, bounded snapshot
   saturation and every error/reference cleanup path.
3. Before/after every write and flush event, all existing persistence modes,
   wrapped journal and two recoveries with exact allocation ownership. Linux
   fsck/debugfs and exact old/new EOF, bytes, metadata and account-floor audits.
4. Descriptor sync argument/type/error ordering, directory publication across
   parents, retained object after rename/unlink, delayed final close, saved
   cleanup errors, failed flush/checkpoint, unrelated mount nonparticipation.
5. Disposable installed-root BIOS/UEFI SMP=1/4 on NVMe/USB: 10 MiB staging,
   logging, isolated reader latency and real SSH request latency. Guest TSC
   calibration independent of IRQ-driven uptime, host round-trip observations,
   repeated runs, slow/failed flushes and snapshot exhaustion. Preserve release
   outputs and unrelated TLS/HTTPS sources; no physical acceptance implied.

Proposed review thresholds, not accepted product guarantees: eightfold lower
staging transaction/barrier count on the aligned contiguous 4 KiB fixture;
no weakened durability/error/ownership result; no more than 10% median regression
in isolated-reader or SSH p95 latency against matched repeated baseline runs.
Report maxima and absolute values too. The following absolute targets are
proposed acceptance gates in addition to those relative/durability checks;
they are not measured results or delivered product guarantees.

### Proposed absolute responsiveness and completion budgets

| Measurement under concurrent package staging | Proposed target | Measurement boundary |
| --- | --- | --- |
| SSH keystroke latency, p95 | **<100 ms** | On an established interactive SSH session, host monotonic time from sending a uniquely identifiable keystroke until its terminal echo is received; include SSH/network/scheduling delay |
| SSH command execution, p95 | **<500 ms** | On an established SSH connection, host monotonic time from command submission to complete output and exit status; freeze a typical-command corpus before testing, including small directory listing, small-file read and process listing; report each command separately |
| Complete 10 MiB package install | **<10 seconds** | Local signed package already available; from install invocation through verification, staged writes, required synchronization, activation and durable installed-record/intent completion |
| Complete 100 MiB package install | **<60 seconds** | Same complete-install boundary, with a fixture large enough to preserve account headroom plus package staging/recovery/log budgets |
| Concurrent logrotate | **<60 seconds per scheduled cycle**, no missed cycle | Fixture schedules a rotation every 60 seconds; measure from scheduled due time through successful rotation and configured compression/cleanup, including exclusion waits; verify resulting bytes and health |
| Concurrent crond tick jitter | **<100 ms maximum absolute deviation** | Compare observed daemon tick times with their intended schedule using an independently advancing calibrated clock; do not use delayed IRQ tick delivery as the reference clock |

The previous 9.69–11.21-second guest observation measured a 10 MiB staging probe
with a reader/logger peer, not a complete package install. It neither proves
nor disproves either proposed install target. Report staging-only and complete
install durations separately. The installed-record/intent protocol does not
exist yet, so a prototype lacking it can only pass the staging subtest; the
complete-install gates remain NOT RUN and cannot justify package rollout.

Apply the targets separately to each admitted BIOS/UEFI, SMP=1/4 and NVMe/USB
configuration; never average a slow configuration into a passing one. Freeze
package layout/file counts, compression, command corpus, network setup and
fixture geometry before comparing baseline and prototype. Exercise both payload
sizes with SSH, scheduled logging/rotation and crond running concurrently.
Sustain or repeat staging load for at least ten 60-second rotation intervals;
a single ten-second staging run is insufficient to establish the daemon gates.
Collect at least 1,000 keystroke samples, 100 executions per command and ten
install repetitions per size/configuration. Report per-run p95, maxima, install
duration of every run, sample counts and all missed daemon events. Every install
repetition must meet its completion target; each run's p95 must meet the SSH
targets. Do not pool away a failed run or omit timeouts, dropped sessions,
skipped rotations or missing tick samples. Missing evidence is NOT RUN, not PASS.

Measure crond dispatch lateness and scheduled-job start lateness separately from
tick jitter so deferred work cannot appear healthy merely because the daemon's
tick observation is timely. QEMU timing establishes acceptance only for the
recorded emulation setup. Physical rollout needs these same targets verified on
the intended hardware through separately authorized physical fixtures; passing
QEMU does not authorize physical writes or demonstrate hardware responsiveness.

Any target miss is a documented **FAIL** that blocks rollout of that candidate
in the affected scope. Preserve its raw logs, matched baseline and workload;
the next review must identify and address the responsiveness or completion gap.
Do not silently relax a threshold, discard a failed run, or trade away durability
to obtain a timing pass. Slow/failed-flush cases remain mandatory diagnostics:
mark target misses as failures for those profiles and retain the no-rollout
result there, even when the unchanged baseline also fails. An injected fault's
safe error/recovery PASS is separate from responsiveness/completion acceptance.

Passing all budgets justifies the performance portion of rollout review only
after the crash, ownership, health, space, API and package-protocol gates above
also pass. It does not by itself approve shipping SYS_STAGE_WRITE or packages.

If larger batches fail responsiveness, reduce the trial cap and repeat. If
barrier latency dominates even at the old cap, the next review is device and
transaction ownership outside IRQ-save exclusion, not further batching. Do not
remove a JBD2 barrier based solely on a lower flush-count target.

## Implementation review points

- `src/fs/ext4_mutate.inc`: shared 64-image engine, planner reset, allocation
  floors, rebuilt extent trees and staged images.
- `src/fs/ext4_write.inc:e4_file_plan`: plans one coherent write from home bytes.
- `src/fs/ext4_transaction.inc:e4_file_write_actor`: old 32 KiB clamp, actor
  policy, preflight/staging, commit, offset publication and abort/invalidation.
- `src/fs/ext4_mount_journal.inc:e4_jwrite`: mounted exclusion and refresh/taint.
- `src/fs/jbd2_write.inc`: single writer, bounded copied images, four ordered
  commit barriers plus two checkpoint barriers for this data-write workload.
- `src/fs/vfs.c` and `src/fs/vfs.h`: serialized shared offsets and owned file refs.
- `src/kernel/syscall.c:sys_write`: old validation and pointer contract, unchanged.

This review does not implement or authorize deferred writeback, barrier removal,
new lock ranks, I/O outside exclusion, boot ordering, DMA lifecycle changes,
physical writes, a release build, commits or pushes. The user requested the
staging/batching review and the gated prototype. Production API/runtime delivery
remains subject to the acceptance gates above. This does not close EH-4, EH-5
or package-manager prerequisites.
