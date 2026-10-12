# Persistent root Phase 5: identified spare NVMe acceptance

2026-10-12. **USER-CONFIRMED PHYSICAL INSTALLATION, USB-FREE BOOT AND CLEAN-SHUTDOWN PERSISTENCE; remaining acceptance gates pending.**
Parent: [installer plan](PERSISTENT_ROOT_INSTALLER_PLAN.md).
Phase 4 evidence: [disposable installer gates](../roadmap/persistent-root-phase4.md).

The user intends to install on a spare SK hynix 256 GB PCIe Gen 3 x4 NVMe
in the Dell Latitude 5590. The exact model, serial, namespace, controller path,
sector geometry and sector count must be recorded before binding a physical
installer image to this drive. Vendor and advertised capacity are insufficient
to identify a particular device. The existing OS drive should be removed for
this first spare-drive run.

## Identity-only inspection

`INSTALLER_INSPECT=1` builds a separate kernel with an identity-only INSTALLCTL
backend. Its only callback performs actual NVMe Identify; the returned identity
is deliberately ineligible for a raw lease. Sector read, sector write, flush and
deploy callbacks are absent. The tool calls ENUM only and displays actual model,
serial, controller/namespace, sector count, sector size and identity digest.
It contains no installation prompts or synthetic target.

This profile does not enable TEST_INSTALLER, does not register the disposable
write backend, and loads no installation payload roles. It is a hardware
identification step, not physical installation or storage-integrity acceptance.
The standalone inspection media must retain build/source hashes and must pass
BIOS/UEFI guest tests with an unchanged writable regular-file NVMe sentinel.

Built image: `bin/fortress-nvme-inspect.iso`; BIOS/UEFI identity display and
unchanged-disk tests pass in `build/root-phase4/inspect-q4iexbqg`. With the spare
fitted and the existing OS NVMe removed, write this ISO to the USB using the
same Rufus ISO-image procedure as the accepted preview, boot via F12 UEFI USB,
photograph the displayed actual identity, then power off. Physical write-profile
construction depends on that identity.

### Dell identity display evidence (2026-10-12)

The user supplied a [photograph of the actual inspection screen](../assets/nvme-inspection-dell5590.jpg).
The screen reports model `PC401 NVMe SK hynix 256GB`, capacity 244198 MiB,
500118192 logical sectors of 512 bytes, PCI controller `0:3:0.0`, namespace 1.
This establishes physical Identify and interface display only. It does not
establish installation, flush durability, or an independent unchanged-disk audit.
The user confirmed the complete 64-character inspector identity digest in text:
`7ccecff73728ce361669061613573c2e7089357c533258d64eb97830791da57a`.
This pins the complete identity, including the padded serial/model, controller,
namespace, geometry, NGUID and EUI64, without guessing ambiguous photo glyphs.

The inspected image SHA-256 is
`f6d0bd695307fa2502467b8b073378de851e29c19c1c4befcf1f37fd328cdccf`.
The inspector hashes identities with flags zero (ineligible for a raw lease).
A future physical admission policy must compare that normalized identity digest
before granting eligibility; the eligible installer identity has different flags
and therefore a different digest for backend lease acquisition. The revised UI
asks for `YES`; the full fresh digest comparison remains internal.

Reproduce in an isolated workspace (do not rebuild normal live media in place):

```sh
make -C "$workspace" -j4 INSTALLER_INSPECT=1 bin/fortress.elf bin/initramfs.tar build/tool-install-inspect.elf
python3 scripts/create_installer_media.py --inspect \
  --kernel "$workspace/bin/fortress.elf" \
  --bootstrap "$workspace/bin/initramfs.tar" \
  --installer "$workspace/build/tool-install-inspect.elf"
python3 scripts/test_installer_inspect.py bin/fortress-nvme-inspect.iso
```

## Proposed first installation

`INSTALLER_PHYSICAL=1` builds a separate profile using the fixed inspector digest
in `src/kernel/install_physical_identity.h`. It cannot be combined with the
disposable or inspection Makefile profiles. Fresh Identify is normalized to
flags zero and compared before any target-sector access; subsequent calls also
retain complete baseline revalidation, source-parent and active-storage exclusion,
terminal I/O failure, and explicit final YES confirmation. Registration requires typed
payload validation and boot-source provenance. An unsupported or different drive
gets no raw lease. There is no runtime enrollment or general physical-write toggle.
Normal live admission and the disposable fw_cfg gate retain their existing behavior.

The host sanitizer policy gate `build/root-phase4/leases-i78fva0j` passes matching
normalized identity, wrong-drive zero-sector-access rejection, replacement,
source-parent, payload/provenance and active-storage exclusions. This is adapter
evidence, not hardware installation evidence. Separate virtual build and exact
Dell-image rejection gates pass: 38 virtual bound-profile cases, 12 real-bound
image rejection cases, and two exact final-image raw USB UI/rejection cases.
See the [Phase 5 evidence](../roadmap/persistent-root-phase5.md).
Prepared image: `bin/fortress-installer-dell5590.img`. On 2026-10-12 the user
explicitly authorized erasure of the identified spare and the stated test layout:
"yes erase everything do whatever you want with it"; the user does not need its
existing contents. Erasure authorization is complete; do not request it again
for this drive and procedure. Physical acceptance remains pending.
Installation procedure: [Dell 5590 guide](../installer-dell5590.md).

Initial layout follows the tested bounded template: 1 MiB BIOS partition,
64 MiB ESP and 256 MiB journaled root; the remaining capacity is unallocated.
This is not an automatic filesystem expansion or secure erase of every sector.
Erasure of this identified spare and this layout are already explicitly
authorized. Do not ask again for this installation procedure.

Acceptance must then establish installation completion, source removal,
independent UEFI boot, login/sudo, password/account/home/configuration persistence,
clean shutdown/restart, authenticated recovery, and independent Linux GPT/FAT/
EXT4, file-hash and permission audits. Capture device volatile-cache/flush
behavior and qualify durability by observed hardware results.
Deliberate power interruption experiments require their own procedure and
authorization. No physical disk-sector writes have been performed by the agent.

2026-10-12: the user confirmed installation, boot without the installer USB and
file persistence across clean shutdown/restart. Do not claim full Phase 5
acceptance from these initial physical checks alone.

The subsequent user-confirmed file/shutdown/reboot check passed. Directory,
permission, nano, ps, sysinfo and top smoke checks also passed manually. Physical
authenticated recovery and independent audits remain open.
