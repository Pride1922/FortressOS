# Offline kernel update prototype

The completed acceptance result is **PASS** for BIOS and UEFI. The user also
confirmed physical laptop kernel-update acceptance PASS on 2026-11-01: baseline
installation, offline update, reboot into `kernel-update-candidate-2`, retained
rot13 1.0 and preserved home-file/account checks. The user subsequently confirmed
rot13 1.0 → 2.0 upgrade and reboot persistence on that updated laptop PASS.
Manual previous/recovery boot checks remain physically unconfirmed.
The full QEMU receipt is
`build/kernel-update/media-snul1hju/acceptance.json`; the public image receipt is
`bin/fortress-kernel-update.manifest.json`.

This implements the user's chosen bootable update USB before any online/base
package updater. The accepted scope is a kernel-only update on a new two-slot
baseline. The installed root, accounts, package database and common bootstrap are
preserved. Ordinary app packages retain their existing ownership boundaries.

Design: [offline kernel update](../plans/OFFLINE_KERNEL_UPDATE.md).
Laptop sequence: [Bazzite instructions](../kernel-update-bazzite.md).

Implementation uses the existing installer-mode raw lease and source exclusions.
No syscall or raw permission was added. Preflight admits only the exact installed
GPT/FAT profile and predecessor kernel. The inactive fixed-capacity bank is
written, flushed and read back, followed by its existing readiness record; menu
selectors are written last, with independent flush/readback. No FAT, directory,
partition, bootstrap, root or BIOS-stage sector is written by the updater.

The baseline installer reserves identical A/B kernels. Both readiness records
remain zero in the template and copied payload; after root provisioning and all
payload/layout verification, B and then A are published durably. Legacy single-slot
installation keeps its original publication sequence. Boot module admission adds
only the explicit second readiness basename and retains size/role/count, identity,
checksum, kernel-hash and bootstrap-hash validation.

Evidence, 2026-11-01:

- `build/root-phase4/fat-s7md94sf/manifest.json`: existing native FAT editor,
  ASan/UBSan, independent `fsck.fat` and byte-exact mtools extraction PASS.
- `build/root-phase4/payload-22adu0xa/manifest.json`: archive 1548 cases,
  deployment 286 cases and independent GPT/MBR/payload oracle PASS, including
  secondary readiness after provisioning and failures at each final publication.
- `build/kernel-update-root-boot-host.log`: actual boot module admission and
  readiness validation ASan/UBSan PASS, including the new explicit name and
  rejection of an arbitrary readiness basename.
- `build/root-phase4/runtime-6qsbgvf_/manifest.json`: actual legacy single-slot
  runtime deployment, account personalization and independent Linux audits PASS.
  Its storage-only host adapter now links the menu helper, excludes the unused
  package engine and supplies a no-input mouse stub. Production package admission
  and raw-write gates are unchanged; the QEMU campaign uses the normal package
  engine. Earlier adapter compile failures are retained under
  `runtime-3v6bklc_` and `runtime-thapqh1c`.
- `build/kernel-update/media-snul1hju/updater-build/manifest.json`: exact candidate,
  predecessor, executable and source hashes; strict freestanding compilation and
  all updater function stack frames <=1024 bytes PASS. The ELF also fits existing
  executable snapshot allocation and mapped-page budgets.
- `build/kernel-update/accept-8_eelmxd/bios/host.log`: actual-code ASan/UBSan,
  six malformed GPT/FAT/menu/readiness/root cases, exact predecessor refusal,
  fifteen partial-write/flush failure points and no implicit mutation retry PASS.
  A write-range oracle admits only the inactive bank, readiness and menu sectors;
  the old kernel/readiness and common bootstrap remain byte-identical. Linux FAT
  audit of the resulting volume PASS.

Actual USB installation uses SMP=1; offline USB update and all installed/recovery
boots use SMP=4, across BIOS and UEFI. Both firmwares passed real predecessor-bound
USB update, byte-exact root preservation, repeat-media refusal, new-kernel execution,
rot13 1.0 → 2.0 from the existing HTTPS repository, clean reboot, retained old-kernel
execution with persisted rot13 2.0, and both authenticated recovery boot sets. Linux
FAT/ext4, byte-exact kernel/package records, accounts and user-setting oracles pass.
Actual updated ESP oracles are under
`build/kernel-update/accept-8_eelmxd/{bios,uefi}/updated-esp-linux-oracle.json`.

The first all-in-one receipt `build/kernel-update/accept-8_eelmxd/manifest.json`
is retained as FAIL: after completing UEFI update/package/reboot, the old harness
stalled at an explicit `Accounts changed; retry login` response on the previous
kernel boot. The harness now retries authentication at most three times, only on
that exact response; it never retries a package/kernel mutation. Remaining UEFI
boot-set gates were completed from a retained post-upgrade regular-file disk:
`build/kernel-update/finish-n70l4eng/manifest.json` PASS, including another candidate
boot/package audit, previous installed kernel and both authenticated recovery sets.
The aggregate receipt above links both campaigns rather than erasing the first
failure. Selected images are unchanged across this test-harness correction.

Earlier iterations are retained, not counted as acceptance. The first USB startup
failed because the minimal archive duplicated built-in volatile directories.
Removing those exposed a second startup failure: the debug-bearing candidate made
the updater executable exceed ordinary snapshot allocation's 2 MiB batch limit.
The producer now removes candidate debug sections and verifies all loadable bytes
against the original. Readiness publication order and Limine's one-based selector
numbering were corrected before the final campaign. Earlier images/receipts under
`build/kernel-update/media-{d20u9_wt,83k7i14_,nx7l5npa,ed5qc62q}` are superseded.

Boundaries: the two kernels share features/user ABI and differ in embedded release
identification. This tests actual kernel replacement/execution, not a new feature
release. USB possession/boot is the trust boundary; no signed repository base
update, bootstrap/base-userland replacement, automatic failed-boot detection is claimed. Physical acceptance is limited to
the user-confirmed kernel-update checks above. Partial-prefix/flush fault injection is a
host I/O model, not a physical device power-loss guarantee. Previous/recovery boot
selection is explicit on disposable image copies, not an automatic rollback test.
