# Private durable account-service and redo integration

2026-10-23. Extends [the paired-record primitive](PKG2_ACCOUNT_DURABLE.md).
Implemented only by exact overlays of copied source and a host fixture adapter.
Production account service, boot ordering, syscalls, installation and production
trust provisioning remain unchanged and disabled for PKG. No release build,
commit, push or physical device write is part of this work.

The private probe calls the actual `accounts_prepare`, account-control
BEGIN/UPDATE/FINISH, account redo engine, detached-root account namespace
admission, VFS, EXT4 and JBD2. The fixture contains coherent locked-root-only
passwd/group/shadow databases; the updated files are each 8192 bytes, including
valid comment padding. The root home is an actual root-owned mode-0700 directory.
Neither synthetic trusted flags nor temporary root publication are used.
Enrollment uses public test keys, an externally supplied descriptor hash/policy,
and actual inode identities, maps and allocation/ownership audits. Other grant,
ownership, transaction/trust resources remain synthetic fixture placeholders.
This is not complete PKG grant/state/trust admission or production enrollment.

## Separate generations and durable decision

The existing operation generation advances on BEGIN and identifies an interrupted
writer operation. A separate durable credential generation restores the actual
account service's authentication generation at fresh-process admission. They must
not be interchangeable. RAM writer PID/token state never survives a restart.
The service remains exclusive through its existing operation gate; filesystem
transactions keep the existing rank-1 exclusion. No lock rank is changed.

Private generation-record version 2 retains the 1024-byte pair, sequence,
operation generation, enrollment identities and origin hash. Its additional fields
are little endian:

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 144 | 8 | Nonzero credential generation |
| 152 | 8 | Issued target credential generation, or zero |
| 160 | 2 | Redo phase: 0 pre-intent, 1 pending, 2 cleanup |
| 162 | 6 | Zero |
| 168 | 160 | Complete planned redo record, or zero |
| 328 | 96 | Three old/current canonical SHA-256 hashes |
| 424 | 12 | Three old/current canonical lengths |
| 436 | 556 | Zero |
| 992 | 32 | SHA-256 of preceding bytes |

No format migration from version 1 is implemented. The private enrollment's
profile hashes remain synthetic test-policy values; they are not hashes of a
production account-service implementation or a provisioned trust contract.

After validating an UPDATE, the actual service increments credential generation.
A paired JBD2 transaction persists that generation, the old database hashes, and
the complete planned intent before any redo staging. The actual intent.tmp to
pending rename joins the phase-1 pair update in its own semantic transaction.
Pending unlink joins phase 2 and its exact ACTIVE origin witness atomically.
All eleven account roles use actual fixed-role removal callbacks and actual
inode generation, owner, parent and allocated-range checks. Final orphan inode
retirement clears its witness in the same transaction. Namespace abort clears
the volatile paired-staging tag, preventing reuse by a later transaction.

Successful redo removes staging, refreshes canonical hashes and clears the target
and intent while leaving the operation OPEN. FINISH closes it to IDLE. Recovery
continues the existing operation, validates/cleans the actual redo files, then
closes it before enabling authentication. An already-IDLE second recovery leaves
both durable counters unchanged. Conservative process-exit handling disables
authentication and drops volatile writer ownership without I/O. Cancellation at
the winning final IDLE commit still returns interruption; authentication requires
a fresh admission attempt even when disk cleanup is already complete.

## Read-only preview and legitimate restart states

Before replay, fixed enrolled resources must retain their exact identities and
maps. Canonical and temporary account files must have valid modes, root ownership,
one link, bounded dense mappings and distinct inode identities. The actual
filesystem constructor audits global allocation and ownership separately.

Pre-intent canonical files must match the saved old triple. Pending must match
the complete issued intent, including credential generation, lengths and hashes;
all three staging files must be complete, hash-matching and coherent. A present
canonical file must match its old or new version. A canonical name may be absent:
redo can remove an existing .publish temporary during a second interrupted
recovery before creating its replacement. The complete bound stage triple is
required in that case; requiring a linked temporary would strand a valid restart.
Cleanup requires the complete new canonical triple. IDLE disallows all staging,
publish and intent roles. ACTIVE origin must name the sole actual orphan, whose
fixed role is absent and whose remaining allocation is within the recorded map.

## Verification and limits

`make -f tests/pkg_grant_candidate.mk test-pkg-account-service-durable` builds
only copied source under ASan/UBSan with strict warnings. The runner enumerates
UPDATE semantic commits across 1/2/4 KiB filesystems and 512/4096-byte sectors.
Each UPDATE cancellation blocks subsequent writes/flushes and authentication,
then a fresh process recovers, and a second fresh process verifies idempotence.
Recovery is separately interrupted at its first semantic commit and resumed
once, with an independent Linux audit of the resulting IDLE database.
Actual constructor orphan cleanup precedes arming the service cancellation
counter; it is not counted as a service redo commit. Existing paired-primitive
callback-cut evidence remains separate.

`scripts/test_pkg_account_service_redo.py EVIDENCE_DIRECTORY` enumerates every
recovery commit from three additional 4 KiB fixture states: complete intent.tmp,
group.publish and shadow.publish. Every directed recovery cancellation is then
recovered in two fresh processes, with fsck and a zero-commit second recovery.
This covers later cleanup callbacks that a first-commit cut cannot exercise.

The supplemental `scripts/test_pkg_account_service_power.py EVIDENCE_DIRECTORY`
recompiles an instrumented copy of the probe and cuts before/after every callback
of the pending-publication transaction, under cached/all/alternating/inverse
sector persistence. It checks fresh-process recovery twice and Linux fsck.
Resealed negative records and modified actual redo payloads must fail before any
write/flush, release constructor ownership and leave the input bytes unchanged.
Its inputs explicitly identify the base private build and current fixture include;
base copied dependencies are hashed before/after. No production source is edited.

Linux `e2fsck -fn` and independent Python/debugfs canonical bytes and sealed IDLE
records audit service recovery images. Supplemental power cuts use fsck and the
probe's exact canonical-family checks. Neither gate proves torn-sector recovery,
real hardware ordering, controller faults, physical power loss, low-space
progress or allocation/pool/credit exhaustion. Semantic cancellation is injected
at durable commit boundaries, not real signals or scheduler teardown. Host VMM
range checks are stubs. There is no Ring 3, QEMU, boot, media-build, kernel
lifecycle or physical acceptance claim. Source-copy and shared-media preservation
are reported independently from runtime results. Exact results and retained
failures are recorded in the roadmap after testing.

The next private layer replaces those package placeholders with the bounded
[pending/state/trust mount profile](PKG2_MOUNT_PENDING_TRUST.md), including actual
grant ownership refinement and version-3 account snapshot binding. Its acceptance
and limitations are separate from this version-2 service campaign.
