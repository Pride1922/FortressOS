# Authenticated private retirement provenance

Continuation of [five-record JBD2 coupling](PKG2_RETIREMENT_JBD2.md).
This adds a pure read-only recovery gate under tests/, without a mount caller,
filesystem mutation, production signer, enrollment provisioner or release build.
Production integration and installation remain disabled.

## Authority and lifetime

The new retirement-role policy is an independent caller input: one to eight
Ed25519 public keys, a threshold and the exact full certificate SHA-256.
It is separate from enrollment-role and catalog-role policies. Input records
cannot choose any of those keys or pins. Fixture signing uses public,
deterministic test keys; this is not a production key-management design.

The gate freshly verifies the enrollment signatures, retirement signatures and
retained catalog signatures using the existing cryptographic implementation.
Every supplied signer must be recognized and valid; IDs must be strictly sorted,
and unknown/invalid extra signatures refuse even after quorum. Exact hash pins
do not bypass signature verification.

A certificate authorizes retirement of exactly one charged archive role 1 in one
existing RETIRING/install transaction. It binds the signed enrollment, all five
volume identities, epoch, marker and resource/account profiles, transaction,
trust/state/intent, retained artifact and executable, platform/base, original
grant/account sequences, credential generation and original ownership row.
The separate retirement authority thereby names the exact inode and generation
eligible for cleanup; a catalog signature alone never authorizes inode deletion.

Current grant/table bytes must reconstruct the exact signed original grant/table:
restore the original selected row and sequence, reverse only the corresponding
G/Q changes, and recompute hashes. Other rows and immutable grant bytes must
remain identical to that original. Signed account/origin anchors mask only their
specified mutable sequence/hash/checksum fields; account operation/credential
generation, canonical database hashes and all other bytes remain bound.

The current five-record set is then validated with the existing retirement codec.
Grant/account sequence deltas must agree and fit the number of possible unlink,
positive block-refund and final-inode commits. A linked target cannot advance.
Refunds cannot invent ownership or change totals; the signed baseline is retained.

The existing pending/trust validator checks the canonical initial-state lineage,
exact published new state, immutable intent, admitted catalog policy, retained
FPK/manifest/ELF and executable identity. The actual account v4 record is fully
checked first; a documented scratch-only v3 projection invokes that existing
validator without modifying its format or publishing the projection.

The certificate binds the original intent admission time and its issuance window.
Recovery verifies that original admission, so catalog/certificate expiry cannot
select a new transaction or payload but does not strand that exact existing
retirement. This API is recovery-only; it must not be used for fresh admission.
Current clock and independent current policy remain trusted caller obligations.

## Canonical certificate v1

A 1024-byte signed body followed by 1..8 sorted 96-byte signer-ID/signature
records. Pure RFC8032 Ed25519 signs all 1024 body bytes, including total length and
signer count. Unknown versions, lengths, roles and reserved fields refuse.

| Offset | Field |
| --- | --- |
| 0 / 16 / 20 / 22 | 16-byte zero-padded FORT-PKG-RET-v1 / u32 full length / u16 version=1 / u16 signature count |
| 24 / 32 | u64 epoch / five 16-byte UUIDs in enrollment order |
| 112 / 144 | Full signed enrollment hash / enrollment marker full hash |
| 176 / 208 / 240 | Resource profile hash / account profile hash / transaction UUID |
| 256 / 288 / 320 | Full trust / installed state / retained intent hashes |
| 352 / 384 | Full retained artifact / executable hashes |
| 416 / 424 | u64 artifact / executable lengths |
| 432 / 440 / 448 | u64 issued / expiry / original admitted time |
| 456 / 464 | u64 base version / 32-byte platform identity |
| 496 / 504 / 512 | u64 original grant / account sequence / credential generation |
| 520 | Exact original 64-byte selected ownership row |
| 584 / 616 | Full original grant / ownership-table hashes |
| 648 / 680 | Canonical account / origin stable-anchor hashes |
| 712..1024 | Zero |

Account anchor hashes all 1024 bytes after zeroing [24,32), [112,144),
[440,480), [592,624) and [992,1024). Origin anchor zeros [24,32) and
[992,1024). These anchors avoid a hash cycle: the retirement ledger contains
the certificate hash and the account record contains the retirement-ledger hash.
The certificate never hashes either of those cyclic mutable links.

## Scope limits before integration

This gate authenticates retained evidence against a separately trusted retirement
authority; it does not prove actual filesystem identities, maps, bitmap uniqueness,
physical free observations, initial account capacity, user database coherence,
process writer lifetime or durable publication of the certificate itself.
Those remain required from the future exclusive filesystem adapter.

The artifact and signed intent evidence must remain available independently of
the selected archive being reclaimed. The current JBD2 workbench still uses opaque
synthetic capability pins and is not silently upgraded by this parser. Joining
these two workbenches requires durable evidence locators, actual allocation
refinement and interruption tests before any mount-admission change.

A whole-filesystem rollback, compromise of trusted caller policy or authorized
retirement keys, key provisioning/rotation/revocation and hardware clock assurance
are not solved here. Failure preserves public output and clears verified status;
workspace is bounded caller-owned scratch with no allocation or I/O. No boot,
Ring 3, SMP, scheduler, filesystem or physical acceptance is added by this gate.

## Verification and retained findings (2026-10-23)

Final ASan/UBSan gate `pkg-retirement-auth-20261023-190004-121888`: **506 cases PASS**, 10 accepted,
496 refused. Every indexed refusal preserves output and clears verified status.
Each case also checks NULL-input rejection; an unexpected tool syscall traps.
This is a pure-library test, not a filesystem zero-write admission claim.

Accepted cases cover the linked baseline, pinned orphan, data-freed orphan and
inode tombstone; each also recovers under historical catalog/certificate expiry.
Three valid signatures at threshold two and eight valid signatures at threshold
eight pass. Refusals cover forged signatures despite matching identity pins,
unknown/duplicate/invalid extra signers, key/threshold bounds, clock/base mismatch,
all 312 reserved certificate bytes, signed context mismatches, missing/trailing
inputs and mixed five-record generations.

The coherent forged-catalog and authorized-but-unsupported manifest fixtures
independently pass enrollment and five-record consistency checks first.
Coherent account-hash, operation/credential generation, other ownership and
logical-size/generation substitutions also independently pass the five-record
codec before the authority gate refuses them. This prevents invalid endpoints
from masking the intended authority checks. Real FPK/catalog/Ed25519 code runs;
signing and wire generation use independent Python cryptography/hashlib/struct.

Freestanding strict compile passes with a 1024-byte frame ceiling. New-module
frames: `tests/pkg_retirement_auth.c:72:12:verify	336	static; tests/pkg_retirement_auth.c:108:5:pa_verify	32	static`.
This measures this module, not a new bound on the entire transitive crypto stack.
Its large caller workspace must remain in BSS/heap. Existing crypto implementation
and its separately reviewed verifier frame exception are unchanged.

Source/copy guards and AGENTS.md, README.md and shared ELF/initramfs/ISO/raw-image
hashes pass separately. No boot, release-media, Ring 3, filesystem, kernel teardown,
SMP or physical acceptance is inferred. Full attempt index and hashes:
`.codex-remote-attachments/pkg-retirement-auth-summary-20261023/manifest.json`.

Run: `make -f tests/pkg_grant_candidate.mk test-pkg-retirement-auth`.

Retained failures and superseded evidence:

- `pkg-retirement-auth-20261023-184902-567771`: missing upstream crypto
  header include path; compile failed.
- `pkg-retirement-auth-20261023-185206-716596`: missing VFS include path
  while resolving the existing gzip source; dependency generation failed.
- `pkg-retirement-auth-20261023-185409-489647`: missing host syscall symbol
  from unused gzip I/O branches; link failed. Adding a separate host-only syscall
  trap produced a retained 185-case PASS follow-up without rewriting the raw FAIL.
  An additional coherent size-increase probe then reproduced erroneous acceptance.
  Fixed by requiring the original logical size in every charged state; six
  up/down charged-state regressions cover this defect.
- `pkg-retirement-auth-20261023-185738-643316`: 504-case PASS, superseded.
  A separate coherent-generation probe reproduced acceptance of an unlink and
  block refund folded into one advance. Fixed the minimum advance count;
  final tests also reject mismatched account/grant advance counts. Original PASS
  and both reproduction logs remain retained, rather than relabeled final.

Next is durable certificate/evidence storage and authenticated binding in the
private JBD2 adapter, with actual allocation checks and interruption recovery.
That adapter still has synthetic capability pins; these parser results do not
upgrade its authority or mount acceptance. Production integration, installation,
trust provisioning, release-media builds, pushes and physical-device writes stay
disabled. Changes remain uncommitted.


## Combined private integration acceptance (2026-10-23)

The subsequent bounded private milestone is now closed. See
[final acceptance, exact evidence and limits](PKG2_PRIVATE_RETIREMENT_ACCEPTANCE.md).
This supersedes the private-integration next-step statement above only; it does
not enable production integration, installation or trust provisioning.
