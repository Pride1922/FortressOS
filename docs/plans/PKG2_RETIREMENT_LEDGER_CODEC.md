# Private retirement ledger and atomic-accounting gate

2026-10-23 continuation of [low-space recovery](PKG2_LOW_SPACE_REFUNDS.md).
This is a pure codec/transition candidate, deliberately before mount integration.
Production integration, installation, production trust provisioning, release-media
builds, pushes and physical-device writes remain disabled. Changes are uncommitted.

`tests/pkg_retirement_candidate.{h,c}` validates a retirement ledger together
with the existing marker, grant and ownership table, a private account-generation
version **4**, and the existing empty origin-slot grammar. It changes no existing
account parser, filesystem implementation, mount hook, syscall or release target.
The current private mount profile continues to accept its existing version 3;
it cannot consume this version 4. There is no migration or fallback.

## Independent expectations and limits

The trusted embedding supplies the existing enrollment binding plus the opaque
retirement-capability hash, account-profile hash, transaction UUID, exact
trust/current-state/intent hashes and selected role/inode/generation. These pins
must never be inferred from the supplied mutable records. Coherently rewriting
the grant, ledger, table and account snapshot cannot select a new transaction,
trust policy, state, intent or object identity for the same caller expectations.

The codec checks correspondence with those expectations. It does **not** verify
a retirement capability's signatures, retained manifest/executable authorization,
actual inode ownership/maps/bitmaps, account database coherence, live writer
ownership, initial resource capacity or mount policy. Checksums and an opaque
capability hash are not authentication. Full-filesystem rollback protection is
not established. The future caller must establish all these properties before
admission and must provide actual audited allocation observations.

The bounded record profile requires RETIRING/install grants. The selected
charged role is 1..20; the fixture uses payload role 2 with a separate unchanged
charged payload role 1. The existing ownership codec still validates all table
entries and global Q, rather than trusting the selected object's counts alone.
Selected baseline-victim records, other grant phases/operations, credential
changes, nonzero completed-step bits and overlapping account transactions are
unsupported and refuse.

## Retirement ledger v1

Exactly 1024 bytes, unsigned little-endian integers, no native struct layout.
The first 16 bytes are zero-padded `FORT-PKG-RLG-v1`. SHA-256 covers [0,992),
with the digest at 992. Unknown versions, trailing bytes, short input and
nonzero reserved bytes refuse. Decode failure preserves the output view.
`pr_encode` is canonical-byte validation/copy, matching the existing grant
codec convention; it is not a native-struct serializer or an authorization API.
Its exact 1024-byte output buffer remains unchanged on failure.

| Offset | Field |
| --- | --- |
| 16 / 20 / 22 | u32 length=1024 / u16 version=1 / u16 reserved=0 |
| 24 / 32 | u64 grant/ledger sequence / enrollment epoch |
| 40 / 56 / 72 / 88 | Transaction / enrollment / filesystem / repository UUID |
| 104 | Independently pinned opaque retirement-capability hash |
| 136 / 168 | Full grant / ownership-table hashes |
| 200 / 208 | u64 account-record sequence / credential generation |
| 216 / 218 / 220 | u16 role / u16 class / u32 linked-orphan flags |
| 224 / 228 / 232 | u32 inode / u32 inode generation / u64 original logical size |
| 240 / 256 / 272 | Two u64 values each: global T / G / Q, blocks then inodes |
| 288 / 296 | Selected object's remaining allocated blocks / allocated inode |
| 304..992 | Zero |

Sequences, identities and pin hashes are nonzero. All additions are checked
before enforcing T=G+Q. The selected object's Q cannot exceed global Q. Class 1
permits linked=1 or orphan=2 and requires one still-allocated inode. Class 3 is a
tombstone: flags, size and remaining blocks/inodes are zero; inode/generation
identity remains. Ledger fields mirror the exact selected ownership-table row.

The ledger binds the account sequence and credential generation, rather than
hashing the entire account record. The account record hashes the complete
ledger. This avoids a circular ledger/account hash dependency.

## Account-record version 4

The candidate retains the existing account record's 1024-byte header, checksum,
identity, credential and grant-binding layout. It requires account phase IDLE,
nonzero operation generation and no target/redo/intent payload at [152,328).
Canonical account hashes are nonzero and their lengths remain in 1..8192; this
is structural validation, not validation of the actual databases.

Version 4 adds the full retirement-ledger hash at [592,624), and reserves
[624,992) as zero. Existing grant sequence/hash, trust, current-state, intent
and transaction bindings at 440/448/480/512/544/576 must match the supplied
RETIRING grant and independent expectations. The full origin-slot hash at 112
must match the actual supplied origin record. The origin must be canonical
EMPTY, share epoch/enrollment/filesystem/profile and operation generation, and
have exactly the same account sequence. No account-orphan authority is invented
from this empty-slot profile.

## Allowed accounting transitions

Both endpoint sets must validate independently, even for negative transition
tests. Marker and all immutable bytes remain unchanged. A transition advances
grant, table and retirement-ledger sequence by exactly one, and account/origin
sequence by exactly one. Credential generation, canonical account hashes and
lengths, operation generation, trust/state/intent and every other ownership row
are immutable. Counter exhaustion rejects advancement; there is no wrap/restart.

- Linked to orphan: same identity, size and allocation; F, G and Q do not move.
- Orphan data release: same logical size, fewer allocated blocks, inode remains
  charged. Actual F increases exactly as G increases and Q decreases.
- Final inode release: only after the selected object's blocks reach zero.
  The row becomes a tombstone, and exactly one actual free inode restores G.
- Exact replay: all five mutable records and actual free observations are
  identical; return zero refund without advancing a sequence.

All non-replay transitions preserve T and effective F-G in both dimensions.
Caller observations must satisfy F>=G; the codec cannot establish the correct
initial account-capacity floor R. If independently certified before the plan,
this conservation preserves it. Unrelated allocation/free, baseline refunds,
unused-escrow release, transfer, regrowth/relink, zero-refund orphan advancement,
direct linked-to-tombstone deletion and combined data/inode retirement refuse.
Only documented mutable byte ranges may change, including all checksum/hash
bindings; re-sealing an unauthorized change does not authorize it.

`pr_transition` is read-only and failure preserves its result and input buffers.
The future executor must compare the expected current generation/bytes under
filesystem exclusion, validate actual planned allocation changes and commit
grant, ownership table, retirement ledger, account and origin **with those
physical frees** in one real JBD2 transaction. No such executor is added here.

## Verification boundary

`make -f tests/pkg_grant_candidate.mk test-pkg-retirement-codec` runs a private
copied-source ASan/UBSan gate and a freestanding strict frame build. Independent
Python code constructs records and re-seals semantic defects. The host tests
also cover every short ledger length, trailing input, output capacity, rehashed
reserved bytes, exact output preservation and input-buffer immutability.

The atomic-publication tests are explicitly a **serialized host model**. For
each of four transitions, all 32 old/new selections across the five mutable
records are checked: only the complete old or complete new set may validate.
The model publishes a whole set once, rejects a stale expected set without
changing its current state, and checks repeated recovery gives no refund.
This does not demonstrate concurrent atomic stores, actual journal durability,
power-loss recovery, filesystem allocation identity, mount/replay ordering,
Ring 3, scheduler teardown, firmware boot or physical acceptance.

Next: private actual-JBD2 five-record commits with audited physical frees,
authenticated retirement provenance and interruption tests. Mount integration
remains gated on that evidence and the existing complete allocation/trust checks.


## Exact results and retained attempts

Final gate `pkg-retirement-codec-20261023-162253-398949` PASS; manifest SHA-256 `28775920c0218c48e2f796c296c019929fdfce55a9ecba9b99c0155de331f8f5`. [Machine-readable summary](../../.codex-remote-attachments/pkg-retirement-summary-20261023/manifest.json) retains every attempt and source/media identity.

- 1,507 independent record-set fixtures: 12 accepted, 1,495 rejected, including 1,428 rehashed account/origin reserved-byte cases.
- 39 accounting transition fixtures: five accepted, 34 refused. Both endpoints of every transition are independently required to validate, preventing malformed endpoint checks from masking accounting defects.
- 128 mixed-generation combinations: eight complete old/new sets accepted, 120 mixed sets rejected. Four serialized mock publications reject stale expected sets; four subsequent exact replays grant zero credit.
- 3,780 counted codec/length/padding/output-preservation refusals, plus canonical byte-exact round trip and oversized output rejection.
- 12,288 one-bit integrity mutations, one at each byte of the five mutable records, all rejected. This is not all possible bit patterns or physical byte/sector tearing.
- Strict freestanding frame maximum 400 bytes; all frames <=1024. Individual function measurements are in the summary; they do not measure total nested kernel stack usage.

The first run `pkg-retirement-codec-20261023-160627-444225` failed in the test index reader: its fscanf assertion expected ten conversions for nine fields. The parser was corrected to nine; the raw FAIL, logs and manifest remain retained. Earlier PASS checkpoints remain retained and superseded as independent authorization pins, full padding/integrity coverage, precise probe labels and zero completed-step admission were added. Older logs used the label `byte_tears` for one-bit-per-byte integrity probes; the final report uses `byte_bit_probes` to avoid implying physical torn-write testing.

ASan/UBSan runtime, Python compilation, target dispatch and whitespace checks pass. All final tested repository inputs and private copies are unchanged. AGENTS.md, README.md and all four existing shared kernel/initramfs/ISO/raw-image hashes are preserved. Preservation is independent of runtime acceptance; no firmware boot, Ring 3, native filesystem/JBD2 commit or physical acceptance was run in this step. Production integration and installation remain disabled, with no release-media build, commit, push or physical-device write.


Continuation: [actual private JBD2 coupling and interruption evidence](PKG2_RETIREMENT_JBD2.md). The pure module remains unchanged. The separate host-only adapter supplies actual allocation/free observations; authenticated retirement authority and mount admission remain open.
