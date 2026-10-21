# PKG-1: bounded package validation and recoverable application publication

2026-10-21. Candidate format and transaction design, based on HEAD `199b575`.
This document supersedes stale prerequisites in PACKAGE_MANAGER_PLAN.md.
Envelope, identity, manifest and read-only artifact integrity validators are
implemented and host-tested. Candidate signed-catalog authentication and artifact
composition are implemented against caller-supplied policy; see
[catalog design](PKG1_CATALOG_DESIGN.md). No production trust provisioning,
installer, writer lease, reservation, package database or recovery runner exists.
EH-1B timestamp policy remains open. No kernel ABI or protected contract changes.

## Scope and storage review

Continuation: read-only CLI source is implemented in `user/pkg/cli.c`; isolated
host/mock and freestanding link evidence is in `docs/roadmap/pkg1.md`. It is not
added to default initramfs or release media. `pkg inspect ARTIFACT BYTES SHA256`
checks integrity only. `pkg verify CATALOG PACKAGE ARTIFACT` requires policy
supplied by trusted embedding and kernel UTC; default `pkg_main` has no policy
and rejects it. No CLI key bypass, install/remove command or mutable state.
Lease/reservation/durable-state review is now concrete in
[PKG2_TRANSACTION_REVIEW.md](PKG2_TRANSACTION_REVIEW.md), including the unresolved
cross-boot recovery-space problem. It does not authorize kernel changes.

First milestone: one static executable and up to fifteen documentation files,
one application at a time. Local signed bundles first. No scripts, links,
devices, users/groups, services, configuration, dependencies on other packages,
base replacement, self-update, kernel/boot/ESP or partition changes. Network
repository and trust rotation implementation follow separate gates.

| Implemented operation | Use and qualification |
| --- | --- |
| SYS_RENAME_REPLACE 84 | Both regular-file names must exist, same healthy journaled mount, singly linked closed source. Consumes source name; retains old destination handles. No directory replacement. |
| SYS_STAGE_WRITE 85 | Append at expected EOF/shared offset, non-O_APPEND, at most 32 filesystem blocks and 128 KiB, four snapshot credits; pre-I/O EAGAIN is possible. Loop on positive short counts. |
| SYS_FSYNC 86 | Retains fd and synchronizes its selected ext4 mount, including cleanup/device flush; not an isolated inode or a writer freeze. Name durability must include the relevant mount. |
| Existing SYS_RENAME | Absent destination only; EH-6 can relocate bounded directory trees atomically on journaled ext4. It cannot exchange a live tree or commit a package plus database. |

Source review: src/fs/ext4.h, vfs.h, vfs.c, ext4_mount_journal.inc,
src/kernel/syscall.c and both ext4_stage_test.inc files (historical names,
production dispatch). Their fixture-only comments are stale relative to the
unconditional includes/84–86 dispatch and public aliases; no runtime change is
made here. Backend clamps a batch to 32 blocks minus its unaligned EOF prefix.
Contracts/evidence: EXT4_ATOMIC_REPLACE_DESIGN.md, EXT4_STAGING_BATCH_DESIGN.md,
EXT4_DIRECTORY_RENAME_DESIGN.md, EXT4_DURABILITY_HEALTH_DESIGN.md and corresponding
roadmap atomic-replace, stage-prototype, directory-rename, durability-health and
package-scale reports. Earlier deferred-fsync wording describes EH-3 history;
86 now exists. EH-4 proves a storage workload, not this transaction protocol.
TLS phases 0–5 are accepted in their recorded scope; HTTPS never authorizes a
package. EH-6 is useful for absent immutable documentation generations only.

## Candidate FPK v1 bytes

All integers unsigned little endian, no native structs, padding or implicit NUL.
Envelope is exactly 128 bytes:

| Offset | Bytes | Value |
| --- | --- | --- |
| 0 | 8 | `FORTFPK` followed by zero |
| 8 / 12 / 16 | 4 each | version 1 / header length 128 / flags zero |
| 20 / 24 / 28 | 4 each | manifest length / zero / zero |
| 32 / 40 | 8 each | compressed payload length / expanded USTAR length |
| 48 / 80 | 32 each | SHA-256 of exact manifest / compressed payload bytes |
| 112 | 16 | zero |

Manifest length 1..16384; compressed 1..8 MiB; expanded 1..16 MiB.
Container length is exactly 128 + manifest + compressed; no suffix. Envelope
parser success establishes structure only. The reader assembles 128 bytes in
caller context across arbitrary short reads; EOF/error is distinct from retry.
No copying a payload into a stack buffer, allocation or filesystem action.

Candidate manifest encoding: `FMN1`, u16 version=1, u16 file_count (1..16),
u64 build (>0), u64 base_min, u64 base_max (inclusive), 32-byte platform ABI ID,
then four u16-length-prefixed byte strings: package, command, display version,
description. Package/command are 1..32 ASCII lowercase alnum with single
interior hyphens. Display version 1..32 and description 0..256 printable ASCII.
Base range must be ordered and platform ID nonzero. Architecture is fixed by ABI ID to Fortress x86_64;
no runtime editable version string establishes compatibility.

Then file_count records, lexicographically sorted by raw relative path bytes:
u8 role (1 executable, 2 documentation), u8 reserved=0, u16 mode,
u64 byte length, SHA-256[32], u16 path length, path bytes. Exactly one executable,
path `bin/<command>`, mode 0755, 1..4 MiB. Documentation mode 0644, nonempty
files under `doc/`; total expanded bound includes all tar framing. No trailing
manifest bytes. No UID/GID field: all installed objects root:root. Build numbers
order versions; display strings never do. This grammar is a review candidate;
manifest parser and independent Python wire vectors are implemented; a complete
release artifact producer is NOT implemented. Independent Ed25519 catalog
vectors now test the candidate role described in PKG1_CATALOG_DESIGN.md.
An independent deterministic Python fixture producer covers full containers.

Payload: exactly one gzip member, flags zero (no optional fields), valid
CRC32/ISIZE, no concatenation/suffix,
containing strict USTAR regular files in manifest order. Require zero numeric
uid/gid, exact modes/sizes, empty link/prefix names and no extensions. Validate
octal fields/checksum without overflow, exact zero padding, two terminal zero
blocks and no extra blocks. Reject duplicate paths, unknown types, sparse/PAX/GNU
entries, embedded NUL suffix ambiguity, absolute paths, empty/dot/dotdot
components, backslashes, colon, control/non-ASCII bytes; component <=63 bytes,
relative path <=100 (USTAR name field with forbidden prefix). Exactly 100-byte
names use the entire bounded field without a terminator; shorter names must
zero-fill the remainder. This narrows the older 192-byte candidate to avoid an
unrepresentable archive name. No filesystem extraction until whole artifact authorization,
manifest, member hashes, stream lengths and strict ELF validation pass. During
verification a bounded private quarantine copy may be written, never published.

ELF acceptance must duplicate the actual loader's file AND mapped-image 4 MiB
limits, static executable profile and segment permission checks; a small file
with oversized BSS fails. A separate trusted platform/base descriptor is needed.
Use existing SHA-256, Ed25519 and decompression code; do not design new crypto.
All workspaces in caller-owned BSS/context; no recursion or payload-sized memory.
Require <=1024-byte compiler frames and measured full Ring 3 call-chain/argument/
signal stack margin. The loader currently maps 16 KiB of user stack; its argument
table budget is constrained within the top 4 KiB. Earlier plan references to a
4 KiB total user stack were stale. Preserve the conservative 1024-byte frame
gate; no compiler frame check alone proves full call-chain safety.

## Implemented read-only profile

Caller-owned context is 19,808 bytes, with borrowed manifest views and one
serialized static gzip decoder workspace. Reads accept arbitrary positive
prefixes; negative/overreported reads fail and zero means EOF. The verifier
issues no extraction callback or syscall. It compares whole-container length
and SHA-256 to externally supplied identity and checks manifest, compressed
payload and every file digest. External identity is not authenticated by this
API. Every error clears `verified`; partial context data is scratch. Input,
expected hash and reader storage must not alias that context.

Numeric tar fields use zero-padded octal plus NUL; checksum is six octal digits,
NUL, space. Mtime, uid/gid, uname/gname, device, prefix and remaining reserved
bytes must be zero; regular type is ASCII `0`. Mode/size match the manifest.
Gzip flags are zero; timestamp/XFL/OS convey no filesystem metadata. Padding is
zero, exactly two terminal tar blocks are required, no concatenated gzip.

ELF checks use loader constants from `src/kernel/elf.h`, safe little-endian
decoding, file bounds, W^X, page-zero/canonical limits, page congruence,
page-rounded overlap including reserved regions, mapped-page budget and entry
coverage. Package policy is stricter than the loader: phoff >=64 and reject
PT_DYNAMIC/PT_TLS as well as PT_INTERP. It never loads/executes the ELF or proves
syscall/ABI compatibility. Review this validator when loader policy changes.

`make test-pkg-verify-host` covers complete artifacts. Results and shared gzip
EOF-progress correction are recorded in `docs/roadmap/pkg1.md`. No Ring 3,
installed NVMe, filesystem recovery or physical acceptance is claimed here.

## Authorization, ownership and conflicts

FPK hashes detect corruption, not authorship. Install accepts only an artifact
whose full length/hash is in a valid pinned Ed25519 catalog, including offline
bundles. Domain-separate signed root/catalog bytes with versioned role and
repository UUID. No unsigned local override, SSH host signing key or TLS-only
authorization. Retain plan's threshold roles, sequential dual-threshold root
rotation, unique signers, expiry and persistent revision/build floors. Exact
root wire bytes/provisioning and final catalog-format approval remain separate
decisions; a tested candidate catalog format now exists. Do not call
the envelope parser a trusted verifier. Floor publication must precede intent
and cannot be undone by package removal, cache cleanup or interrupted install.
Reject bad/unavailable time; no claim against whole-disk rollback or withholding.

Derive destinations: `/usr/bin/<command>` and immutable
`/usr/share/pkg/<package>/<build>/doc/...`. No arbitrary destination in a record.
All parents must be root-owned, non-writable by others and on the admitted
installed filesystem; reject unexpected types/mount transitions. No claiming an
existing unmanaged path. Check current digest, mode, owner and complete inventory
ownership before update/removal; local changes block. Reject base names, shell
builtins and collisions with any base command even outside /usr/bin. The base
ownership descriptor must come from trusted provisioning, not a scan that adopts
unknown files. Ancestor substitutions by another privileged writer are outside
protection until a reviewed namespace exclusion contract exists.

Only installed-base ABI/range dependency. Reject optional dependencies, conflicts,
provides, alternatives and unsupported metadata rather than ignoring fields.
Same package/build/digest may be a verified no-op; same build/different digest
fails. Upgrades strictly increase build. Renaming command/package is unsupported.
128 installed packages maximum; inventory <=256 KiB, intent <=16 KiB.

## Durable state machine (proposed, mutation disabled)

One kernel-enforced owner-lifetime writer lease is preferred. No such ABI exists.
O_EXCL lock files with PID/time/read-then-unlink stale takeover are insufficient.
Handle lifetime/dup/inheritance, owner death, recovery takeover and competing root
namespace writers need discussion before implementation. No new rank-1 nesting.

State is a canonical whole-inventory snapshot with generation, predecessor hash,
trust floors and bounded ownership records, root:root 0600. Pending is immutable
checksummed canonical intent, private directory 0700. Hashes bind exact old/new
state bytes, staged artifact identity, fixed operation roles and generation.
Checksums detect damage, not malicious root. First state creation uses absent
rename; later state publication uses 84 with a closed durable source.

1. Acquire lease; reject existing pending except exclusive recovery. Validate
   installed root, health, trust, base descriptor, current files and space budget.
2. Fully verify package. Prepare authoritative immutable staged bytes, a separate
   activation copy, complete next inventory, intent candidate and cleanup list.
   Set metadata, close sources, hash/read back, then 86 on retained mount fd.
3. Publish documentation to an absent immutable generation (EH-6 rename optional),
   sync. It is not yet referenced by inventory. Publication before intent is safe
   only because it is immutable, unreachable from executable behavior and tracked
   as transaction-owned garbage. No overwrite of an existing generation.
4. Publish intent to absent pending name with no-replace rename; sync. This is
   the durable decision to roll forward. Preserve authoritative stage independently
   from the activation source consumed by rename. Never start another transaction.
5. Install: no-replace rename to absent command. Upgrade: 84 to expected existing
   command. Remove: unlink only exact owned command after durable intent. Sync,
   verify destination bytes/metadata or expected absence. Command visibility can
   precede inventory. No package-wide atomic visibility is promised.
6. Publish complete next inventory, sync and verify. This is the success commit
   point for pkg readers; they must report pending while it exists. Delete pending,
   sync; only then garbage-collect transaction stage/old docs in bounded steps.
   Do not report durable completion on failed pending-retirement barrier.

Recovery under lease revalidates checksummed intent, trusted authorization,
old/new inventory hashes, stage and fixed derived destinations. Do not execute
arbitrary paths from disk. Compare full content AND metadata, not digest alone.

| Observed durable state | Recovery action |
| --- | --- |
| No pending, old inventory | Reclaim only proven transaction-owned pre-intent garbage; no activation |
| Pending, old inventory, old/absent expected command | Publish from retained verified stage then next inventory |
| Pending, old inventory, complete new command | Publish next inventory without repeating activation |
| Pending, next inventory, complete new command | Retire pending and finish bounded cleanup |
| Remove pending, old/next inventory, expected absence | Finish next inventory/retirement |
| Unknown inventory hash, foreign/modified command, missing stage, malformed intent | Stop with pending preserved; explicit diagnostic/offline review |

Remove pending with an unchanged old command may unlink it once. Never delete an
unexpected command or infer ownership from basename alone. A consumed activation
copy may be recreated from authoritative stage only with reserved resources.
Repeat recovery twice and after every recovery interruption. Recovery is roll
forward; no promised rollback, automatic downgrade or forced repair. Mixed
executable/database states are visible until recovery, while optional command
bytes are always a complete old/new version. Multi-file runtime assets remain
unsupported: directory relocation alone does not solve generation activation.
No boot ordering changes; recovery is explicit from unchanged base tooling.

## Space and errors

One transaction; staging quota 32 MiB plus bounded metadata/database workspace,
artifact cache quota 16 MiB. These are upper bounds, not admission guarantees.
Precompute actual block-rounded artifact, authoritative files, activation copy,
new inventory/intent, directory growth/extent metadata, old pinned generations,
recovery recreation and cleanup overhead. Include inode, journal/cache/orphan
limits. Preserve the kernel's 256 KiB +16 inode account floor; package operations
never select its fixed-role bypass. Raw mount free counts include this floor.

Admission still requires a reviewed reservation honored by competing log/account
allocations, or another proved exclusion mechanism. Free-space preflight and
package-only lease do not reserve space. This is a hard target-mutation blocker.
Preallocate activation and state material before intent; do not assume overwriting
allocated bytes is atomic or that recovery needs zero additional credits.

Before intent ENOSPC aborts without live command change, retaining/reclaiming only
owned garbage. After intent it leaves pending and recovery material; block new
work, no blind cache eviction. Cleanup is restartable; partial cleanup never
removes the only recovery copy. Account operations retain their reserved floor.
On taint/namespace uncertainty stop mutations and require admitted reboot recovery.
EIO/failed flush is uncertain, never evidence for retry or rollback. Healthy
resource EAGAIN follows bounded retry only when source contract proves pre-I/O;
recovery-required uses EH-3 drain/revalidate rules, never a general retry loop.

## Acceptance gates and decisions

PKG-1a (this change): host-only envelope/identity/manifest tests under ASan/UBSan, strict
freestanding compilation and stack records. Does not authorize file extraction.
PKG-1b: finalize manifest/producer/verifier with independent Python golden bytes,
one-byte/random short reads, truncation at every offset, max/+1 bounds, integer
wrap, unknown fields, path attacks, gzip/archive adversaries, signed hash/threshold/
expiry/rollback and ELF/BSS rejects. Determinism checked across two independent
builds. Full stack gate in private Ring 3 fixture before claiming target support.
PKG-2: only after lease/platform/provisioning/reservation contracts are approved,
implement the transaction and actual-code host fault adapters. Enumerate before/
after every write/flush/publication and every recovery step, all persistence
profiles, journal wrap, ENOSPC blocks/inodes, pool exhaustion, concurrent logs,
two writers, owner death/PID reuse, SSH loss/signals, retained opens/spawns, local
edits and corrupt/missing recovery material. Assert exact inventory/bytes/metadata,
account floor, unrelated data, allocation ownership and repeated recovery.
Independent Linux replay/fsck/debugfs audits on disposable copies.

Installed acceptance: private copied-workspace kernel/initramfs, release-excluded
probe, disposable regular-file GPT NVMe ONLY, BIOS/UEFI x SMP=1/4; validate final
QEMU argv every boot, bound waits, terminate QEMU, reboot twice and audit with
Linux after each phase. Reject mutations on live/nonjournaled/tainted roots.
Retain failures/source hashes and concurrent-work preservation qualifications.
No release ISO/image, physical disk, installer rollout or physical power-loss
claim. Existing EH-4/EH-6 runs are prerequisites, not PKG acceptance.

Input required before PKG-1b/2 dependent work:

- Confirm candidate gzip/USTAR framing and 16-file single-command scope; simpler
  raw framed payload is possible but would change the existing proposal.
- Select platform ABI/base descriptor authority and provisioning/version policy.
- Select signing custodians, initial fingerprints/thresholds (recommended 2-of-3
  offline root), catalog authority and offline expiry recovery policy.
- Approve a separate lease/reservation design review; no syscall number chosen.
- Confirm explicit manual optional-package recovery is sufficient initially;
  automatic pre-service recovery would require protected boot-order discussion.

These unresolved decisions do not prevent testing pure syntax. They do prevent
claiming an installable package format or enabling target mutation.
