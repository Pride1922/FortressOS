# Private per-operation mutation context and concurrent admission

2026-10-22. Extends the private [service integration](PKG2_PRIVATE_SERVICE_INTEGRATION.md)
and [capacity preview](PKG2_ACCOUNT_PREVIEW.md). All implementation is in private
test sources and copied-source adapters. Production integration, installation
and production trust provisioning remain disabled; changes stay uncommitted.

## Admission and lifetime

`tests/pkg_mutation_candidate.{h,c}` implements an eight-slot arena. A key names
one generation and slot; generations never wrap or reset on mount changes.
Only a quiescent arena can change mount identity. Ordinary, package and account
entry points are trusted internal issuers, not a user-supplied classification ABI.
The account issuer requires a trusted account-service adapter; it does not itself
authenticate a user. Actual account control still checks its credentials, token
and service generation.

Pinning returns an immutable classification/mount view. A pinned slot cannot be
finished or reused. Package admission first retains the existing process-owned
writer operation, then enters the arena; failed admission releases that reference.
Completion releases it after dropping the arena guard. An admitted package call
can finish after owner exit and final lease close, while new admission fails.
Durable grant ownership is not erased by process death.

Seven ordinary/package admissions leave one slot for account priority. A queued
account blocks new ordinary/package admissions and pins, waits for existing pins
to drain, and then pins exclusively. Already pinned operations are not revoked.
Unpinned account cancellation releases priority; stale keys cannot reuse it.
This is a nonblocking retry protocol, not a scheduler wait queue. A caller that
never releases its pin can prevent progress; bounded cancellation and actual
kernel lifecycle binding remain future work.

The rank-1 arena guard owns values only. No filesystem/process/scheduler lock
may be held at API entry. Writer retain/release and callbacks run outside it;
the filesystem lock is never nested with it. Simultaneous admission does not
make filesystem transactions parallel: the existing filesystem lock serializes
planning, fresh budget checks and semantic commit.

## Private actual-code adapter

`tests/pkg_mutation_integration.inc` delivers a pinned view through host thread
local storage. TLS is a host delivery adapter, not production authorization or
a retained TCB binding. Each call owns its certificate workspace, account R/G
snapshot, grant sequence/full-ledger hash and mutation target. The copied commit
dispatcher uses this view rather than the historical global lane switches.
Old baseline helpers remain compiled for fixture setup; they are not dispatched
for the concurrent workload. Shared ledger/table/marker scratch remains protected
by the actual filesystem lock.

Account callbacks read the real grant and capacity certificate, track required
capacity across successful semantic commits, and rederive it after each backend
operation. The grant identity must match the call snapshot. Account exclusion
spans callback gaps; a paused real account update rejects fresh mutations and
canonical account readers. Ordinary commit rederives current R/G under filesystem
exclusion and applies F-G >= max(A0,R). Package commit retains the existing tagged
allocation/ownership validation. Missing contexts, cross-kind account control,
cross-target writes and a grant overspend reject before device writes.

The workload deliberately covers one prepared ordinary file, one fixed tagged
file, and actual account begin/update/finish. Namespace enrollment and initial
writer acquisition use trusted **quiescent** setup helpers. This does not admit
arbitrary namespace operations or establish concurrent enrollment/acquisition.
The seven-block overspend append stays within the backend's 32 KiB bound while
exceeding an eight-block grant after two blocks were charged. It is not a Ring 3
SYS_WRITE boundary test. Host-only stack scratch is not kernel stack evidence.

## Validation and remaining scope

Standalone admission tests exercise 128 rounds with four simultaneously pinned
pthread workers, priority/cancellation, reserved capacity, stale/mount rejection,
failed-retain cleanup and actual process-registry owner death. Filesystem tests
use actual EXT4/JBD2, account service, process registry and descriptor close
hooks in copied-source host builds. Two package and two ordinary writers race
through serialized filesystem commits; a real account update pauses between
callbacks to check exclusion. An already admitted package operation writes after
actual process exit/lease close, then drains all volatile references.

Independent Linux oracles require clean e2fsck, exact ordinary/package blocks,
exact canonical account bytes, ledger/table digests and final T=(8,1), G=(5,0),
Q=(3,1). The final RESERVED grant is intentionally retained, not retired to IDLE.
Copied compiled-input and repository compiled-input preservation are checked
separately from runtime and Linux results. Exact campaigns and preliminary
failures are recorded in the roadmap.

Run the standalone targets in `tests/pkg_grant_candidate.mk`:
`test-pkg-mutation-admission`, `test-pkg-mutation-context`, and
`test-pkg-mutation-context-tsan`. TSan uses per-process `setarch x86_64 -R` to
avoid its observed WSL shadow-mapping initialization failure; no global setting
is changed.

No new interruption sweep, near-full filesystem admission, charged-orphan
recovery, kernel scheduler/IRQ/SMP or Ring 3 result follows. Low-space account
priority policy, authenticated enrollment, complete pending/state/trust admission,
retained kernel call binding and general namespace coverage remain open. No boot,
release-media build/preservation audit, production integration, installation or
physical disk acceptance is claimed.
