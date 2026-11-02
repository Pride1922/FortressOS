# Repository base OS updates

Boot/root contract approved by the user on 2026-11-02. Implementation and
focused host evidence are tracked in [the delivery notes](../base-repository-updates.md).
Firmware and physical acceptance remain separate.

## User-visible outcome

`sudo pkg upgrade` and `sudo pkg upgrade -y` discover a signed `fortress-base`
release in the existing HTTPS repository. Application upgrades retain their
existing behavior. A base upgrade prepares the next complete base system and
reports that reboot is required. It never reformats the disk or reinstalls
accounts. The prior base system remains selectable from the boot menu.

The 2026-11-02 installer does not contain this mechanism. It must be superseded
by an image containing the reviewed implementation before it becomes the intended
last-reflash baseline. Do not describe that earlier image as supporting base
repository updates.

## Why the existing mechanism is insufficient

`src/lib/kernel_update.c` validates a predecessor-bound kernel-only layout. Both
kernels use `/boot/bootstrap.tar`. It writes only the inactive kernel, readiness
record and four menu selector sectors. It deliberately never updates bootstrap or
installed-root base programs. `INSTALL_ACQUIRE` refuses mounted/source devices and
requires installer mode; using it against the running installed root would break
its exclusion contract.

`installed_root_boot()` authenticates the boot modules, validates a detached ext4
root, and publishes it. `vfs_boot_publish_root()` currently retains only bootstrap
`/dev`, `/run` and `/tmp`. Replacing a bootstrap alone therefore does not replace
the installed `/bin` and `/sbin` programs. App packages cannot safely substitute
for a boot-aware base transaction.

## Proposed complete A/B boot sets

Keep the existing 64 MiB ESP and root partition geometry. Reserve two fixed-size
kernel banks and two fixed-size bootstrap banks, each at most 8 MiB, plus separate
readiness and release-state records, and two alternating rollback-floor records. This fits the ESP without moving partitions.
Require the new layout at installation; refuse other layouts before writes.
Existing kernel-only layouts remain supported by their original implementation,
without silently interpreting them as complete base-update layouts.

Each menu entry selects exactly one kernel, its bootstrap and its readiness
record. Keep installed and authenticated recovery entries for both sets. The
bootstrap carries that release's base executables, firmware and immutable data,
plus the installation's independently retained recovery credential. Validate the
complete boot-set binding before root replay and before user processes start.

Retain all fixed allocations after personalization; runtime updating never edits
FAT allocation chains or directories. A base writer validates both GPT copies,
the full FAT allocation graph, reserved extents, all menu copies, installation
identity, root binding and current boot-set identity before preparing writes.
Floor records alternate generations so preparing another release never overwrites
the sole highest accepted catalog/build record.
Unlike the old updater, its bounded inspection window must cover both complete
boot sets. Do not reuse the old 32 MiB-prefix assumption without proving bounds.

## Base userland and mutable root

For this new layout only, route base-owned executable and firmware namespaces
(`/bin`, `/sbin`, `/lib`) to the authenticated selected bootstrap. Route immutable
base assets and documentation through an explicit bounded ownership map. Keep
`/usr/bin` and `/usr/share/pkg` on ext4 for application packages. Specify directory
lookup and listing behavior together; no silent fallback to stale base binaries
when a selected release omits a previously shipped file.

Keep `/etc`, `/home`, `/root`, `/var`, installed application records and artifacts,
and user data on ext4. Never replace account, network, desktop, timezone, trust,
SSH host-key or service configuration with archive defaults. New configuration
schemas must remain compatible with the predecessor for rollback, or the release
must be rejected. No arbitrary migration/install scripts in this milestone.

Adapt detached-root executable validation to validate the selected base programs;
continue validating accounts, home permissions and root identity on ext4. Preserve
root display authorization and package command-ownership checks. App admission
must compare compatibility against the selected base release, rather than a
stale `base_version` string in mutable configuration.

## Repository and privileged verification

Use the existing publisher identity, Ed25519 trust keys, signed catalog and
content-addressed object transport. Reserve `fortress-base` as a distinct artifact
type; the ordinary application backend must reject it. Do not let an app manifest
request ESP writes or base-owned paths.

Define a bounded, versioned base manifest containing release build, platform,
update-layout version, supported predecessor/base ABI range, kernel/bootstrap
lengths and hashes, and complete immutable-path ownership. Bind every byte and
identity to the signed catalog target. Document exact size limits in producer,
fetcher, syscall ingestion and verifier; existing FPK limits are not implicitly
raised. Publisher tooling rejects duplicate identities and retains other catalog
targets. Signing secrets remain external; implementation does not publish or
create keys automatically.

Downloads finish before acquiring the package writer. The privileged backend
retains regular-file descriptors, copies bounded inputs and independently verifies
catalog signatures, time, repository identity, platform, release compatibility,
artifact hashes and rollback floors. Userspace verification is never authority.
Serialize base and app mutations through the existing package owner. Persist the
highest accepted catalog/release identity without lowering it during manual
previous-set recovery. Reconcile base floors with existing per-package catalog
floors; a separate base path must not permit cross-package catalog rollback.

## Restricted boot writer and activation

Add a dedicated installed-root base-update operation, not a general raw-device
lease. Userspace supplies source descriptors only: no disk selector, LBA, write
ranges, readiness bytes or authorization flags. Bind the target to the already
admitted installed-root disk and its validated ESP. Retain immutable credential
and process ownership through bounded execution. Verification, allocation and
ownership checks run without filesystem/process locks. Each bounded ESP I/O
uses the existing ext4 exclusion, because ordinary NVMe/root I/O relies on that
serialization; shutdown uses the same exclusion. Preserve driver synchronization
and DMA quarantine.

Write only the inactive kernel/bootstrap extents, its readiness/release records,
and fixed selector sectors. Root filesystem blocks, GPT, FAT, directories, BIOS
stage and bootloader executable are outside this writer's authority. Flush and
read back complete inactive payloads before publishing readiness. Select the
complete new set last. Any uncertain write/flush fences further update attempts;
never loop on a partially failed operation.

Record preparation/selection state durably and provide an explicit recovery path
for mixed menu selectors. A kernel from either selectable set must have matching
bootstrap, readiness and release metadata. Do not reuse the previous set until a
successful boot of the newly selected set has been durably confirmed. Verify the
running boot-set identity when confirming, not just a userspace claim.

Limine's current static menu does not supply automatic failed-boot fallback.
This milestone retains explicit previous-set selection and authenticated recovery;
automatic fallback is a separate bootloader design. Recovery must work offline.
Do not claim the older set was preserved merely because an entry still exists:
verify the complete kernel/bootstrap/record combination after interruptions.

## Implementation and evidence gates

1. Review this boot/root contract under PROTECTED.md and Stage 5 of
   `PKG_PRODUCTION_DESKTOP.md` before changing protected behavior.
2. Implement the versioned format, deterministic producer and actual-code verifier;
   reject bad signatures, malformed ranges, oversized input and substitutions.
3. Implement complete-set template/personalization, boot admission, namespace
   routing and compatibility metadata; retain legacy behavior outside enrollment.
4. Implement the restricted writer, durable state/floors, confirmation and recovery.
5. Integrate automatic repository fetching and `pkg upgrade [-y]`, with precise
   reboot-required and recovery diagnostics, plus publisher documentation.
6. Build the final complete installer only after its update substrate exists.
   Supersede the earlier 2026-11-02 image; avoid accumulating duplicate images.

Focused actual-code interruption evidence must establish old-or-new complete boot
sets and zero root/config/account/application changes. Firmware boot evidence must
exercise new, previous and authenticated recovery sets. Physical writes remain
user-operated; QEMU evidence is never physical acceptance. The user's earlier
request to avoid another QEMU campaign remains in effect; agree on validation scope
before launching such a campaign. No physical disk access, publication, signing
key provisioning, commit or push is authorized by this proposal.

Scope: kernel, bootstrap, base executables, firmware files and immutable assets.
Bootloader replacement, disk-layout migration, arbitrary configuration migrations
and automatic failed-boot fallback need separate designs. Ordinary app/desktop
updates continue through the existing package mechanism.
