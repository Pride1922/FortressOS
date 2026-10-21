# PKG-2 durable grant and mount admission — candidate

2026-10-21. Design only: no disk ABI is frozen and no production caller,
provisioning path, allocator change or boot integration is implemented here.
Installation and production trust provisioning remain disabled. This refines
[the recovery-space review](PKG2_RECOVERY_SPACE_REVIEW.md), not an acceptance
claim for the proposed mechanism.

## Authority and explicit enrollment

A grant belongs to an enrolled filesystem and transaction UUID, never a PID,
descriptor or lease. Process death does not release it. Mount-lifetime fences
protect the account namespace, package metadata, transaction slot and admitted
destination parents even when the ledger is idle.

Use a future, explicitly recognized `boot_mode=installed-pkg-v1`, together with
a separately validated readiness-v2 selector binding installation ID, filesystem
and partition UUIDs, enrollment UUID, marker hash and compatible kernel/bootstrap
hashes. This is a proposal, not a supported boot argument. The current parser
rejects an unknown boot-mode value but ignores other unknown tokens: adding only
`pkg_policy=required` would not exclude an old kernel. Existing readiness-v1 is
not this new authority. Selector integrity outside ordinary root VFS writes and
the upgrade/rollback policy require review before implementation.

| Trusted selector | Reserved enrollment artifacts | Result |
| --- | --- | --- |
| Legacy | All verified absent | Existing legacy admission |
| Required | Complete, valid, identity-matched | Continue protected admission |
| Either | Unreadable, partial, malformed or mismatched | Independent recovery |
| Legacy | Any enrollment artifact present | Independent recovery |
| Required | Absent | Independent recovery |

Absence must be distinguished from lookup/I/O failure. Never infer a zero grant
from missing `pending.v1`, a missing ledger or a failed parser. No writable
fallback, target-root login, live ETCFS substitution or automatic repair is
permitted on admission failure. Independently authenticated bootstrap recovery
remains the route; its authority must not depend on the rejected root database.

Future enrollment first prepares and durably reads back the fixed root metadata,
namespace and idle ledger, then commits the required boot selector. Intermediate
cross-media states fail closed; there is no cross-device atomicity claim. This
does not prevent a boot administrator selecting an old kernel with a legacy
command line, or offline rollback of the whole filesystem. Deployment exclusion
of those paths needs its own reviewed policy; no hardware antirollback is claimed.

## Fixed storage and canonical encoding

Candidate paths under `/var/lib/pkg` are `enrollment.v1` (512 bytes), `grant.v1`
(1024 bytes), `ownership.v1` (8192 bytes), and one precreated reusable directory
`transaction.v1/`. This replaces the earlier candidate dynamically allocated
`transactions/<id>/`; the ledger binds the slot to a transaction UUID. There is
one active transaction. `state.v1`, `trust.v1` and `pending.v1` retain their
semantic roles; their publication grammars must be bound to this design before
use. Their initial authenticated baseline is not assumed to exist today.

All integers are little-endian, hashes SHA-256, UUIDs exact 16-byte identities;
unknown flags, nonzero reserved bytes, wrap, short reads and trailing bytes fail.
Records are root-owned, mode 0600, single-link regular files; private directories
are mode 0700. Marker and ledger each occupy one initialized filesystem block
on supported 1/2/4 KiB geometries. The ownership table has a dense preallocated
immutable map, at most four extents, no holes, unwritten extents or allocation
beyond EOF. Inode generation, map ownership and aliases must be checked, not just
pathname metadata. These baseline allocations cannot recursively spend a grant.

Candidate marker layout (offsets in bytes):

| Offset | Field |
| --- | --- |
| 0 | 16-byte magic `FORT-PKG-ENR-v1` followed by one zero byte |
| 16 | u32 length=512; u16 version=1; u16 flags=0 |
| 24 | u64 nonzero enrollment epoch |
| 32,48,64,80,96 | filesystem, partition, installation, repository, enrollment UUID |
| 112 | 32-byte resource-profile hash |
| 144,152,160 | u32 inode/u32 generation pairs: ledger, ownership table, transaction slot |
| 168 | u32 minimum boot contract=2; u32 profile version=1 |
| 176 | 304 zero bytes |
| 480 | SHA-256 of bytes [0,480) |

Candidate ledger layout:

| Offset | Field |
| --- | --- |
| 0 | 16-byte magic `FORT-PKG-GRT-v1` followed by one zero byte |
| 16 | u32 length=1024; u16 version=1; u16 phase |
| 24,32 | u64 nonzero sequence, enrollment epoch |
| 40,56,72,88 | transaction, enrollment, filesystem, repository UUID |
| 104,136,168,200,232,264 | 32-byte hashes: resource profile, intent, old state, new state, authorization snapshot, ownership table |
| 296,304 | u64 total blocks, total inodes |
| 312,320 | u64 remaining blocks, remaining inodes |
| 328,336 | u64 charged-live blocks, charged-live inodes |
| 344 | u32 account profile; u16 operation (0 idle, 1 install, 2 update, 3 remove); u16 flags=0 |
| 352 | u32 transaction-slot inode; u32 generation |
| 360,368,376,384 | u64 old/new state generations, trusted-root version, catalog revision |
| 392 | 32-byte admitted-plan hash |
| 424 | u64 completed-step bitmap (at most 64 typed steps) |
| 432 | 560 zero bytes |
| 992 | SHA-256 of bytes [0,992) |

The ownership table has a 256-byte header: 16-byte `FORT-PKG-OWN-v1` magic
with one zero byte; u32 length=8192; u16 version=1; u16 count<=64; u64 epoch;
transaction UUID; u64 sequence; enrollment/filesystem/repository UUIDs; profile
hash; zeros through offset 256. Then 64 fixed 64-byte entries: u16 typed role,
u16 class, u32 inode, u32 generation, u32 bounded flags, transaction UUID,
u64 logical size, u64 actual allocated blocks, u64 allocated inodes (0 or 1),
eight reserved zero bytes. Entries are role-sorted; unused entries are zero.
There are 3808 zero bytes after the entries and a hash of [0,8160) at 8160.
Classes are EMPTY, CHARGED, BASELINE_VICTIM and RETIRED tombstone. Reject duplicate
roles/inodes, overlapping maps, unrecognized role/flag combinations and stale
generations. A role is a kernel-validated object identity, not an arbitrary path
or caller-supplied block address. Ledger binds the complete table hash; table
sequence matches ledger sequence. No cyclic hash dependency is needed.

Checksums detect corruption; they do not authenticate a maliciously rewritten
disk. Authority derives from reviewed enrollment, trusted selector, signed
package admission and enforced fences. Sequence numbers reject local stale
pairings, not complete filesystem rollback. Journal recovery selects one complete
logical state; do not pick the highest valid record from unrelated copies.

## Conservation and admission

For blocks and inodes independently, let F be actual free resources, G remaining
escrow, Q charged-live resources and T the outstanding grant total. Require
`T = G + Q` with checked arithmetic. Q counts actual allocated data, extent-tree
blocks and inodes, not rounded file size. Only wholly new grant-owned objects
are CHARGED. Mixed baseline/grant growth and adoption of an existing object are
forbidden in this first design.

| Committed operation | F | G | Q | T |
| --- | --- | --- | --- | --- |
| Allocate d to charged object | -d | -d | +d | unchanged |
| Actually reclaim charged q | +q | +q | -q | unchanged |
| Reclaim baseline victim q | +q | unchanged | unchanged | unchanged |
| Transfer published charged t to baseline | unchanged | unchanged | -t | -t |
| Release unused r | unchanged | -r | unchanged | -r |

Refund only once, after actual unpinned reclamation, in the same journal commit
as bitmap/table/ledger changes. Unlink alone does not refund. Transfer does not
create free space. Release requires proof that the remaining typed recovery plan
fits G; owner death and last close are not such proof.

Ordinary and package plans require, after the complete plan,
`F_after - G_after >= max(A0, R_acc(next))`. A0 is the existing ordinary floor
(256 KiB/16 inodes); R_acc is the validated remaining account-recovery cost for
the exact admitted namespace/state. Account operations may consume A0 but must
preserve `F_after - G_after >= R_acc(next)`. The current scoped account allocator
bypass does **not** enforce this: adding G only to the ordinary path is unsafe.
BEGIN, UPDATE and boot redo must all use effective free resources excluding G.
Legacy roots keep their existing policy. No production R_acc bound is frozen by
the earlier finite models; an actual-planner refinement proof remains required.

Ledger and ownership-table updates must be journal metadata images in the same
transaction as corresponding allocation, free, orphan and phase changes. Normal
ordered file writes or `SYS_STAGE_WRITE` followed by fsync are insufficient.
Preflight includes these extra images plus cache, orphan, revoke and log credits;
do not silently raise the current 64-image ceiling. Uncertain commits fence the
mount until recovery resolves the complete before/after state.

Namespace capacity is a separate resource. The existing opt-in account scanner
does not certify package parents. A typed plan must certify protected destination
and private-parent capacity without creating a placeholder at an absent command
name before intent. Existing baseline-parent growth is forbidden until provenance
accounting is separately designed. The 64-role table is a hard representation
bound, not proof that every 16-file archive/deep path fits. Reject unsupported
layouts before intent. PKG-1 currently permits EH-6 relocation only for absent
immutable documentation generations; this design does not authorize application
directory swaps or directory exchange.

## Durable phases and authorization

* IDLE (0): operation/transaction and T/G/Q are zero; table has no outstanding
  ownership. Old/new state hash and generation both identify current state.
  Namespace fences remain active.
* RESERVED (1): atomically reserve G, bind transaction, old state, resource and
  namespace plan and initialize table. Final intent/new-state hashes are zero
  until prepared. Fresh objects are created only through typed grant operations.
  Death retains escrow; pre-intent cleanup must durably free/transfer all tagged
  objects before returning to IDLE.
* PENDING (2): publish immutable authenticated intent and transition together.
  Require exact old/new hashes, generations, authorization and plan binding.
  Recovery recognizes only reviewed old/new publication states; a consumed source
  with the exact destination may advance. Missing both never triggers payload
  regeneration. Extra allocation must already fit the admitted plan and G.
* RETIRING (3): exact new command/state verified; pending retirement and phase
  transition are atomic. Only cleanup remains, with provenance and authorization
  retained in the ledger. Pinned victims delay reclamation and block a fresh
  transaction. Return to IDLE only after charged objects are transferred or
  reclaimed, cleanup is complete and remaining escrow is durably released.

Checksummed but semantically impossible phase/role/hash tuples are invalid.
New transaction admission requires IDLE, no pending record and no residual tags.
Sequence overflow refuses before I/O. Trust/state baseline updates need their
own typed publication rules; arbitrary file replacement is not authority.

Candidate recovery policy: a valid clock and signed catalog authorize admission
of one exact immutable intent. That durable capability may finish that same
intent after catalog expiry; it cannot authorize a new download, changed payload,
new transaction or trust rotation. Trust rotation is excluded while pending.
This policy needs security review, including explicit revocation and unsupported
policy versions (route to recovery while preserving G). No production keys or
trust provisioning are supplied by this document. Eventual I/O completion,
available bounded runtime resources and eventual release of pinned victims are
explicit liveness assumptions, not guaranteed by disk escrow.

## Mount sequence and failure routing

1. Validate independent boot selector/readiness/bootstrap credentials and root
   identity. Reject enrollment ambiguity before root writes.
2. Retain current read-only JBD2 preview and allocation/reference ownership
   checks. Validate marker, ledger, table, phase and all referenced identities
   against that preview **before the first actual replay write**. Failure routes
   to independent recovery, not a best-effort zero-grant mount.
3. Replay coherent journal transactions; reopen and verify matching identities,
   table hash and sequence. Reconstruct volatile G and role provenance before
   journal activation and orphan cleanup. Activation must have admitted baseline
   credits; it must not allocate unaccounted package resources.
4. Perform grant-aware orphan cleanup. Charged and baseline orphans have distinct
   refund behavior; ledger/table updates accompany actual reclamation atomically.
   This must precede neither provenance reconstruction nor preview validation.
5. On the detached trusted root, revalidate and arm account and package namespace
   fences, capacity certificates, authorization and account recovery headroom.
   All grant checks must be active before `accounts_prepare`, whose callbacks
   can create directories and run allocating redo.
6. Recover accounts, rederive headroom and validate mandatory system execution,
   credentials and package semantics. Publish the writable root only after all
   admission checks pass. No target logging or Watchtower allocations intervene.

The current constructor performs replay, journal activation and orphan cleanup
before returning the detached root; the public account namespace admission API
requires that detached trusted journal root. Therefore steps 2–4 need a reviewed
internal read-only validator and constructor hooks. Do not fake trusted flags,
call the public API too early or publish a root temporarily to satisfy it.
Diagnostics must distinguish refusal before replay writes from failure after
recovery writes. Neither case permits writable fallback.

Current installed roots can have `/var` mode 0700; the account namespace profile
requires 0755 ancestors. Such roots are ineligible under that profile. Enrollment
must explicitly resolve the policy mismatch in a separate review; do not silently
chmod or relayout a user's root. Structural admission does not authenticate the
account redo contents or package database.

## Review gates and evidence limits

Before implementation: review boot authority and failure routing; numeric account
and package planner bounds; destination capacity/provenance; fixed format/parser;
atomic ledger and allocation integration within existing credit ceilings; account
bypass enforcement; capability expiry/revocation; session ownership and protected
operations. This document authorizes none of those production changes.

`scripts/test_pkg_grant_design.py` is a finite arithmetic and ordering model.
It checks scalar block/inode conservation, whole-transaction restart selection,
refund/release/transfer distinctions, mount policy and required ordering, with
negative controls. Its small integers are not production quotas. It executes no
EXT4/JBD2 C code, parser, boot or Ring 3 workload; it cannot prove actual planner
coverage, flush ordering, authentication, SMP safety or liveness. Existing namespace
and account evidence remains separate. Future acceptance must cut every real
write/flush and recovery boundary, reboot twice, exercise competing allocation
and retained handles, and audit Linux ownership/fsck/bytes across BIOS/UEFI ×
SMP=1/4. Boot results and source/media preservation must be reported separately.

## Private ledger adapter checkpoint

`tests/pkg_grant_candidate.{h,c}` implements the candidate ledger's exact-size
little-endian structural decoding, SHA-256 check, reserved-byte and phase-field
checks, conservation and failure-preserving outputs. Its encoder validates and
copies an already canonical wire record; it is not a native-object serializer.
Only candidate account profile 1 is recognized. Sequence UINT64_MAX remains
readable, but a mutating arithmetic plan refuses it. RESERVED permits neither
completed steps nor a final next-state generation. PENDING/RETIRING require the
next generation to equal old+1 without overflow. These are private candidate
restrictions, not a frozen on-disk ABI.

The two-dimensional planner preflights charge/refund/transfer/release/baseline
free/ordinary allocation, capacity and recovery floor, including partial-dimension
failure. The caller must establish actual maps, typed ownership, phase authority,
remaining recovery work and unpinned reclamation separately. This adapter does
now also implements the private record cross-binding described below, but not
namespace lookup, signatures, filesystem exclusion or allocation. A valid checksum or budget never
establishes authority to mount or mutate. There is no production linkage.

Run in WSL: `make -f tests/pkg_grant_candidate.mk test-pkg-grant-candidate`.
The standalone makefile intentionally avoids the concurrently edited release
Makefile. The runner generates independent Python wire fixtures, builds the C
adapter under ASan/UBSan and compiles that adapter alone with freestanding flags.
Byte-tear checks reject mixed records unless bytes equal a whole valid outcome;
mock restarts select whole in-memory budgets before/after commit. Neither check
implements or verifies a real journal, device flush, allocator or atomic commit.
Next integration must test ownership-table/bitmap/ledger coupling in actual JBD2
transactions; these passing mocks cannot substitute for those cut tests.

## Private marker and ownership cross-binding checkpoint

`pg_validate_set` accepts exact 512/1024/8192-byte records plus independently
supplied expectations: the five UUIDs, enrollment epoch, resource-profile hash,
complete 512-byte marker hash, and inode/generation pairs for ledger, table and
transaction slot. Expectations are a separate caller input; deriving them from
untrusted records does not establish authority. No production caller supplies
them today. Marker self-hash and external full-record pin are distinct checks.

The gate checks canonical marker structure, expected identities and nonaliasing
fixed locators; ledger identity/epoch/slot; table header transaction, sequence,
identity and profile; table self-hash and complete-table ledger hash; sorted
unique roles and inode identities; zero padding/unused slots; and overflow-safe
charged block/inode sums matching the ledger. It publishes the ledger view only
after the whole set validates; every failure preserves output.

Private role profile 1 narrows the candidate's representation: roles 1–16 are
payloads, 17 activation, 18 next state, 19 intent, 20 documentation, 21–36 old
inventory victims. The first group permits CHARGED or RETIRED; victims permit
BASELINE_VICTIM or RETIRED. Other roles reject. This is a test candidate, not a
production layout promise for all packages. CHARGED/BASELINE_VICTIM require one
inode and exactly one flag (linked=1 or orphan=2). RETIRED requires zero flags,
size and allocation counts, while retaining role/inode/generation/transaction
identity. IDLE requires an empty ownership table. Reserved entry bytes are zero.
Baseline victims do not contribute to charged totals; tombstones cannot refund.

This verifies record claims, not actual inode metadata, extent ownership or
bitmap counts. In particular it cannot detect a nonzero entry generation that
disagrees with a real inode, overlapping real extents, a false linked/orphan
claim, stale complete-filesystem rollback or a false trusted context. Those need
an actual-code filesystem adapter and authenticated enrollment/mount integration.
No signature, catalog-expiry decision or namespace mutation is performed here.
The narrower private role profile and retained-inode tombstone policy require
review before any disk ABI is frozen. Real JBD2 ledger/table/bitmap cut tests
remain next; record consistency does not prove crash atomicity.

The subsequent [private filesystem/journal adapter](PKG2_FILESYSTEM_GRANT_ADAPTER.md)
adds actual inode/map/ownership refinement and bounded block charge/refund and
replay cut tests. Its backup-identity geometry requirement and narrow linked-file
scope are explicit; full grant, inode/orphan and production mount integration
remain later gates.

The [private inode/orphan lifecycle workbench](PKG2_GRANT_LIFECYCLE_ADAPTER.md)
now tests debit/refund, pre-intent release to IDLE, competing ordinary planner
checks and fresh durable reservation admission through copied hooks. Those private
serialized fixtures do not implement account recovery bounds, task-owned writer
sessions, trusted enrollment or production mount admission.
