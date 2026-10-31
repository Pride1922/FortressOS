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
- `make -j4`: normal kernel/CLI/media build. No QEMU campaign added, following
  the existing Dell-first POC plan. Test code and keys remain outside normal media.

Earlier exploratory low-space testing modified an in-memory descriptor counter
and produced EIO rather than a valid full-filesystem condition; that fixture was
replaced with a debugfs-filled image and independent Linux fsck validation. The
initial staging location in work-v2 was rejected by the old engine's strict
private-work scan; the final implementation uses a separate root-only directory
and preserves that scan. Failed exploratory copies remain in local evidence.

User-reported Dell evidence covers Stage 2 install, repeat install, execution by
`/usr/bin/rot13`, and catalog/download of published 2.0. Default login PATH still
omits `/usr/bin`, so these acceptance commands use its absolute path. Dell upgrade
and post-upgrade reboot acceptance are pending the new kernel/CLI refresh.

Final transport evidence: `.codex-remote-attachments/pkg-system-host-20261031-090344-129138/manifest.json`
records runtime/source preservation PASS. The final media build uses
`build/root-phase4/media-u0u6vg5p`; the normal ISO/raw image contain the matching
kernel and upgrade-capable CLI. Build outputs are local artifacts, not git inputs.
