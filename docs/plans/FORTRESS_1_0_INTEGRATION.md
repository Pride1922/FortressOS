# Fortress 1.0 integration and first hardware package trial

2026-10-25. User direction: the work from the recent development period becomes
one integrated FortressOS release, called **Fortress 1.0**, including package
management as a normal system feature. First hardware target: the Dell Latitude
5590 with its spare NVMe. This is a release target, not a released version or a
claim that the current normal image contains the accepted package backend.

## Starting point and development workflow

Package checkpoint `0b23223` preserves `9098755` and the intervening ABI work.
Its bounded private CLI/source campaign passes eight normal boots, four COMMIT
cuts and six refusals, with runtime and preservation PASS. See the
[exact acceptance audit](../roadmap/pkg-private-cli-source-acceptance.md).

The shared tree is unfrozen. Preserve subsequent commits and concurrent edits;
do not reset it to this checkpoint. Use isolated worktrees or retained source
snapshots for implementation and tests. Record the exact compiled source and
artifact hashes, and report concurrent shared-tree changes separately. Freeze
the candidate's inputs rather than requiring all development to stop.

The 1.0 feature inventory includes existing kernel/SMP/memory work, storage and
installed roots, accounts/permissions, shell/jobs/signals, networking and DHCP,
USB/hubs/HID/RTL8153, Wi-Fi, SSH server/client, TLS/HTTPS, Watchtower/logging,
tools/compression, poll, HTTP/Telnet, pthreads, crash dumps and the ABI contract.
Use AGENTS.md and subsystem evidence to reconcile implementation, default build
dependencies, packaged commands/configuration, and actual acceptance for each.
Presence in an archive is not runtime or physical acceptance. Deferred projects
remain deferred unless separately added to the release scope.

Initial read-only artifact inventory is retained as
`fortress1-inventory-20261025-174803-357454/manifest.json` under
`.codex-remote-attachments/`. The existing bootstrap has 130 regular files.
The selected shell, network configuration/tools, Wi-Fi, SSH, services, logging,
TLS anchor, compression and HTTP/Telnet paths are present; `bin/pkg` is absent,
and `bin/pkg-boot` is absent. Archive contents were unchanged across inspection.
This checks membership only: build/source equivalence, driver/kernel inclusion,
runtime behavior and release acceptance are not established by that inventory.

## Confirmed package integration gaps

- The accepted backend is composed into copied kernels by Python overlays;
  reusable implementation still lives partly under tests/.
- The normal Makefile has package host/private test targets but does not put
  the accepted mutating package CLI/backend into the ordinary initramfs recipe.
- The private profile requires a QEMU fw_cfg local-MAC key and fixture admission.
- It hard-codes a unique 64 MiB NVMe, virtual GPT identity, partition geometry,
  and an exact four-command /bin inventory. These do not describe the Dell's
  installed root.
- It starts /bin/pkg-boot automatically. Real interactive login/SSH package
  campaigns, normal mutable account/log writers and running installed
  application execution have not been accepted by this package checkpoint.

Changing a boot-mode string or flashing the private acceptance image does not
close these gaps. Preserve the existing private fixtures and their admission
restrictions as regression controls.

## Implementation order and exit evidence

1. **Normal source integration, disabled by default.** Extract reusable codecs,
   owner/session logic, executor and CLI transport into reviewed normal modules.
   Separate fixture identities, signing seeds, traps and fault controls from
   those modules. Trace concrete callers and preserve lock ranks, ownership,
   bounded resources and admission-before-replay/publication. Define and review
   the normal versioned package ABI before allocating its syscall number;
   private syscall numbers 200/203 do not become a production ABI by copying
   them. Run equivalence/host tests and private boot regressions against the
   actual extracted implementation. Existing default boot remains unchanged.
2. **Integrated interactive candidate on disposable roots.** Wire /bin/pkg to
   the normal modules under explicit enrolled-root admission. Use real login,
   sudo and SSH; cover status/install/remove/recover, unprivileged rejection,
   competing commands, credential loss, disconnects, signals, open/running
   executables, low space, ordinary account/log activity and reboot recovery.
   Preserve the initial one-source, one-executable/up-to-fifteen-documents
   bounds. Local/download ingestion, multiple packages and upgrades require
   their own implementation and acceptance; they are not silently enabled.
3. **Hardware admission and TEST enrollment design.** Bind actual device,
   partition/filesystem/installation identities, resource profile and measured
   base inventory. Define hardware local-MAC-key delivery, external minimum
   sequence/hash management, selector integrity, older-kernel exclusion and
   independent recovery. Signing private keys remain external. No QEMU fixture
   key, UUID, block map, permissive fallback or unreviewed key-in-media shortcut
   becomes production authority. A MAC stored with the disk is not whole-disk
   rollback protection. Validate this hardware-shaped policy first using
   disposable QEMU devices with different geometries and wrong-target controls.
4. **Dell read-only inspection, then a bounded write trial.** Inspect fresh
   identity, GPT, filesystem profile/capacity and enrollment absence/state.
   Preserve the existing installation and establish backup/recovery before
   enrollment. Review the exact image, target identity, key custody, changed
   on-disk records and recovery procedure before constructing a physical write
   candidate. Trial normal install/remove, application execution, clean sync,
   shutdown and reboot, with independent offline content/permission/fsck audit.
   Deliberate power cuts are a later separately specified experiment.
5. **Fortress 1.0 candidate and release acceptance.** Build an isolated candidate
   with explicit feature/command/configuration/license manifests. Audit default
   builds for private package probes, fault-control dispatch and fixture
   authorities; make an explicit decision about engineering diagnostics already
   present in normal images. Run combined BIOS/UEFI/SMP and installed-root
   regressions, followed by the selected Dell campaign. Publish a release only
   after reconciling the four package production gates and the recorded hardware
   limitations. Test authority does not become production trust provisioning.

## Selected hardware and current boundary

The recorded spare is PC401 NVMe SK hynix 256GB, namespace 1, 500118192 sectors
of 512 bytes, with normalized inspection digest
`7ccecff73728ce361669061613573c2e7089357c533258d64eb97830791da57a`.
See [the existing physical procedure and evidence](PERSISTENT_ROOT_PHASE5.md).
Treat this as prior evidence to compare with fresh inspection, not proof of
the current disk contents or authorization to enroll an arbitrary root.

The earlier erasure authorization for that identified spare and its documented
installer layout remains recorded; do not ask for it again for that same
procedure. Package enrollment and its changed metadata/key/recovery policy must
be defined explicitly before modifying the installed system. General hardware
testing intent does not choose an unidentified disk or authorize power cuts.

This planning checkpoint changes no boot, ABI, storage gates, trust or mutation
behavior. Production package enablement, production provisioning, canonical
release-media builds, pushes and physical writes remain disabled during the
preparation stages. The next implementation batch is item 1, followed by item 2;
the first physical step is identity/read-only inspection.
