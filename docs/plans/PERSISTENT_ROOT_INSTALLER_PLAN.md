# Persistent Root and Installer Plan

Date: 2026-10-11. Status: **PHASE 2 COMPLETE — installed boot, recovery and shutdown**.
Detailed decisions: [Phase 0 contracts](PERSISTENT_ROOT_PHASE0.md).
Executed gates and limitations: [Phase 0 evidence](../roadmap/persistent-root-phase0.md).
Phase 1: [contracts](PERSISTENT_ROOT_PHASE1.md) and [executed evidence](../roadmap/persistent-root-phase1.md).
Phase 2: [contracts](PERSISTENT_ROOT_PHASE2.md) and [executed evidence](../roadmap/persistent-root-phase2.md).

This is the next milestone after bounded journaled EXT4, permissions, and the
SMP/memory stability work. It expands the [EXT4 Phase 10 handoff](EXT4_PLAN.md)
into sequenced deliverables. Writing this plan does not authorize real-disk
erasure or change the protected boot, mount, or internal-NVMe contracts.

## 1. Outcome and scope

A user can boot live media, install FortressOS onto one explicitly selected
disposable disk, remove the live media, and boot an independent system whose
programs, accounts, configuration, and home files survive reboot. Failed root
admission leads to a clearly identified recovery environment. An interrupted
installation must never be reported as complete.

Deliver persistent root before the installer. First release: fresh whole-disk
installation to supported internal NVMe storage only, with BIOS and UEFI acceptance.
Live USB provides installer and recovery media; USB installation is out of scope.
Preserve a live boot build and a separate installer build, each in ISO and raw
image formats. Normal make continues producing the live artifacts; a separate
installer-image target will produce installer media without changing live outputs.
Physical acceptance uses a separately obtained spare NVMe, not the existing OS
disk. No dual boot,
partition shrinking, automatic migration of existing /mnt data, online root
switch, general package manager, or unattended system upgrade in this milestone.

## 2. Existing foundation and concrete gaps

- Normal image generation already produces journaled EXT4 user storage at
  /mnt; this is not an installed root. See [storage status](../subsystems/storage.md).
- VFS builds a synthetic root with TarFS-backed system files and volatile
  devfs/runfs. Root publication and mount routing need explicit design.
- Initial login in src/kernel/main.c requires node->data and passes those
  initramfs bytes to process_spawn. Persistent EXT4 executables need the owned
  executable snapshot path, including permissions and credential decisions.
- vfs.h exposes owned node references and exec_snapshot. Audit initial login,
  subsequent spawn, failure cleanup, and reaper paths together.
- EXT4 has a restricted feature profile and bounded node/transaction budgets.
  Existing rename replacement, directory rename, symlink, hard-link, and mount
  limitations must be audited against the exact payload before design relies
  on them. This plan does not assume POSIX rename replacement exists.
- Account databases are currently built into the image. Persistent /etc enables
  account tools, but durable multi-file database updates need their own protocol.
- scripts/create_boot_img.py uses host formatting and bootloader tools. A native
  installer cannot invoke those Linux tools inside FortressOS.
- Physical internal NVMe is excluded; raw block nodes remain read-only under
  the permissions plan. Neither completion of EXT4 nor root privileges removes
  these policies.

## 3. Proposed architecture

### Boot and root ownership

Keep kernel and a self-contained bootstrap/recovery initramfs on the boot
partition. Select installed root by GPT PARTUUID, never enumeration order or
device name. Proposed boot settings are root=PARTUUID=<guid> and an explicit
live/recovery mode; final syntax is a Phase 0 deliverable.

Discover and validate the selected device, geometry, GPT, filesystem profile,
durability, and installation manifest before admitting root recovery writes.
For a supported dirty journal, admit replay only after device policy checks;
publish root only after recovery and mandatory system validation succeed.
Journal recovery may write even when a later validation fails: preserve that
distinction in diagnostics and tests. Rejected admission before recovery is
zero-write. Degraded GPT does not qualify for normal installed RW boot.

Publish the installed namespace once, before starting user login. Proposed VFS
model: a boot-only root attachment with explicit volatile /dev and /run mounts;
no overlay or union semantics. Keep bootstrap namespace/backing alive for
recovery and existing references; do not reclaim Limine module frames. Specify
which root validation occurs before publication and how later login failure
returns to recovery without inventing an unsafe live root swap.

Recovery uses bootstrap binaries and credentials independent of installed /etc.
Default diagnostic recovery must not silently grant an unrestricted privileged
shell. Phase 0 specifies authentication/physical-console policy; provisioning
must provide a recovery credential or a separately authorized rescue mode.
Never autoformat, repair GPT, or retry uncertain writes in recovery.

### Proposed directory layout

| Path | Backing and purpose | Initial ownership/mode |
| --- | --- | --- |
| / | Journaled EXT4 installed root | root:root 0755 |
| /bin | Static system executables; trusted set-ID policy | root:root 0755 |
| /etc | Accounts and machine configuration | root:root 0755; shadow 0600 |
| /home/<user> | User home, HOME target | user:primary-group 0700 |
| /root | Administrator home | root:root 0700 |
| /var | Persistent machine state and bounded logs | root:root 0755 |
| /mnt | Explicit external data mounts | mount-specific; nosuid/nodev |
| /dev | Volatile device namespace | existing devfs policy |
| /run | Volatile runtime state | existing runfs policy |
| /tmp | Volatile scratch, if supported | root:root 01777 |

User homes use /home/<user>. Do not add symlink aliases implicitly.
If volatile /tmp or sticky-directory enforcement is
missing, implement and test it or document a narrower initial scratch policy.
Installed root must permit the reviewed sudo set-ID behavior; external data
mounts retain nosuid. Live-media passwordless defaults must not become installed
credentials. No copy-up overlay or fallback to bootstrap /etc during normal login.

### First installer: bounded image deployment

Recommend a host-built installation payload containing preformatted FAT32 boot
and restricted journaled EXT4 root partitions, with a versioned, hashed manifest.
The native installer streams these partition images and creates target-specific
GPT/boot metadata. This avoids making a complete native mkfs implementation a
prerequisite. Manifest hashes detect corruption; they do not authenticate an
untrusted downloaded payload. First release uses trusted local live-media payloads.

Phase 0 chooses and validates a fixed supported root capacity and boot partition
capacity, based on payload size and filesystem limits. Larger disks may retain
unused space, explicitly shown in the preview. No automatic EXT4 growth. Reject
undersized disks before writes. Initial installer supports 512-byte logical
sectors; 4096-byte logical geometry requires a separately generated and
verified payload/layout before admission. Root parser tests still exercise both
supported block-adapter geometries.

Create primary and backup GPT at the actual target ends rather than raw-copying
a small disk image onto a larger device. Give each installation fresh disk and
partition GUIDs; Phase 0 chooses a reliable GUID-generation source and collision
policy. Do not claim cryptographic randomness without an entropy source.
Test filesystem UUID duplication policy separately. Patch boot root selector to
the new PARTUUID. Audit Limine BIOS deployment requirements, protective MBR,
stage placement, FAT32 file/config modification, and UEFI removable-path loading.
A host-only Limine install utility is not an available native installer API.

Reject non-NVMe installation targets, the existing OS disk, the live-source disk,
active root, mounted targets, ambiguous identities,
unsupported durability, and disconnected/replaced devices. Protect the entire
parent disk, not merely a source partition. The narrow installer write interface
must hold exclusive ownership, bind a target generation/identity, bound every
range, invalidate on exit/disconnect, and prevent concurrent filesystem access.
Root/CAP checks are necessary but insufficient: preserve explicit installer mode
and existing raw-write gates. Choose exact ABI in Phase 0, not unrestricted
writable /dev nodes by default.

## 4. Sequenced implementation

### Phase 0 — Freeze contracts and fixtures

Deliver root-selection/namespace design, boot dependency diagram, payload format,
disk capacities and sector support, recovery authentication, GUID strategy,
installer write ABI, and installation completion protocol. Enumerate bounds for
paths, manifests, files, nodes, journal credits, copy buffers and device waits.
Trace every affected initial-login/spawn/sync/shutdown caller. Read PROTECTED.md,
AGENTS.md sections 4/7/9 and matching headers before implementation.

Resolve boot/root-publication contract changes explicitly before kernel edits.
Define the narrow replacement for the internal-NVMe exclusion: only an explicitly
selected NVMe target may be installed, and only the configured installed root may
be mounted. Other NVMe disks remain excluded from writes and automatic mounting.
Record installer raw-write and mount-policy changes before implementation.
Support multiple NVMe controllers/namespaces, or fail closed if identities cannot
be distinguished; never rely on nvme0n1 being the intended disk. Create
Linux-audited disposable installed-root fixtures
with exact accepted EXT4 features and metadata. No production default change.

Gate: reviewed contracts, complete dependency inventory, reproducible fixtures,
and no unresolved dependency on unsupported filesystem or host-only APIs.

#### 0A — UEFI boot mechanism

Proposed first-release mechanism: deploy the x86_64 fallback loader at
EFI/BOOT/BOOTX64.EFI on the target ESP. Do not require Boot####/BootOrder creation
or SetVariable/runtime-services support if the target firmware can independently
boot that path on internal NVMe. Phase 0 must prove this using disposable QEMU
NVMe and the intended machine's firmware procedure. USB fallback boot is not
evidence of internal-NVMe fallback support. If a persistent firmware boot entry
is required, implement the supported entry-creation mechanism before release;
SetVariable becomes a dependency if that mechanism uses runtime services.
Do not leave boot-entry creation as an undocumented host step. BIOS deployment
retains its independent Limine stage-placement gate.

Gate: remove installer media and use fresh OVMF variable storage to boot the
target through the firmware-supported fallback/menu path. Record the actual
physical boot procedure. No hidden dependency on an entry created by the host.

#### 0B — Recovery authentication

Proposed first-release decision: require a separate recovery passphrase during
installation, chosen by the user rather than generated by the installer.
Before any destructive target write, prompt "Choose a recovery passphrase" and
"Confirm recovery passphrase" with echo disabled. Require matching, nonempty
input within documented bounds; Phase 0 defines minimum length, maximum encoded
length and supported input bytes. Reject invalid input and allow cancellation
without target writes. There is no skip option: installation cannot proceed
without a confirmed recovery passphrase. Explain that the user must remember or
securely store it, and that losing it prevents this authenticated recovery path.
External live-media rescue remains available under its separate authorization
policy; loss of the passphrase does not imply that all data recovery is impossible.
The installer hashes the confirmed passphrase with a fresh per-install salt,
clears plaintext working buffers after use, and provisions only the salted hash
in the target-specific recovery initramfs, stored on that target's FAT32 ESP.
Limine loads this personalized archive as the bootstrap module, so its credential
is available in memory before root selection, admission or recovery writes.
Verification never reads installed root or its account databases.
The shared live-media initramfs contains no installed recovery credential, and
the build must not embed a common recovery password/hash. Each installation
requires explicit secret input; there is no default or copied media credential.
No implicit physical-presence fallback is enabled in the first release. Adding
one requires a separately reviewed, explicitly selected mode with its weaker
security explained before installation; refusal or skipped input must never
silently select that mode or produce an installation without recovery.
Physical console presence alone does not authorize a privileged shell. Failed
normal boot offers bounded diagnostics and an authenticated recovery entry.
No network recovery login and no production passwordless rescue boot flag.
Live-media rescue remains a separate explicitly selected environment whose disk
writes still require target authorization.

Phase 0 must specify how the installer produces and validates the personalized
initramfs on the target ESP and makes the target boot configuration select it,
including bounded archive construction, hash salt generation, secret
input and retry throttling. If this cannot be implemented safely, revisit the
design before Phase 2; do not fall back silently to installed shadow or an open
root shell. Password changes in installed /etc do not change the recovery secret;
recovery-secret rotation is deferred until boot-artifact updates are supported.
This protects the normal recovery entry, not against someone able to replace
unsigned boot media; secure boot and disk encryption are separate work.

Gate: missing/corrupt root or shadow still permits recovery authentication;
wrong credentials deny access, correct credentials permit it, and plaintext
secrets never enter payloads, logs or temporary files. Installer validates the
personalized archive and the independent recovery login before completion.
Two independently provisioned installations must have distinct salts/hashes;
different passphrases must not authenticate across installations. Verify recovery
with root absent and confirm the loaded credential came from the target ESP,
not source media. A missing or malformed recovery record fails closed.
The ESP/archive may expose the hash to offline reading; do not claim password
secrecy from FAT permissions or resistance to boot-artifact replacement.
Exercise empty input, mismatch, length bounds, cancellation and attempted skip:
none may admit destructive target writes. Verify no-echo input and plaintext
buffer cleanup on success, retry, cancellation and provisioning failure.

#### 0C — Durable account updates

Proposed first-release decision: a serialized redo protocol with immutable staged
copies of the complete new passwd/group/shadow set and a durable intent record.
Keep standard canonical /etc filenames. Do not require rename replacement or
claim all three names change atomically for arbitrary filesystem readers.

1. Acquire an exclusive account-update lease. All login/sudo/account consumers
   acquire a coherent reader lease; drain readers and block new authentication
   while publishing an update. Existing sessions retain their credentials.
   Leases must not hold a spinlock across filesystem I/O or scheduling.
2. Validate the complete proposed database set and stage immutable copies under
   a root-only transaction directory. Shadow and every shadow-derived temporary
   file remain 0600. Sync the admitted root mount and check the barrier result.
3. Publish a bounded versioned intent record by writing a temporary record,
   syncing, renaming to an absent pending name, and syncing again. Record exact
   staged lengths/hashes and generation; never trust arbitrary paths from it.
   Before intent is durable, canonical account files remain unchanged.
4. Once intent is durable, roll forward. For each canonical file, construct a
   complete temporary copy from immutable staging, sync it, remove the old
   canonical name and rename the temporary file into the absent name, then sync.
   Authentication remains blocked through the deliberate namespace gaps.
5. Validate and sync the complete canonical set. Remove the pending record and
   sync before releasing readers or reporting success. Only then reclaim staged
   copies. Before that removal is durable, retain everything required for redo.

On restart, resolve pending intent before any installed authentication or general
user process starts. Repeat publication idempotently from validated staged copies.
No intent means staging is abandoned and canonical files were not intentionally
changed, or the completed canonical set is already durable. Invalid intent,
missing staging, exhausted space or uncertain I/O blocks authentication and
enters authenticated recovery; never accept a mixed set or promise rollback
after an uncertain write. Protect transaction paths against ordinary users.

This is a proposed protocol requiring a precise state/cut-point proof in Phase 0
and actual-code failure tests in Phase 3. Establish that existing root-wide sync
orders staged bytes, directory mutations and barriers as required; otherwise add
the missing durability API before implementation. Audit every database reader,
including concurrent sudo and account tools. Generic privileged file readers may
observe a publication gap; they are outside the coherent account-reader API.

Gate: every write/rename/unlink/barrier interruption produces the coherent old
set before durable intent, the coherent new set after successful redo, or blocked
authentication with authenticated recovery. Failed updates never return success.

### Phase 1 — Persistent executable and namespace plumbing

**COMPLETE**: [contracts](PERSISTENT_ROOT_PHASE1.md),
[host and BIOS/UEFI SMP acceptance](../roadmap/persistent-root-phase1.md).

Implement bounded owned executable loading for initial login through the same
reviewed snapshot/credential semantics as subsequent spawn. Add boot-only root
attachment and volatile mount routing with explicit node/mount ownership. Audit
permission traversal, cwd, open handles, cache eviction and failure rollback.
No user process starts while namespace publication is incomplete.

Gate: actual-code host sanitizers plus BIOS/UEFI disposable-root tests for login,
shell, sudo/nosuid, malformed ELF, short I/O, OOM, pinned nodes and cleanup.
Exercise cache churn beyond 1024 nodes and spawn/exit cycles at SMP=1/4/8;
assert resource reclamation and stable bootstrap backing.

### Phase 2 — Installed boot, recovery and shutdown

**COMPLETE**: [contracts](PERSISTENT_ROOT_PHASE2.md),
[host and BIOS/UEFI SMP acceptance](../roadmap/persistent-root-phase2.md).
Accounts remain immutable until Phase 3; installer media and physical acceptance
remain separate Phases 4 and 5.

Wire explicit PARTUUID selection and root admission into the existing dependency
order, preserving PMM/VMM initialization and BSP network ownership. Mount volatile
namespaces before login. Dispatch global sync and shutdown to all admitted
writable mounts; freeze mutations before reporting clean completion. Preserve
read-only versus tainted error distinctions.

Gate: BIOS/UEFI x SMP=1/4/8 on disposable NVMe installed roots: three boots with
independent file/hash/metadata oracles, persistent executable execution, missing
root, duplicate PARTUUID, unsupported features, degraded/conflicting GPT, failed
flush/replay, malformed manifest and login failure. Supported interruption cases
recover; rejected admission writes nothing; recovery remains usable and clearly
identified. Live boot still works with absent/unusable storage.

### Phase 3 — Durable machine identity and accounts

Add passwd and the minimal account-creation tool needed by provisioning. Preserve
SHA-256-crypt compatibility and shadow secrecy. Define persistent hostname,
network config and shell-history precedence; installed /etc is authoritative,
with explicit compatibility for existing /mnt/.fortress configuration.

Implement serialized, restartable passwd/group/shadow changes. Before coding,
choose a supported staging/recovery-record protocol and startup replay ordering;
add atomic replacement and durability APIs only if that protocol requires them.
Current no-replace rename cannot simply be treated as atomic replacement.
Interrupted updates must resolve to a coherent old or new account set before
login, without transient world-readable shadow or privilege escalation.

Gate: password/account/home/ownership changes survive reboot; interruption at
every durable update boundary preserves coherent authentication. Concurrent
updaters, ENOSPC/OOM, write/flush errors, unprivileged access and sudo regression
coverage pass. Installer credentials are mandatory; secrets stay out of logs.

### Phase 4 — Installer engine on disposable targets

Implement local payload validation, dry-run preview, exclusive target admission,
bounded copy/readback, target-specific GPT/boot setup and account provisioning.
Preview model/serial or available physical path, capacity, sector size, source
identity, proposed layout, unused space and whole-disk erasure. Require explicit
selection and confirmation tied to that identity; never preselect the first disk.
Revalidate target identity immediately before writes.

Installation states: validated -> target admitted -> layout -> payload ->
provisioned -> verified -> complete. Publish completion only after root account
configuration, manifest, boot files and required barriers/readback succeed.
Define crash-safe readiness checks in bootstrap; a root manifest alone cannot
prove boot-partition completion. Failure may leave a partial erased target;
report the failed stage and require an explicit fresh retry, never claim rollback.
No installed-user code executes during privileged provisioning.

Gate: zero-write dry-run/rejection; malformed payload/path/length/hash tests;
source-parent exclusion; target replacement and descriptor inheritance tests;
short/failed writes, failed flush, disconnect, ENOSPC and interrupted stages.
Hash non-target disks before/after. Independent Linux GPT/FAT/EXT4 and permission
audits pass. The final disk boots after removing installer/source media.

### Phase 5 — NVMe release and physical acceptance

Deliver a text installer in live media, clear progress/failure diagnostics,
recovery instructions and installation summary. Automated full flow covers
BIOS/UEFI, different target capacities, two attached disposable disks, independent
source removal, three reboots, and SMP=1/4/8 installed boot. Validate bootloader
publication interruptions as well as root installation interruptions.

Physical gate: separately obtained spare NVMe, identified by model, serial,
controller/namespace and capacity, with explicitly authorized
erasure, install, source removal, independent boot, password/sudo/config/home
persistence and offline Linux hashes/fsck. Physical interruption experiments need
their own authorization and procedure. Qualify durability by actual device mode.
Only then enable the installer in release live images.

Verify NVMe controller write/flush/error behavior and target exclusivity before
physical deployment. Automated tests attach two disposable NVMe disks and prove
the non-target remains byte-identical. Prefer physically removing the existing OS
NVMe for the first spare-drive acceptance run. If it remains attached, identify
and explicitly exclude it; ambiguous target identity rejects installation.

## 5. Verification and evidence rules

Proposed targets (not existing commands): test-root-host, test-root-boot,
test-root-recovery, test-accounts-persistent-host, test-accounts-persistent,
test-installer-host and test-installer. Add targets only with real dependencies
and runners. Run relevant existing EXT4 journal/USB, permissions/login/sudo,
memory-node/VMM, SMP and shutdown regressions when integration changes.

Use actual code with labelled host adapters, ASan/UBSan and deterministic failure
injection. Every guest boot preflights final argv and exact allowed image paths;
never attach host raw devices. Pair disposable OVMF vars, bound waits, reap QEMU,
retain failures and exact commands. Crash models distinguish volatile writes,
barriers and sector tears. Reject counterfeit success banners with independent
byte/metadata/boot oracles. Run offline e2fsck -fn only on unmounted images;
automatic repairs cannot establish correctness of the original output.

Each phase records commit, fixture features, geometry, firmware, SMP count,
commands, before/after hashes, logs, audit results and limits under docs/roadmap/.
Record physical claims separately. Performance measurements include payload size,
transport and device durability; no speed promise substitutes for correctness.

## 6. Completion checklist

- [x] Phase 0 contracts and protected changes reviewed; runtime implementation deferred.
- [x] Persistent root and executable ownership gates pass.
- [x] Root recovery and clean shutdown gates pass (disposable Phase 2 scope).
- [ ] Persistent credentials/configuration update gates pass.
- [ ] Installer rejection, interruption and independent boot gates pass.
- [ ] Spare NVMe physical installation accepted; existing OS disk protected.
- [ ] Release documentation and subsystem status match evidence.

Package management and boot/kernel upgrades follow a separate plan: staged
extraction, ownership, executable replacement, application recovery records,
boot-partition durability and rollback. Journaling does not make multi-file
upgrades atomic. First milestone ends at a reproducible, recoverable fresh install.
