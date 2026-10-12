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
