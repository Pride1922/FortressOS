# PKG-1 candidate catalog authentication

2026-10-21. Pure read-only code and test policy, not production trust provisioning.
Extends [PKG1_DESIGN.md](PKG1_DESIGN.md). No root-metadata parser/rotation,
on-disk trust floors, network fetching, package installation or boot changes.

The trusted caller supplies a snapshot of repository UUID, pinned catalog-role
public keys (1..8), threshold (1..key count), exact authorized root version,
revision floor, full object hash at that floor, and current UTC time. None of
these is adopted from untrusted input. Reject duplicate/zero policy keys or
zero UUID/root version. Unknown or invalid extra signatures fail closed, even
if other signatures meet threshold. Keys count once; signature records must be
strictly sorted by SHA-256(public key). Signing keys are never obtained from SSH
host identities or TLS. Test keys are explicitly public deterministic fixtures.

## Exact candidate bytes

Unsigned little endian integers, no native structs or implicit padding.

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 16 | ASCII `FORT-PKG-CAT-v1` followed by one zero byte |
| 16 | 16 | repository UUID opaque bytes |
| 32 | 8 | authorized root version |
| 40 | 8 | catalog revision, nonzero |
| 48 / 56 | 8 each | issued / expiry UTC Unix seconds |
| 64 / 66 | 2 each | entry count / signature count |
| 68 | 4 | zero |

Then 1..256 entries, each 81 bytes: u8 name length, 32-byte name field (unused
suffix zero), u64 build, u64 full artifact length, SHA-256[32] full artifact hash.
Names use existing canonical identity grammar; strictly sorted raw names,
no duplicates. Build nonzero; artifact length 128 through envelope + maximum
manifest + maximum compressed payload. Unsupported field extensions need a new
role/version; no unknown fields are skipped.

Finally 1..8 signature records, 96 bytes each: SHA-256(public key)[32] and
Ed25519 signature[64]. Pure RFC8032 Ed25519 signs **the complete header and all
entries**, including role, UUID, root, revision, times and signature count.
This is not Ed25519ph or an ad hoc prehash scheme. Signature bytes are excluded
from their own signed message. No trailing data. Maximum currently representable
object is 21,576 bytes, below the defensive 256 KiB input cap.

UTC must be within 2026-01-01 through 2100-01-01; issued >=2026, issued <=now,
expiry >now and <=2100. Reject absent/implausible clock; no clock correction or
expired offline override. Revision below caller floor rejects. Same revision
requires byte-identical full-object SHA-256, including signature records, to
match the floor hash. A higher revision may verify but **is not persisted** by
this API. Caller must durably publish accepted root/revision floors before any
future installation. This code cannot prevent whole-disk rollback or withholding.

## Composition and lifetime

`fpk_catalog_verify` returns zero only after structure, policy, hashes and every
signature pass. Output unchanged on error; on success entries borrow immutable
input. `fpk_catalog_find` accepts only such verified caller-retained views.
Caller must keep input/policy immutable and all outputs disjoint throughout.
The library cannot defend against a caller fabricating its own verified C struct.

`fpk_verify_authorized` re-verifies the catalog against supplied policy, resolves
the requested package, verifies complete artifact length/hash, then matches the
artifact manifest package/build to that entry. Failure clears `context.verified`,
including errors after successful artifact integrity. The flag means supplied
catalog policy plus artifact validation succeeded, never permission to install.
Running-platform identity, base compatibility/ownership, installed package build
floors and durable metadata publication are still caller/next-phase work.

Uses unchanged Fortress `crypto_ed25519_verify` / SHA-256 and pinned Monocypher
4.0.3. No new primitive or upstream changes. The established upstream verifier
1,280-byte frame exception and stack qualification in
[crypto.md](../subsystems/crypto.md) remain; package-local frames keep <=1024.
Private Ring 3 call-chain/argument/signal measurements now pass in the bounded
fixtures recorded in [pkg1.md](../roadmap/pkg1.md); this does not establish a
universal worst-case bound or a production cryptographic audit.

Production input decisions remain: signing custodians/fingerprints, root-role
thresholds and separate catalog delegation, platform provisioning, expiry
recovery, sequential dual-threshold root rotation and protected durable floors.
This candidate catalog role can be tested independently of those decisions.
