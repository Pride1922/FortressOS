# µSSH Phase 3: host identity design review

2026-10-17. Design proposal against `d67ccf0d16930270770e9e8ea7b94e806c0b79b9`.
Implementation added 2026-10-17; acceptance is recorded separately below.
No SSH daemon or physical acceptance is claimed.
Parent: [crypto/SSH plan](CRYPTO_SSH_PLAN.md), Phase 3 / issue #52.
Implementation evidence and limits: [host-key report](../roadmap/ssh-phase3-hostkeys.md).

## Initial review outcome

The crypto and retained descriptor metadata prerequisites are available.
Provisioning is **not ready to implement as a userspace open/write/rename
sequence**. The missing contract is safe creation and publication against a
retained installed-root directory, including crash recovery. Build the bounded
codec first; review the storage interface before wiring provisioning or boot.
Keep all physical write gates and default SSH disablement intact.

### Implemented decisions

The storage API is now `SYS_HOSTKEY` 79, `(operation, buffer, bytes)`.
`HOSTKEY_READ` is a **private load**, restricted to effective UID 0 plus
CAP_SYS_ADMIN, requiring a writable 1024-byte buffer and returning its used
length. `HOSTKEY_PUBLISH` accepts 1..1023 readable bytes and returns zero.
Errors precede side effects: invalid operation/size, privilege, user range,
installed-root availability, allocation/backend failure. Failed reads publish
no output. Live roots have no backend; detached installer mounts cannot be
selected by this syscall. The ABI is in `src/include/hostkey_abi.h`.

Crypto stays exclusively in userspace. The kernel publishes opaque bytes from
an authorized root caller; it does not assert their cryptographic validity.
The command validates generation and independently loaded bytes. Every future
monitor must decode/validate the private container before signing. There is no
persistent provisioned flag, public status syscall, implicit generation, or
daemon enablement. Public export happens in the privileged command.

The synchronous backend operation owns its references and scratch until return;
there is no cross-call lease or PID-owned transaction to strand on exit.
One atomic operation gate returns EAGAIN on contention. Backend mutations retain
existing filesystem locking and journal transactions. On any uncertain failure,
staging residue remains for explicit administration; rollback never unlinks a
name. Existing canonical keys are never replaced. Privileged namespace mutation
remains outside the integrity threat boundary stated below.

Installer v4 preserves the v3 prefix and adds a bounded optional input pointer
and length. To preserve the userspace-only crypto boundary, generation occurs
on the target **after confirmation and exclusive lease acquisition**, just
before synchronous deployment. Publication still occurs after detached root and
account preparation. This resolves the provisional generation timing below;
no crypto enters kernel storage code. Missing entropy sets a host-key warning
and permits a healthy installation with SSH disabled. Storage failure remains
fatal. Private bytes are wiped after deployment, and runtime retained-error
owners wipe their secret buffer too. Final freeze/discard is followed by a
fresh detached mount and exact metadata/byte validation before acceptance.

`/sbin/ssh-keygen --provision` creates or validates the fixed identity;
`--public` and `--fingerprint` export public information. All require root and
CAP_SYS_ADMIN. Existing valid identity is retained; invalid existing data fails
without rotation. The parser accepts zero padding bytes for an already aligned
private section, matching the pinned OpenSSH serializer's loop.

| Inspected implementation | Consequence |
| --- | --- |
| `src/include/crypto.h`, `src/lib/crypto/curve.c` | Keygen obtains fresh strong entropy; secret representation is seed[32] followed by public[32]. Recompute from the seed when loading, rather than trusting the stored public half. |
| `src/kernel/syscall.c:sys_fstat`, `docs/roadmap/fstat.md` | Descriptor metadata survives name replacement, but snapshots neither authenticate ancestors nor freeze later metadata changes. |
| `src/fs/vfs.h`, `src/fs/vfs.c:open_common` | No O_EXCL, O_NOFOLLOW or directory-relative user ABI. O_CREAT retries lookup after EEXIST; it must not be used as exclusive secret creation. |
| `src/fs/ext4.h`, `ext4_permissions.inc`, `ext4.c` | Owned nodes, creation attributes and backend no-replace rename are reusable. No-replace is not a universal user-requested rename guarantee. Actor decisions occur under filesystem exclusion. |
| `src/kernel/install_runtime.c:provision`, `root_file_at` | Detached target provisioning already has an exclusive lifetime and freeze/discard. Existing byte readback precedes final freeze; it does not establish independent durable host-key readback. |
| `src/kernel/syscall.c:sys_sync` | Calls storage_sync_all; no per-file fsync or retained-directory durability ABI. Do not infer target-specific durability from its name or stale /mnt comment. |

## Identity and threat boundary

One canonical private file: `/etc/ssh/ssh_host_ed25519_key`, regular, uid/gid
0:0, exact permission bits 0600 (no special bits). Require `/`, `/etc` and
`/etc/ssh` to be actual directories, root owned, with no group/other write or
special bits; create ssh as 0700. Bind traversal to the admitted installed-root
mount. Reject unsupported node kinds, mount crossings and ambiguous paths;
never follow links or substitute live-media files. The implementation must
establish these properties while retaining each object, not stat then reopen.

Defend against unprivileged pathname changes, malformed files, storage failures,
concurrent provisioners and interruption. A malicious privileged writer or
offline disk modifier is outside the integrity boundary; permissions and
checkints do not authenticate the disk. Never log key bytes, seeds, decoded
containers, or secret-bearing buffers. RAM wiping/unlink is not disk erasure.

The private container is authoritative. A .pub file is optional derived export,
never a second identity or readiness record. Fingerprint is SHA256 over the
51-byte SSH public blob, printed as SHA256: plus unpadded base64. Public export
uses `ssh-ed25519 <base64(blob)>` with a fixed public comment. No machine secrets
or usernames in comments. Missing, malformed or unsafe existing identity fails
closed; neither boot nor daemon startup generates or repairs a key.

## Bounded OpenSSH codec

Pin the reference profile to OpenSSH portable `V_10_2_P1`
[PROTOCOL.key](https://raw.githubusercontent.com/openssh/openssh-portable/V_10_2_P1/PROTOCOL.key)
and [RFC 8709](https://www.rfc-editor.org/rfc/rfc8709), with the matching upstream
Ed25519 serialization source pinned and hashed before codec acceptance.
This is a format baseline, not a claim about the latest client release.

Accept only armored openssh-key-v1, cipher none, KDF none, empty KDF options,
one Ed25519 key and an exactly consumed container. Set total armored input cap
to 1024 bytes and comment cap to 64 printable ASCII bytes. Writer uses a fixed
comment and LF, 70-column base64; reader permits LF/CRLF at line boundaries,
validates base64 padding/unused bits, and rejects extra data or embedded NULs.
All SSH strings use checked big-endian uint32 lengths; bound before advancing.

Decode public blob as string algorithm plus string 32-byte key. Decode private
section as two equal checkints, string algorithm, string public[32], string
secret[64] = seed || public, string comment, and sequential padding to the
none cipher's 8-byte block boundary. Confirm exact padding behavior against
the pinned serializer, including already aligned input. Require all three
public copies to match the seed-derived public key. Equal checkints alone are
not validation. Check every length, key count, algorithm, padding byte and EOF.
Reject encryption, alternate KDFs, multiple keys and unsupported variants.

Generation obtains seed through crypto_ed25519_keygen and a fresh random
checkint; any random failure aborts without publication. Perform a fixed local
sign/verify self-check. Own and wipe seed, secret, signatures, decoded/private
serialization, hash contexts and all failure scratch. Keep bulk buffers off
the stack; preserve the existing verifier-only stack exception.

## Original storage proposal (superseded by implemented decisions above)

Prefer a narrow installed-root host-identity service over introducing a broad
openat/fsync family solely for this task. Proposed operations are status/public
export and explicit root provision. No syscall number or struct ABI is assigned
by this review. Specify credential snapshot, copy limits, error precedence,
retained references and process-death cleanup before implementing that ABI.
The later privileged SSH monitor needs a separately reviewed private-load path;
the status/export operation must never return private material.

The service owns one bounded provisioning transaction at a time, with no
spinlock held across randomness, crypto, I/O waits or process switches. Use
existing filesystem exclusion for each authoritative mutation; do not introduce
rank nesting. Check installed-root readiness and mount durability admission
before generating or writing anything. Installer adapter uses only its already
authorized detached target, never the global live `/etc` namespace.

1. Retain and validate the root/ancestor chain. Existing canonical file: open
   without truncation, validate metadata and codec, return its public identity;
   invalid existing file is an explicit error, never overwritten.
2. Obtain randomness and construct/check the key in owned memory. Atomically
   create a reserved staging name in the retained ssh directory, root:root
   0600 from first visibility, with EEXIST rather than opening another file.
3. Write to that retained object with bounded short-write loops; zero progress,
   overflow, cancellation and I/O errors fail. Validate exact size, metadata,
   seed/public consistency and byte readback. Complete the mount's supported
   durability barrier before publication.
4. Recheck authoritative parent policy and publish the exact staged object to
   the canonical basename with atomic no-replace semantics. A competitor's
   valid key is preserved; never replace or truncate it. Sync publication.
5. Release cached mount state where the lifecycle permits, independently reopen
   and reread through the admitted target, validate bytes/metadata/pair, and
   compare the public identity. Only then report provisioned. Installer proof
   includes freeze, discard and fresh mount before final acceptance.

The implementation must prove data-before-name ordering and old-or-new namespace
recovery from actual journal code and fault tests. Two successful-looking calls
are not proof. Post-publication barrier failure returns uncertain/disabled,
never success and never deletes the canonical name as rollback. On next boot,
normal journal recovery plus full key validation and a successful required
barrier can establish readiness anew; no stale boolean bypasses validation.

Staging residue never authorizes loading or automatic regeneration. Return an
explicit incomplete-provisioning diagnosis; administrative recovery validates
the canonical key first, and removes only verified service-owned residue under
transaction exclusion. Do not unlink an arbitrary preexisting staging file.
Before publication, cleanup may remove only the exact retained object created
by this transaction; uncertain durability still remains a reported failure.

## Installation, rotation and readiness

Generate on the target after detached root/account preparation, before final
freeze. A pre-write entropy/unavailable failure can leave an otherwise healthy
installation usable with SSH disabled. A tainted filesystem or failed final
freeze remains an installation error under the existing contract; optional SSH
must not swallow storage failures. No generated key enters payload templates.

For existing installations use an explicit privileged provisioning command.
Successful provisioning does not enable SSH. Every daemon start validates the
canonical key again, and protocol randomness retains Phase 1's fail-closed gate.
Live mode remains disabled. Rotation is explicit, requires the daemon stopped
and signing references drained, and is a separate follow-up design: current
no-replace rename does not supply atomic replacement. No delete-then-create
rotation or claim of interruption-safe rotation is authorized by this design.

## Implementation order and acceptance

1. Pure codec/public export with actual-code ASan/UBSan negative/fuzz/wipe tests;
   independent OpenSSH import/export and SHA256 fingerprint comparison.
2. Review and implement retained-directory/exclusive-create/publication service
   and death cleanup; test competing creators and ancestor/name replacement.
3. Detached installer and explicit installed-root provision adapters, followed
   by restart/reboot readiness and failure diagnostics. Keep service disabled.
4. Disposable BIOS/UEFI installed-root gates with independent Linux metadata,
   bytes, journal recovery and fsck audits; only then assess issue #52 closure.

Inject ENOSPC, OOM, short/zero writes, read errors, barrier failures and cuts at
every mutation/barrier boundary, including rename and post-publication verify.
Cover wrong owner/type/mode/ancestor, truncation, mismatch, oversized lengths,
bad armor/padding, malformed variants, concurrent provision/death and leftover
staging. Verify old valid identities survive and invalid keys never become
ready. Two fresh targets must differ; each target retains its fingerprint across
daemon restart and reboot. Regress accounts, permissions, FSTAT and installer
exclusions. Release archives/images must contain no generated private key.

Production-random test secrets stay only on disposable private fixtures; no
secret logs or retained secret-bearing disk artifacts. Retained evidence records
public fingerprints, metadata, counts and results. Deterministic published test
vectors may be retained and must be clearly distinguished from provisioned keys.
No network listener or physical write is needed for this review or codec gate.

## Rotation and incomplete-provisioning recovery review (2026-10-17)

Neither operation is implemented or enabled by this review. The existing
no-replace backend cannot provide atomic rotation, and ordinary pathname unlink
cannot establish that the object removed is the retained staging object.
Adding a delete-then-create command would violate preservation of the old valid
identity. No syscall number or operation value is reserved here.

Rotation needs all of the following before implementation:

1. An explicit root/CAP_SYS_ADMIN request carrying the expected old public
   fingerprint. Refuse invalid/unsafe old keys, stale expectations, missing
   entropy, pending recovery and concurrent provisioning. A broken key is not
   permission to regenerate it.
2. A proven stopped daemon and drained monitor/signing references. There is
   currently no daemon lifecycle with which to establish this condition; a
   missing pidfile or a process-name check is insufficient.
3. A new filesystem transaction which compares the retained old inode and
   parent/mount identity and replaces the canonical entry with the fully
   validated, durably staged new inode in one journal commit. Existing rename
   explicitly refuses replacement. Old-inode orphan/reclamation and rollback
   must be audited together; no new lock rank or nested rank-1 lock is implied.
4. A successful publication barrier and independent private/public/metadata
   readback before returning the new public fingerprint. A failed barrier
   reports uncertainty, never successful rotation or a deletion rollback.
   Recovery may find either complete identity; a stale retry must compare the
   current fingerprint again. Journal copies preclude secure-erasure claims.
5. Actual-code fault tests at every mutation/barrier and concurrent-creator,
   stale-expectation, retained-old-reader, ENOSPC/OOM, reboot and OpenSSH
   changed-key tests. Physical and daemon-restart acceptance remain separate.

Incomplete provisioning has three distinct cases. A valid canonical identity
is preserved regardless of residue. Missing canonical plus residue is diagnosed
as incomplete, never automatically promoted or regenerated. Invalid canonical
is diagnosed as corrupt and preserved for explicit administrative recovery.
None of these conditions authorizes deleting an arbitrary reserved-name file.

A future cleanup interface must retain the exact staging object, validate the
ancestor chain and mount, and compare object identity under filesystem exclusion
in the unlink transaction. A pathname, mode/UID check, or byte hash alone is not
an object-identity or provenance proof. After reboot, current in-memory ownership
is gone: a durable service-owned receipt tied to filesystem identity, inode
generation and transaction identity would need its own bounded journal/crash
design. No such receipt exists today. Until it is reviewed and tested, cleanup
and promotion stay unsupported; no automatic rm, temporary-name reuse or boot
repair is added. Existing canonical loading must still validate the complete
container and metadata and must never interpret residue as readiness.

Issue #52 therefore stays open for explicit rotation/recovery contracts and
their implementation/acceptance, the future daemon/monitor restart boundary,
and agreed physical host-identity provisioning/persistence acceptance. Passing
the automated installer and ABI gates does not close those separate claims.
