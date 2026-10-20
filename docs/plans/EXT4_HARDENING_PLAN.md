# EXT4 hardening for installed systems and package updates

**Status:** EH-1A host verified; EH-2 implemented with automated acceptance, 2026-10-19. User authorized starting this work independently
of TLS/HTTPS delivery. Implementation status and evidence must remain explicit.

Plan committed as `e34e628`. Timestamp implementation and bounded host
results are committed as `ce529c9` and recorded in
[EH-1A evidence](../roadmap/ext4-timestamps.md).
All six block/sector profiles and mounted short-extra-area failure preservation
pass; the 1 KiB ownership comparison defect is corrected. EH-2 now implements
additive one-file replacement; see [results and limits](../roadmap/ext4-atomic-replace.md).
Broader timestamp policy remains EH-1B. EH-3 durability/health/headroom has bounded
host/QEMU acceptance (2026-10-20), with a qualified concurrent-work preservation
guard on the broad account regression; see the [reviewed contract](EXT4_DURABILITY_HEALTH_DESIGN.md)
and [results and limitations](../roadmap/ext4-durability-health.md). Later phases
remain open.

## Objective

Strengthen the existing bounded ext4 implementation before package mutation:
correct metadata, safe regular-file replacement, clear durability/error APIs,
recoverable disk-full behavior and measured service responsiveness.

This builds on accepted JBD2, ownership/checksum validation, orphan recovery and
VFS lifetime contracts. It does not replace them or imply general Linux ext4
feature compatibility. See the [source audit](../roadmap/ext4-package-readiness-audit.md)
and [package-manager plan](PACKAGE_MANAGER_PLAN.md).

## Boundaries

- Preserve existing lock ranks, no-lock-across-switch, filesystem exclusion,
  journal ordering, cache publication, taint and DMA quarantine.
- Preserve raw-write gates, target identity/admission and test isolation.
- No release image creation, physical writes, push or automatic production
  rollout is part of this plan's implementation phases.
- Current no-replace rename and stat ABI layouts remain unchanged. New operations
  or metadata layouts are additive and require their own reviewed contracts.
- Kernel/root boot ordering, runtime namespace activation and lock/scheduler
  redesign remain protected changes requiring discussion before implementation.
- TLS work continues separately; tests must not depend on unfinished HTTPS code.

## Sequence

| Phase | Scope | Completion gate |
| --- | --- | --- |
| EH-0 | Baseline support/claim matrix, regression inventory and prerequisite contracts | Source pointers, current guarantees and evidence limits reviewed; no speculative runtime guarantees |
| EH-1A | Correct creation/write timestamp encoding and epoch extensions | Actual-code regression reproduces current defect; pure codec vectors and mounted Linux metadata oracle pass |
| EH-1B | Consistent mutation timestamp policy and optional additive stat/time APIs | Reviewed create/write/truncate/rename/chmod/chown policies; no existing stat layout change; host and Ring 3 tests for any new API |
| EH-2 | Atomic same-filesystem regular-file replacement | Reviewed namespace/inode/orphan/cache contract; old-or-new crash and concurrent lifetime tests pass |
| EH-3 | Durability/error API and recovery headroom | Explicit barrier semantics, late-error reporting and block/inode exhaustion recovery tests pass |
| EH-4 | Package-scale installed-root acceptance | Repeatable workload/reboot/interruption campaign, exact counters/bytes/modes and Linux audits pass |
| EH-5 | Measure latency and make narrowly justified optimizations | COMPLETE (32-block / 128 KiB staging batching in `SYS_STAGE_WRITE` 85 / `SYS_FSYNC` 86; 3.8–4.1× speedup; host ASan/UBSan 6/6 PASS; QEMU 8/8 PASS; SSH keystroke p95 55.5 ms < 100 ms SLA gate PASS on BIOS/UEFI SMP=4) |
| EH-6 | Selective application compatibility | Each directory-move/truncate-extension/link/feature addition gets a separate contract and acceptance gate |

EH-1A is the first implementation task. EH-2 and EH-3 are prerequisites for
package-manager target mutation; host package producers/verifiers can proceed
independently. EH-6 is not a requirement for the initial standalone-tool packages.

## EH-0: preserve and clarify the baseline

Record current journaled versus non-journaled behavior, root versus live mounts,
read-only/frozen/tainted states and exact supported geometry/features. Existing
bounded physical USB acceptance is distinct from installed NVMe persistence and
from a future NVMe power-loss campaign. Update present-status summaries without
rewriting historical evidence.

Inventory actual-code parser/allocator/namespace/orphan/lifetime/permission and
crash tests. Reuse their disposable fixture producers and independent Linux
oracles. A full historical matrix is not mandatory after every small edit;
choose relevant checks and broaden when changes cross a tested contract.

## EH-1A: timestamp correctness

Current source writes creation epoch seconds at byte 148 (`i_crtime_extra`)
instead of 144 (`i_crtime`). It also rewrites base mtime/ctime seconds without
resetting/encoding their epoch/nanosecond extensions. Fix these together using a
single explicit codec, not new scattered offset arithmetic.

Required contract:

- Define named offsets for atime/ctime/mtime/creation seconds and extra fields.
- Encode signed low seconds plus the two epoch-extension bits and valid
  nanoseconds according to the ext4 format; no uint32-only year-2038 workaround.
- Validate ranges and preserve output on rejected codec inputs.
- Respect each inode's `i_extra_isize`. Do not write an extension field outside
  its declared area or overwrite an inode's inline attribute bytes.
- For a short-extra inode, use a representable seconds-only timestamp or reject
  an unrepresentable mutation before any write. Freeze this policy in tests.
- Fresh 256-byte inodes declare their existing 32-byte extended layout before
  timestamp assignment. Creation time is set once and survives later writes.
- Clock source remains kernel UTC seconds; this phase adds no RTC writes, local
  timezone storage, per-read atime writes or inode-format conversion.
- Preserve checksum generation, current metadata transaction ownership and
  failure-before-publication behavior. Existing arbitrary-overwrite crash limits
  remain unchanged.

Tests:

1. Independent vectors: current time, signed-32 boundary, 2038, 2100, negative
   representable epochs, invalid nanoseconds and representational endpoints.
2. Reproduce old creation-offset failure against actual mounted creation code.
3. Create files/directories; write/append to an existing file; start with nonzero
   extension bits and verify stale bits are replaced, not inherited.
4. Test valid short-extra inode variants and rejection before mutation.
5. Decode final inode bytes independently and compare Linux `debugfs stat`;
   verify exact data, owner/mode and filesystem checksums/fsck on disposable copies.
6. Test relevant block/sector geometries. Add firmware/Ring 3 coverage where the
   path differs from the actual-code mounted host harness; no new physical claim.

EH-1B owns broader timestamp policy changes. Do not quietly turn EH-1A into a new
stat ABI, `touch` implementation or universal atime feature.

## EH-2: replacement design before implementation

Choose an additive operation after reviewing VFS, inode retention and the
filesystem's transaction boundary. Initial scope is regular files only on one
eligible journaled filesystem. Retain existing no-replace rename behavior.

The [EH-2 implementation contract](EXT4_ATOMIC_REPLACE_DESIGN.md) selects an
additive operation requiring an existing destination, with atomic activation
separate from deferred orphan cleanup. Runtime/ABI implementation and automated
acceptance are recorded in [EH-2 evidence](../roadmap/ext4-atomic-replace.md).

One bounded metadata transaction must account for source removal, destination
replacement, directory checksums, inode link counts, overwritten-file orphan
registration and in-memory publication. Complete staged contents must be durable
before activation. A retained open descriptor sees the original inode; later
lookup/spawn gets a complete admitted version.

Gate: enumerate write/flush/commit/checkpoint/orphan-replay cuts, inject both
before/after events and repeat recovery. Destination is old or new, never missing
or partially overwritten. Cover permission failures, protected account names,
same-inode/path, cross-directory, cross-device rejection, exhausted credits,
concurrent opens/spawn/unlink, pinned old inode and final reclamation.

Do not infer a multi-file/base-system atomic update from this primitive.

## EH-3: durability, health and space

The [EH-3 implementation review](EXT4_DURABILITY_HEALTH_DESIGN.md) retains
SYS_SYNC as the coarse barrier, defers descriptor sync after review, adds health
bits without changing mount-info layout, preserves close-cleanup errors through
sync, and enforces the installed account block/inode floor. Package reservations
remain a separate admission gate; free counters are not reservations. See
[automated evidence and exact scope](../roadmap/ext4-durability-health.md).
The [recovery-required follow-up](../roadmap/ext4-recovery-required.md) adds the
explicit stop-retry bit and bounded caller rules. Descriptor scope and latency
are reviewed separately in [the synchronization review](EXT4_SYNC_LATENCY_REVIEW.md);
no barrier removal or lock redesign is implied by a future fsync syscall.

Document existing synchronous mutation and global `SYS_SYNC` guarantees and fix
stale comments. Review descriptor-level file/directory synchronization separately;
an initial coarse implementation may reuse global barriers, with honest cost and
failure propagation. File data durability and directory-entry durability must
not be conflated.

Audit late I/O/checkpoint/flush errors through write/close/sync/shutdown/recovery.
Define how users see tainted/failed health independently of a nominal RW flag.
Never claim successful cleanup merely because close has no error return channel.

Free block/inode reporting already exists through `SYS_MOUNTINFO`. Add package
recovery headroom policy and, if necessary, a reviewed reservation mechanism;
observing free space is not reserving it. Preserve account recovery headroom.
Test block-full/inode-full, concurrent logs, interrupted cleanup, cache saturation,
short writes and exact reclamation. Protect pending recovery material from cache
eviction and new transactions.

## EH-4 and EH-5: realistic workload, then performance

The [staging/batching design review](EXT4_STAGING_BATCH_DESIGN.md) specifies bounded
synchronous staging batches (`SYS_STAGE_WRITE` 85) with a 32-block (128 KiB) maximum ceiling
and scoped mount barrier `SYS_FSYNC` (86). Multi-firmware prototype benchmarking is
recorded in [the prototype evidence](../roadmap/ext4-stage-prototype.md), showing a
3.8–4.1× speedup and 8× barrier reduction.

EH-5 interactive SLA validation is **COMPLETE**: verified via `make test-ssh-stage-sla`
(`scripts/test_ssh_stage_sla.py`), holding SSH keystroke p95 latency to ~55 ms (budget <100 ms)
and command execution to ~35 ms (budget <500 ms) on BIOS and UEFI under SMP=4 during active
128 KiB staging operations. Evidence recorded in `.codex-remote-attachments/ssh-sla-20261020-204817/`
and `.codex-remote-attachments/ssh-sla-20261020-204907/`.

EH-4 package-scale installed-root acceptance is **COMPLETE**: designed in
[the package-scale design review](EXT4_PACKAGE_SCALE_DESIGN.md) and validated 100% PASS across
all 4 matrix configurations (BIOS SMP=1, BIOS SMP=4, UEFI SMP=1, UEFI SMP=4) on native installed-root
disposable NVMe fixtures (`make test-ext4-pkg-scale`). Demonstrates:
1. Multi-block directory scaling (128 entries in `/opt/pkg/catalog` expanding directory blocks)
   verified by `SYS_READDIR` (6) in-guest and Linux `debugfs` post-shutdown.
2. Synchronous 32-block staging writes (`SYS_STAGE_WRITE` 85).
3. Live running binary atomic replacement (`SYS_RENAME_REPLACE` 84) with open VFS node
   retention and concurrent background worker execution.
4. Clean shutdown and post-boot host Linux Oracle audit: `e2fsck -fn` 0 errors, `debugfs`
   byte-exact hash identity. Evidence recorded in [EH-4 evidence](../roadmap/ext4-package-scale.md).

## Deliverables and work discipline

- Commit this plan and its planning references separately from runtime fixes.
- Keep EH-1A implementation/tests/evidence reviewable as a separate change.
- Leave concurrent TLS edits out of ext4 commits and tests' release artifacts.
- Add meaningful focused test targets; record exact commands/results and limits.
- Stop at the reviewed phase boundary when the next phase changes a protected
  contract or requires an unresolved API decision; complete independent work first.
- No physical interruption campaign without an identified disposable target and
  case-specific authorization. No package readiness claim before its gates pass.
