# Persistent root Phase 4 progress evidence

2026-10-12. **COMPLETE — DISPOSABLE TARGETS ONLY.**
BIOS/UEFI installation/boot/recovery/exclusion/fault matrix: **70/70 PASS**.
Earlier progress and failed attempts below remain historical evidence; the
final acceptance checkpoint is at the end. Physical installation and release
enablement remain Phase 5.
[Implemented contracts and limits](../plans/PERSISTENT_ROOT_PHASE4.md).

## Baseline preservation

Before edits, all **700/700 source hashes matched** the accepted Phase 3 manifest
`build/root-phase3/gate-w5hlde4h/manifest.json` at HEAD `abc6625`.
Git status was inspected; all existing uncommitted Phase 3 changes were retained.
At the first deployment/archive checkpoint, only `Makefile` differed among
those original 700 files. Subsequent authorized Phase 4 work adds descriptor
lease rules, installer source/boot selection, opt-in driver deadlines and detached
filesystem/account provisioning to the existing implementation. Phase 3 work
has not been reset or cleaned. The installer backend remains unregistered;
these additions do not enable physical installation.

No physical device paths were read or written. The file adapter creates a new
0600 target with O_EXCL/O_NOFOLLOW, checks regular-file type, and uses only that
file. Native WSL temporary storage avoids repeated DrvFS I/O; the runner preserves
the output image, logs and source/normal-media hashes under `build/root-phase4`.
Accepted sectors can persist immediately; fsync is the adapter's barrier. This
does not emulate volatile NVMe cache, sector tears, IRQ delivery or disconnect.

## Executed checks

`make test-install-payload-host` runs the actual freestanding native deployment
and archive implementations with ASan/UBSan and labelled host adapters.
The complete workbench source version and commands are recorded by its retained
manifest. This is a workbench gate, not the requested end-to-end installer gate.

- Deployment: **284 cases** covering all 256 single-byte manifest corruptions,
  malformed length/reserved fields with recomputed checksum, truncated manifest,
  geometry/capacity/duplicate GUID rejection, zero-target-callback dry-run,
  corrupt source/slot rejection, failed admission, six initial invalidation cuts,
  failures before/after each of four flushes, failed mock provisioning, positive
  short-write rejection without retry, readback mismatch, and successful output.
- Archive: **1,548 cases** covering header/padding corruption, unsafe names,
  duplicate aliases, links, regular-file parent conflict, nonzero hidden trailer,
  malformed recovery record, overlap rejection and correct personalization.
  Python's independent tar reader checks exact record bytes and 0700/0600
  root-owned metadata.
- An independent Python byte oracle checks protective MBR capacity/signature,
  both actual-end GPT header CRCs and equal 128-entry arrays, all three partition
  ranges/type GUIDs, unique v4 partition GUIDs, zero unused entries and all copied
  synthetic partition bytes, including readiness publication.
- Strict `make -j4 bin/fortress.elf` passed with both new freestanding libraries.
  `make test-root-boot-host test-account-redo-host` passed: existing root/module/
  recovery/PCI parser gates and 2,586 account redo interruption/corruption cases.
  This does not rerun or supersede the accepted 966-case Phase 3 full gate.
- Compiler stack-usage check after caller-owned buffer refinement reports 848
  bytes for deployment and 256 bytes for payload validation at default
  optimization; callback/whole-call-chain stack use is not yet acceptance.

The first synthetic deployment run `payload-0_kzxsae` passed the C assertions,
then the independent oracle found a test fixture copying 28 of the synthetic
readiness text's 29 bytes. It is retained as a failed manifest/log, not a pass.
That attempt did not preserve its temporary output disk; the runner now preserves
output even when the C test or oracle fails. Corrected `payload-56g9k9gq` passed
the 284-case deployment gate and independent oracle before the later stack-workspace
refinement. The final combined gate is recorded separately below.

Final unchanged-source combined workbench gate:
[payload-tuxq9dru/manifest.json](../../build/root-phase4/payload-tuxq9dru/manifest.json),
**PASS**, 1,548 archive cases + 284 deployment cases and independent Python
USTAR/GPT/MBR/full-partition byte oracles. `archive.log`, `host.log`,
`bootstrap.tar`, and the 384 MiB `target.img` are retained beside the manifest.
The manifest records 13 relevant source hashes and exact compiler/test commands;
it does not describe a full 700-file Phase 4 integration gate.

## Acceptance limits

Partitions in this gate contain synthetic byte patterns, **not** FAT, EXT4,
kernel/bootloader payloads or production readiness records. The provisioning
callback checks ordering and returns injected status; it does not mount EXT4 or
exercise the Phase 3 service. Source identity and descriptor lifetime are not
implemented by the original synthetic adapters. Later component gates below
cover actual syscall admission, descriptor lifetime, source-parent policy and
Linux filesystem audits separately. No native text installer, installer-image
build, actual driver disconnect acceptance or independently booting native
installation output is claimed.
Normal live ISO/raw-image hashes remain unchanged during the workbench gate.
Phase 4 remains open; physical acceptance remains Phase 5.

## Subsequent component gates

The strict kernel build compiles the admission, NVMe adapter, FAT editor and
detached provisioning components. Normal live media outputs are unchanged.

- [leases-rktnkekc/manifest.json](../../build/root-phase4/leases-rktnkekc/manifest.json):
  ASan/UBSan PASS for actual control service plus verbatim production descriptor
  and fcntl functions. Covers privilege/shape/range rejection, source/mounted
  exclusion, stale identity, failed I/O, ownership, dup/fcntl, CLOEXEC, clone and
  close/exit. The actual NVMe admission adapter separately covers source-parent
  GUID rejection, CRC-invalid GPT, unknown inventory, prepared storage, replacement
  and terminal failure. Hardware/PCI/Identify/current-process/VMM remain labelled
  adapters; driver MMIO/deadlines and guest spawn-mapping execution are not claimed.
- [fat-4y7lptng/manifest.json](../../build/root-phase4/fat-4y7lptng/manifest.json):
  native FAT graph/VFAT/slot validation and personalization under ASan/UBSan;
  malformed input leaves template bytes untouched. Independent fsck.fat and
  mtools confirm exact six file bytes after slot shrinking and readiness zeroing.
- [provision-g8l0sfpw/manifest.json](../../build/root-phase4/provision-g8l0sfpw/manifest.json):
  actual EXT4/JBD2/account redo provisioning with labelled regular-file transport
  at 512/4096 sectors, sealed running namespace, rejected publication, invalid
  database zero-write check, no account-service publication, exact files, freeze,
  reference reclamation and independent Linux e2fsck/dump audits. This does not
  establish that a runtime lease handoff is connected or fault-complete.
- [templates-qn6ce62w/manifest.json](../../build/root-phase4/templates-qn6ce62w/manifest.json):
  host restricted EXT4/FAT template production, actual native FAT validation,
  independent Linux filesystem checks, unchanged normal-media audit and identical
  pinned Limine BIOS capsules from 384/512 MiB fixtures. Records immutable input
  hashes and binary role manifest. Operator/root are locked; no completed install.

Retained failed attempts include FAT `fat-vmwjp_dk` (mformat omitted backup
FSInfo; controlled template producer now normalizes it) and provisioning
`provision-lc0eaxrp`/`provision-58q7fv3m` (DrvFS staging inode owners/modes had not
been normalized; producer now sets every inode's explicit metadata). Compilation
failures in lease attempts are retained too. None is counted as acceptance.

Final component refresh after ordinary VFS allocation initialization:
[leases-zxurd37w](../../build/root-phase4/leases-zxurd37w/manifest.json),
[fat-rbj9rcap](../../build/root-phase4/fat-rbj9rcap/manifest.json),
[provision-r87_bu3a](../../build/root-phase4/provision-r87_bu3a/manifest.json), and
[templates-vq8ecznq](../../build/root-phase4/templates-vq8ecznq/manifest.json) all
PASS. Provisioning now also opens actual terminal/ordinary VFS files under
ASan/UBSan and verifies that neither carries the exclusive lease flag. Strict
kernel, pipe/SIGPIPE host, root/recovery/PCI host and set-ID/spawn/sudo host gates
pass. The 1,548/284-case synthetic archive/deployment refresh is
[payload-96ihzplq](../../build/root-phase4/payload-96ihzplq/manifest.json).

Focused Phase 3 regression `gate-h4flgw9c` used the earlier allocation code and
lost echoed child output during second boot. It was deliberately stopped after
finding the missing initialization; only its owned QEMU processes were stopped.
Its artifacts are retained as a failed/superseded run, not acceptance. Ordinary
terminal and file allocation now explicitly clears `exclusive_owner`.

The corrected focused Phase 3 regression is
[gate-ymfiojpo/manifest.json](../../build/root-phase3/gate-ymfiojpo/manifest.json):
**30/30 PASS**, 723 unchanged source hashes. It includes BIOS/UEFI three-boot
password/account/home/configuration/history persistence, rejection controls,
SMP=4 login-publication controls, authenticated recovery and live absent/unusable
storage. This is `--quick --skip-host --jobs 2`, explicitly a focused regression,
not a replacement for the original 966-case complete Phase 3 gate or a native
installer acceptance claim. Normal media hashes still equal the original ISO
`1fd918dff349a2928c2c56936357aaa844ae5bba84e4f92a65a5e1f0c358248c`
and image `cb6b774d35d4ac1cad6728179e7907efa275e2bb1c3039e5b97f26c291294119`.

## Runtime and separate media checkpoint

The installer now runs as a separate Ring 3 bootstrap program, with typed
payload modules, exclusive `INSTALL_DEPLOY`, private FAT/USTAR personalization,
real detached EXT4/account provisioning, immutable boot re-audit and readiness
publication last. Normal live media remain byte-identical. Writes require both
the separate `INSTALLER_TEST=1` kernel and exact disposable fw_cfg key.

The audited media checkpoint is
[media-lx0x7izp](../../build/root-phase4/media-lx0x7izp/manifest.json).
The focused BIOS end-to-end checkpoint is
[guest-94thyvya](../../build/root-phase4/guest-94thyvya/manifest.json), **PASS**:
real text installation to a fresh 384 MiB regular-file NVMe fixture, zero writes
through credentials/confirmation, no password echo, Linux GPT/FAT/EXT4 and
metadata/credential/binding audits, source removal, three persistence boots,
abrupt-stop recovery, and independent authenticated recovery with root absent.
This is a focused BIOS run, not the full Phase 4 matrix.

Host refreshes include
[leases-5wiesgbd](../../build/root-phase4/leases-5wiesgbd/manifest.json),
[cli-tx8zv0v1](../../build/root-phase4/cli-tx8zv0v1/manifest.json),
[payload-d7nr8wkh](../../build/root-phase4/payload-d7nr8wkh/manifest.json),
[fat-c9gafjfx](../../build/root-phase4/fat-c9gafjfx/manifest.json),
[provision-o1s0479d](../../build/root-phase4/provision-o1s0479d/manifest.json),
and [runtime-fhtddpxc](../../build/root-phase4/runtime-fhtddpxc/manifest.json).
Actual heap ASan/UBSan covers multi-batch OOM, retained free batches, successful
bulk allocation/reclamation and unchanged ordinary expansion cap. The CLI gate
covers cancellation, skipped/short/oversized/mismatched input, reused operator
password, wrong identity confirmation, no echo, request wipe, terminal restoration
and lease release. Typed-module role/size/provenance controls pass too.

Retained failed/interrupted attempts include the earlier syscall-dispatch
rejection and stage-zero bulk allocation failure; `guest-lfwgpi9e`,
`guest-t2m6plfq` and `guest-pg2nrelr` were interrupted to diagnose throughput.
`guest-vhi8ln5o` reached provisioning and exposed a nested EXT4 ownership query;
the condition now checks the controlled handoff first, with a host regression.
The singleton PCI scan is pinned at admission; every operation still refreshes
the selected controller/namespace identity. No hotplug enrollment is supported.

`guest-y4ca691u` passed installation, SMP=1/4/8 and initial recovery cases, then
failed an overly strict runner hash assertion: manifest/login tree rejection
occurs after permitted detached journal writes. The runner now audits clean root
state, unchanged canonical accounts and immutable boot bytes for those cases.
`runtime-chnrwev3` failed to link the new bulk allocator adapter; the repaired
runtime refresh above passes. A media attempt also failed its unchanged-source
check after the producer script was edited; it is excluded from acceptance.

A disposable throttled-backend probe
[fault-probe-mb4bx1m4](../../build/root-phase4/fault-probe-mb4bx1m4/manifest.json)
exercised the actual NVMe I/O timeout, quiescence and stage-2 failure report with
no installation completion. Its dynamic probe labels are superseded by the
explicit fault case in the full runner; it is not a complete fault matrix.

Full BIOS/UEFI acceptance, raw-source runtime, additional capacity, exclusion
controls and final unchanged-source evidence remain pending. No physical disk
was opened or written, and no release installer enablement is claimed.

### Interface-only Dell preview (2026-10-11)

User requested a bootable installer interface preview, without installation.
The separate `INSTALLER_PREVIEW` tool uses only a synthetic target and has no
INSTALLCTL syscall path. A normal kernel excludes TEST_INSTALLER/backend
registration, and the preview boot configuration omits every installation
payload module. Final confirmation reports disabled preview completion.
Normal ISO/image hashes remain unchanged, and all existing changes are retained.

Audited producer [media-exsa30o9](../../build/root-phase4/media-exsa30o9/manifest.json)
publishes `bin/fortress-installer-preview.iso` and the separate 72 MiB raw image.
ISO [preview-s8hvqgcg](../../build/root-phase4/preview-s8hvqgcg/manifest.json)
passes BIOS/KVM and UEFI/TCG through target selection, hidden distinct example
passphrases and exact final confirmation. Whole writable NVMe sentinel and
source hashes stay identical; screenshot/argv/serial evidence is retained.
Actual CLI sanitizer regression `cli-_scgxvrd` also passes.

Raw USB BIOS passes, but OVMF USB boot of FAT/hybrid preview images stalled
before kernel entry with both KVM and TCG. Interrupted attempts are retained
in `preview-3z58uyjq`, `preview-mg3vksor`, `preview-lj5ji6ea`,
`preview-o0d89fjb` and `preview-kseibbl0`; they are excluded from UEFI USB
acceptance. Recommended Dell preview route is ISO extraction onto USB;
[instructions](../installer-preview.md). Physical interface acceptance later
passed in user testing, as recorded below. This does not enable physical installation or complete
Phase 4.

Separately, the xHCI installer path now quiesces the controller before DMA
allocation/storage enrollment. The production BOT path is unchanged. Audited
disposable installer [media-v3o1l983](../../build/root-phase4/media-v3o1l983/manifest.json)
and BIOS [raw-probe-tv5wwiz1](../../build/root-phase4/raw-probe-tv5wwiz1/manifest.json)
pass raw USB source installation into an existing valid-GPT regular fixture,
independent Linux audit and source-free installed boot. Blank raw-source target
admission and full BIOS/UEFI installation/recovery gates remain outstanding.

### Dell preview completion (2026-10-11)

User-provided Dell photos confirm the interface-only preview boots, accepts
target selection and password confirmations, rejects matching operator/recovery
secrets, accepts the displayed exact confirmation, and reports “Preview complete.
Installation remains disabled.” The supervisor restarts the preview afterward.
Final user photo: [Dell preview completion](../assets/installer-preview-dell.jpg).
Record this as physical interface preview PASS only. Installation remains
disabled by the preview build; physical installation, disk-integrity audit and
full Phase 4 acceptance are not established by these photos.

Evidence paths under `build/` refer to retained local runs and are excluded from
Git. Source, reproducible runners, contracts and this Dell photo are committed;
generated disk images and full local test logs are not GitHub release artifacts.

## Final disposable acceptance (2026-10-12)

**70/70 PASS**, using audited [media-ktv6f0hm](../../build/root-phase4/media-ktv6f0hm/manifest.json):

- BIOS/KVM [guest-slp8fcki](../../build/root-phase4/guest-slp8fcki/manifest.json): 35/35.
- UEFI/KVM [guest-nw3dh4na](../../build/root-phase4/guest-nw3dh4na/manifest.json): 35/35.

Exact commands: `FORTRESS_INSTALL_ACCEL=kvm python3 scripts/test_installer.py
--media build/root-phase4/media-ktv6f0hm --firmware bios` and the same invocation
with `--firmware uefi`. Each firmware covers seven zero-write input rejection
profiles; ISO installation at 384/512 MiB and raw USB installation onto a blank
384 MiB target; independent Linux GPT/FAT/EXT4, byte, permission and credential
audits; three persistence boots; SMP=4/8 installed boot; eight authenticated
root rejection/recovery cases; absent gate, unsupported geometry, two NVMe
controllers, undersized target, source-parent exclusion; a real throttled NVMe
deadline with quiescence; raw-source removal/boot; and cross-install recovery
secret rejection. All six installations have distinct disk GUIDs, root PARTUUIDs,
installation IDs and both credential salts, including across firmware runs.
Both runs retain identical source-hash maps and unchanged normal ISO/raw hashes.

The blank-target adapter now permits a zero MBR/primary-header/final-header
tuple for generic source media. Nonzero malformed metadata, a remaining source
protective MBR/backup header and read failures still reject before writes.
Readonly source-parent exclusion and fresh Identify binding remain enforced.
Actual-code host tests cover these admission decisions.

QEMU xHCI/OVMF stalls before kernel entry; EHCI boots the same raw preview bytes
in [preview-oo2nhbvu](../../build/root-phase4/preview-oo2nhbvu/manifest.json).
The final UEFI raw installer fixture therefore uses EHCI; BIOS retains xHCI.
This qualifies UEFI raw USB/EHCI acceptance, not QEMU xHCI firmware acceptance.
Earlier UEFI [guest-ga01v8_a](../../build/root-phase4/guest-ga01v8_a/manifest.json)
passed ISO install/persistence/recovery but was deliberately superseded to apply
the blank-target and EHCI fixture changes; its interrupted result is not counted.

Fresh `make test-root-phase4-host` passes: control/inspection
[leases-60302657](../../build/root-phase4/leases-60302657/manifest.json),
archive/deployment [payload-8ty611xq](../../build/root-phase4/payload-8ty611xq/manifest.json)
(1548 archive cases, 284 deployment cases and independent output oracle),
FAT [fat-v18hqbp0](../../build/root-phase4/fat-v18hqbp0/manifest.json),
detached provisioning [provision-ft1hbw9r](../../build/root-phase4/provision-ft1hbw9r/manifest.json),
actual runtime [runtime-6m2hm77r](../../build/root-phase4/runtime-6m2hm77r/manifest.json),
CLI [cli-opnntpwt](../../build/root-phase4/cli-opnntpwt/manifest.json),
actual heap bulk/OOM/reclamation tests, and boot/PCI/authenticated recovery host
regressions. All mocks retain their previously labelled scope. Normal media
remain SHA-256 `1fd918dff349a2928c2c56936357aaa844ae5bba84e4f92a65a5e1f0c358248c`
(ISO) and `cb6b774d35d4ac1cad6728179e7907efa275e2bb1c3039e5b97f26c291294119` (raw).

No host physical device was opened or written. These results complete Phase 4
for disposable targets; physical cache durability, disconnect, interruption and
installation remain Phase 5 acceptance.

## Identity-only spare-drive preparation (2026-10-12)

The user supplied “SK hynix 256 GB PCIe Gen 3 x4” and intends to fit the spare
in the Dell 5590. Exact identity is pending the separate read-only inspection
screen. See [Phase 5 preparation](../plans/PERSISTENT_ROOT_PHASE5.md).

`bin/fortress-nvme-inspect.iso` is separately built with INSTALLER_INSPECT=1 and
without TEST_INSTALLER. Only ENUM identity is available: eligibility is cleared,
and sector read/write, flush and deploy callbacks are absent. Host tests prove
the callback/identity policy; [inspect-q4iexbqg](../../build/root-phase4/inspect-q4iexbqg/manifest.json)
passes BIOS/UEFI real-driver identity display with unchanged writable NVMe
sentinel hashes. The ISO SHA-256 is
`f6d0bd695307fa2502467b8b073378de851e29c19c1c4befcf1f37fd328cdccf`.
This is not a physical write-enabled installer. The first packaging attempt used
incorrect relative paths and failed before producing accepted media; rerunning
with absolute isolated-workspace paths passed. Inspector source is now in the
repository with `--inspect` producer support and a retained guest gate.

### Installer usability revision — 2026-10-12

The Dell operator reported hidden input, hard-to-read disk identity fields, a
12-character password minimum, a fixed `operator` name, a 64-character erase
confirmation and full restarts after input mistakes. The revised interface:

- Shows one admitted disk by model, human-readable capacity and serial. Selection
  is `1`; source media and other hardware remain excluded by the backend.
- Echoes public choices and masks passwords with asterisks; file checking, hashing,
  copying, provisioning and verification announce their work.
- Accepts 8–128 printable ASCII characters for login and independent recovery.
  Recovery authentication also uses the same eight-character minimum.
- Lets the user choose a bounded lowercase username. Deployment ABI v2 is 288
  bytes with the complete v1 prefix and a 32-byte name field; legacy v1 still
  provisions `operator`. The detached account redo transaction installs the
  chosen passwd/group/shadow records and a validated 0700 home owned by UID/GID
  1000, with membership in wheel.
- Uses final `YES` confirmation while preserving fresh complete identity/generation
  checks, exclusive lease ownership and the compile-bound physical fingerprint.
  Input mistakes retry the current step; `NO`, Escape or Ctrl-C cancel and wipe.

Evidence: `build/root-phase4/bound-d3trzhts` passes 38 BIOS/UEFI regular-file
checks with username `fabio` and eight-character login, including seven input
retry/cancel paths per firmware, Linux audits, three boots, authenticated recovery
and six hardware/exclusion controls. `build/root-phase4/credentials-b8f6xuja`
passes eight further BIOS/UEFI install/audit/boot/recovery checks with both login
and recovery passwords eight characters long; recovery rejects the login secret.
The full host suite passes in `guest-workspace-hx7368pc` (log
`build/root-phase4/ux-host-isolated.log`). Updated recovery host boundaries pass
in `build/root-phase4/ux-recovery-host.log`; chosen-name runtime and Linux audits
pass in `build/root-phase4/runtime-ievg451i`; final CLI sanitizer checks pass in
`build/root-phase4/cli-yyjdwb32`. Sub-gigabyte capacity formatting was corrected
after the guest suite and verified by the actual CLI host adapter.

The first guest attempt, `build/root-phase4/bound-qx6j2eqf`, hit a real NVMe
timeout during copying while host gates also ran. Its failure is retained; no
success is claimed for that attempt and no cause was established. The separate
subsequent guest run passed without changing the driver timeout policy.

Normal live ISO/image hashes remain protected. All agent test writes are to
disposable regular files. The user's physical install and clean-shutdown
persistence results are recorded separately in the [Phase 5 evidence](persistent-root-phase5.md).
