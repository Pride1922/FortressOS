# Bounded ordinary-root package upgrades

2026-10-31. Extends the normal Stage 2 single-package database. No boot-order,
lock-rank, user-transition, storage-admission or account-policy changes.

## Interface and authority

`pkg update`, `pkg get rot13`, `sudo pkg upgrade rot13` uses the existing catalog
and cache. Upgrade refresh/download remains explicit, matching the Dell POC.
The privileged CLI reads the installed record only as a hint for downloading the
previous content-addressed artifact. The server must retain previous objects.
A download, cache filename or userspace verification is never kernel authority.

SYS_PKGCTL adds operation 8 with a 112-byte v3 request, retaining the v1/v2 ABI
and 80-byte result. It contains the v2 new catalog/artifact descriptor tuple plus
a previous artifact descriptor/length. The kernel retains all three files,
copies their bounded bytes into one exclusive memory credit with independent
positions, rechecks the retained writer and credentials, and invokes the backend
once. Copyout failure never retries activation. Every returned path releases
operation/source references and memory. The CLI releases its session and fds.

New content passes signed catalog, full artifact/ELF/hash, repository, root,
platform/base, stable package/command and increasing numeric build checks.
Catalog revision rollback/equal-revision substitution and equal-build artifact
substitution refuse. The previous artifact must hash exactly to the installed
record; its integrity/ELF and every currently owned inode/content are rechecked.
It does not require an unexpired previous catalog. The old database checksum,
filesystem identity, canonical encoding and closed installed state are mandatory.
Local checksums do not defend against offline administrator editing.

## Staging and activation

Prepare a bounded heap workspace and check space before creating new version
paths. Stage immutable documents under the new numeric build directory and the
executable under root-only `/var/lib/pkg/upgrade-v3/upgrade-BUILD`. The existing
executable and inventory stay active. Every staged write uses the ordinary
journaled filesystem callbacks, so allocation/barrier failures return errors.
No package spinlock is held across callbacks or credential checks. An interrupted
prefix is reusable only if metadata and all existing bytes match the verified
candidate. Foreign content is never truncated or deleted as recovery.

The existing journaled replacement planner invokes a narrow package hook only
for the registered upgrading actor, filesystem and exact source/victim pair.
Under the existing filesystem lock it revalidates the old inventory/content,
its exact captured hash, and all candidate file/directory identities and bytes.
It adds the two new inventory block images before final credit allocation,
sealing and commit. Executable name replacement, old-inode orphan intent and
inventory publication are one JBD2 transaction. The normal replacement adapter
publishes the new cached dentry only after success. Old open readers retain their
inode; orphan cleanup uses the existing close/sync/boot paths.

## Interruption and limits

Before activation, failure leaves the old executable/inventory active. A retry
may reuse verified staging prefixes. At activation, normal JBD2 replay restores
old or new names/inventory together, with no network or catalog needed for replay.
An uncertain I/O outcome retains the existing taint/namespace fence until reboot;
there is no blind in-boot retry. `pkg recover` remains a status/recovery interface
for the prior engine; this upgrade path has no additional pending journal to replay.
Use a normal reboot after uncertain storage I/O.

Prior build documentation is deliberately retained. Numeric root-owned build
directories under the package parent are outside current-version deletion
ownership; active build files/directories remain checked exactly. The bounded
directory scan may eventually refuse more retained builds. Garbage collection,
manual downgrade and base/service updates are outside this change.

The backend remains one installed application at a time, one command and at most
15 documentation/data files. Same-version upgrade is a verified no-op. The
combined internal decode/verification workspace is capped at 32 MiB; source
snapshots are separately bounded by the ABI. Space can be consumed concurrently;
a staging allocation failure preserves the old version rather than promising a
full-filesystem reservation. No claim of broader package-store/escrow redesign.

## Verification

`make test-pkg-upgrade-host` uses the actual ordinary installer, upgrader, VFS,
EXT4 and JBD2 with host disk/credential adapters and ASan/UBSan. It checks two
consecutive upgrades, byte-exact executable hashes, no-op, rollback/signature
refusal, retained old executable reads, reboot, settings preservation and sampled
staging cuts plus every event in the final 200-event activation/checkpoint window
on cached and write-through disks. Retry follows replay; every case checks that
the executable and inventory build agree. Linux `e2fsck -fn` audits the completed
multi-upgrade image. This is sector-atomic host fault injection, not physical
power-loss or scheduler/IRQ acceptance.

`make test-pkg-stage2-host` covers CLI request shape, progress, previous-source
download and error/session/fd cleanup. `python3 scripts/test_pkg_system_host.py`
checks actual transport v3 validation, aliased descriptor references, immutable
copies, failure cleanup and kernel/CLI stack compilation. Normal builds and
compiled stack reports accompany the host results. Dell acceptance remains a
separate user-operated test after the required kernel/CLI refresh.

## Recorded local results

- `make test-pkg-upgrade-host`: PASS. Actual ordinary install/reinstall regression,
  two consecutive upgrades (the second has all 16 files), no-op, bad previous
  artifact/signature and rollback refusal, allocation failure, retained readers,
  reboot, 424 interruption/replay/retry cases, and a genuinely full disposable
  filesystem (8192/8192 blocks used) returning ENOSPC before staging. Both Linux
  `e2fsck -fn` oracles pass. Evidence: `.codex-remote-attachments/pkg-ordinary-54m_q8rc/`.
- `make test-pkg-stage2-host`: PASS 20/20, including upgrade download failure and
  uncertain previous-cache replacement refusal with no unlink/retry.
- `python3 scripts/test_pkg_system_host.py`: PASS actual transport/owner tests,
  v3 copied sources, concurrent status-busy behavior, error cleanup and stack gate.
- New compiled frames: upgrade 768 B, upgrade stage helper 672 B, service 608 B,
  previous-source frontend 576 B. No new frame exceeds 1024 B.
- `make -j4`: normal kernel/CLI/media build. The initial delivery followed the
  Dell-first POC plan; the subsequent QEMU campaign is recorded below. Test code
  and keys remain outside normal media.

Earlier exploratory low-space testing modified an in-memory descriptor counter
and produced EIO rather than a valid full-filesystem condition; that fixture was
replaced with a debugfs-filled image and independent Linux fsck validation. The
initial staging location in work-v2 was rejected by the old engine's strict
private-work scan; the final implementation uses a separate root-only directory
and preserves that scan. Failed exploratory copies remain in local evidence.

User-reported Dell evidence covers Stage 2 install, repeat install, execution by
`/usr/bin/rot13`, and catalog/download of published 2.0. The original acceptance commands used the absolute `/usr/bin/rot13` path.
The later physical upgrade/reboot confirmations are recorded below.

Final transport evidence: `.codex-remote-attachments/pkg-system-host-20261031-090344-129138/manifest.json`
records runtime/source preservation PASS. The final media build uses
`build/root-phase4/media-u0u6vg5p`; the normal ISO/raw image contain the matching
kernel and upgrade-capable CLI. Build outputs are local artifacts, not git inputs.

## Fresh-image QEMU upgrade acceptance (2026-10-31)

`scripts/test_pkg_upgrade_qemu.py` now creates a normal installed-root disk,
downloads and installs the real `rot13` 1.0 through `/bin/pkg`, shuts down, and
retains that bootable baseline. A private signed HTTP repository then offers 2.0
while retaining the 1.0 object. Every upgrade starts from a separate baseline
copy, verifies 1.0 after boot, runs `pkg update`, `pkg get rot13`, and privileged
`pkg upgrade rot13`, checks version 2.0, filter output, new `--help` and a verified
up-to-date repeat. A second boot verifies 2.0 as the ordinary operator.

BIOS/UEFI × SMP=1/4 PASS under QEMU/KVM: four upgrade/reboot pairs, plus the
initial 1.0 installation boot. Every shutdown passes Linux `e2fsck -fn`,
byte-exact executable and CLI comparison, installed-record checksum/build checks,
and preservation of account files, repository configuration and a user note.
The original 1.0 image hash remains unchanged. No kernel or production userland
behavior was modified for this test.

The Makefile entry point also passes under TCG (BIOS, SMP=1):
`make test-pkg-upgrade-qemu PKG_UPGRADE_QEMU_ARGS="--firmware bios --cpus 1 --port 18081"`.
Evidence: `build/pkg-upgrade-qemu/gate-7cr0ueqc/manifest.json`, including a fresh
1.0 installation, upgrade/no-op and reboot with the same Linux oracles.

Evidence: `build/pkg-upgrade-qemu/gate-h8sijlvq/manifest.json`. The retained
`fortress-rot13-1.0.img` is a 384 MiB installed QEMU test disk, not installer
media. It uses explicitly disposable credentials (`operator` / `qemu-pkg-test`)
and a test repository public key; the ephemeral signing key is never written to
disk. The directory includes the public 2.0 repository, both signed catalogs,
QEMU argv, serial/action/HTTP logs, filesystem oracles and `README.txt`.

Reproduce on a Linux build host with Python cryptography, QEMU, OVMF, e2fsprogs,
mtools and the normal build toolchain:

```sh
make test-pkg-upgrade-qemu
# Optional acceleration when /dev/kvm is available:
make test-pkg-upgrade-qemu PKG_UPGRADE_QEMU_ARGS="--accel kvm"
```

The default uses TCG and covers BIOS/UEFI × SMP=1/4. Every boot attaches exactly
one disposable regular-file NVMe disk, with no additional data disk or physical
device. The test HTTP server stops at completion; restart it using the retained
README to test another copy manually. This closes the fresh-image QEMU gap;
Dell upgrade/reboot acceptance is recorded separately below.

## Flashable installer with the 1.0 baseline

`scripts/create_rot13_installer.py` builds
`bin/fortress-rot13-1.0-installer.img`, a real USB installer with the existing
published 1.0 package preinstalled in the root template. Normal account creation,
repository configuration, filesystem expansion and kernel behavior remain.
The package backend, EXT4 and JBD2 seed the image through a private host adapter
under ASan/UBSan; no copied inventory or manual executable placement substitutes
for installation. The producer authenticates the seed with the already-public
demo repository identity and retains the original trust policy.

The initial candidate consumed the template's deliberately free descriptor-growth
padding, which the unchanged installer correctly refused to expand. Failed image
and runtime evidence remain as `bin/fortress-rot13-1.0-installer.failed-padding.*`
and `build/rot13-installer/accept-hip_oa3u/`. The corrected host source copy skips
those padding blocks during seed allocation, in addition to setting its test
clock. Shipped allocation code is untouched. Actual unchanged expansion on a
seed copy and Linux `e2fsck` now precede media packaging.

The candidate's receipt is `bin/fortress-rot13-1.0-installer.manifest.json`;
runtime acceptance is required before its result becomes `PASS`.
`make test-rot13-installer` exercises copies of the shipped USB image through
normal BIOS/UEFI installation, account personalization, boot with 1.0, upgrade
from the real HTTPS repository at SMP=4, verified no-op and reboot. It uses only
regular-file USB/NVMe devices without a private installer write bypass. The
receipt links the runtime evidence and image SHA-256. See
[Bazzite flashing and upgrade instructions](../rot13-upgrade-bazzite.md).

Final candidate SHA-256:
`17a45575fdf65c8c46fe1c612955bd61693555b8ddf246484652839563f5046f`.
`make test-rot13-installer` PASS for BIOS and UEFI, including real USB-to-NVMe
installation, exact preinstalled 1.0 ELF, live repository upgrade/no-op, SMP=4
reboot and Linux filesystem/record/account oracles. Runtime evidence:
`build/rot13-installer/accept-m6vbo3it/manifest.json`. Physical installation and
upgrade remain user-operated; no physical device was written during this work.

2026-11-01: the user confirmed physical 1.0 → 2.0 upgrade and reboot persistence
PASS. The first acquisition attempt failed and the second succeeded; its generic
CLI diagnostic did not retain the original error code. The subsequent
[session-acquisition investigation](../roadmap/pkg-session-acquire.md) records
the missing filesystem exclusion, corrected diagnostics, before/final evidence
and the boundary between the accepted old image and the new local kernel build.

The user subsequently confirmed rot13 1.0 → 2.0 upgrade and reboot persistence
PASS after installing the two-slot baseline and applying the offline kernel
update, including the corrected package-session implementation. Physical manual
previous/recovery kernel selection remains a separate unconfirmed check.

Next milestone: [multiple installed packages](PKG_MULTIPLE_INSTALLED.md).
