# Packaging and repository updates

Proposed implementation plan, 2026-10-30. Starting checkpoint: `main`, `9df5b53`.
This replaces the enrollment approach. This planning change touches no runtime
code or boot media.

## Intended workflow

The publisher packages an application, signs the catalog and adds it to the
existing Ubuntu repository. The installed OS supports:

```text
pkg get TOOL
sudo pkg install TOOL
TOOL
```

After publishing a newer version, upgrade it without reinstalling the OS.
These are proposed commands, not a claim that the current image supports them.
The first deliverable is this complete flow on Dell.

There is no device enrollment, laptop identity, device secret, key USB or account
lineage. The publisher keeps its private signing key; the OS stores the URL and
trusted public keys in ordinary root-owned configuration. Existing signature/hash
verification stays mandatory. Local records need not resist offline disk editing.

## Reuse and actual gaps

| Component | Current source and required change |
| --- | --- |
| Package format/verifier | Reuse FPK, gzip/USTAR, file hashes, ELF and signed catalog verification. |
| App builder | Generalize `scripts/build_desktop_package.py` into an app packaging tool. Keep the existing desktop entry point as a wrapper. |
| Publisher | Reuse `scripts/pkg_repository.py`, `catalog.v1` and `objects/sha256/<hash>.fpk`. No repository format redesign. |
| Download | Existing `pkg fetch PACKAGE DIRECTORY` verifies downloads. Add name-based `get`, cache handling and catalog refresh. |
| Trust | Existing `/etc/pkg/repository.conf` and `/etc/pkg/trust.conf`; software/repository identity only, no per-device setup. |
| Installation | Reuse SYS_PKGCTL 95 and bounded descriptor ingestion. Connect an ordinary installed-root backend to the retained planner/transaction engine. |
| Upgrade | The current planner rejects a different installed build/artifact. Implement an explicit old-to-new transaction. |

Existing repository: `https://fortress.ameyerocha.com`, Ubuntu document root
`/srv/fortress-repository`. No server redesign is needed.

## CLI contract

| Proposed command | Behaviour |
| --- | --- |
| `pkg update` | Download/verify and refresh the cached catalog; install nothing. |
| `pkg get TOOL` | Resolve, download, verify and cache the current package. Print its version/build. |
| `sudo pkg install TOOL` | Install the cached package; fetch it first if absent. Reverify in the privileged path. Identical installed build/hash is a no-op; a different installed version requires upgrade. |
| `sudo pkg upgrade TOOL` | Fetch/install a newer build. Same version reports up to date. Reject older builds and equal-build substitution. |
| `pkg list` / `pkg info TOOL` | Show installed version, available version and pending transaction state distinctly. |
| `sudo pkg remove TOOL` | Remove only package-owned files, preserving settings and user data. |
| `sudo pkg recover` | Complete or undo an interrupted transaction and report the result. |

Keep explicit-directory `pkg fetch` for diagnostics. Downloads may run without
root where their cache is writable; mutation uses existing root/CAP_SYS_ADMIN
authorization. No install scripts, post-install hooks or arbitrary command execution.

## Stage 1 — Package and publish a real app

Provide `scripts/pkg_build.py` with a small JSON input: name, command, version,
increasing numeric build, platform/base compatibility, executable and optional
data/documentation. Resolve local paths relative to that JSON. Reuse deterministic
FPK encoding and validators. Start with one static executable and the existing
bounded file set; reject unsupported links, devices and set-ID payloads.

Document one full Ubuntu example: build TOOL, inspect it, sign/publish the catalog,
then deploy objects before activating the catalog. Retain other offered packages
when publishing a new version; do not accidentally drop them from the catalog.
Keep signing keys outside both the repository checkout and HTTP document root.

Deliverable: a real runnable FortressOS tool package and a second version, with
repeatable build/publication commands. A synthetic test fixture is not the POC.

## Stage 2 — First Dell download/install/run POC

Add the name-based commands/cache and bind installation to the normal writable
installed root, after normal filesystem/account initialization. Package management
must not gate root boot. Create package directories and an empty database on the
first authorized operation using ordinary filesystem operations.

Use `/var/cache/pkg` for shared cached content and `/var/lib/pkg` for root-owned
inventories, pending transactions and recovery inputs. An unprivileged download
may use a private cache. Installation retains bounded regular-file descriptors
and independently verifies their bytes; a cache pathname is never trusted proof.

Refactor the retained engine to use ordinary inventories and transaction records.
Remove its requirements for a device MAC key, independent minimum record hash,
physical inode anchors and package admission before journal replay. Local checksums
can detect damaged/torn records; they are not offline-forgery protection.

Publish commands under `/usr/bin` and immutable package data under `/usr/share/pkg`.
Refuse base-command collisions, foreign files and cross-package ownership conflicts.
Keep one package writer at a time, existing filesystem locks, account reservations,
DAC checks, bounded cancellation and I/O-failure fencing. Do not modify account files.

Deliver a matching kernel/CLI build and exact FortressOS instructions. An old kernel
cannot gain a new backend by copying an app: identify and prepare any required
one-time kernel/boot-file refresh explicitly, without asking for an OS reinstallation
or format. Physical actions remain user-operated and separately authorized.

Acceptance on Dell: get TOOL, install TOOL, run it, inspect its installed version,
reboot and run it again. Show invalid-signature rejection without installed-file
changes. This is the first product POC; no new QEMU campaign is a prerequisite.

## Stage 3 — Repeated upgrades and recovery

Implement an installed old build plus a verified new build. Stage new files and
durable recovery inputs before changing public names. Keep the old executable/data
until new publication and inventory are durable. Use the single-command package
boundary as the activation point; several renames are not globally atomic.

Record transaction intent first. After interruption, recovery must reconcile both
files and inventory to the previous complete version or new complete version.
Recover before a new package mutation or launch of an affected pending package.
Uncertain/corrupt state produces a clear error, not blind cleanup or repeated I/O.
Retain local recovery inputs so recovery needs neither a network nor a newly valid
catalog. Catalog expiry still rejects new installs; it must not block recovery of
an already recorded transaction.

Persist the highest accepted catalog revision and package build/hash in ordinary
root-owned state. Reject network rollback and equal-version substitution. Automatic
failure recovery may restore the previous executable without lowering the highest
accepted release counter. Whole-disk rollback defence and a manual downgrade command
are outside the first implementation.

Acceptance: initial install, two consecutive upgrades, reboot persistence,
same-version no-op, rollback/substitution rejection, low-space refusal, interrupted
update recovery and remove/reinstall. Preserve a sentinel setting and unrelated
file throughout. Failed verification never publishes a command.

## Stage 4 — Desktop updates

Apply the working app workflow to `fortress-desktop`, command `desktop`, installed
at `/usr/bin/desktop`. Preserve base `/bin/compositor` for recovery. Stop the active
desktop to the console, install, then launch the new desktop explicitly. Preserve
existing display authorization; no automatic session restart is needed for the POC.

Keep `/etc/desktop.conf`, timezone, network settings and user documents outside
package-owned files. Never replace existing settings with packaged defaults.

The checkpoint's Notepad is a separate process at `/bin/notepad`; the existing
desktop builder includes only the compositor. The first desktop update must remain
compatible with base clients and their current window protocol. Updating several
executables together needs a reviewed bounded format extension or companion packages
with explicit compatibility checks. A compositor-only update is not a whole app-set
update, and this dependency work must not block the first standalone tool POC.

Acceptance on Dell: two desktop upgrades, retained settings, launch after each
upgrade/reboot and successful base compositor recovery if the package is unavailable.

## Stage 5 — Base OS updates

Use the same repository and publisher trust for a later `fortress-base` update type.
Specify its manifest, kernel/bootstrap/base compatibility and boot-file publication
together. Ordinary app packages cannot write kernel, bootloader, ESP, account files
or arbitrary base-owned paths.

Prepare the next complete boot set, preserve the current bootable set and recovery
entry, and switch selection last after durable writes. Retain the previous set until
the new boot is confirmed. Interrupted preparation must leave the old boot usable.
Update base-version metadata consistently with boot selection. No reformat, partition
change or full OS reflash is part of the intended update workflow.

Before implementation, discuss the concrete boot/root contract changes required by
PROTECTED.md. This plan does not authorize raw physical writes or assume an existing
trial-boot/automatic-fallback mechanism. Deliver a separately reviewed boot-aware
design and old/new/recovery boot evidence. This stage must not delay app/desktop POCs.

## Verification and delivery

- Focused actual-code host tests and normal kernel/CLI builds during implementation.
  Cover bad packages/signatures, permissions, failed publication and recovery cuts.
- Respect Dell-first verification: no new QEMU campaign before the Dell POC. Host
  interruption tests use disposable regular-file images, never physical disks.
- Deliver runnable vertical slices with commands matching the built FortressOS CLI.
  Separate host, firmware and Dell evidence; downloads do not prove installation,
  and app updates do not prove base OS updates.
- Keep fixture adapters/test keys out of normal media. Do not generate/provision
  publisher secrets, touch physical disks, commit or push without explicit requests.
- Update the existing Ubuntu instructions to match final implemented commands.
  Ask for server details only when deployment actually needs them.

After plan review, implement stages 1–2 as the first complete delivery: real app,
publication instructions, normal installation binding and matching Dell build.
No app store GUI, dependency solver, remote management or new filesystem framework
is a prerequisite.
