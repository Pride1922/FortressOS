# Private pending/state/trust validation before replay

Continuation: [low-space account recovery and charged refunds](PKG2_LOW_SPACE_REFUNDS.md)
replaces the conservative private account-capacity floor described below with
actual remaining role capacity, including the private account-service BEGIN
gate. Package-charged orphan admission is still a separate open integration.

2026-10-23. Extends [durable account service](PKG2_ACCOUNT_SERVICE_DURABLE.md)
and [grant preview](PKG2_GRANT_PREREPLAY.md). Production integration,
installation, production trust provisioning, release-media builds, commits,
pushes and physical-device writes remain disabled for this task.

`tests/pkg_mount_candidate.{h,c}` implements a read-only bounded record validator.
`tests/pkg_mount_admission.inc` refines it against actual EXT4 inode generations,
names, modes, links, dense maps, bitmap/ownership auditing and effective free
resources. Exact copied-source overlays call it from the existing unpublished
JBD2 preview, before replay, dirty marking, orphan cleanup or account recovery.
No `fs->engine` publication or fabricated trusted flags are used. Rejection
follows the real constructor unwind without repair, writable fallback or login.

## Supported profile and authority

This private profile covers one package/command `demo`, install operation only,
one static executable and no documentation, a retained FPK archive <=8192 bytes,
and inline dense extents. The existing FPK/manifest/ELF verifier and signed catalog
implementation run against actual retained bytes. Payload identity, manifest
package/build/command, base version, platform ABI and executable hash/length must
agree. The actual staged or published executable must match that authorized
manifest. A present destination consumes the activation source; two names or
missing both refuse. General inventory, update/remove operations, arbitrary
paths, links and unsupported ownership classes/flags refuse rather than fall
back to a zero grant.

Catalog keys, root version, exact revision/hash floor, current time, platform
and base version are independent caller inputs. Disk records cannot select them.
The enrollment descriptor is verified using the existing public test-key policy;
it pins actual fixed inode/maps, marker, trust snapshot and base/profile identity.
The resource profile is SHA-256 of `Fortress bounded mount profile v1`, rather
than the prior placeholder bytes. The account profile has its own versioned hash.
There is no production boot selector, root-object codec, root rotation, hardware
antirollback or production key enrollment. The external fixture policy represents
a trusted embedding; checksums alone do not authenticate rewritten mutable disk
records or prevent complete filesystem rollback.

The trust snapshot retains the complete catalog and is pinned by both enrollment
and independent catalog policy. Signatures are still verified even when the
catalog hash is externally pinned. A fully coherent fixture with a forged
signature and matching hash pins must reject. Current trust versions/floors must
match exactly; equal-version/different-byte and rotation-while-pending cases
cannot silently adopt target policy.

New RESERVED admission checks the actual current clock against catalog validity.
Existing IDLE inventory verifies its historical accepted snapshot. PENDING and
RETIRING finish only the same immutable intent, verified at its durable admission
time (not before the trust snapshot's accepted time and not after current time).
Thus catalog expiry can block a fresh reservation while allowing exact existing
recovery. This does not authorize new downloads, payloads or trust rotation.

## Durable state matrix

The existing exact marker/grant/ownership codec remains authoritative. Real
on-disk grant records carry conserved `T=G+Q` for blocks and inodes. Q is refined
against the actual named objects and their allocated blocks, not logical size.
The constructor's full filesystem ownership audit still runs first. An orphan
claimed by a package ownership entry is unsupported and refuses; actual account
orphans retain the separate generation/provenance protocol.

| Grant | State/command | Retained transaction roles |
| --- | --- | --- |
| IDLE | Exact current state and optional authorized installed command/archive | None |
| RESERVED | Exact old state; destination absent | Archive 1, activation 17, next state 18 |
| PENDING | Old state + absent/new command, or new state + new command | Roles 1/17/18 and immutable pending intent 19 |
| RETIRING | Exact new state and command | Roles 1/17/18 and retained retirement intent 19 |

The intent name is `transaction.v1/pending.v1` while pending, and
`transaction.v1/retired.v1` while retiring. Retirement retains the capability
rather than discarding its evidence before charged cleanup. Both names at once,
unexpected transaction children, residual IDLE transaction roles, extra package
metadata, unexplained generations or source/destination aliases refuse. The
activation entry follows its same actual inode to `/usr/bin/demo` when its source
has been consumed. Completed-step bits must be zero in this profile; unsupported
operation/step encodings reject.

The account database remains the coherent locked-root-only host fixture from
the prior service gate. Destination bytes are validated but never executed;
this does not establish ordinary-user application or boot acceptance.

These fixtures initialize a complete enrolled grant after building disposable
objects and derive Q from their actual maps. They do not implement production
grant issuance or new allocation/cleanup transactions. The typed runtime grant
sequence update used by tests changes ledger, ownership sequence and account
snapshot in one actual JBD2 transaction. It allocates nothing and preserves G/Q.
Charged-orphan refund/release and low-space progress remain subsequent work.

## Private canonical records

State, trust and intent are exact 1024-byte records: 16-byte zero-padded role
string, u32 length=1024 at 16, u16 version=1 at 20, flags/operation at 22,
SHA-256 of [0,992) at 992. All unlisted bytes must be zero. These are a new
bounded test grammar, not the proposed general 192-byte-header database ABI.

| State (`FORT-PKG-ST-v1`) offset | Field |
| --- | --- |
| 24 / 32 / 48 / 80 | u64 generation / repository UUID / trust full hash / predecessor state hash |
| 112 / 120 / 152 / 184 | u64 build (0 for explicit initial empty state) / artifact hash / executable hash / u64 executable bytes |
| 192 / 193 | u8 command length / <=32 command bytes plus zero padding through 225 |
| 226..992 | Zero |

Only empty generation one and installed generation two are supported. The
validator computes the canonical empty record's expected hash from the pinned
repository/trust snapshot, without writing or regenerating any disk object.
Every installed/current/next predecessor and active grant old-state hash must
identify that same initial record. Active grant old/new generations are 1/2;
unexplained history refuses even after the old source has been consumed. A
current new state requires the new command. IDLE inventory also binds its
command and archive identities to the authorized manifest and retained bytes.

| Trust (`FORT-PKG-TR-v1`) offset | Field |
| --- | --- |
| 24 / 32 / 40 | u64 root version / catalog revision / accepted time |
| 48 / 64 / 96 | Repository UUID / full catalog hash / u32 catalog length |
| 100..256 | Zero |
| 256 | Complete signed catalog, <=736 bytes; remaining body zero |

| Intent (`FORT-PKG-PN-v1`) offset | Field |
| --- | --- |
| 22 / 24 / 40 / 56 | Operation=1 / transaction UUID / enrollment UUID / repository UUID |
| 72 / 104 / 136 / 168 / 200 | Old state / new state / trust / artifact / admitted-plan hashes |
| 232 / 240 / 248 / 256 / 264 | u64 old/new generations / root version / catalog revision / admitted time |
| 272 / 280 / 288 / 289 | u64 artifact bytes / build / u8 command length / command bytes |
| 328 / 360 | Executable hash / u64 executable bytes |
| 368..992 | Zero |

The plan hash covers full next-state hash, artifact hash and the resource-profile
hash, in that order. The ledger binds that plan, complete intent, old/new state,
trust authorization snapshot, ownership-table hash and fixed transaction slot.

Account generation-record **version 3** preserves the prior service fields and
adds: zero [436,440), u64 grant sequence at 440; hashes of full grant/trust/current
state/retained intent at 448/480/512/544; transaction UUID at 576; zero [592,992).
Absent intent requires a zero intent hash. The existing pair seal and origin
binding cover these fields. All account pair updates revalidate the same durable
grant snapshot. A typed grant update stamps and journals the new snapshot with
both account records. Version 1/2 account records are not migrated or accepted.

The private adapter rejects ordinary writes to protected package data/maps and
changes to exact protected inode slots before commit. Comparing individual inode
slots permits adjacent account inodes in the same table block to change. Only
the explicit typed grant transaction bypasses that fence and still passes the
complete record validator. Account read/write ownership and cancellation remain
under the existing account gate and filesystem exclusion; no lock ranks change.

Actual/planned free counters must exclude G and preserve the existing ordinary
floor plus a conservative full account-recovery cost. This is a deliberately
strict bound, not proof of progress at minimum space. No allocator credits are
raised, and the 64-image transaction limit remains unchanged.

## Tests and evidence limits

Run `make -f tests/pkg_grant_candidate.mk test-pkg-mount-admission` in WSL.
The runner copies source, overlays only those copies, and builds ASan/UBSan and
strict freestanding/frame checks. It covers 1/2/4 KiB x sectors 512/4096 and seven
phase/publication states per geometry, actual service recovery and selected
UPDATE commit cancellations, repeated fresh-process recovery, read-only preview
rejections and committed-uncheckpointed grant/table/account snapshots.

Directed grant-update I/O cuts cover before/after first, middle and last actual
callbacks under four persistence policies. This is a sample of callback cuts,
not an exhaustive package transaction power-loss sweep. Second recovery must
make zero semantic commits. Independent Linux audits compare seals, account/grant
bindings, G/Q conservation, inode generations, modes, owners, links and actual
Blockcount values. A signed-descriptor pin never substitutes for signature checks.

The new pure validator uses caller-owned scratch; failure clears its verified bit
and preserves its result output. The first stack layout exceeded the 1024-byte
ceiling; moving policy scratch into the workspace reduced its final measured stack usage
to 816 bytes (dynamic, bounded; strict frame gate passes). This does not prove a full kernel call-chain stack bound.

Review regressions cover a correctly signed but unsupported two-file archive
and a consumed-source pending state with foreign genesis. The former must clear
artifact verification success on rejection; the latter must reject before replay
writes rather than accepting an unrecognized predecessor. Fully resealed IDLE
command/archive tuple mismatches are separate negative cases.

No production root publication, real signals/scheduler teardown, Ring 3/QEMU,
boot, release-media, installation, physical power loss, torn sectors, unknown-I/O
retry or hardware durability acceptance is claimed. Exact results, source/media
preservation and retained failures are recorded separately in the roadmap.
