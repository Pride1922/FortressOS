# EH-3 durability, health and recovery space

2026-10-20. Implementation contract; acceptance is recorded separately in
`docs/roadmap/ext4-durability-health.md`.

## Barrier/API review

Journaled mutations stage bounded images, write ordered data, commit JBD2, and
checkpoint home metadata before publishing caches/returning success. Each
positive write count covers only that returned prefix (the mounted callback
accepts at most 32 KiB; SYS_WRITE rejects requests above its unchanged 16 KiB
limit). Callers must loop on short writes. In-place overwrite
is not an atomic file update; replacement is the separate EH-2 operation.
Successful namespace mutation includes directory metadata durability. A failed
checkpoint can follow a durable commit: EIO means uncertainty, never rollback.

SYS_SYNC calls storage_sync_all: installed root and external mount are attempted
separately, even if the first fails. The first error wins. ext4 drains eligible
orphans and completes a device flush under existing exclusion; pinned zero-link
files remain valid and do not make mid-session sync fail. Sync does not freeze
writers or clear RECOVER. A later writer can mutate immediately after exclusion
ends. With no writable mount, the existing fixture fallback returns EROFS.
Nonjournaled ext4 has barriers but no journal crash-atomicity guarantee; the
legacy ext2 external fallback is a device flush, not an ext4 health guarantee.

Descriptor file/directory sync was reviewed and is **deferred**: today's writes
and directory edits already synchronously checkpoint, and SYS_SYNC supplies a
coarse explicit barrier. Adding fdatasync/fsync numbers now would imply backend,
descriptor lifetime, directory-open and per-object late-error semantics not
needed by EH-3. A future additive API must retain the fd object across dispatch,
reject unsupported streams/backends, distinguish file bytes from name
publication, specify global-barrier cost and test Ring 3 argument precedence.
No existing syscall number, argument or structure layout changes here.
The follow-up [descriptor sync and latency review](EXT4_SYNC_LATENCY_REVIEW.md)
separates fd scope from the shared device barrier and records the measurement
and locking gates. No fsync syscall or barrier optimization is implemented yet.

Close releases references; its void backend callback cannot return cleanup
failure. Preserve the first such error in the mount and fence new mutations
with the existing recovery-required EAGAIN until drain completes. Sync retries cleanup but
reports that saved error once even if retry succeeds, before clearing it. A
failed retry preserves the error; taint remains sticky until reboot. Freeze
does not mark clean on the acknowledgement call; a subsequent explicit retry
can finish. Shutdown already refuses a clean-success claim on freeze error.

## Passive health

Add independent MOUNT_FLAGS bits in the existing mount_info.flags word:
TAINTED, NAMESPACE_UNCERTAIN, CLEANUP_PENDING, CLEANUP_ERROR and
RECOVERY_REQUIRED. RW remains the
nominal policy bit for compatibility, not proof that mutation is admitted.
Reserved stays zero; size/offsets and stat ABI stay unchanged. `disk info`
prints health alongside policy. Query takes existing ext4 exclusion, allocates
nothing and performs no I/O, including on failed devices. Tainted free counters
are only the last published checkpoint snapshot. Cleanup pending includes
healthy pinned files and is not itself an I/O error. Uncertainty fences only
the existing EH-2 namespace admission; retained-read policy is unchanged.

## Recovery-required flag and caller rules (follow-up)

`MOUNT_FLAGS_RECOVERY_REQUIRED=32` is set when the engine's
`orphan_recovery_required` predicate is true, or when the mount is tainted or its
replacement namespace is uncertain. The passive snapshot reads those predicates
under the same filesystem exclusion used to reject mutation. Healthy pinned
orphans alone set CLEANUP_PENDING, not RECOVERY_REQUIRED. A frozen mount and an
open replacement source can also produce EAGAIN without this flag.

A checkpoint/write/flush uncertainty taints the mount and returns EIO; it does
not become an endlessly retryable EAGAIN. Close has a void callback and returns
no cleanup error itself: failed cleanup publishes the recoverable predicate and
saved error before unlocking. Subsequent namespace/file mutations encounter
EAGAIN while that predicate remains true. A failed drain keeps it set; only a
successful drain clears the recoverable predicate. Permanent taint/uncertainty
keeps RECOVERY_REQUIRED set until reboot recovery.

Separate syscalls cannot pair a historical error atomically with a later health
snapshot. Another thread can drain between EAGAIN and SYS_MOUNTINFO; another can
fail after a healthy snapshot. The guarantee is that an outstanding recovery
fence is visible in a snapshot serialized with that fence, not a promise that a
later query always reproduces the original error's cause. Admission always
rechecks. Missing health information or an absent flag is not proof of success.

PKG-2 caller policy, before package mutation is implemented:

| Observation | Required caller action |
| --- | --- |
| TAINTED or NAMESPACE_UNCERTAIN, with any return code | Abort the package attempt, preserve recovery material, issue no mutation retry. Require reboot through admitted recovery. If that fails, use separate offline maintenance; never run a repair writer on the live mounted filesystem. |
| RECOVERY_REQUIRED without either fatal bit | Stop retrying the mutation. At most one explicit SYS_SYNC drain per package invocation, followed by a fresh health query. A missing query, failed drain or remaining flag aborts the attempt and reports the state/error. |
| Drain reports the saved close error but health becomes healthy | The earlier error is acknowledged, not erased. Abort the current package attempt; a fresh invocation must revalidate package intent, descriptors, file identities and namespace. Never reinterpret an earlier EIO as rollback or blindly replay activation. |
| EAGAIN without a recovery/fatal bit | Retry only if the operation's documented pre-I/O denial and a known progress condition permit it. Close owned staging descriptors or redo a retired-dentry lookup as applicable. At most two total attempts, with one 20 ms wait if waiting is appropriate. Otherwise abort with a contention/admission diagnostic. |
| RO/frozen policy, unsupported backend or failed/missing health query | Abort; waiting does not make the operation admitted. |

Even if a one-shot drain returns zero and clears the flag, the initial PKG-2
policy ends that package invocation and requires revalidation on a fresh attempt.
This avoids inventing an automatic multi-file resume protocol. The retry limit
bounds issued calls/waits; it does not preempt a synchronous device operation or
promise a 20 ms syscall latency. No unbounded retry loop, automatic fsck, generic
EIO retry, or permanent-failure downgrade to EAGAIN is permitted.

## Recovery space policy

Once account service setup completes on a trusted installed root, ordinary
allocations must leave at least 256 KiB of free blocks and 16 free inodes.
Enforce on staged superblock totals for each allocation, including extent nodes
and directory growth, under e4_lock. Fail ENOSPC before I/O and discard the plan.
Root/capabilities and logging cannot bypass the floor. Only fixed-role account
create/write/rename callbacks select a scoped bypass while holding the same lock;
they clear it before publication/unlock. Reclamation needs no free allocation.
The account headroom preflight now also checks inodes. Existing mounted trees
below the floor remain readable/reclaimable: this is not a mount rejection.
This floor does not reserve heap/cache/journal credits or orphan slots and cannot
repair I/O. Fully pinned bounded pools can still reject; recovery may require
releasing those pins or reboot. No unconditional low-memory progress is claimed.
Mount-info free counts remain raw filesystem free counts, including the floor;
they are not ordinary-writer available counts. No counter/structure ABI changes.

Package staging must leave this account floor **plus** its own worst-case
staging, activation, rollback and log budget in blocks and inodes. Free counts
alone are not a package reservation. No package allocator/transaction exists
yet, so no unreviewed generic reservation ABI is introduced: package mutation
must remain unavailable until its owner can serialize budget admission and
competing package/log allocations or implement a separately reviewed reservation.
Live/disposable /mnt mounts retain their existing exhaustion behavior.

Pending orphan intent remains on disk and in the bounded engine array, independent
of dentry eviction. Existing recovery-required checks fence new transactions;
pins/generations prevent premature reuse. Interrupted cleanup is retried through
the same bounded right-edge algorithm on sync/reboot, never unlinked twice.

No timestamp-policy, lock rank, boot ordering, physical-write gate, release
packaging, TLS or account-name protection change is included.
