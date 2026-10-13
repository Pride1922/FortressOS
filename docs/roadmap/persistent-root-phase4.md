# Persistent root Phase 4 progress evidence

2026-10-12. **COMPLETE — DISPOSABLE TARGETS ONLY.**
BIOS/UEFI installation/boot/recovery/exclusion/fault matrix: **70/70 PASS**.
Earlier progress and failed attempts below remain historical evidence; the
original acceptance checkpoint is retained below. Later sections record Dell
acceptance and subsequent installer improvements with their own test scope.
The latest logging revision has focused BIOS/UEFI acceptance; physical USB
logging and two-NVMe acceptance remain pending.
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

### Two-NVMe selection and stepped installer — 2026-10-13

The separate `INSTALLER_MULTI=1` profile adds a numbered model/capacity menu
without serials, with explicit selection among at most eight supported NVMe
controllers. Queue/DMA/geometry/failure state is retained separately per
controller. Complete boot discovery is pinned once; fresh pinned PCI and full
selected Identify checks remain mandatory, without rescanning every firmware
bus on each I/O. No hotplug enrollment is claimed. Original live, disposable,
inspection and fingerprint-bound registration profiles remain separate.

The user's clean-screen and stepped interface changes are retained: keyboard
layout, disk, chosen username/login password, independent recovery password and
final YES confirmation. Byte-driven framebuffer progress uses the terminal
writer so ANSI erase-line sequences render correctly. Completion waits for
Enter and releases the lease, wipes requests and restores terminal attributes
before reboot. UART-only boot/network banners no longer enter the framebuffer
or dmesg. `/etc/fortress-install.log` records provisioning without passwords;
filesystem errors while creating it propagate before readiness is committed.

Valid existing MBR and unrelated GPT targets are admitted. Read errors reject
discovery; matching primary or backup source GUIDs remain excluded even with a
damaged primary. Partition-summary bytes are enumeration-only and are excluded
from the selected baseline identity. Installed boot scans drives read-only for
one coherent matching root PARTUUID before publishing writable state;
cross-drive duplicate identifiers enter authenticated recovery without writes.

Host evidence: `leases-xly23kb2` passes actual control/descriptor, indexed lease,
pinned driver inventory and transport-policy adapters, including MBR/GPT,
backup source exclusion, invalid CRC, read failure and summary-free identities.
`cli-jabpsr72` passes the actual CLI sanitizer adapter with keyboard selection,
input retries, secret masking/wiping, cancellation and reboot cleanup.
`provision-ahsxto9n` passes detached provisioning and Linux audits at 512/4096
sectors; `runtime-qefni231` passes the actual deployment engine with username
`fabio`, detached namespace/service isolation and independent Linux audits.
`multi-host-current.log` records heap and root boot/recovery sanitizer regressions.
`multi-timeout-tcd_reox` passes two BIOS/UEFI real-driver timeout cases using
QMP throttling of regular-file targets: the updated failure screen waits for
Enter, reports no success, and preserves target/source hashes. This focused
deadline check uses one NVMe; the indexed two-controller cases are separate.
The earlier aggregate host run failed its source-freeze assertion after source
edits; the affected provisioning check was rerun successfully, rather than
claiming acceptance for that interrupted aggregate.

Final media producer: `build/root-phase4/media-vijrdnvd`; isolated build and
overlay recorded in `build/root-phase4/multi-build-final-receipt.json`.
All 333 kernel/user source files compared byte-identically with that build.
`build/root-phase4/multi-_43ow6a3` passes the final BIOS/UEFI two-controller
matrix: cancellation before writes, installation to each indexed target over
existing MBR/GPT layouts, unchanged unselected/source/seed hashes, independent
Linux filesystem/accounts/log audits, three boots with both controllers present,
second-controller root discovery, duplicate-root authenticated recovery with no
writes and installer-source clone exclusion. Final source/media hashes match.
Framebuffer captures `multi-progress-final.png` and `multi-progress-uefi.png`
verify clean progress rendering. All agent writes used disposable regular files.

Earlier `multi-2yk1o5cm` was interrupted after exposing per-I/O full PCI rescans;
that performance problem was corrected with pinned BDF checks. `multi-xvxkktj9`
was interrupted to fix literal ANSI characters in the progress bar. These
partial runs are retained and do not establish full acceptance.

Raw image: 545259520 bytes (520 MiB), SHA-256
`d56f1ec068fe6b366c83593e46b589220fc0ac54e8ae22d6dac8488f2338cc9e`.
The [disk-selection instructions](../installer-multi-nvme.md) describe the
separate image and supported scope. Physical two-NVMe acceptance remains
pending; the earlier Dell single-drive installation evidence does not accept
this new revision.

### Simple disk-number confirmation — 2026-10-13

Following the agreed design, the final prompt now requires the chosen one-based
disk number instead of YES. Blank, YES and other numbers retry the same prompt
with answers retained; NO, Escape and Ctrl-C still cancel. The summary shows
simple partition-type hints (Windows-type, Linux-type, EFI boot, other), an
unknown-contents warning when needed, and "Installing will erase this disk."
No filesystem/OS/used-space detection is claimed; no partitions does not imply
an empty disk. Full identity admission and lease behavior are unchanged.

`make test-install-cli-host` passes ASan/UBSan in `cli-21u0gcln`, including correct
disk-number acceptance, incorrect/blank/YES retries, warning variants and cleanup.
`FORTRESS_INSTALL_ACCEL=kvm make test-install-confirmation
INSTALLER_MEDIA=build/root-phase4/media-sp50jz5j` passes all four BIOS/UEFI x
disk-1/disk-2 cases in `confirmation-fxtgrga9`. Actual Ring 3 prompts, retries and
cancellation are exercised; target, non-target and source hashes remain unchanged.
These guest cases stop at confirmation and do not repeat the previous full
install/boot/recovery matrix. Screenshots and argv/logs are retained.

Updated media producer: `media-sp50jz5j`; raw image remains 520 MiB, SHA-256
`f722c84f32ee5863a18975d89aa2800a21d9400fe139a6b2138703f953b793d9`.
All test writes were to disposable regular files; no physical disk access.

### Advisory installer password strength — 2026-10-13

Both login and independent recovery password entry now show Weak / OK / Strong
after entry. The eight-character minimum remains; weak uncommon passwords can
continue, and uppercase/digit/symbol mixing is not required. A small built-in
common-password list rejects known words, keyboard/numeric patterns, project
defaults, case variants and numeric/!/? suffixes. Comparison creates no extra
plaintext copy. Hints use bounded length/distinct-character checks, with short
repeating patterns rated Weak; this is advisory, not an entropy estimate or a
breach-database check. Existing password confirmation, independence, hash format,
masking and wiping remain intact. This policy is installer-only.

`make test-install-cli-host` passes ASan/UBSan in `cli-xchrjiki`: common-case/suffix
rejections, weak acceptance, lowercase OK/Strong passphrases, repeat hints, actual
login/recovery retry flows and existing cleanup/lease tests.
`FORTRESS_INSTALL_ACCEL=kvm python3 scripts/test_installer_confirmation.py --media
build/root-phase4/media-dh1s6i_8 --password-guidance` passes four BIOS/UEFI x
disk-choice guest flows in `confirmation-x6pi797q`, with four password-guidance
and four confirmation/cancel result records. Common login/recovery retries,
Weak eight-character acceptance, OK/Strong lowercase hints and no plaintext echo
are checked in actual Ring 3. Cancellation preserves target/non-target/source
hashes. These cases stop before installation; prior full installation acceptance
remains labelled by revision. All fixtures are regular files.

Final media producer `media-dh1s6i_8`, raw image 520 MiB, SHA-256
`cb9cb36d2f86fdc059c62d804a7ee6d6052bbb8afb577a5a79cc086eb63bcc7a`.

## 2026-10-13 - Recovery password reveal and storage acknowledgement

The agreed recovery flow retains full masked confirmation, offers SHOW and
requires YES to acknowledge safe storage. SHOW uses the existing TERM_LOCAL
route to display the password only on the local framebuffer. Enter clears it
before restoring the original route. Plaintext stays in existing short-lived,
wiped password buffers, never deployment requests, UART or provisioning logs.
Other answers retry; Escape/Ctrl-C cancels before deployment. Missing local
screens refuse SHOW while leaving the password hidden.

`make test-install-cli-host`: ASan/UBSan PASS in
`build/root-phase4/cli-y4m1fmt_`, including repeated reveal, cancel while visible,
routing restoration, request wiping, lease cleanup and unavailable-screen refusal.
`FORTRESS_INSTALL_ACCEL=kvm python3 scripts/test_installer_confirmation.py
--media build/root-phase4/media-rhgf610x --password-guidance`: four BIOS/UEFI x
both disk choices PASS in `build/root-phase4/confirmation-5ng8vtcc`. Verified
SHOW/Enter/NO/YES flow, masked/common-password retries, no plaintext UART leakage
and unchanged target/non-target/source hashes. Local reveal screenshot visually
checked. Disposable regular files only; no physical writes. These focused cases
cancel before installation; the historical full install/boot/recovery matrix
was not rerun.

Updated image: `bin/fortress-installer-multi.img`, 545259520 bytes (520 MiB),
audited producer `build/root-phase4/media-rhgf610x`, SHA-256
`44adc99f1a5233a39cdadd40c4811531a49d886f35881e805ec939fff7d379c7`.

## 2026-10-13 - Previous-step navigation and separate cancellation

Escape returns to the previous numbered screen before installation. Step 1
stays at keyboard layout. Ctrl-C cancels at every input prompt; CANCEL also
cancels at non-password prompts (NO remains valid at final confirmation).
The selected disk, layout, username and completed password hashes survive
backtracking. Enter keeps them; CHANGE requests a password replacement.
Replacing login wipes recovery and requires independent recovery setup again.
Shorter usernames clear their old tail. Partial secret buffers wipe on back or
cancel. Navigation is unavailable after lease acquisition/deployment starts;
final disk-number confirmation and fresh complete identity checks are unchanged.

`make test-install-cli-host`: actual CLI ASan/UBSan PASS, evidence
`build/root-phase4/cli-h_3evf2j`. Includes all-step traversal without password
rehashing, disk switching, login/recovery replacement, shortened username,
partial-secret cancellation, request wiping, terminal and lease cleanup.
`FORTRESS_INSTALL_ACCEL=kvm python3 scripts/test_installer_confirmation.py
--media build/root-phase4/media-59gc1tlx --password-guidance`: BIOS/UEFI x both
disk choices PASS in `build/root-phase4/confirmation-n48b433g`. All four flows
walk from final confirmation through recovery/account/disk/layout, then retain
answers with Enter, retry wrong confirmation and cancel. Common-password and
local-only recovery reveal checks pass; both disks and installer media remain
hash-identical. Disposable regular files only; no physical writes. These gates
stop before installation, not a rerun of the historical full installation matrix.

An initial runner attempt `confirmation-2jd8he0z` waited for old prompt wording;
its disposable QEMU was stopped via QMP and artifacts retained before fixing
the runner and passing the complete four-flow gate.

Updated image: `bin/fortress-installer-multi.img`, 545259520 bytes (520 MiB),
audited producer `build/root-phase4/media-59gc1tlx`, SHA-256
`7c1e732aacf9e97f797e96a96e6166aebcb8adbbbcfa19aed938819960ca9ffe4`.

## 2026-10-13 - Show/hide passwords while typing

Tab toggles visibility for login/recovery entry and confirmation. Every input
read starts hidden. Before showing any plaintext, switch to existing TERM_LOCAL
routing. Redraw bounded input locally after edits so backspace/hiding removes
previous characters, including wrapped text. Enter/Escape/Ctrl-C clear the
local display before restoring prior output routing. Without a local screen,
Tab keeps input hidden. Existing password bounds, independent recovery checks,
secret-buffer wiping, navigation and raw-write gates are preserved.

`make test-install-cli-host`: actual CLI ASan/UBSan PASS in
`build/root-phase4/cli-bpxc2zrz`, including toggles before/after typing, repeated
show/hide, backspace, confirmations, Escape while visible, unavailable-screen
fallback, 128-character bounds, output-mode restoration and no UART plaintext.
`FORTRESS_INSTALL_ACCEL=kvm python3 scripts/test_installer_confirmation.py
--media build/root-phase4/media-qwcgtg6h --password-guidance`: BIOS/UEFI x both
disk choices PASS in `build/root-phase4/confirmation-06xblrw8`. Each flow reveals
both password inputs and confirmations, types an extra character/backspaces,
hides and accepts. Framebuffer capture visually checked; no secrets in UART
transcripts. Existing common-password, recovery-storage, all-step navigation and
confirmation-cancellation cases pass, with both disks and source media hash
unchanged. Disposable regular files only, no physical writes. Focused cases
stop before installation; the historical full install/boot/recovery matrix was
not rerun for this UI change.

Updated image: `bin/fortress-installer-multi.img`, 545259520 bytes (520 MiB),
audited producer `build/root-phase4/media-qwcgtg6h`, SHA-256
`c14992e3fbf563f32ffc08061d191b1ce58ab4f39589b92d41b353eac694eeed`.

## 2026-10-13 - Consistent installer headers and screen context

Centralized screen clearing/header rendering: FortressOS installer - Step X of
5, followed by the current screen title. Disk inventory/layout use step 1;
selection/file checking use step 2; account and password hashing use step 3;
recovery, reveal and hashing use step 4; confirmation, deployment and results
use step 5. Password redraws preserve account/recovery context and distinguish
entry from confirmation. Local secret clearing redraws the header before
restoring output mode. The existing progress line remains beneath the deployment
header. Navigation is still absent after deployment starts. No storage,
identity, lease, boot-entry or error-policy changes were made.

`make test-install-cli-host`: actual CLI ASan/UBSan PASS in
`build/root-phase4/cli-p94nnf9p`, including password context/confirmation labels,
deployment and success/failure headers, routing restoration, secret wiping,
retained answers and lease cleanup. `FORTRESS_INSTALL_ACCEL=kvm python3
scripts/test_installer_confirmation.py --media build/root-phase4/media-dqhop2om
--password-guidance`: BIOS/UEFI x both disk choices PASS in
`build/root-phase4/confirmation-p0m14qt6`, with all five numbered headers,
file-check context, local password visibility/confirmation, recovery handling,
back navigation and cancellation. Local confirmation screen visually checked.
Both disks and source media retain their original hashes. Disposable regular
files only; focused guest cases stop before writes. Full install/boot/recovery
matrix and physical acceptance were not rerun for this presentation change.

Updated image: `bin/fortress-installer-multi.img`, 545259520 bytes (520 MiB),
audited producer `build/root-phase4/media-dqhop2om`, SHA-256
`870155827e9bad1139770967804e71591516b7b94f309914416c14cbc3516ad0`.

## 2026-10-13 - Plain installer errors with troubleshooting details

Inventory, file validation, target acquisition and deployment failures now show
plain explanations and next steps, with original signed stage/error/call codes
below. Guidance covers memory/space exhaustion, busy or changed disk identity,
unsupported or unsafe configurations, invalid installer files/settings and
stage-specific preparation/copy/account/verification failures. Unknown errors
use a conservative fallback. Syscall return values and raw VFS backend status
errors remain distinct; a mapped explanatory reason never overwrites original
codes. Incomplete status remains failure even when the call returned zero.

Pre-deployment failures say no installation data was written. Once deployment
has been attempted, the screen warns of possible partial erasure and advises
against booting the disk. Error screens wait for Enter before returning. No
automatic write retries, extra disk authority or hardware-cause inference added.

`make test-install-cli-host`: actual CLI ASan/UBSan PASS in
`build/root-phase4/cli-60h26cfi`, including pre-write validation/acquisition
failures, deployment verification failure, partial-erasure guidance, namespace
separation, zero-call backend error, unknown-code fallback and cleanup.
`FORTRESS_INSTALL_ACCEL=kvm python3 scripts/test_installer_confirmation.py
--media build/root-phase4/media-nsjqr1af --disk-timeout`: actual NVMe timeout
BIOS/UEFI 2/2 PASS in `build/root-phase4/confirmation-o_u3in3a`; preparation
failure wording, partial-install warning, signed details and Enter return are
checked. Captured framebuffer visually checked. The retained manifest has the
older generic confirmation scope label; its case records identify timeout runs.
QMP throttles disposable regular-file target I/O; source/target hashes unchanged.

The same command with `--password-guidance` passes four BIOS/UEFI x disk-choice
UI flows in `build/root-phase4/confirmation-8krmtjt5`, preserving visibility,
recovery, headers, all-step backtracking and confirmation/cancellation, with
unchanged disks/media. These are focused UI and failure gates, not a rerun of
the full install/boot/recovery matrix or physical acceptance. No physical writes.

Updated image: `bin/fortress-installer-multi.img`, 545259520 bytes (520 MiB),
audited producer `build/root-phase4/media-nsjqr1af`, SHA-256
`d1aa6862b1ef508532fb9e2e78788439c9493e3c96e4eb02b09387233dd3f59a`.

## 2026-10-13 - Installed and USB installer logs

New installations save `/var/log/fortress-install.log`; installer USB media
exposes `/fortress-install.log` on the separate FAT16 FORTRESSLOG volume.
The bounded 4 KiB record includes session, selected model/capacity, username,
stages, cancellation and signed error/result details. Passwords, password
hashes and recovery secrets are excluded. Repeated attempts retain recent
events. Log-only failures warn without blocking installation; filesystem
health, freeze and discard failures still prevent a success result.

The private USB writer admits only the boot-source GUID and audited fixed GPT
and FAT metadata, writes the eight preallocated file-data sectors, flushes and
reads them back. It never changes boot files, GPT, FAT allocation or directory
metadata, and latches off after a write/flush/readback failure. No general USB
write authority or disk registration is added.

Actual-code ASan/UBSan gates PASS: CLI `build/root-phase4/cli-d1y2gph8`, control
`build/root-phase4/leases-9c1dip9m`, runtime/EXT4
`build/root-phase4/runtime-t3jzt6uw`, and bounded USB writer
`build/root-phase4/logging-f73ww1zo`. Writer cases cover malformed metadata,
wrong identity, unsupported devices, failures, bounded retention and signed
codes, with independent byte-range, FAT and Linux filesystem audits.

KVM guest logging gates PASS under BIOS in `build/root-phase4/log-guest-960reo8p`
and UEFI in `build/root-phase4/log-guest-8_c9qpvd`: actual installation, both
log copies, three boots including abrupt-stop recovery, independent authenticated
recovery, read-only USB warning with successful installation, cancellation and
real NVMe deadline failure. USB comparisons permit only the 4096-byte log slot;
all other source bytes remain unchanged. BIOS uses xHCI directly. UEFI uses
the existing EHCI preboot fixture plus an identical source-GUID xHCI clone for
kernel logging because of the QEMU/OVMF xHCI preboot limitation. This does not
establish physical USB logging support. Earlier failed/interrupted fixture runs
remain retained; these are focused logging gates, not the entire Phase 4 matrix.

Updated image: `bin/fortress-installer-multi.img`, 553648128 bytes (528 MiB),
audited producer `build/root-phase4/media-emxhicsa`, SHA-256
`59ac446baaec70bada55b4216138b1a4eca7648c04717f70c0f63158e36a9149`.
All agent tests use disposable regular files; physical acceptance is pending.
