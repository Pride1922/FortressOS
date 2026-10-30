# Repository packages and desktop updates

The user's 2026-10-30 scope supersedes the earlier enrollment proposal.
FortressOS is an experimental personal OS. Package installation must not require
device enrollment, device identities, local authentication secrets, signed boot
selectors, separate key media or authenticated account lineage.

## Required workflow

- Build an app as an FPK with the existing builder and package format.
- Sign the catalog and publish content-addressed objects to the existing Ubuntu
  repository with `scripts/pkg_repository.py`.
- Configure the OS with a repository URL and trusted publisher public keys.
- Download, verify signatures/hashes and compatibility, then install atomically.
- Track installed versions in ordinary root-owned files and preserve settings.
- Upgrade desktop packages while retaining base `/bin/compositor` for recovery.
- Add boot-aware kernel/base updates with a previous bootable version retained.

The publisher alone holds private signing keys. Repository UUID and platform/base
versions identify a repository and software compatibility, never a laptop.
Ordinary local version records need not defend against offline disk forgery.

## Current source boundary

The FPK builder, publisher, catalog/artifact verifier, package planner/transaction
engine, SYS_PKGCTL transport and descriptor ingestion remain. The normal download
client now reads `/etc/pkg/repository.conf` and `/etc/pkg/trust.conf`; enrollment
authority and selector files have been removed from that path.

The abandoned enrollment boot modes, module-key delivery, public authority and
POC capsule/bundle tools have been removed. Their normal enrolled-root backend
was also removed; the existing transaction engine remains available for reuse.
The simple installation backend and repeated-version transition still need to
be connected. This cleanup does not claim a Dell installation or working upgrade.

The delivery order is one app download/install/run on Dell, a second version with
reboot/settings/recovery checks, desktop upgrades, then boot-aware base OS updates.
No further QEMU run is a prerequisite before the user's requested Dell POC.
