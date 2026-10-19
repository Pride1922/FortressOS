# FortressOS package manager

**Status:** DESIGN PROPOSAL, 2026-10-19. No implementation, package repository,
signing service or production update acceptance is claimed.

## 1. Goal and delivery boundary

Install FortressOS on the laptop, connect over SSH, install a tool and upgrade it
without reflashing the USB or reinstalling the OS. Eventually update the supplied
userland through the same verified release pipeline.

The foundations exist: persistent journaled root, owned executable snapshots,
Unix permissions, Ed25519/SHA-256, bounded archive/compression tools, networking,
SSH and Watchtower. HTTPS is **not yet an accepted prerequisite**: TLS-2 is in
progress; the TLS plan still has trust-store, adversarial, wget and firmware gates.

Deliver in two useful milestones:

1. **Application packages:** install/remove/update standalone Ring 3 tools under
   `/usr/bin`, preserving the installed base. Signed local packages come first;
   verified HTTPS repository access follows TLS acceptance.
2. **Base userland updates:** coordinated updates to supplied commands and
   services, with boot-time transaction recovery and explicit SSH lifecycle rules.

Kernel, Limine, ESP, partition layout and installer updates are a separate future
boot-update design. `pkg upgrade` must never imply it updates those components.
The first milestone cannot upgrade `/bin/shell`, `sudo`, `login`, `sshd`,
Watchtower, the package manager itself or any other installer-owned base file.

## 2. Proposed user interface

All commands below are proposed, not commands available in the current image.

```sh
pkg list                       # Installed packages and versions
pkg info netcheck               # Description, files, compatibility and origin
pkg search network              # Search the locally cached verified catalog
pkg status                     # Repository revision, pending work, reboot state
sudo pkg refresh               # Download and verify current repository metadata
sudo pkg install netcheck       # Show plan, then install the signed catalog target
sudo pkg upgrade               # Plan and upgrade eligible installed applications
sudo pkg remove netcheck        # Remove package-owned files only
sudo pkg verify                 # Check installed files against committed records
sudo pkg recover                # Resolve a pending transaction; never guess
```

Mutations display versions, bytes required, additions/removals and any service
impact before a confirmation. `--yes` makes that same plan noninteractive;
`--dry-run` changes nothing. Do not prompt for authorization twice. Local/offline
installation uses a signed repository bundle, not an arbitrary unsigned tarball.

Use `/bin/pkg` without set-ID bits; administrative mutations require root plus
the necessary existing capabilities. Read-only commands work without sudo and
must not expose credentials or transaction-private bytes. `pkg verify` checks
content and metadata; it does not prove absence of arbitrary root compromise.

An SSH session can start a transaction. Disconnect/SIGHUP before durable intent
leaves only reclaimable staging. After durable intent, interruption leaves a
recoverable pending operation. Do not claim transactions survive terminal loss
until real SSH disconnect/signal acceptance demonstrates that behavior.

## 3. Current code constraints and prerequisite decisions

| Existing contract | Consequence |
| --- | --- |
| EXT4 advertises `rename_no_replace=true` (`src/fs/ext4.c`) | Existing `SYS_RENAME` cannot safely replace an installed executable in one namespace operation. Unlink-then-rename is not an acceptable runtime update primitive. |
| EXT4 ordered journaling protects metadata; overwriting visible file data is not promised atomic | Never truncate or rewrite the live executable in place. Stage new bytes, make them durable, then publish. |
| `SYS_SYNC` calls `storage_sync_all()` | A global barrier exists, despite the older `/mnt`-only ABI comment. Per-file fsync is not currently an API; every error must propagate and barrier behavior must be audited for installed root. |
| `vfs_exec_image` captures owned bytes and admission metadata | A running program need not execute newly replaced bytes, but retained descriptors, inode lifetime, set-ID admission and concurrent spawn still require replacement tests. |
| VFS executable snapshots and ELF mapped image have separate 4 MiB bounds | A package builder must enforce both. A small ELF file can still exceed the mapped-memory bound through BSS. |
| Account files have a dedicated fixed-role recovery protocol | Package contents cannot write passwd/group/shadow, account redo, host identities or their reserved paths. |
| Watchtower loads definitions at supervisor startup | `service restart` does not discover changed service files. Installing a new daemon is not equivalent to registering it at runtime. |
| No general package ABI compatibility identifier is established | Define a trustworthy running-platform identity before accepting remotely built binaries. An editable version string alone is insufficient. |

PKG-0 must review an **additive atomic replacement API**: same mounted filesystem,
regular files only, permission checked, immutable/protected paths honored, retained
opens preserved, ordered data-before-publication and bounded journal credits.
It must not change existing no-replace rename semantics or add a raw-write bypass.
Choose the exact API/number only after VFS/EXT4 lifetime and crash review.

This plan does not authorize implementation changes to protected boot ordering,
locking or root ownership. Base recovery integration is a separate reviewed gate.

### Three engineering gates before target mutation

**A. Replacement is an EXT4 transaction, not just a VFS pointer swap.**
Keep the additive API decision open until its lifetime and durability contract is
reviewed. FortressOS does not gain a new per-dentry lock from this proposal: use
the existing authoritative filesystem exclusion and journal writer discipline.
Do not nest another rank-1 lock, invalidate retained opens, or hold locks across
scheduling. A cache/dentry change alone cannot establish on-disk atomicity.

The operation must couple source-name removal, destination replacement, link
counts, overwritten-inode orphan handling and cache publication into the reviewed
bounded metadata transaction. New file contents and metadata are synced before
publication. Test crashes at each underlying journal write/flush boundary, including
commit and checkpoint, then replay and inspect independently. After recovery the
destination must name the complete old or complete new file, never be absent or
partially overwritten. A successful post-publication durability barrier requires
the new file to survive reboot. Failed barriers remain uncertain failures.

Also prove concurrent lookup/spawn sees an admitted complete version; an already
open destination descriptor retains the old object until final close. Include
same-inode, cross-filesystem, source/destination type, permissions, protected-path,
ENOSPC/credit exhaustion and orphan-reclamation cases. JBD2's presence alone is
not acceptance evidence for a newly implemented operation.

**B. Streaming parsing still needs a whole-call-chain stack budget.**
The container reader is an explicit bounded state machine: fixed header,
manifest framing, manifest records, compressed payload, archive records and final
digest/length checks. It must accept arbitrary short reads, including one-byte
fragments; SHA-256 consumes exactly the verified stream incrementally. A 512-byte
I/O buffer belongs in the manager's BSS/context, rather than adding 512 bytes to
every active stack frame. SHA-256 state, gzip workspace, archive header and parser
tables must also be accounted for. Streaming does not make nested buffers free.

Compile with stack-usage output and a per-frame ceiling, inspect the deepest
parser/decompressor/hash/syscall chain, and exercise it in real Ring 3. Keep ample
room for entry arguments, return frames and signal delivery within the existing
4 KiB stack. Host ASan/UBSan cannot prove that user-stack budget.

**C. Boot identity is necessary for persistent owner records, but insufficient
for safe stale-lock takeover.**
FortressOS currently has neither Linux's `/proc/sys/kernel/random/boot_id` path
nor an established public stable process-birth handle. Do not pretend either API
exists. Uptime or wall-clock timestamps cannot substitute for a unique boot ID:
they repeat across boots, and wall time can change.

An owner record can contain a kernel-issued boot identity, PID, process generation
and acquisition token. A different boot identity proves the *recorded owner* is
from a prior boot. It does not prove the pathname still names that record when
another process unlinks it. Two reclaimers could otherwise delete each other's
fresh lock; an empty/torn record can also belong to a live creator that has not
finished writing it. A same-boot crash additionally requires process-generation
checks, not just PID liveness.

Preferred PKG-0 direction: review a kernel-owned exclusive writer lease tied to
the actual owner lifetime, released on owner exit/reboot, with an unforgeable or
kernel-validated acquisition identity. Persistent intent survives independently;
a lock file is diagnostic, not the source of exclusivity. Exact handle, close,
dup/inheritance and cancellation semantics require review before choosing an ABI.
If a filesystem lock is selected instead, acceptance requires atomic conditional
claim/quarantine and release against the exact observed file identity; ordinary
read-record-then-unlink is prohibited. Neither mechanism is implemented here.

Prove concurrent acquisition, death before/after owner-record publication, same-
boot PID reuse, reboot with reused PID, two simultaneous stale reclaimers, failed
release, inherited handles and crashes during recovery. A malformed record must
never authorize deleting a potentially live writer's lock.

## 4. First milestone: deliberately small application packages

First accepted package class:

- One statically linked executable, one public command name, optional documentation.
- Only dependencies on a fixed compatible installed base; no optional-package
  dependency graph, shared libraries, interpreter requirements or plugin loading.
- No maintainer scripts, shell hooks, arbitrary service actions, users/groups,
  set-ID bits, capabilities, devices, links or writes to configuration/user data.
- Executable destination is derived as `/usr/bin/<command>`; it is not chosen
  by an arbitrary archive path. Documentation stays in the package's immutable
  content directory. Runtime assets at fixed global paths are deferred.

This fits the current freestanding userland well. Later versions may add richer
packages after defining how their multiple runtime files become visible together.
Do not advertise arbitrary Linux ELF compatibility, Debian/RPM compatibility,
or installation of software whose required syscalls/runtime do not exist.

Add `/usr/bin` to the shell's normal PATH after `/bin` and `/sbin`, and verify
completion and `sudo` lookup separately. Refuse package command names colliding
with shell builtins or any base executable. Never replace an unmanaged file.

`pkg upgrade` commits one application at a time and reports partial batch success
precisely. Because v1 applications cannot depend on each other, this does not
create an unsupported mixed optional-dependency graph. It is not an atomic
whole-system upgrade.

## 5. Package and repository formats

Use a dedicated `.fpk` container and a strict parser, not privileged invocation of
general `tar` extraction. Proposed envelope:

1. Versioned fixed header: magic, header length, manifest length, compressed and
   expanded lengths, manifest SHA-256 and payload SHA-256; reserved bits zero.
2. A canonical manifest using fixed-width integers and explicitly length-prefixed
   strings. No ambiguous text normalization, native structs or unknown fields.
3. A bounded gzip-compressed USTAR payload containing the exact listed files.

The manifest binds package name, display version, monotonically increasing build
number, architecture, compatible platform epoch/base range, command, file sizes,
SHA-256 digests and final mode. The signed catalog binds the **entire container's**
length and SHA-256, so unsigned envelope fields cannot redirect verification.
Freeze byte layouts and domain-separation strings in PKG-0 with golden vectors;
these logical fields do not yet constitute a finalized wire ABI.

Reject absolute/traversal/dot-component names, duplicate or case-confusing command
identities, links, special files, GNU/PAX extensions, sparse entries, unsupported
permissions, extra payload members, integer wrap, trailing garbage, truncated
gzip and mismatched manifest/container/header lengths or hashes. Check entry
checksums, gzip CRC/ISIZE and exact expanded-size totals. Archive metadata never
overrides the manager's ownership and path policy.

Initial proposed hard limits, to be validated against fixtures before freezing:

| Item | Bound |
| --- | --- |
| Package/catalog name | 32 ASCII bytes; lowercase letters, digits and single interior hyphens |
| Display version | 32 bytes, informational; ordering uses uint64 build numbers |
| Installed packages / catalog packages | 128 / 256 |
| Files per application package | 16, including exactly one executable |
| Manifest / catalog | 16 KiB / 256 KiB |
| Compressed / total expanded package | 8 MiB / 16 MiB |
| Command component / manifest relative path | 32 / 192 bytes; each component <64 bytes |
| Root keys / signatures per object | 8 / 8; unique signer IDs only |
| Root metadata / transaction record | 8 KiB / 16 KiB |

No payload-sized allocation or user-stack buffer. Stream through small fixed
buffers; keep parser tables in bounded BSS/context storage. Preserve the 4 KiB
Ring 3 stack and independently check nested call chains, not just single frames.
These limits are manager limits, not changes to global gzip, VFS or loader limits.

## 6. Trust, freshness and publication

HTTPS protects transport. Package authorization comes from independently pinned
Ed25519 signing keys, never from whatever CA happened to authenticate a mirror.
Never use the machine's SSH host identity as a repository signing key.

Proposed v1 trust model:

- Installer seeds a versioned repository root with repository UUID, offline root
  public keys and role thresholds. Recommended root threshold: 2 of 3 keys.
- Root metadata authorizes a separate catalog-signing role. Routine catalogs do
  not require bringing offline root keys online.
- One signed catalog binds its repository UUID, root version, uint64 catalog
  revision, validity interval and the complete bounded package list with exact
  artifact hashes/lengths. A single catalog avoids mixing independently fetched
  package records from different repository snapshots.
- Root key rotation is sequential and requires thresholds under **both** old
  and new root policies. A key counts once; unknown/duplicate signatures cannot
  satisfy a threshold. Skipped versions, unsupported algorithms and malformed
  root updates fail closed. Expiry/recovery policy is frozen in PKG-0.
- Persist accepted root/catalog revision floors before installing anything they
  authorize. Removal, cache deletion and interrupted install do not lower floors.
  Retain the authenticated current metadata alongside those floors.
- Package build downgrades are refused even if offered by a newer catalog.
  Recovery of a pending transaction is distinct from authorizing a downgrade.

Do not claim TUF conformance: this is a small Fortress-specific signed catalog.
Use TUF's documented rollback/freeze/mix-and-match threats as a design-review
checklist; adopt full TUF only with an explicit implementation/footprint decision.

TLS certificate checks and signed metadata expiry share the clock threat boundary
in the TLS plan. Reject unavailable/implausible time and expired metadata; report
the retained metadata version when refresh fails. Unauthenticated SNTP plus
version floors cannot prove that a mirror is serving the newest release, prevent
indefinite withholding, or survive malicious rollback of the whole disk image.
No secure monotonic hardware counter is claimed. Do not offer `--insecure`.

Repository URLs live in root-owned `/etc/pkg/repositories.conf`; v1 supports one
repository/channel to avoid cross-repository priority ambiguity. HTTPS only for
network refresh/install. Redirects must remain HTTPS and within the configured
origin allowlist. No credential-bearing URL or embedded signing secret in logs.
Offline bundles contain root updates, signed catalog and matching artifacts;
they undergo the same trust, revision, expiry and platform checks.

Host-side tools build deterministic manifests/archives, compute all lengths and
hashes, sign catalogs and publish content-addressed artifacts **before** publishing
the catalog that references them. Signing keys stay outside repository/build
outputs and release images. No credential, shadow, host-key or disk snapshot may
enter a package. Publish repository root/key fingerprints for independent checks.

## 7. Installed state and recovery

Proposed root-owned layout:

```text
/etc/pkg/repositories.conf
/etc/pkg/root.v1
/var/lib/pkg/state.v1                 committed inventory + trust floors
/var/lib/pkg/transactions/<id>/       private staging and immutable intent
/var/lib/pkg/pending.v1               one durable pending operation
/var/cache/pkg/sha256/<digest>.fpk    verified artifact cache
/usr/bin/<command>                   manager-owned application executable
/usr/share/pkg/<name>/<build>/       immutable documentation/manifest
```

Transaction directories/state are 0700/0600. Published commands are root:root
0755; documentation is 0644. Cache/state have byte quotas. Running out of space
must preserve pending recovery material rather than trigger blind eviction.
No file may be claimed by two packages, and base/unmanaged files are not silently
adopted. The installer must seed the immutable base ownership/platform descriptor
so a fresh system is not misidentified as wholly unmanaged.

Acquire one manager lock using exclusive creation and an identity/token protocol.
A PID alone is not proof of ownership after reuse or reboot. Never delete a lock
just because a timeout passed. Reader commands may inspect the last committed
snapshot and report pending work; writer recovery is exclusive. Freeze the
stale-owner/boot-generation protocol and test it before PKG-1 mutates storage.

For each v1 package:

1. Validate installed-root identity, permissions, writable non-tainted mount,
   platform compatibility, ownership, bounds and free-space reservations.
2. Fetch into a private exclusive no-follow temporary; verify transport completion,
   signed catalog binding, full container, decompressed payload and ELF policy.
3. Stage the new executable **on the target filesystem**, documentation, intended
   final state and any old bytes needed for recovery. Hash/read back, then sync.
4. Publish a checksummed immutable intent into an absent pending name; sync.
   It binds transaction ID, operation, package/builds, old/new state hashes,
   fixed destination role, staged lengths/hashes and trust revision. Recovery
   never treats arbitrary intent paths as privileged instructions.
5. Use the reviewed atomic replacement operation for an upgrade, or absent-name
   publication for first install. Create a separate publication copy under the
   private transaction directory, assign its final metadata, hash and sync it
   before replacement. Keep the authoritative immutable staged bytes: publication
   must not consume the only recovery copy. Both names are on the installed
   filesystem. Never mutate the live inode's data in place. Read back the
   published metadata/digest and sync.
6. Atomically publish the new committed inventory, sync, retire pending and sync.
   Reclaim old staging only after durable completion.

After durable intent, recovery is **roll forward** from verified immutable staging.
If publication is uncertain, return failure/pending, not success or guessed rollback.
On repeated invocation, distinguish already-published old/new content by the
expected digest/metadata. Anything else, corrupt intent, missing stage or I/O/flush
failure blocks new mutations and provides a concrete recovery diagnostic.

Removal follows the same durable-intent protocol and deletes only unchanged,
package-owned files. Modified managed executables block mutation by default.
There is no broad `--force` or `--purge /etc` escape hatch in v1.

Application milestone recovery can be invoked through the unchanged base SSH and
`pkg` tools. A crash may leave the complete old or new optional command visible
while inventory is pending; it must never leave a partially overwritten command.
This is crash-recoverable single-command publication, **not** atomic file+database
visibility to arbitrary concurrent readers. Pending state must be explicit.

## 8. Base userland and services: second milestone

Base updates require a separate transaction/recovery design review before coding:

- Treat mutually dependent base binaries as one signed compatibility bundle;
  retain bootstrapped rescue tools independent of the updated base.
- Recover pending base transactions against detached installed root **before**
  publishing ordinary processes. This is a protected boot-order change; reuse
  architectural lessons from account redo without extending its fixed roles.
- Prohibit new executions/service restarts against a partially published bundle,
  or adopt a reviewed generation-based namespace activation mechanism. Until one
  is implemented and crash-tested, multi-file base publication stays disabled.
- Preserve `/etc`, homes, passwords, SSH host keys, authorized keys, crontab,
  local service definitions and administrator choices. Ship new defaults as
  separate candidate files; never silently overwrite an edited configuration.
- Existing service binary updates can restart using unchanged loaded definitions,
  but new/changed definitions need a separately accepted Watchtower reload path
  or a clearly reported reboot. Do not invent `service reload` support.
- Never automatically restart the SSH daemon/session carrying the transaction.
  Stage SSH updates and report deferred activation; apply via a separate explicit
  action or reboot with recovery available. A dropped connection is not success.
- Package-manager self-update, set-ID programs and trust-anchor/root-key updates
  have dedicated policy/gates. Generic application packages cannot gain these
  powers merely because they are signed by the application catalog role.

Early `pkg upgrade` output must distinguish applications updated now, base updates
unsupported/deferred, service activation pending and reboot required. Root/base
update success requires committed inventory and coherent next boot, not just a
successful download or service start response.

General dependencies come after a compatible-set activation mechanism: exact
package/build requirements, bounded cycle detection and topological planning,
no SAT solver, implicit alternatives or arbitrary version-expression language.
Refuse removal while installed reverse dependencies remain. Reserve a signed
base-platform epoch independent of display version and build provenance.

## 9. Implementation sequence and acceptance gates

| Phase | Work | Exit gate |
| --- | --- | --- |
| PKG-0 | Freeze wire formats, trust/rotation policy, running-platform identity, ownership rules, writer identity, replacement and barrier contracts; host package/catalog producer | Design review resolves prerequisites; malformed/golden vectors independent of client; no live updates |
| PKG-1 | Strict parsers, signature verification, pure revision-state logic, read-only CLI and signed offline bundle verification | Actual-code ASan/UBSan, independent crypto oracle; no persistent trust-state or package mutation yet |
| PKG-2 | Additive atomic regular-file replacement, optional single-command install/remove/upgrade, durable intent and recovery, installer state/PATH integration | Mounted EXT4 interruption/fault campaign and BIOS/UEFI installed-root reboot audit; base files unchanged |
| PKG-3 | Verified HTTPS catalog/artifact fetch, search/refresh/install/upgrade UX, bounded cache and deadline behavior | TLS-3/4/5 accepted; hostile repository/transport tests and real guest download-to-install oracle |
| PKG-4 | Base compatibility bundles, boot recovery/activation review, configuration preservation and service/SSH integration | Separate protected-contract approval, coherent old/new boot and zero mixed-base execution acceptance |
| PKG-5 | Dell installed-system acceptance and operational release process | User installs through SSH, upgrades/removes a tool, checks persistence; later base update/reboot preserves identity and recovery |

Proposed test families: `test-pkg-host`, `test-pkg-storage-host`, `test-pkg`,
`test-pkg-repository`, and a separately gated physical checklist. These targets do
not exist yet. PKG-0/1 can proceed while TLS-2 continues; PKG-3 cannot claim HTTPS
readiness based on TLS-2 trust-store compilation alone.

Required negatives include: bad/unknown/duplicate signatures, wrong repository,
old root/catalog/build, expired metadata, same revision with different content,
bad lengths/hashes, unsupported ABI, forbidden names/modes, duplicate archive
entries, zip bombs, truncation, malicious redirects, missing authenticated TLS
closure, disk full, read/write/flush failure, OOM, tainted root, concurrent writers,
stale lock identity, Ctrl+C/SIGHUP, concurrent spawn/open/rename/unlink, installer
base collision, locally edited files and interrupted remove/self-update attempts.

Inject stops before/after every persistence barrier and publication step; replay
twice to prove idempotence. Audit exact file bytes/modes/ownership, database,
orphan/resource reclamation, unrelated files and offline Linux fsck on copies.
BIOS/UEFI × SMP=1/4 acceptance must boot from disposable installed-root targets;
live media reject package mutations. Existing physical-write gates remain intact.
No forced physical power cuts without a separately agreed disposable procedure.

## 10. References and next action

- [TLS plan](TLS_PLAN.md), including clock assurance and remaining HTTPS gates.
- [Persistent executable ownership](PERSISTENT_ROOT_PHASE1.md).
- [Account redo lessons and protected paths](PERSISTENT_ROOT_PHASE3.md).
- [EXT4 crash/durability boundaries](EXT4_PHASE9.md).
- [Watchtower runtime capabilities](../subsystems/watchtower.md).
- [TUF specification](https://theupdateframework.github.io/specification/v1.0.36/)
  for the update threat model; this design does not claim protocol conformance.

Recommended next implementation task: **PKG-0 format/transaction review**, then
the host producer and verifier. Do not begin by copying downloaded executables
over `/bin` on the laptop.
