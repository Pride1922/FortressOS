# Private retained call binding, cancellation and namespace workbench

2026-10-22. Extends [per-operation admission](PKG2_MUTATION_CONTEXT.md). This is
a copied-kernel host candidate. Production integration, installation and trust
provisioning stay disabled. No production TCB/header or filesystem code changes.

## Ownership and cancellation

`pkg_call_candidate.{h,c}` binds an owned admission to the current copied kernel
TCB. An exact overlay appends a private `package_call` field to the copied
`thread.h`; the on-disk production header is unchanged. The binding arena holds
at most eight records, with nonwrapping serials and immutable PID, admission,
mount/kind and kernel-context values. `pm_describe` supplies a checked value
snapshot before transfer. Start rejects a noncurrent task, nested binding, dead
registry identity, duplicate PID/key or arena exhaustion. A post-registration
registry check closes the registration/exit-notification race.

Successful start transfers an **unpinned or ready** admission, including queued
account priority. The current owner separately pins before filesystem work.
Failed start leaves ownership with its caller; the filesystem entry adapter
consumes its input admission and releases it on workspace OOM/start/pin failure.
Each package admission continues to retain its process-owned writer operation.
End clears the self-owned TCB pointer before recycling the binding, then unpins
and releases the admission outside the binding guard. No subsystem callback,
process query or filesystem operation runs under that rank-1 guard.

Only the current task may dereference or end its TCB binding. Remote cancellation
uses a numeric PID scan and atomic cancellation publication; it never retains or
dereferences another TCB or its context. PID allocation remains the kernel's
monotonic contract. A child must start with a cleared binding; the workbench's
actual process-birth helper zeroes its TCB. A copied foreign task/binding is not
usable as the current operation. This does not validate the full spawn/exec path.

The copied commit dispatcher looks up the retained binding through
`thread_current()`. Host TLS now delivers **only the current TCB**, not mutation
authority. Contexts still own their workspace and account R/G snapshots. The
actual process registry's copied abort/forget/accounted-exit hooks publish
cancellation after dropping its lock. Normal return and explicit owner unwind
acknowledge cancellation by ending the binding before TCB storage disappears.

Cancellation and commit acquisition use one atomic state word. Cancellation
before the commit CAS prevents the original semantic commit and returns EINTR
before its first device write. If commit wins the CAS, that bounded semantic
commit may finish; cancellation remains set and prevents the next commit. End
refuses while a commit is active. No filesystem-to-binding spinlock nesting,
asynchronous free, forced rollback or remote TCB teardown is introduced.

This is cooperative cancellation with retained acknowledgement, not forced
termination of a stalled device/caller. Multi-commit operations may already have
durable intent when cancellation blocks a later commit; restart/recovery remains
necessary. No new account-update interruption or arbitrary ordinary-orphan
recovery claim follows. Production thread-exit/reaper/exec wiring, real scheduler
delivery and an acknowledgement wait path remain outside this host candidate.

## Broader namespace coverage

The copied filesystem harness preserves the previous concurrent ordinary/package
and account workload, then exercises actual credentialed VFS create, mkdir,
write, truncate-to-zero, file rename, directory relocation, unlink and empty
rmdir. Two more simultaneously pinned callers create/write separate sandbox
files while account priority queues. A destination collision rejects EEXIST
without writes. Missing context, account/package paths and dot-component escape
routes reject without writes.

Header discrepancy: the older 8.3 summary in `ext4_engine.h` still says directory
moves reject; the mounted `ext4.h` contract and existing EH-6 implementation
support bounded directory relocation. This work exercises that existing mounted
behavior and does not change its production semantics.

The trusted workbench exposes only finite operations below prepared `/work/a`
and `/work/b`. It retains the real sandbox root/parent objects, validates every
path component and checks actual parent ancestry/mount identity under filesystem
exclusion before calling VFS. No sandbox-root relocation, symlink, hard-link or
external import is exposed. This is controlled namespace coverage, not a generic
untrusted-path confinement API or permission to mutate arbitrary directories.
Concurrent movement of the sandbox by an outside API is excluded.

Truncate and unlink use the real EXT4 orphan protocol. The earlier private
certificate rejected every orphan, blocking valid truncate cleanup. The new
overlay permits only one transient ordinary orphan matching the current
operation's captured real inode/generation. Account anchors/role inodes, package
record identities, the charged tagged file and unrelated orphan identities
remain rejected. It changes neither production orphan behavior nor the shared
certificate calculation. Ordinary budget checks still derive current account R
and durable G under filesystem exclusion and enforce F-G >= max(A0,R).

Cancellation tests run actual create/mkdir planners and package writes after
cancellation and require EINTR, zero device writes and no created objects.
Actual accounted exit cancels an owned ordinary binding; its owner unwinds before
the test TCB leaves scope. Workspace OOM releases queued account priority and
all volatile references. Cancellation during a held commit is tested with the
atomic protocol and pthread barriers in the standalone test, not by pausing
filesystem I/O under a spinlock.

## Evidence boundaries

Standalone ASan/UBSan and TSan tests use the actual process registry and copied
TCB definition. Filesystem tests use actual EXT4/JBD2, VFS, account service,
registry and extracted descriptor functions with host memory/device/current-task
delivery. TSan uses per-process `setarch x86_64 -R`; global settings stay unchanged.
Independent Linux checks require clean e2fsck, exact namespace/package/ordinary/
account bytes, absent cancelled/removed names and exact retained RESERVED grant
T=(8,1), G=(5,0), Q=(3,1), including record hashes.

Standalone targets in `tests/pkg_grant_candidate.mk`: `test-pkg-call-binding`,
`test-pkg-call-namespace`, `test-pkg-call-namespace-tsan`. Existing admission tests
remain a separate regression. Exact results, source preservation and retained
preliminary failures are in the roadmap. No new boot/media, Ring 3, scheduler,
IRQ/SMP, physical disk, low-space recovery or production admission acceptance is
claimed. Changes remain uncommitted.
