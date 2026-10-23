# Authenticated enrollment and durable account-orphan provenance

2026-10-23. Private design checkpoint following
[mid-callback recovery](PKG2_ACCOUNT_ORPHAN_RECOVERY.md). This is a proposed
contract, not implemented enrollment, a frozen wire ABI or acceptance evidence.
The [private codec gate](PKG2_ENROLLMENT_CODEC.md) now supplies a versioned
candidate encoding and signature/structure tests. That codec gate alone does not
provide durable generation storage, atomic hooks or production selection.
The subsequent [private durable storage workbench](PKG2_ACCOUNT_DURABLE.md)
implements the generation/origin pair and actual JBD2 planner hooks for one
synthetic pre-intent shadow-removal operation. Account-service phase integration
and production selection remain open; its qualified evidence is separate.
Production integration, installation, production trust provisioning, release-media
builds, pushes and physical disk writes remain disabled.

## Authority boundary

The existing `.ctx` and `.origin` inputs are trusted fixture expectations.
Neither their hashes nor their current lifetime authenticate durable provenance.
In particular, saving `.origin` after cancellation unwind cannot survive abrupt
loss between unlink and unwind. It must not become a production admission input.

Use two distinct authorities:

- A separately authenticated, immutable enrollment descriptor binds the selected
  filesystem and the protected metadata storage to an approved policy.
- A kernel-maintained mutable provenance slot records the actual fixed-role
  inode before its last link is removed. Its authority derives from exclusive
  ownership, enforced write exclusion and atomic JBD2 updates on that enrolled
  filesystem. Its checksum detects damage; it is not a signature.

The second guarantee covers crashes and excluded ordinary writers. It does not
authenticate arbitrary offline rewrites, a compromised kernel or a privileged
raw writer outside the exclusion policy. Signing static enrollment does not
extend that guarantee to mutable records. Whole-filesystem rollback and selecting
an older unprotected kernel remain outside it. A stronger mutable-record threat
model requires a separate authenticated monotonic storage/key design; do not
silently substitute a disk-resident MAC key or call a self-hash authentication.

## Enrollment descriptor and selector

The trusted embedding supplies pinned enrollment-role public keys, threshold,
accepted descriptor version/epoch and a complete descriptor digest from outside
the target root. No key, required-policy flag, epoch floor or expected digest is
learned from the filesystem being admitted. Use the existing Ed25519 verifier
and SHA-256; no private signing key is installed in the kernel or target root.
The enrollment role is distinct from catalog, TLS and SSH roles.

A candidate canonical descriptor must bind all of:

- A versioned enrollment signature domain, exact length and schema version.
- Installation, partition, filesystem, repository and enrollment UUIDs, plus
  nonzero enrollment epoch and required mount-contract version.
- Exact enrollment-marker digest, resource/account profile digest, compatible
  kernel/bootstrap identities and initial trust/base-policy identities.
- Inode/generation and immutable initialized allocation maps for every protected
  record, including the provenance slot; protected parent directory identities
  and the fixed account namespace anchor identities.
- Bounds for account roles, original size, logical extents, physical allocations,
  journal image credits and allowed recovery operations.

Canonical unsigned little-endian bytes, exact lengths, zero reserved fields,
sorted unique typed locators/signers, checked arithmetic and no suffix are
mandatory. Sign the complete canonical body including signature count and role
domain using ordinary Ed25519. Reject duplicate/unknown signers and invalid extra
signatures as in the catalog candidate. Exact offsets and independent golden
vectors are the next implementation deliverable, not implied by this field list.

The future installed selector must require this contract and pin the descriptor;
the existing readiness-v1 and a free-form ignored boot token are insufficient.
Current production boot parsing is unchanged. A private embedding may supply a
verified descriptor without implementing that selector, but must label that
remaining trust boundary explicitly. Missing, unreadable, partial or mismatched
enrollment on a required root rejects before replay or any write/flush. Legacy
writable admission must reject any reserved enrollment artifacts; absence must
be proven rather than inferred from lookup failure.

Provisioning eventually prepares, reads back and flushes all fixed storage and
the initial empty provenance slot before publishing the required selector.
Interrupted cross-device setup fails closed; no cross-device atomicity or
automatic enrollment/repair at mount is proposed. Production provisioning stays
disabled throughout this work.

## Durable provenance slot

Start with one bounded slot and at most one account orphan. Serialize fixed-role
removal and all cleanup until that slot is empty; do not reuse it merely because
the account callback returned or its process died. Ordinary orphan coexistence,
linked truncate orphans and charged-package orphans remain rejected in this
first profile. This restriction must be enforced before mutation, not discovered
only at the next mount.

Use a new explicitly versioned record; do not reinterpret the old marker's
reserved bytes or native `account_orphan_origin` layout. Storage is preallocated,
included in enrollment ownership auditing and inaccessible to ordinary namespace,
descriptor writes, truncate, replacement and raw aliases. Its fixed allocation
belongs to baseline capacity, never to an account orphan or package charge.

The record has EMPTY and ACTIVE states. Both carry enrollment identity, epoch,
nonwrapping slot sequence, schema/profile and checksum. EMPTY has canonical zero
payload. ACTIVE additionally carries fixed account role and parent identity,
inode/generation, original size/mode/UID/GID, initialized logical-to-physical
extents and original physical allocation ranges including extent metadata.
Bind this to a persistent account-operation generation and validated redo phase;
a volatile PID, retained-call pointer or package transaction UUID is insufficient
for pre-intent account cleanup. Defining that generation's durable storage and
atomic advancement is a prerequisite to the on-media implementation.

Keep the original allocation witness immutable during right-edge cleanup.
Validate the remaining inode as an allowed prefix/subset of that original map,
then use the full bitmap/reference ownership audit to reject overlap or reuse.
Count actual remaining allocated blocks and the still-allocated inode in the
capacity certificate. Never credit the original size or an expected future free.

## Atomic mutation and recovery

The actual account remove path is `e4_ac_remove` -> `e4_junlink_account` ->
`e4_file_unlink` in the orphan implementation. The witness must be staged inside
the same filesystem planner as removal of the directory entry, zero-link inode
and traditional orphan-chain insertion. Capturing only in the outer callback
leaves a gap; an extra preceding provenance commit leaves an ambiguous armed
record unless another fully specified state is added. This design chooses one
atomic activation commit and no ARMED state.

| Committed preview state | Admission/recovery |
| --- | --- |
| EMPTY, no account orphan | Continue remaining state/trust checks |
| ACTIVE, exact unlinked account orphan and vacant role | Validate complete witness and redo binding; permit bounded cleanup only |
| EMPTY with an account orphan | Reject without writes |
| ACTIVE with linked inode, occupied role, freed/reused inode or no orphan | Reject without writes |
| Multiple/unknown/charged orphan, stale epoch/generation or invalid map | Reject without writes |
| Malformed slot, unknown state, sequence overflow, I/O failure | Reject without repair or writable fallback |

Each partial cleanup commit changes real extents/bitmaps and preserves ACTIVE.
The final commit frees the inode, detaches it from the orphan chain and changes
the slot to EMPTY together. No interval may expose an EMPTY record with an
allocated account orphan, or ACTIVE with a freed/reusable identity. Include the
slot and operation-generation images in journal credit preflight before any I/O;
insufficient image capacity rejects without a fallback split transaction.

Read the slot through the committed JBD2 preview, with the same view used for
the inode, directory and allocation checks. Home-block-only validation can pair
an old slot with a newer journaled unlink and is invalid. Complete all enrollment,
provenance, capacity and pending/state/trust checks before actual replay writes.
Do not publish a root, enable authentication or fabricate mount ownership flags
to run validation. Step 2 still owns completion of pending/state/trust policy;
this design alone cannot make the private mount admissible for production.

Cancellation after a winning commit retains ACTIVE until real cleanup completes.
An uncertain write/flush taints and stops the current owner; restart must analyze
the durable journal result. Do not retry an assumed outcome, clear the slot on
unwind or regenerate a missing witness from the orphan being inspected.

## Implementation and acceptance order

1. Implement a private read-only canonical descriptor/slot codec and enrollment
   signature gate with independently supplied test policy and independent wire
   vectors. Negative cases isolate signer, signature, selector, epoch, profile,
   locator/map, reserved-byte, length and overflow failures; output stays unchanged.
2. Define and implement the durable account-operation generation, then add
   copied-source planner hooks for atomic slot activation and retirement.
   Replace sidecar origin authority only in the new opt-in campaign; retain the
   previous sidecar campaign and its exact evidence unchanged.
3. Cut actual JBD2 writes/flushes before/after activation, each partial reclaim
   and final retirement. Exercise committed-uncheckpointed preview and replay
   interruption, then recover twice in fresh processes without an origin sidecar.
4. Require zero writes/flushes and byte-identical images on admission rejection;
   independently audit successful final allocations/inodes/bytes with Linux.
   Cover 1/2/4 KiB blocks and 512/4096-byte sectors with exact measured cut counts.

These are implementation gates, not completed tests. Near-full progress and
charged-orphan refunds remain step 3 of the broader plan. Kernel lifecycle,
Ring 3, boot acceptance and physical durability require separate evidence;
source/media preservation checks cannot substitute for any of them.
