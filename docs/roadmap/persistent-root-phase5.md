# Persistent root Phase 5: Dell spare-drive acceptance

2026-10-12. **User-confirmed physical installation and first USB-free boot; persistence accepted; recovery and independent audit pending.**
Contract: [Phase 5 plan](../plans/PERSISTENT_ROOT_PHASE5.md).
Phase 4 remains [complete for disposable fixtures](persistent-root-phase4.md).

The Dell 5590 inspector displayed the actual PC401 NVMe SK hynix 256GB,
500118192 sectors of 512 bytes, controller `0:3:0.0`, namespace 1.
The [user photograph](../assets/nvme-inspection-dell5590.jpg) establishes Identify
and display only. The user subsequently confirmed the inspector digest in text:
`7ccecff73728ce361669061613573c2e7089357c533258d64eb97830791da57a`.

The separate physical profile compares fresh Identify with flags normalized to
zero against this fixed digest before target-sector access. It retains typed
source/payload admission, source-parent exclusion, singleton NVMe inventory,
active-storage exclusion, baseline identity revalidation, finite I/O failure,
and explicit target/password/whole-disk confirmation. The ordinary live build
and disposable fw_cfg admission are unchanged. No agent physical disk writes.

## Preparation evidence

| Gate | Evidence | Result |
| --- | --- | --- |
| Actual transport/control with host adapters, ASan/UBSan | `build/root-phase4/leases-i78fva0j` | PASS; matching normalized identity, wrong-drive zero sector access, replacement, source, payload/provenance and storage exclusion |
| Actual text installer helpers, ASan/UBSan | `build/root-phase4/cli-4h38vwo9` | PASS; cancellation, credential separation, confirmation, no echo, wipes and lease cleanup |
| BIOS/UEFI virtual bound-profile acceptance | `build/root-phase4/bound-x34a988d` using `media-83fwxk8b` | 38/38 PASS; raw USB installation, seven input rejection paths per firmware, independent Linux GPT/FAT/EXT4/account audit, three source-free boots, persistence/sudo/shutdown/restart, authenticated recovery, wrong identity/geometry/inventory/source/capacity rejection |
| Dell-bound image rejects virtual hardware | `build/root-phase4/bound-i4587tfr` using `media-yfawliif` | 12/12 PASS; BIOS/UEFI ISO/raw USB and inventory/geometry/source/capacity controls, all disposable media unchanged |
| Exact final image raw USB boot and physical UI | `build/root-phase4/bound-jrjx2lx4` using `media-t3uaw870` | 2/2 PASS; BIOS/UEFI physical banner and wrong-drive rejection, source and writable target unchanged |

Virtual acceptance uses a separate compiled digest for the identified QEMU
384 MiB namespace with serial `SPARE-INSPECT-TEST`, controller `0:0:2.0`:
`a465886eab4c2d302aee9d774a864189a79e808d356b97485f9c993329629bc6`.
All loaded ELF segments of the real and virtual kernels compare identically
after substituting these 32 fingerprint bytes. Separate-workspace debug metadata
differs. The virtual fingerprint is never included in the Dell release image.

The final real image is repackaged in `media-t3uaw870` with a physical-profile
banner ("identified spare NVMe only") and bound-target rejection wording.
These final UI strings differ from the initial virtual acceptance tool; installer
mechanics and loaded kernel code are unchanged. Exact final-image raw USB boot
and wrong-drive/no-write checks pass. The first physical-test release was
`bin/fortress-installer-dell5590.img` (520 MiB), SHA-256:
`63a6a72a297cd408c071917c9c4687d2750c60618ed8517d033622bcde68ce47`.
Its receipt is preserved as `bin/fortress-installer-dell5590-first-physical-manifest.json`.
The current revised image has a separate receipt at
`bin/fortress-installer-dell5590-manifest.json`.
Normal live ISO and raw image hashes retain their Phase 4 baseline values.

## Remaining physical gates

The user explicitly authorized erasure of this exact spare and the reviewed
bounded layout on 2026-10-12. No further erasure approval is required for this
installation. Layout: 1 MiB BIOS, 64 MiB ESP, 256 MiB root; remainder unallocated.
This replaces existing partitions and is not secure erasure of all sectors.
Use the raw GPT image rather than ISO extraction so boot-source GPT provenance
is preserved. Installation completion, USB-free boot and clean-shutdown file
persistence are user-confirmed below. Broader account/configuration/history
persistence, authenticated recovery, independent Linux metadata/file audits and
cache/flush qualification remain open. Full physical durability acceptance is
not claimed from the clean-shutdown file check alone.
The [physical installation guide](../installer-dell5590.md) is now ready for the user.

### First physical installation and USB-free boot — 2026-10-12

The user reported: “its installed, the installer itself works, i booted without
the usb stick!!!!”. This establishes user-confirmed installation and first boot
from the authorized Dell 5590 spare with the previous installer image. It is not
yet a persistence, independent Linux audit or authenticated recovery result.

The user also reported poor usability: hidden input, technical disk details,
12-character minimum, fixed operator name, long erase digest, and restarts after
input errors. The revised installer addresses those issues; its own guest gates
and image receipt are recorded separately. No reinstallation is required to
continue acceptance of the already installed spare.

### Physical persistence and tool smoke acceptance — 2026-10-12

The user created `install-check.txt` containing `dell5590-first-install`, ran
`sync` and `sudo shutdown`, restarted without USB and confirmed that `cat
install-check.txt` printed the same text. This establishes user-confirmed
clean-shutdown persistence and another boot from the installed spare.

The user also reported successful directory, permission, nano, ps, sysinfo and
top checks. These are manual smoke results; exact command outputs and independent
filesystem audits were not supplied. Authenticated physical recovery and Linux
GPT/filesystem audits remain pending. No further erasure authorization is needed.

### Revised installer release — 2026-10-12

The usability changes and evidence are detailed in the [Phase 4 revision](persistent-root-phase4.md).
Current image: `bin/fortress-installer-dell5590.img`, 545259520 bytes (520 MiB),
SHA-256 `ca0dca21ce6adc2d3741a1ec6165dc5ef186cdeb9bc8973c3a24a670da6cad83`. Producer: `build/root-phase4/media-a4d77j2b`.
The exact final image passes both raw USB boot/wrong-drive checks in
`build/root-phase4/bound-f4l1mucn`; writable virtual targets remain unchanged.
Loaded kernel segments match the virtual acceptance kernel except the fixed
32-byte binding. The final cosmetic capacity formatter is covered by the actual
CLI host gate; full guest input/install/boot/recovery acceptance used the same
protocol and payload before that formatting-only change.

The user's successful physical installation and persistence used the first
release (`63a6a72a297cd408c071917c9c4687d2750c60618ed8517d033622bcde68ce47`), not this revised image.
The current installation remains valid and does not need to be replaced for
continued physical recovery/audit acceptance. The revised release remains
restricted to the same authorized spare; agent physical writes remain zero.
