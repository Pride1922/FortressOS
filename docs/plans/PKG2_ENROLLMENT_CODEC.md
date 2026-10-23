# Private enrollment and origin codecs

2026-10-23. Implements the first private gate of
[the enrollment/provenance contract](PKG2_ENROLLMENT_PROVENANCE.md).
Sources are `tests/pkg_enrollment_candidate.{h,c}`; no production source, boot
parser, filesystem format, admission caller or trust provisioning is changed.
This format is an explicitly versioned private candidate, not a frozen disk ABI.

## Enrollment bytes

All integers are unsigned little endian. Exact descriptor length is
`2464 + 96 * signature_count`, with 1..8 signatures. The first 2464 bytes are
signed using ordinary Ed25519, including signature count and all locators.
Signature records are SHA-256(public key)[32] followed by signature[64], strictly
increasing by signer ID. Unknown/duplicate signers and any invalid extra signature
reject even if the threshold would otherwise pass. Keys, threshold, exact epoch,
five expected UUIDs and full descriptor digest are separate trusted policy inputs.
No policy is loaded from this descriptor or an inspected filesystem.

| Offset | Encoding |
| --- | --- |
| 0 | 16-byte `FORT-PKG-ENR-v2` domain, including terminal zero |
| 16 | u32 exact length, u16 version=2, u16 signature count |
| 24 | u64 enrollment epoch, 1..UINT64_MAX-1 |
| 32 | u32 mount contract=2, u32 block size=1024/2048/4096 |
| 40 | Five 16-byte UUIDs: filesystem, partition, installation, repository, enrollment |
| 120 | Seven 32-byte hashes: marker, resource profile, kernel, bootstrap, initial trust, base policy, account profile |
| 344 | u64 filesystem blocks, u32 inode count |
| 356 | u16 locator count=16, u16 account role count=11 |
| 360 | u32 maximum original account size=8192 |
| 364 | u16 maximum origin extents=16, u16 maximum physical ranges=32 |
| 368 | u32 planner image bound=64, u32 allowed operation mask=1 (account unlink) |
| 376 | 40 zero bytes |
| 416 | Sixteen 128-byte typed locators |
| 2464 | Sorted signer records |

UUIDs and hashes must be nonzero. Geometry is bounded to 1 TiB and 1,048,576
inodes, with at least 32 inodes. A signed geometry is an expectation only;
an actual filesystem adapter must independently compare it before use.

Locator roles 1..16, in that exact order: marker, ledger, ownership table,
transaction directory, origin slot, account-operation state, package state,
package trust, package metadata parent, `/etc`, `/var`, `/var/lib`,
`/var/lib/fortress`, account directory, executable parent and documentation parent.
Roles 4 and 9..16 are directories; others are regular files. Parent naming and
actual inode type/owner verification remain future filesystem-adapter work.

Each locator encodes u16 role, u16 kind (1 regular/2 directory), u32 inode,
u32 generation, u32 permission mode, u64 logical size, u64 allocated data blocks,
u16 extent count, six zero bytes, four 16-byte inline extents, then 24 zero bytes.
Unused extent slots are zero. Inodes are unique, within geometry and >=11;
generation is nonzero. Regular modes are 0600; directory modes are 0700 or 0755.
Sizes are 1..1 MiB. Each initialized extent is u32 logical start, u32 block
length, u64 physical start; count is 1..4. Logical coverage is dense from zero,
exactly ceil(size/block size), and equals allocated blocks. Physical endpoints
must fit geometry and maps must be disjoint within/across all locators. This
first fixed-storage profile has no external extent-tree blocks. It does not yet
constrain each role to its eventual exact storage length or directory slot layout;
those are additional actual-adapter admission checks, not codec guarantees.

## Origin bytes

Exactly 1024 bytes; SHA-256 at 992 covers [0,992). It detects corruption and
does not authenticate mutable provenance. Decode requires a retained successful
enrollment result plus independently supplied exact slot sequence and account
generation. Neither expected counter is derived from the supplied origin bytes.
Both are 1..UINT64_MAX-1; wrap and stale pairings reject.

| Offset | Encoding |
| --- | --- |
| 0 | 16-byte `FORT-ACC-ORG-v1` domain, including terminal zero |
| 16 | u32 length=1024, u16 version=1, u16 state (0 EMPTY/1 ACTIVE) |
| 24 | u64 slot sequence, u64 enrollment epoch, u64 account-operation generation |
| 48 | Enrollment UUID[16], filesystem UUID[16], account-profile hash[32] |
| 112 | u16 account role (0..10), u16 phase (0 pre-intent/1 pending/2 cleanup) |
| 116 | u32 inode, generation, parent inode, parent generation, permission mode, UID, GID |
| 144 | u64 original size, u16 extent count, u16 physical range count, four zero bytes |
| 160 | Sixteen 16-byte logical/physical extents, same representation as enrollment |
| 416 | Thirty-two ranges: u64 physical start, u32 length, u32 zero |
| 928 | 64 zero bytes |
| 992 | SHA-256[32] |

EMPTY requires all payload bytes [112,992) zero. ACTIVE requires a nonprotected
inode, nonzero generation, exact role-derived parent inode/generation, root
UID/GID and the fixed-role permission mode. All eleven account roles use the
existing public ordering. Roles 0/1/6/7 are 0644; others are 0600. Private roles
3..5/9..10 bind the account directory; others bind `/etc`.

Size is <=8192; intent/pending are empty or exactly 160 bytes in this first
profile. Zero-size created files have zero ranges/extents but retain inode
identity. Nonempty files require both maps. Data extents have dense logical
coverage of the original size and disjoint physical mappings; each is contained
in exactly one physical range. Ranges are sorted, nonoverlapping and nonadjacent
(adjacent intervals have one canonical merged range), bounded by geometry and
disjoint from every enrolled map. Total allocation is at least the data blocks
and at most data blocks plus one metadata block when data exceeds four blocks.
This is an original witness codec, not validation of a partially reclaimed inode
or proof that an extra block actually contains a valid extent tree.

No validation of on-disk allocation bits, aliases, orphan chain membership,
role vacancy, live redo contents or phase coherence is performed here. Actual
filesystem and pending-state validation remain mandatory. The API trusts the
caller to retain immutable verified enrollment bytes; a fabricated C view or
changing bytes after verification defeats that caller contract.

`pe_origin_encode` is a wire-to-wire canonical encoder: it validates an existing
wire record, then copies it to a disjoint exact-size output. It does not generate
records from a native struct or authorize filesystem mutation. Every failure
preserves output. Pointer length wrap and output/input overlap reject; callers
must provide accessible memory. No heap, recursion, I/O or persistent state.

## Verification boundary

Run `make -f tests/pkg_grant_candidate.mk test-pkg-enrollment` in WSL.
The runner copies sources to a private evidence directory and compiles unchanged
Fortress SHA-256/Ed25519 with ASan/UBSan. Python struct/hashlib and
cryptography/OpenSSL independently serialize, hash and sign public test fixtures.
Malformed structural descriptors are re-signed and re-pinned to isolate parser
checks; malformed origin fields are rechecksummed. Separate signature/policy
tests isolate authentication. The C harness checks all-byte mutation/truncation,
output preservation, overlap refusal and exact canonical origin re-encoding.
Freestanding compilation enforces <=1024-byte candidate frames; upstream crypto
stack qualifications remain unchanged and no full call-chain bound is claimed.

This gate neither persists account generations nor activates/clears a provenance
slot with JBD2. The previous `.origin` campaign is unchanged. There is no new
filesystem recovery, Ring 3, boot, hardware or production acceptance. Production
integration, installation, production trust provisioning, release-media builds,
pushes and physical disk writes remain disabled.

The later [private durable generation/origin workbench](PKG2_ACCOUNT_DURABLE.md)
uses this codec with actual inode/map enrollment and JBD2 planner hooks. Its
runtime results and concurrent-media qualifications do not change this codec
gate's narrower acceptance scope or enable production admission.
