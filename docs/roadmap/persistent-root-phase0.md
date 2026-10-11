# Persistent Root / NVMe Installer — Phase 0 Evidence

2026-10-10. **Phase 0 contracts, code audit and disposable fixture gates complete.**
No persistent-root kernel implementation or native installer is claimed.
Design: [Phase 0 contracts](../plans/PERSISTENT_ROOT_PHASE0.md).

## Authorized scope and decisions

The user authorized Phase 0 after reviewing NVMe-only scope, user-chosen required
recovery passphrase and per-install ESP provisioning. They additionally requested
separate live and installer builds. Phase 0 preserves production defaults and
implements only fixture tooling plus architecture/evidence documents.

Dell 5590: user confirmed F2 "Add Boot Option" with a file chooser and F12 boot
selection. Physical procedure will use that firmware UI to select
EFI/BOOT/BOOTX64.EFI on the spare NVMe. SetVariable is outside the implementation
critical path for this supported workflow. Actual installed-NVMe firmware boot
remains Phase 5 physical acceptance, not a claim from menu availability.
The OS NVMe will be physically removed. SK hynix SC311 SATA 128 GB was identified
as ineligible for NVMe-only installation; the user subsequently reported another
NVMe. Its model, serial, capacity and logical-sector geometry are still pending.
No physical disk was read or written during this work.

## Executed gate

From PowerShell in C:\Sources\FortressOS:

```powershell
wsl -d Ubuntu-24.04 -- bash -lc "cd /mnt/c/Sources/FortressOS && make test-root-phase0"
```

Final command exited 0. Evidence directory:
build/root-phase0/fixture-2a4jqgzx. Its manifest.json contains source/artifact
SHA-256s, exact host commands, disk GUIDs/layouts and boot results. Each firmware
has an exact argv file, serial log and stderr log. No source/installer disk was
attached. UEFI used a fresh copy of system OVMF variables, not a seeded host boot
entry. Source kernel/initramfs were snapshotted; the final make invocation also
regenerated the normal live initramfs through its existing dependency rule.

Source commit: 4c0720d76d21f1d89e745de005df0079221f544f, with Phase 0 working-tree
changes. Tools: GCC 13.3.0, e2fsprogs 1.47.0, QEMU 8.2.2, fsck.fat 4.2; full
diagnostics in tools.log, root-audit.log and esp-audit.log.

| Gate | Result / exact scope |
| --- | --- |
| Restricted journaled root profile | 256 MiB, 4 KiB filesystem blocks, 256-byte inodes, 4096 inodes, 4 MiB journal; masks 0x4 / 0x42 / 0x403 |
| Actual EXT4/VFS ASan/UBSan | 512 and 4096 sector adapters PASS; mounted at existing /mnt, owned lookup/metadata, exact marker read, create/write, sync/freeze and teardown |
| Independent post-mutation audit | e2fsck -fn exit 0 for both actual-code output copies; Linux debugfs exact probe bytes PASS |
| Initial profile / inode metadata | Linux clean fsck, executable presence, shadow 0600 and operator home UID/GID 1000 verified |
| Recovery personalization fixture | Two ephemeral test inputs, distinct salts/hashes, correct/incorrect verification via Linux libcrypt; hash-only USTAR copied to ESP and extracted byte-identically |
| FAT32 boot artifacts | BOOTX64.EFI byte-identical to pinned Limine loader; fsck.fat -n exit 0; directory/archive audit PASS |
| GPT / BIOS stage layout | 384 MiB and 1 GiB whole disks, target-end backup, equal CRC-checked 128-entry arrays, explicit slot-3 Limine deployment; root/ESP bytes unchanged by deployment |
| BIOS NVMe-only loader | kernel handshake and target TarFS initialization PASS; image hash unchanged |
| UEFI NVMe-only loader | fresh OVMF vars, kernel handshake and target TarFS initialization PASS; image hash unchanged |

Final kernel SHA-256:
4c8c7f6d5f85726a8a9a54130118bf7b2231b80e70134ec3e935514ebabce3a7.
Final root template SHA-256:
9db542f963b32bd30f567e05eddd3a5e2737332c542da66f24c2f7333351486b.
Filesystem UUID and randomized target GUIDs are recorded in the manifest; the
fixture procedure is reproducible, but random identities and FAT timestamps
mean outputs are not promised to be bit-reproducible.

## Retained failed attempts and corrections

- fixture-zj4r5x5g: shadow mode audit failed because host DrvFS chmod did not set
  the intended authoritative EXT4 inode mode. Fixture generation now sets modes
  and ownership explicitly with Linux debugfs, including filesystem root.
- fixture-68tqihtt: BIOS loaded the kernel and TarFS, then the legacy QEMU storage
  suite rejected the 384 MiB disk because it expects exactly 32 MiB. The timeout
  and serial log are retained. Loader smoke now stops after TarFS initialization,
  before storage tests; no kernel fixture gate was weakened or changed.
- fixture-n2z5qu4v: actual filesystem/fsck passed, but the independent byte oracle
  found the test probe had omitted its final newline due to a hardcoded length.
  The host test now writes sizeof(payload)-1; the final Linux oracle passed.
- Intermediate successful firmware and single-adapter runs remain retained.
  Only fixture-2a4jqgzx is the final combined gate for the current tooling.

## Boundaries and next implementation dependencies

The fixture mounts the existing filesystem at /mnt in a host harness; it does not
switch root, exercise IRQs, grant installed set-ID policy or execute Ring 3 login
from EXT4. The recovery archive contains a fixture record with magic/hash only;
the complete installation-bound record and authentication program are specified
but not implemented. Ephemeral generated inputs are test data, not a production
installer passphrase policy. No known default credential is shipped.

Boot smoke establishes the NVMe firmware/loader/module path only, not a complete
installed boot. It deliberately stops before the existing geometry-specific
QEMU storage suite. The BIOS stage utility and mtools are host fixture tools;
native deployment requires the specified capsule and bounded FAT slot editor.

Readiness modules, full NVMe identity/namespace rejection, exclusive installer
leases, root attachment, EXT4 coherent exec snapshots, mutable account loading,
persistent homes, account redo/snapshot service and root sync routing are future
phase implementations with explicit contracts and test obligations. No general
128 GB root capacity or arbitrary NVMe/UEFI firmware support is established.
The 256 MiB first root profile leaves larger disks mostly unallocated and must
be clearly disclosed in installer UX.

Phase 1 may proceed against the detailed contract. No physical flash, erasure,
installed-root production rollout or protected runtime contract has occurred.
