# Persistent root Phase 4 implementation progress

2026-10-12. **COMPLETE — DISPOSABLE TARGETS ONLY.**
Parent: [installer plan](PERSISTENT_ROOT_INSTALLER_PLAN.md). Existing
[Phase 0](PERSISTENT_ROOT_PHASE0.md) and [Phase 3](PERSISTENT_ROOT_PHASE3.md)
contracts continue to apply. Admission, template editing and detached provisioning
components are now connected through the bounded text installer and
`INSTALL_DEPLOY` runtime adapter. Separate installer ISO/raw media are built;
the BIOS/UEFI installation, boot, recovery, exclusion and timeout matrix passes
70/70 cases. The separate interface-only preview completed on the Dell, with
installation disabled throughout. Physical installation and release enablement
remain Phase 5; see [identified spare-drive preparation](PERSISTENT_ROOT_PHASE5.md).

## Native deployment workbench

`install_payload.c` is freestanding transport-independent code, also compiled by
the strict kernel build. `install_runtime.c` is its trusted runtime caller;
device discovery and write privilege remain in the separately gated backend.
Its source/target callbacks are trusted adapters, not user ABI.
`test-install-payload-host` exercises it against disposable regular files.

The provisional local manifest is exactly 256 bytes: `FOSPAY01`, little-endian
version 1 and size 256; four little-endian lengths; four SHA-256 values; 48 zero
reserved bytes; SHA-256 over the preceding 224 bytes. Fixed source roles are ESP,
root, BIOS capsule and MBR code. Their sizes are respectively 64 MiB, 256 MiB,
1 MiB and 440 bytes. There are no manifest paths, variable partition roles,
download/authenticity claims, or native mkfs. Real template construction and
semantics validation are provided by the separate host template producer below;
runtime payload integration validates typed boot modules and their provenance.

All role bytes are hashed before target admission. The dry-run invokes no target
callbacks, even read/admit/flush. Target plans require 512-byte logical sectors,
at least 324 MiB, subtraction-checked ranges and four distinct nonzero v4 GPT
GUIDs. RNG acquisition and attached-identity collision checks remain the adapter's
responsibility; the workbench does not generate identities.

The caller supplies a disjoint 9216-byte heap/static workspace; source validation
and deployment do not put copy buffers on the 16 KiB kernel stack. Copies are
4096 bytes, with exact return codes and immediate full readback. A positive
partial completion is an error. There is no retry, sector fallback or rollback.
Copied roles are rehashed to prevent changed source bytes from reaching readiness.

Admission must bind an explicit previously confirmed generation/digest and exclude
source/root/mounted devices. Before layout, the engine invalidates MBR, both GPT
headers, the fixed ESP boot sector and readiness slot, and the fixed root
superblock sector, then flushes. It constructs matching 128-entry arrays and
CRC-checked GPT headers at the actual target ends, with the fixed Phase 0
partitions. It copies partition capsules and combines the 440-byte BIOS MBR
capsule with a newly constructed protective partition entry and signature.
The host template producer compares fixed-position capsules from the pinned
Limine utility at 384 and 512 MiB. Focused BIOS native installation and subsequent
source-free target boots pass under both BIOS and UEFI.

Readiness must be zero in the personalized input ESP. After payload barrier, the
trusted provision callback must hand ownership to a detached filesystem adapter,
provision and validate accounts/homes/manifest, and freeze/sync the root before
returning exclusive raw ownership. No installed user code may execute. This
callback is a mock in the synthetic deployment test. The runtime gate instead
uses the actual detached EXT4 mount and account redo engines.
Immutable ESP/BIOS/GPT/MBR are audited again after the callback, followed by a
barrier. Readiness is written last, read back, flushed and read back again. Only
then is the workbench result `complete`; product installation completion cannot
use this result until all real adapters and boot-artifact validations exist.

## Native bootstrap archive workbench

`install_archive.c` personalizes a caller-owned buffer from a hash-only validated
Phase 2 recovery record. It validates the complete source before copying:
USTAR magic/version/checksum, bounded octal lengths/metadata, root ownership,
directory/regular types, safe relative component names, duplicates including
trailing-slash aliases, regular-file parent conflicts, zero data padding and a
complete zero trailer. Input has at most 510 entries and output at most 512;
source/output are bounded at 8 MiB. Links, devices, existing `bootstrap` entries,
truncation, hidden trailers and overlapping output are rejected.

The builder appends root-owned `bootstrap` mode 0700 and
`bootstrap/recovery.v1` mode 0600, then two zero blocks. It copies no plaintext
passwords. Valid disjoint output storage is zeroed before use and wiped on failure.
Invalid or overlapping buffers remain untouched. Actual input prompts, masked-secret
handling, new salt generation and mandatory operator/recovery choices remain
implemented by `/bin/install`; this API never invents a default credential.

## FAT naming discrepancy

Phase 0 specifies short-name ESP slots, while Phase 2's implemented basenames
`bootstrap.tar` and `readiness.v1`, plus Limine's `limine.conf`, exceed FAT 8.3
bounds. The editor explicitly validates the corresponding fixed VFAT long-name
entries and their checksum/order/short aliases, preserving existing installed
boot fixtures. No basename is silently changed. `install_fat.c` validates paths and aliases,
their checksum/order/ASCII text, the full allocation graph, duplicate names,
cycles/cross-links, both FAT copies, boot backup and both FSInfo copies.
Shrinking a preallocated slot also requires maintaining FAT chain length and
free-space metadata consistently; zeroing trailing bytes alone is insufficient
for an independent `fsck.fat` acceptance claim.

## Admission and lifecycle components

`install_control.c` implements the reserved 71 ABI (ENUM through DEPLOY) with
exact shapes/reserved-byte checks, user ranges, root ADMIN+RAWIO, BSP ownership,
one process-owned lease, snapshot write buffers and terminal I/O failure.
Production descriptor operations reject duplication, inheritance, spawn mapping,
and clearing close-on-exec. Ordinary close and process exit release the lease.
Normal live builds have no registered backend and return EOPNOTSUPP.

`boot_info.c` snapshots installer source provenance only when all modules match
the kernel's media type and (for generic media) a nonzero GPT disk GUID. Unknown
origins, TFTP, missing modules and mixed parents reject. Explicit installer mode
has no root selector and skips legacy raw disk diagnostics and USB mounts.
`install_nvme.c` is a trusted adapter registered only by the separate
`INSTALLER_TEST=1` kernel and exact disposable fw_cfg key, requiring an explicit
disposable gate, complete source evidence and no registered/prepared/active
storage. Generic source-parent admission reads a CRC-valid GPT header, or admits
an unformatted target only when its MBR, primary GPT header sector and final
backup-header sector are all zero. Nonzero malformed metadata and read failures
reject; this does not repair a GPT. Fresh
NVMe Identify evidence binds the singleton inventory, BDF, serial/model, namespace
identity and conventional 512-byte geometry; replacement and uncertain I/O latch
failure. The actual driver adds calibrated monotonic installer command deadlines
and bounded CAP.TO transitions while retaining existing clock-fault poll caps.
The complete singleton PCI inventory is pinned at admission; hotplug enrollment
is unsupported. Every transfer still refreshes the selected BDF, controller,
complete active namespace list, geometry and namespace identity. Installer
polling has bounded MMIO/PAUSE relaxation points and monotonic deadlines.
Guest fault acceptance remains distinct from host adapter fault tests.

`ext4_prepare_installer_root` constructs a detached namespace after user namespace
sealing, without changing boot preparation. Its namespace cannot be published.
The trusted caller must suspend raw lease operations throughout this handoff.
`accounts_provision_detached` validates homes and coherent account bytes and uses
the actual account redo engine without enabling account syscalls/authentication
or publishing account-service state. Freeze/discard returns ownership only after
all references are closed. Runtime handoff wiring is implemented; a failed
discard retains its callback owner and latches backend failure for boot lifetime.

## Template producer and FAT editor

`scripts/create_installer_payload.py` creates a new regular-file workspace under
`build/root-phase4`. It builds the restricted 256 MiB journaled EXT4 profile from
the bootstrap programs, with root/operator locked, persistent operator home and
fixed filesystem UUID `813ea876-9933-4e1c-ae62-2f6c0f90a441`. It creates the
64 MiB ESP with kernel, fallback EFI/BIOS loaders and the fixed reserved slots,
normalizes mformat's zero backup FSInfo, and runs the actual native FAT validator
plus Linux audits. It extracts identical 1 MiB BIOS and 440-byte MBR capsules
from the pinned Limine utility at two capacities, then seals the binary manifest.
No usable credential or completed installation is produced.

`install_fat_personalize` edits private owned template bytes only after validation.
It replaces bootstrap/config slots, releases unused chain tails in both FATs,
zeros old slot contents and writes exact free-space hints in both FSInfo copies.
Readiness remains a zero 512-byte file; its relative LBA is returned for the final
deployment barrier. Workspace includes bounded name/allocation records and must
be heap/static storage rather than a kernel stack frame.

## Runtime integration and remaining gates

The text installer displays controller/model/serial/namespace, capacity, sector
size, layout and unused space, requires explicit index selection, validates the
payload without target callbacks, and collects independent masked credentials.
It rejects reuse of the operator password as the recovery passphrase, creates
fresh salts/GUIDs, and requires `YES` after showing the selected disk and username before lease acquisition.
The backend still compares the complete digest and generation; human confirmation
does not weaken drive binding. Deployment ABI v2 adds a bounded chosen username
while retaining the v1 prefix and legacy operator provisioning. Login and recovery
passwords accept 8–128 printable characters; secrets are masked and other input
is echoed. Invalid choices, passwords and confirmation retry their current step.
Payload checks and installation stages report work before long operations.
Escape/Ctrl-C and fatal failures wipe the request, release any lease and
restore terminal attributes. No plaintext password enters the deploy ABI.

Private ESP/archive buffers use `kmalloc_large`, which grows through individually
transactional capped batches. Ordinary `kmalloc` retains its expansion cap.
The runtime personalizes and validates recovery/config/readiness bindings,
provisions accounts/homes/manifest/hostname through the detached mount, freezes
and discards it, then re-audits immutable boot bytes and publishes readiness last.

`create_installer_media.py` stages six typed modules on separate audited ISO/raw
artifacts and provides installer/live-rescue menu entries. Normal media and its
bootstrap are preserved. Booting installer media alone does not enable writes.

Both BIOS and UEFI raw USB source installation into blank regular-file targets,
Linux audits and source-free boots pass. BIOS uses QEMU xHCI; UEFI uses EHCI.

The interface-only `INSTALLER_PREVIEW` build replaces device enumeration with a
synthetic target, contains no INSTALLCTL syscall path, uses a kernel without
TEST_INSTALLER, and omits payload modules. BIOS/UEFI ISO guest tests and the
user's Dell full prompt/confirmation flow pass. See [preview instructions and
limits](../installer-preview.md). The QEMU xHCI/OVMF preboot stall is isolated to
that firmware/controller fixture. The same raw bytes boot through EHCI under
UEFI; this does not claim UEFI QEMU xHCI acceptance.

Accepted scope includes 384/512 MiB targets, ISO and raw USB source transports,
three persistence boots, SMP=1/4/8 installed boot, authenticated rejection/recovery,
distinct installation identities and credential salts, input/selection zero-write
checks, excluded disk/source hashes and bounded real NVMe timeout/quiescence.
Host interruption/failure adapters remain separately labelled; these tests do
not establish physical cache durability, physical disconnect or physical
installation. Normal live media remain byte-identical.

Executed workbench evidence: [Phase 4 progress report](../roadmap/persistent-root-phase4.md).

## Explicit NVMe disk selection extension

The later `INSTALLER_MULTI=1` profile implements the user's request to list and
choose between NVMe drives. Its model/capacity menu omits serial numbers and
selects no default. This is a separate authority profile from the original
disposable and Dell fingerprint-bound installers; it does not reinterpret their
single-controller gates. See the [disk-selection contract and instructions](../installer-multi-nvme.md).

Indexed enumeration and selection preserve complete identity checks, exclusive
leases, source exclusion and failure latching. Installed boot locates a unique
root PARTUUID across the pinned controllers before writable publication. The
confirmation UI requires the selected one-based disk number; blank input, YES
and another number retry without losing answers. Partition summaries use type
hints, with an explicit unknown-contents warning rather than claiming an OS or
used-space measurement. No disk is preselected.
The two-controller workbench must establish unchanged non-target/media hashes,
source exclusion and duplicate-root rejection in BIOS and UEFI. Physical
two-controller acceptance remains separate from disposable-file evidence.

Recovery storage acknowledgement: retain full masked password confirmation, then
accept YES only after the user says it is stored safely. SHOW is an optional
local-framebuffer reveal using the existing per-process TERM_LOCAL route; clear
the display before restoring the prior route. No recovery plaintext is sent to
UART or provisioning logs. Cancellation before deployment wipes temporary
password buffers and performs no disk writes. A missing local screen leaves
the password hidden. This acknowledgement does not prove external storage.

Navigation extension: Escape returns to the previous numbered screen before
lease acquisition (step 1 remains there). Ctrl-C and non-secret CANCEL provide
separate cancellation. Retain selected disk, layout, username and completed
hashes; no plaintext retention. Enter keeps saved values and CHANGE requests
password replacement. Replacing login invalidates recovery to preserve the
independent-secret check. Partial secrets wipe on back/cancel. Final target
number confirmation and fresh identity lease checks remain mandatory; back
navigation is unavailable after deployment starts.

Password feedback extension: Tab toggles visibility while entering and
confirming either password, hidden by default on each read. Switch to TERM_LOCAL
before rendering plaintext; redraw bounded input locally so hiding/backspace
removes wrapped text. Clear on Enter/back/cancel before restoring output mode.
Without a local framebuffer, reveal is refused and input stays masked. Preserve
existing plaintext wiping, password bounds and independent recovery checks.

Screen consistency extension: centralize screen clearing and the branded
step-number/title header. Keep the current numbered step and account/recovery
context on password visibility redraws and hashing waits. File checks stay at
step 2; confirmation, deployment and results stay at step 5. Progress updates
remain beneath the retained header, with navigation absent during deployment.
All secret clearing continues under local-only routing before restoring output.

Error presentation extension: explain inventory, payload validation, lease
acquisition and deployment failures in plain language. Use syscall error values
for user guidance; backend status.error uses VFS errno and must not be treated
as the same namespace. Keep original signed stage/error/call details. Before
acquisition/deployment, state that no installation data was written; after
attempted deployment conservatively warn of possible partial erasure. Wait for
Enter before returning. Never retry writes automatically or report incomplete
status as success; generic I/O errors do not identify a physical cause.


Installer logging extension (2026-10-13): save a public diagnostic record at
`/var/log/fortress-install.log` on the selected installed root and at
`/fortress-install.log` on a separate FAT16 `FORTRESSLOG` USB volume. Never log
plaintext passwords, password hashes, recovery material or captured console
input. Record only generated stage/result/error events and validated public
model/capacity/username fields. Each file is bounded to 4096 bytes; preserve
recent events across the supervisor's installer retries within one boot.

The compiled MULTI profile and explicit `installer_log_sha256=` installer entry
permit private xHCI BOT enrollment solely for this log slot. Normal installer,
preview, inspection, live and installed mount policies remain separate. No USB
block/devfs/GPT registry enrollment or general FAT writer is added. Require
trusted non-optical boot-source GUID equality, exact primary/backup GPT at the
known image boundary, matching arrays and exact three-partition ranges, and a
compiled fixed FAT16 metadata SHA-256 matching the configured selection. This
admits a larger as-flashed stick without rewriting/relocating GPT. The complete
BPB, both FATs and directory/LFN allocation must match before any write. Only
its eight preallocated file-data sectors may change; never write GPT, FAT,
directory, BIOS, ESP or transport payload bytes. Respect existing BOT durability
classification and DMA quarantine. Latch log write/flush/readback failure until
reboot; never retry or fall back to another disk.

`INSTALL_LOG` accepts a fixed 32-byte event, validates user ranges, root/admin/
raw-I/O authority and BSP ownership, and shares the existing serialized control
gate. It grants no target sector access and can report failures even when target
inventory is unavailable. Safe optional log failures warn without blocking the
installation. A failed root filesystem write that taints storage, failed freeze/
discard, or non-allocation errors during the final detached-root handoff remain
installation failures; optional logging must not falsely certify unhealthy
storage. The root log is refreshed after the actual readiness verification,
then read back and frozen/discarded before the CLI reports completion.

New source-write gates compare every byte outside the fixed 4096-byte USB file,
plus independent FAT extraction/fsck and installed EXT4 log/fsck/boot audits.
All automated fixtures remain disposable regular files. No physical-write or
physical USB-log acceptance is inferred from these gates.

## 2026-10-13 - Computer name and whole-disk root

Deployment ABI v3 preserves v1/v2 and adds the bounded computer name. The
kernel validates both username and hostname before deployment; provisioning
writes `/etc/hostname` with root-owned 0644 metadata and exact readback. The
installer retries invalid input and preserves the name across backtracking.
The effective UID, account file and validated hostname determine the shell
prompt; mutable USER variables cannot impersonate another account in it.

V3 extends the root GPT entry to the disk's last usable sector. Fixed seed
payload lengths remain 64 MiB/256 MiB/1 MiB/440 bytes. The new audited root seed
uses 4 KiB blocks, 32768 blocks and 128 inodes per group, with metadata beyond
63 initially free descriptor-growth blocks. Host production temporarily uses
resize_inode to place that metadata, then removes the feature and repairs/
checks the seed before hashing it; production does not support resize_inode.

An exclusive installer-only expansion callback runs before detached provisioning
and before readiness publication. It validates the seed geometry, checksums and
growth padding, initializes group bitmaps/inode tables and all sparse backup
superblocks/descriptors, seals checksums and flushes/readbacks without retry.
Failure remains an incomplete installation with invalid readiness. This grants
no mounted resize or general formatter API. V1/v2 fixed-root fixtures remain
compatible. Geometry is bounded to 1 TiB, 8192 groups and 1048576 inodes; orphan
ownership auditing retains complete validation with bounded bulk heap buffers,
and fails on allocation exhaustion. Per-file/transaction/directory limits remain.
Installed boot accepts bounded variable capacity while retaining PARTUUID,
coherent GPT, readiness, filesystem UUID, account and executable checks.

### Installed /var privacy policy (2026-10-13)

At the user's explicit request, new installer templates set `/var` and all
included directories beneath it to root:root 0700, and included files to
root:root 0600. The installed log is created and refreshed as 0600, verified
with root ownership during both writes. Parent traversal/search prevents
ordinary users from accessing anything beneath `/var`; public paths outside
it retain their existing permissions. This is an installed-system policy;
FAT USB logs have no Unix ownership enforcement.

Boot admission accepts root-owned 0700 `/var` as well as legacy root-owned
0755, retaining boot compatibility for previously installed roots. It does
not silently rewrite an existing installation. Applying the policy there
requires an explicit chmod of `/var`, `/var/log` and the installer log.
