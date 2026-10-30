# Gate 2A: private test enrollment bundle and selector

Historical fixture design. Superseded for the product by the 2026-10-30
[ordinary repository package scope](PKG_PRODUCTION_DESKTOP.md); this mechanism
must not be a prerequisite for fetching or installing packages.

2026-10-24. Follows [private kernel acceptance](PKG2_GATE1_KERNEL_ACCEPTANCE.md).
This is the next bounded **host artifact** milestone of the
[production integration plan](PKG2_PRODUCTION_INTEGRATION.md), not completion of
Gate 2 or activation of enrolled production roots. The original plan is unchanged.
Installation, production key selection/provisioning, release-media builds, pushes
and physical-device writes remain disabled.

## Authority and supported profile

`tests/pkg_selector_candidate.{h,c}` is a freestanding read-only C validator.
`scripts/pkg_test_enroll.py` signs and publishes prepared initial-record artifacts
with external TEST seed files. `scripts/pkg_test_enroll_read.py` independently
classifies artifact presence and delegates cryptographic/semantic validation to
the separately compiled C reader. Neither tool opens or modifies a block device,
allocates filesystem objects, mounts a root or changes production dispatch.

The only accepted producer profile is `fortress-pkg2-test-only-v1`. Its selector
role is TEST (`TSEL`), with 2..8 external signers and independently supplied
threshold policy. The campaign pins three public keys and a threshold of two for
each of selector/root authorization, enrollment and delegated catalog roles.
All three key sets must be disjoint; TLS and SSH authority are unrelated.
Every extra signature must be valid, recognized and strictly signer-ID ordered.

The root-authority JSON is a canonical public audit artifact. Its complete hash,
role keys/thresholds, repository/platform identity and version are independently
pinned by the test policy. It is not self-authenticating and is not a production
root-manifest/delegation loader. The C API receives trusted policy values from
outside the inspected records. Tool output or a root-resident policy file must
never be treated as its own authority. Real root validity/rotation and production
delegation provisioning remain future reviewed work.

Policy binds all five UUIDs (filesystem, partition, installation, repository,
enrollment), resource profile, platform, compatible kernel/bootstrap hashes,
base version, exact trusted-root version/hash and exact catalog revision/hash.
Root/catalog advancement is unsupported under an unchanged policy. A caller must
explicitly supply independently authorized new policy; the disk cannot raise or
lower those expectations. The selector epoch has an external floor; at the floor,
the complete selector hash must match exactly. A signed equal-epoch substitute
cannot change identity or authorization under the existing floor hash.

Selector issue/expiry must contain the caller's validated UTC within 2026–2100.
The delegated catalog is freshly signature/expiry checked at that same time;
an unexpired selector cannot revive an expired catalog. Test vectors use a fixed
clock. No real-clock calibration or reboot-safe floor store is implemented here.

## Private selector encoding

Little-endian, exact length `768 + 96*N`, `1 <= N <= 8`; unknown fields reject.
The complete 768-byte body is signed with ordinary Ed25519. Each suffix row is
SHA-256(public key)[32] and signature[64]. The format is a private candidate,
not a frozen production disk ABI.

| Offset | Field |
| --- | --- |
| 0 | 16-byte `FORT-PKG-SEL-v1` including terminal zero |
| 16 | u32 exact length, u16 version=1, u16 zero flags |
| 24,32,40 | u64 issued, expires, nonzero/nonwrapping enrollment epoch |
| 48,52 | u32 kernel and bootstrap contracts, both exactly 2 |
| 56,64,72 | u64 trusted-root version, catalog revision, base version |
| 80 | 16 zero bytes |
| 96 | Five UUIDs, 80 bytes in the order above |
| 176 | Resource-profile SHA-256 |
| 208 | Complete 512-byte marker SHA-256 |
| 240 | Signed enrollment descriptor SHA-256 |
| 272 | Complete prepared-record bundle SHA-256 |
| 304,336 | Trusted-root authority hash and catalog hash |
| 368,400,432 | Platform, kernel and bootstrap hashes |
| 464 | Nonzero 16-byte selector identity |
| 480 | u32 TEST role `TSEL` |
| 484 | u16 signature count, u16 zero |
| 488 | u32 required-enrollment mode=1 |
| 492 | 276 zero bytes |
| 768 | Sorted unique signer rows |

Kernel/bootstrap hashes are compared to independently supplied policy and the
signed enrollment descriptor. This pure reader does not measure an executing
kernel or prove a bootloader selected those exact binaries.

## Prepared initial IDLE records

The bounded bundle has a 32-byte header (`FORT-PKG-BND-v1` domain, exact length,
version 1, count 8, zero reserved), eight 8-byte rows (u32 length, u16 ordered
role 1..8, u16 zero), then dense record bytes with no trailing data. Total size is
at most 32768 bytes. Roles are:

1. Signed enrollment descriptor, 2464..3232 bytes.
2. Marker, 512 bytes.
3. Grant, 1024 bytes.
4. Ownership table, 8192 bytes.
5. Account-operation state, 1024 bytes.
6. Account-origin slot, 1024 bytes.
7. Package state, 1024 bytes.
8. Package trust/catalog record, 1024 bytes.

The actual `pe_verify`, `pe_origin_decode`, `pg_validate_set`,
`fpk_catalog_verify` and `pm_validate` implementations run. The reader requires
IDLE grant sequence 1, no charged objects or transaction, an empty origin/account
operation at generation 1, canonical empty package state generation 1, bound
trust/catalog and profile identities, valid checksums and exact cross-hashes.
Account record schema 3 is explicit; pending redo, installed demo state and
retirement records cannot be adopted as initial enrollment.

The test harness prepares canonical genesis **record images** independently from
the producer. It reads an immutable earlier fixture for identity/locator witnesses
and prepares new IDLE bytes; it never writes them back to that fixture. These
prepared bytes do not claim to match that fixture's existing RETIRING namespace.
The selector authenticates their exact contents; actual inode/maps, directory
ownership, account-file contents, allocation state and planner capacity must still
be established by a filesystem enrollment adapter before deployment. This phase
does not enroll an arbitrary existing root or establish native boot compatibility.

Caller-owned workspace is disjoint from immutable inputs/policy and output.
Results contain copied epoch/hashes only, with no borrowed pointers. All failures
preserve output. Workspace is bounded scratch, not a kernel-stack allocation.
The native 1136-byte policy file used by the host driver is an x86-64 test harness
transport, not a production policy-file format.

## Offline publication and refusal

The producer accepts only bounded ordinary input files and a new output
directory. Seed files must be exactly 32 bytes, outside the repository, and private
under POSIX permissions. The tests use publicly known deterministic TEST seeds
in temporary WSL storage; no production secret or signing material is copied to
output or target root. This Python test tool makes no secure-memory-erasure or
production key-custody guarantee.

Before creating output, the producer verifies the signed selector/bundle with the
independent C reader and an external policy. It then:

1. Creates private output/root/boot directories and synchronizes directory entries.
2. Writes, fsyncs and reads back `root/records.bin`; C verification must pass.
3. Writes and fsyncs `boot/selector.tmp`; reads back and verifies it against the
   prepared root records.
4. Renames to `boot/selector.bin` last, then synchronizes its directory and the
   publication directory. Existing output is never replaced.

Failures and injected process interruptions retain partial outputs. No rollback,
automatic retry, deletion or force-adoption is performed. A reported interruption
after complete selector publication may leave a valid pair; inspection determines
that outcome rather than repeating publication.

The read-only classifier distinguishes verified absence, complete artifacts,
partial/malformed namespace and I/O failure. Legacy classification is possible
only with every reserved artifact and directory verified absent and caller policy
explicitly legacy. Required-but-absent, any partial artifact, unknown extra private
file, temporary selector, wrong type/mode, symlink or unreadable input refuses.
Even a fully valid enrollment artifact refuses under legacy mode.

Artifacts require 0700 directories and single-link, current-host-owner 0600 regular
files. The Windows-backed checkout does not preserve these POSIX modes in this
environment. Publication tests therefore execute under WSL `/tmp`; retained
checkout copies preserve bytes and recorded modes for audit, not active admission.
The producer refuses a filesystem that does not enforce the required private
modes. This host ownership check is distinct from target root:root ownership.

## Threat boundary and remaining Gate 2 work

External floors/pins prevent stale or substituted records only while the trusted
policy itself remains intact. Rolling back the whole root, selector and trusted
policy together is not detected. A boot administrator able to replace the kernel,
bootstrap, trust anchors or boot policy can bypass this private scheme. No TPM,
secure boot, monotonic hardware store or protected boot-selector deployment is
implemented. Ordinary root VFS writes must not be able to rewrite selector
authority in a future production design.

The proposed `boot_mode=installed-pkg-v1` remains unsupported. Current
`root_boot_parse` rejects unknown boot modes but ignores unrelated unknown tokens;
adding only `pkg_policy=required` would not exclude an older kernel. Compatible
kernel/bootstrap measurement, boot-selector integrity and older-kernel exclusion
must be implemented/reviewed before activation. Current boot behavior is unchanged.

Still required: an actual regular-file EXT4/GPT enrollment producer/reader that
allocates and verifies the complete native baseline before selector commit,
cross-media interruption/reboot acceptance, a reviewed trust-root/delegation
loader and durable floor store, and a separate operational production-key decision.
The same-filesystem host process cuts below are not power-loss or cross-media
atomicity evidence. Gate 1 native runtime acceptance remains separate.

## Verification

**Gate 2A private artifact milestone PASS.** Run from WSL `Ubuntu-24.04` at
`/mnt/c/Sources/FortressOS`:

    make -f tests/pkg_grant_candidate.mk test-pkg-selector

Final evidence: `.codex-remote-attachments/pkg-selector-20261024-085713-573157/`.
Strict GCC, ASan/UBSan and freestanding compilation pass. The added C reader's
largest optimized frame is **448 bytes**, below its 1024-byte ceiling; this is
not a transitive crypto/boot stack claim.

| Check | Exact final scope |
| --- | --- |
| Independent Python/C vectors | **449**: five accepted controls, **444 refusals** |
| Publication interruptions | **15** host process/namespace cut points |
| Producer refusals | **7**: production profile, ambiguous UUID widths, one signer, duplicate seed, repository seed path, world-readable seed, short seed |
| CLI end-to-end | **7** successful expected outcomes: publish, required read, legacy refusal with artifacts, no overwrite, interrupted publication, partial refusal, verified legacy absence |
| Admission classifier | All 40 required/presence/validity combinations checked in each C driver invocation |
| Artifact namespace | Missing record/selector, temporary selector, wrong mode, symlink, wrong type and injected EIO refuse; legacy never bypasses present artifacts |

The signed/re-signed corpus includes exact/trailing/short lengths, bundle row
length/order/flag bounds, reserved fields, IDs/profile/platform/boot hashes,
threshold/key bounds, wrong roles and role-key reuse, unknown/duplicate/unsorted
signers, damaged signatures including invalid extras, selector expiry and issue
bounds, root/catalog floors, equal-epoch substitution, a live selector with an
expired catalog, mixed records, non-IDLE grants/tables and incomplete initial
account lineage. Output preservation and pointer/overlap rejection are checked.

Five accepted controls distinguish real policy freedom from malformed state:
the baseline, authenticated selector above its external floor, an independently
repinned selector identity, three valid signers, and an independently authorized
in-range declared credential-file length. The last control is not validation of
an actual credential file; physical namespace/content verification remains open.
Zero/oversized lengths and zero credential-content witnesses refuse.

The producer's bytes exactly match the independent Python wire oracle and the C
reader authenticates them. The baseline selector SHA-256 is
`88f0da5182bca7770eb2035b14e08e475ddea2f7714558c7ad1e92a13df8d26d`;
prepared bundle SHA-256 is
`8d4a71e765feb037f1ccd70d26c834e52d0e980d781d13cb366bb445b0cca387`.

Cuts 1–12 leave partial artifacts and refuse. Cuts 13–15 occur after selector
rename and expose a complete authenticated pair, so the reader reports PROTECTED
despite the producer reporting interruption. These are visible whole-pair outcomes
on the running host, not proof that an unflushed rename survives power loss.
All 15 interrupted outputs refuse legacy classification. Private seeds are removed
with their temporary directory and are absent from retained publication copies.

The [aggregate reconciliation](../../.codex-remote-attachments/pkg-selector-summary-20261024/manifest.json)
records final manifest/CLI/verifier hashes, counts, stack output and independent
rechecks. The final campaign manifest SHA-256 is
`e5d1701024b8df7fddb2645dd37573d8c73eff5b340d6edd9c35e174fce2b6fb`.
Actual CLI argv/exit codes and stdout/stderr are in the final `cli-manifest.json`
and `cli-*.log` files. The native policy and public root-authority JSON are test
inputs/audit artifacts only; neither is promoted into production authority.

### Retained failures and intermediate runs

- `pkg-selector-20261024-083701-246752`: link FAIL from the missing host-only
  `tool_syscall` transport. The host driver now asserts on unexpected verifier I/O;
  actual cryptographic/record code still runs without filesystem syscalls.
- `pkg-selector-20261024-083904-167593`: C vectors passed, but artifact admission
  refused the Windows-backed output's ineffective POSIX modes. The checks were
  retained. Publication now runs under WSL temporary storage, and the producer
  explicitly refuses output filesystems that cannot enforce private modes.
- `pkg-selector-20261024-084130-242034`, `084343-562046`, `084925-274228`, and
  `085229-742088`: intermediate PASS checkpoints, superseded by the final corpus,
  credential-lineage checks and publication checks. Their original manifests remain.
- `pkg-selector-20261024-085452-698382`: vector 0440 FAIL because the test
  incorrectly classified a valid re-signed, externally repinned, in-range declared
  credential-file length as invalid. It is now an accepted control, with separate
  zero/oversized length refusals. The original FAIL is not relabeled or removed.

All retained FAIL campaigns separately reported preservation PASS. Final
compiled-copy, source, guard and original fixture-input hashes reconcile PASS.
An independent comparison to the accepted Gate 1 source inventory finds only
the append-only new standalone test target; Gate 1 implementation bytes are
unchanged. The original production plan SHA-256 remains
`f1e8a769becaac55509a82a1633fda9c6057167588becab8c9cba91521fb1017`.
Default production sources/Makefile/dispatch, trust assets and shared media remain
unchanged. Changes are uncommitted.

**Boot acceptance: NOT RUN in this step.** Host artifact and preservation PASS
do not establish EXT4 enrollment, native selector delivery, cross-media crash
atomicity, boot downgrade exclusion, physical power-loss safety or production
provisioning. Those remain the next Gate 2 integration work.
