# Multiple installed application packages

Implementation checkpoint, 2026-11-01. Physical acceptance is pending.

The bound is **16 application packages**, each with one command and at most 15
other files. SYS_PKGCTL v1/v2/v3 request and result layouts are unchanged. There
is one service writer credit and one activation transaction per package. A batch
is sequential and stops at the first failure; completed activations persist.
Remove remains unsupported by the ordinary backend (`EOPNOTSUPP`).

## Inventory and legacy transition

Slot zero is `/var/lib/pkg/install.v2`. Slots 1–15 are
`/var/lib/pkg/install-01.v2` through `install-15.v2`. Slot discovery, validation
and capacity are bounded. Records bind their slot to a validated package identity.
Install staging is `work-v2` for slot zero and `work-01` through `work-15` for thekey
remaining slots. Upgrade executable staging includes package identity and build.

The legacy transition is an extension in place: the singleton becomes slot zero
without rewriting, copying, renaming or deleting it. Thus there is no migration
window with two authorities, and an old installed command and its artifact,
build, root and catalog identities retain their original record. New empty
records are written under `.new`, checked as exact prefixes on retry, and
published with a durable rename. No root formatting or reinstall is required.

Every admission scans all slots under the existing filesystem exclusion, rejects
malformed and duplicate records, and checks the selected signed catalog/root
against every durable repository floor. Creating a new package cannot reset the
repository floor. Admission also checks actual `/bin`, `/sbin` and `/usr/sbin`
commands, supplementing the fixed protected-name list so new base tools cannot
be shadowed. Exhaustion and command ownership conflicts fail before public
files are published. The artifact is verified before creating a new slot.

Status/recovery inspect the whole inventory. A pending install blocks other
packages; retrying the same install with the exact authenticated artifact resumes
its bounded ledger and validates its staged bytes. Status/recover alone cannot
synthesize a missing authenticated source. Upgrade replay needs no network:
executable replacement and that slot's inventory update remain in the same JBD2
transaction. The other slots are not updated by that transaction.

## CLI

```sh
sudo pkg install TOOL
sudo pkg upgrade TOOL
sudo pkg upgrade
sudo pkg upgrade -y
sudo pkg installed
sudo pkg info TOOL
```

Install and upgrade refresh the authenticated catalog automatically. The all-package
preview uses signed build numbers as version identifiers: catalog v1 does not
contain the manifest's human-readable version string. The targeted completion
message can also display the downloaded manifest version. No artifacts are
requested before confirmation. Noninteractive input requires `-y`; a no-upgrade
invocation succeeds without prompting. Missing catalog entries are reported and
remain installed. Uninstalled catalog entries are excluded.

A private random invocation directory retains the verified catalog snapshot and
package downloads. Predecessor retrieval uses the selected slot's artifact hash.
Downloads occur before acquiring the writer session. Each confirmed installed
record is checked again after acquiring that session. Cache replacement errors
with uncertain outcomes are not unlinked or retried. Diagnostic `fetch` and
explicit staging installation remain available; `update`/`get` are compatibility
commands omitted from normal help and workflow.

## Verification

- `make test-pkg-batch-cli-host`: actual CLI control with mocked inventory,
  catalog, transport and syscalls, under ASan/UBSan. Covers confirmation yes/no,
  noninteractive refusal, `-y`, current packages, partial completion and
  unattempted outcomes, stale installed snapshots and session-free downloads.
- `make test-pkg-multiple-host`: actual EXT4/VFS/JBD2 with a disposable host disk,
  ASan/UBSan and Linux `e2fsck`. Covers the unchanged singleton, two same-build
  packages, independent upgrades, repository floor, inventory corruption,
  ownership collisions, capacity and interruption/replay/retry.
- `make test-pkg-multiple-qemu`: BIOS/UEFI, SMP=4, KVM; one disposable regular-file
  NVMe per boot, signed local HTTP repository, actual CLI and reboot, independent
  Linux filesystem and executable/inventory oracles. No physical disk is attached.
- Existing targeted rot13 QEMU suite: BIOS/UEFI × SMP=1/4, upgrade and reboot,
  byte-exact ELF, settings and Linux `e2fsck`; install/upgrade use automatic fetch.

Exact run results and artifact locations are recorded in the final task report.
The older laptop physical-test notes remain independent evidence of the earlier
single-package kernel and CLI.

### Recorded campaigns

- Final normal kernel and CLI build: `bin/fortress.elf`, `build/pkg/pkg.elf`;
  package CLI maximum frame 912 bytes, new kernel inventory/selection/recovery
  frames at most 512 bytes. No syscall wire layout changed.
- Batch ASan/UBSan PASS:
  `.codex-remote-attachments/pkg-batch-5xa6g3kq/`.
- Actual fetch/verifiers ASan/UBSan PASS (network/filesystem adapters mocked):
  `.codex-remote-attachments/pkg-fetch-20261101-200452-000230/manifest.json`.
- Actual status concurrency/I/O/OOM ASan/UBSan and Linux fsck PASS:
  `build/pkg-status/host-gsn9pjda/final-manifest.json`. Inventory snapshots use
  explicit private contexts; discovery does not temporarily publish a global
  active executor to another status caller.
- Actual two-package host acceptance: retained fixture and sanitizer objects in
  `.codex-remote-attachments/pkg-ordinary-62lx4azw/`; final source refresh is
  compiled as `multiple-final`. The focused cut 1692 exposed mount-time generic
  orphan reclamation occurring before package-owner reconstruction. Resume now
  validates the remaining inventory and refunds only the exact already-authorized
  trash reclamation in a separate durable ledger transaction. The full campaign
  passes 380 install/upgrade cuts across cached and write-through persistence.
- Existing actual upgrade ASan/UBSan: 424 interruption/replay/retry cases,
  low-space refusal and Linux fsck PASS in
  `.codex-remote-attachments/pkg-ordinary-sgu8d50c/`.
- Targeted automatic-fetch QEMU, BIOS/UEFI × SMP=1/4, upgrade/reboot/Linux oracles
  PASS: `build/pkg-upgrade-qemu/gate-_2q5c88w/manifest.json`.
- Final two-package CLI QEMU, BIOS/UEFI SMP=4, confirmation no/yes, noninteractive
  refusal, `-y`, current/unavailable packages, later download failure and retry,
  reboot, byte-exact executable/inventory and credential/configuration oracles
  PASS: `build/pkg-upgrade-qemu/multiple-pyqaglqi/manifest.json`.

Earlier failed test outputs remain available. The host's missing NASM and dynamic
sanitizer libraries were bypassed by using the existing `fortress-dev` Ubuntu
container; no sanitizer checks were disabled. The ordinary host adapter gained
its missing keyboard-device initializer stub, matching its existing mouse stub.

The matching successor update and sidecar are described in
[the laptop commands](../pkg-multiple-installed-laptop.md). The accepted old
installation fixtures are copied before testing; no source fixture or physical
disk is modified.

- Matching final kernel-only successor USB and sidecar CLI PASS on copies of the
  accepted kernel-B/rot13-2.0 roots, BIOS/UEFI SMP=4:
  `build/kernel-update/pkg-multiple-update-yeljug4o/manifest.json`. Each case
  verifies zero root-byte changes during the offline update, rot13 2.0 before and
  after reboot, a new independent application, byte-exact legacy inventory,
  Linux fsck, unchanged source fixtures and read-only repeat-update refusal.
  Artifact receipt: `build/kernel-update/pkg-multiple-rfc4ecw0/manifest.json`.
  The kernel-only updater preserves `/bin/pkg`; the separately delivered CLI is
  executed as a sidecar, as documented in the laptop guide. Physical acceptance
  remains pending.

Final host receipt: `.codex-remote-attachments/pkg-ordinary-62lx4azw/final-manifest.json`.
It binds the fresh package-source build and the 380-cut result, including the
additional actual-base-command shadowing refusal, to the final source hashes.

## Artifact cleanup and next image

On 2026-11-01 the user reported the updater's unsupported/inconsistent/wrong-version
refusal, requested a fresh installation, then stopped the additional installer
verification and requested removal of every generated disk image and ISO. Those
artifacts have been deleted; retained source, hashes and logs remain historical
evidence. The discarded fresh candidate passed BIOS installation and package
commands against the existing HTTPS repository; the additional campaign was
stopped before completing all checks. No fresh-installer physical acceptance is
claimed. A replacement image with the latest Wi-Fi work/redesign awaits separate
user instructions. No new builds or tests were run for this commit.
