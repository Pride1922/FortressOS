# Package writer, reservation and durable-state review candidate

2026-10-21. Design only. No syscall number, kernel implementation, root admission,
boot ordering, lock rank, allocation bypass or package mutation is authorized by
this document. Builds on [PKG-1](PKG1_DESIGN.md) and existing EH-2/3/5/6 contracts.
Target mutation remains disabled until the decisions and acceptance gates below
are resolved. Account recovery remains a separate fixed-role protocol.

## One writer with lifetime ownership

Recommend a kernel-owned package session handle, effective UID 0 and existing
CAP_SYS_ADMIN required at acquisition and each mutation. Exactly one session on
the boot-admitted installed journaled root; no live `/mnt`, ETCFS, raw block,
read-only, frozen, tainted or namespace-uncertain fallback. A writable live `/etc`
does not establish installed-root identity or package eligibility.

Proposed handle properties:

- Session belongs to retained internal task/process identity plus generation;
  never use a user-written PID/boot timestamp as authority. Acquisition cannot
  succeed until the previous owner and all retained in-flight operations finish.
- Nontransferable: dup and descriptor passing reject; spawn inheritance excludes
  it regardless of caller flags. Children cannot use another owner's descriptor.
  A descriptor integer alone is insufficient authority. Exec/self-update remains
  unsupported; any future exec must close the session before image replacement.
- Close/death cancels future work. In-flight synchronous operations reach their
  established storage outcome; never revoke a durable decision halfway through
  checkpoint or reinterpret successful activation as failure. Reuse waits for
  final operation reference release. Do not sleep under filesystem exclusion.
- Ordinary close before durable intent releases unused volatile reservation.
  After intent, release leaves recoverable pending state and blocks a fresh
  install until exclusive recovery succeeds. The lock record is diagnostic only.
- Dropped credentials cannot mutate through an old handle; cleanup/release must
  still be possible without restoring capabilities. Repeated cancellation and
  teardown are idempotent. No automatic lease timeout or stale-file deletion.

Internal synchronization uses existing filesystem rank-1 exclusion for session/
budget state. No persistent spinlock held across syscalls or scheduling. Process
death must publish cancellation/detach outside process/scheduler lock nesting;
retain resources before resolving files and release after filesystem exclusion.
Exact task-reference ownership, last-close callback and reaper sequence require
source-level review before implementation. No external process-generation ABI
or public boot-id syscall is presumed.

Recommend explicit **lease-bound dispatch** for allocation/publication operations,
using retained session and filesystem transaction context. Existing 84/85/86
numbers/arguments and ordinary VFS behavior stay unchanged. The dispatch may
reuse their backend planners after normal permission/health checks; it must not
infer a reservation from UID, PID copied into userspace, or matching filename.
Returning tagged staging descriptors and binding subsequent writes/activation
to their retained session requires review of fd duplication, owner death, close,
offset and source-closed requirements. Generic package paths are not sufficient
authorization. No hidden account bypass or ambient reservation on all root I/O.

## Allocation reservation and account priority

See [account recovery compatibility](PKG2_ACCOUNT_COMPATIBILITY.md) for the
source trace, isolated protocol measurements and proposed independent-headroom
rules. Existing raw-free preflight and scoped bypass do not yet enforce them.

Existing `ext4_mutate.inc` checks the ordinary account floor under filesystem
exclusion; `ext4_root_account_headroom` reports that floor independently.
Preserve at least ceil(256 KiB / block size) free blocks and 16 free inodes.
Only existing fixed-role account callbacks retain their scoped bypass.

Reservation arithmetic must be checked in filesystem blocks/inodes, not artifact
bytes or raw mount free counts. Admission accounts for artifact quarantine,
authoritative staged files, separate activation copies, next inventory, immutable
intent, retained metadata, directory growth, extent nodes, recovery workspace,
old pinned generations, logs and cleanup effects. Credit, heap, dentry and orphan
capacity are independent bounded resources; successful space admission does not
reserve those or guarantee device I/O success.

Candidate ledger: total package grant, consumed blocks/inodes, remaining grant
and durable-intent phase, protected under existing filesystem exclusion. An
ordinary competing allocation must leave account floor + remaining package
grant; a tagged package allocation atomically consumes its own grant while still
leaving account floor. Never count a granted block both as free credit and as
already consumed. Reject the complete allocation plan before I/O on underflow,
overflow, missing owner/generation or insufficient credits. Planner abort restores
only provisional accounting; uncertain committed I/O is reconciled by recovery.
Unlinking package data does not automatically replenish arbitrary grant amounts:
reconcile actual owned allocation changes to avoid double credit/refund.

Account recovery has priority. Its existing floor bypass may consume available
headroom; it must not silently invalidate a promised package recovery grant.
If both cannot be honored, package admission fails before intent. An unreviewed
new allocator exception must never be inserted into account recovery to solve a
package liveness problem.

### Critical cross-boot limit

The [source-backed recovery-space review](PKG2_RECOVERY_SPACE_REVIEW.md)
finds concrete post-intent directory/extent allocation paths, compares both
directions and recommends developing a persistent mount-admitted grant with
prepared publication objects. Account-priority compatibility and atomic durable
spending remain blockers; this recommendation authorizes no kernel changes.

A volatile lease/reservation cannot protect free space after owner death/reboot.
Releasing the grant and letting logs consume it can strand a valid pending
transaction. Preflight free counts or re-acquiring a lease does not solve this.

Two viable directions require separate approval/evidence:

1. Persist/admit a validated package recovery grant before ordinary allocation
   resumes on reboot, with exact accounting against journal replay. This touches
   protected mount/root preparation and needs explicit discussion. Package
   records are never interpreted as arbitrary block ranges or account roles.
2. Prove that every post-intent recovery step is allocation-free using complete
   preallocated authoritative and publication/state objects, stable namespace
   capacity and bounded credit availability. Current rename/replace primitives
   and package-only serialization do not establish that proof. Other root
   namespace writers and pinned pools must be included.

Until one passes, recovery may stop safely with pending preserved under ENOSPC;
there is **no guaranteed autonomous recovery progress**. This limitation blocks
production installation acceptance, even if all pure validation tests pass.
Explicit manual recovery avoids boot-order changes but does not reserve space
before logs start. Do not claim both manual-only integration and guaranteed
cross-boot reservation without an additional mechanism.

## Canonical persistent objects and publication

Fixed root-owned namespace on the admitted installed mount:

```
/var/lib/pkg/state.v1
/var/lib/pkg/trust.v1
/var/lib/pkg/pending.v1
/var/lib/pkg/transactions/<kernel-validated-id>/
```

Private directories 0700, records 0600; regular singly linked files, no links or
foreign ownership. Base/protected account/host-key paths excluded. Validate all
ancestors/mount identities and retain objects while operating. A package lease
alone does not fence an arbitrary privileged writer: lease-bound namespace
exclusion, or a documented privileged-writer threat boundary, must be chosen.
Never adopt an unexpected file after checking only its pathname.

Canonical encoding requirements before serializers are implemented:

- Each object has a unique 16-byte role/version, repository UUID, explicit
  total length, fixed-width LE integers, zero reserved fields, bounded sorted
  records, exact EOF and SHA-256 over its canonical body. Hash is corruption
  detection, not authentication against root; no native structs on disk.
- `trust.v1`: exact authenticated root/catalog identities, versions and retained
  signed objects; max catalog 256 KiB, root 8 KiB. Never reduce floors on removal,
  cache eviction or failed refresh. Equal version requires exact object hash.
- `state.v1`: monotonic nonzero generation, predecessor state hash, repository/
  trust identities and <=128 sorted package ownership records (<=256 KiB).
  Each binds build, full artifact hash, command and <=16 derived paths with
  size/hash/mode/uid/gid. No shared ownership, arbitrary paths or base adoption.
- `pending.v1`: <=16 KiB immutable intent, operation install/update/remove, session
  transaction identity, exact old/new state hashes and generations, selected
  trust/catalog identity, package/build and staged object roles/hash/metadata.
  No unconstrained paths, shell commands or absolute disk addresses. Admission
  binds this intent to the reserved/prepared transaction directory.

Candidate common record header (192 bytes): role/version[16] at 0, UUID[16] at
16, total_bytes u32 at 32, zero flags u32 at 36, generation u64 at 40,
predecessor whole-object SHA-256[32] at 48, root version u64 at 80, catalog
revision u64 at 88, catalog whole-object SHA-256[32] at 96, body_bytes u32 at
128, record_count u16 at 132, zero u16 at 134, body SHA-256[32] at 136 and zero
reserved[24] at 168. Require total_bytes=192+body_bytes, exact EOF, nonzero
generation, role-specific size/count caps and no arithmetic wrap. Predecessor is
zero only for explicitly provisioned generation one. SHA-256 over whole header
and body identifies an object for its successor and pending intent.

Candidate state body: sorted length-prefixed package records. Each starts u32
record_bytes, u8 name_bytes + name, u64 build, artifact SHA-256[32], u8
command_bytes + command, u16 file_count, then file records in canonical path
order. File record: u8 role, u16 mode, u32 uid, u32 gid, u64 file_bytes,
SHA-256[32], u16 relative_path_bytes + relative path (<=100). Derived destination
policy remains authoritative. Counts, name grammar, root ownership, role/modes,
single executable and duplicate/ancestor checks match PKG-1; no implicit padding
or unconsumed record bytes. Record length includes its own four bytes.

Trust body candidate: u32 root_object_bytes, u32 catalog_object_bytes followed by
exact signed objects. Its independent cap is 192+8+8192+262144=270536 bytes.
No installed inventory is needed to advance a trust floor. Pending's body grammar
must be finalized against lease-bound object-role identifiers before coding;
do not let a guessed pathname substitute for that session contract.

Independent serializer/parser vectors and role domain strings remain a review
gate; these candidate offsets are not a frozen disk ABI. Checksummed records alone do not
authorize mutation: recovery re-verifies retained signed authorization, base
ownership and current/staged identities.

Prepare complete replacement records on the same mount, verify/read back, close
sources, fsync selected mount. Absent initial state uses no-replace SYS_RENAME;
existing state uses SYS_RENAME_REPLACE 84. Failed flush/checkpoint means uncertain,
not rollback. Authoritative stage survives source-name consumption. Trust-floor
publication must be durable before installing anything newly authorized.

Post-intent order: publish optional command, sync/verify; publish next state,
sync/verify; remove pending, sync; then bounded garbage collection. Retain stage
and next state until pending retirement is durable. A reader sees last complete
state but must report pending; it cannot claim state/command atomic visibility.
Install/remove/upgrade of a batch remains per application, not all-or-nothing.

Recovery compares exact old/new state and command bytes/metadata. Expected old
state/command rolls forward; new command with old state finishes state; complete
new state/command retires pending. Foreign content, missing authoritative stage,
invalid authorization, unexplained generation or corrupt intent stops with
diagnostic and pending intact. Repeat recovery after every recovery cut. Cleanup
is restartable and never deletes material still needed by pending intent.

## Review decisions and acceptance gates

Decisions before target implementation: nontransferable lease-bound dispatch,
task lifetime/reaper integration, cross-boot reservation strategy, privileged
namespace-writer boundary, trust-floor/state wire encodings, platform/base
descriptor authority and manual versus early-boot recovery. No approval is
inferred from compiling the read-only CLI.

Actual-code host adapters must test two simultaneous acquisitions, owner/child
descriptor misuse, PID reuse, death before/after every publication, blocked or
failed close/drain, credential drop, competing logs/accounts, block/inode/credit/
cache/orphan exhaustion and exact reservation accounting. Enumerate write/flush
cuts across intent/command/state/trust/cleanup and repeat replay twice. Verify
old/new data, full metadata, ownership/allocation sets, account floor and unrelated
files. A safe pending ENOSPC failure is separate from a recovery-progress PASS.

Then private installed NVMe regular-file fixtures, BIOS/UEFI x SMP=1/4, isolated
probe, validated QEMU argv every boot, bounded waits, two recoveries and Linux
e2fsck/debugfs byte/metadata audits. Include real SSH loss/signals and retained
open/exec behavior. No release media, physical disk, raw-write-gate changes,
EH-1B policy change or physical power-loss claim.
