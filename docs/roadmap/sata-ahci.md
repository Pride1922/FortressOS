# SATA/AHCI storage (2026-10-27)

## Scope

FortressOS now has a polling AHCI backend for ATA SATA disks, without a laptop
or vendor whitelist. Register/lifecycle reference:
[Intel AHCI 1.3.1](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/serial-ata-ahci-spec-rev1-3-1.pdf).
Physical acceptance remains pending: the user has no test laptop available.

- PCI class/subclass/interface 01/06/01; up to four controllers/eight SATA
  disks. The shared installer inventory remains bounded to eight targets.
- ATA LBA48, 512-byte or 4096-byte logical sectors, non-NCQ DMA through a
  private 4 KiB bounce page per port; read/write and FLUSH CACHE EXT.
- No advertised flush capability means read-only admission. Installation and
  installed root retain the 512-byte logical-sector profile; 512e qualifies.
- 64-bit and 32-bit DMA controllers; allocation above 4 GiB for a 32-bit
  controller fails before programming DMA. No low-memory allocator was added.

RAID/RST, IDE compatibility mode, ATAPI/optical devices, port multipliers, NCQ,
hotplug enrollment/recovery and automatic SATA power saving are not implemented.
An AHCI driver does not drive a controller exposing RAID/RST.

## Integration and contracts

`ahci.c` retains descriptor/FIS and bounce frames for the entire boot. Command
error, short transfer, timeout, identity change or sticky link-change events
latch failure. There is no automatic retry or DMA-frame reuse/free after
uncertain completion. A subsequent rejected call cannot modify the quarantined
bounce buffer. Changed PCI identity suppresses writes through stale MMIO.
Dedicated UC/NX MMIO mappings carry `PTE_DEVICE` and borrow their physical frames.
An unclassified live link first receives a private FIS page with command
execution disabled, and may receive a bounded boot-time COMRESET. Its signature
must be known before any ATA command is issued. Unsupported ATAPI devices are
then skipped. Classifier pages are reused only after FRE/FR are stopped, or
quarantined on failure; this is not failed-I/O recovery.

Waits use local elapsed calibrated TSC while migration is excluded. A progressing
clock defines deadlines, rather than a CPU-dependent instruction budget;
iteration limits bound unavailable/stagnant clocks. Limits: command five seconds,
engine stop 500 ms, firmware handoff 2.5 seconds, link/signature three seconds,
initial HDD readiness thirty seconds. Firmware partial/slumber/DevSleep policy
is disabled and the link becomes active before ATA commands are issued.

Rank-2 `ahci_io` serializes descriptor/bounce access, including VFS callers under
rank-1 filesystem exclusion. It never sleeps or switches context. Rank-3
`pci_config_ports` makes legacy CF8/CFC address/data pairs atomic across CPUs;
ECAM/VMM operations never run under that leaf lock. Existing ranks and scheduler
ownership are unchanged.

`storage.c` pins a combined boot inventory, NVMe entries first. The existing
installer policy now uses this transport interface. Source-GUID exclusion,
fresh identity, selected-disk confirmation, raw leases, mounted-storage exclusion
and deployment validation remain active. Fixed-fingerprint physical installation
and identity-only inspection retain their legacy NVMe-only admission.

The identity structure remains 192 bytes with reserved bytes zero. NVMe values
and digests are unchanged. For ATA, logical-device ID `nsid` is AHCI port + 1,
serial/model are decoded ATA strings, and `eui64` carries the optional ATA WWN.
BDF plus port keeps same-controller disks distinct. Each installer identity
refresh reissues IDENTIFY and checks pinned geometry/serial/model/WWN.

Ordinary live boots publish `sata0`..`sata7` read-only in block inventory;
existing SATA filesystems are not automatically mounted or formatted.
Installer/installed-root profiles publish their admitted target only. Root
selection scans GPT read-only across both transports, rejects duplicate
PARTUUIDs, then keeps existing GPT/readiness/recovery/EXT4 admission and shutdown
barriers. USB mounting and its exclusions remain intact.

## Automated evidence

Host commands (WSL Ubuntu-24.04):

```
make test-ahci-host
make test-install-control-host
```

- Actual AHCI with labelled PCI/MMIO/DMA/clock adapters passes ASan/UBSan:
  geometry, framing, first/last/invalid ranges, read/write/flush, separate
  multi-port DMA, identity changes, no-flush read-only admission, partial/error/
  timeout quarantine, ownership timeout, inventory truncation, quiescence,
  inherited low-power wake, progressing clocks beyond the old iteration cap,
  frozen clocks, and previously unclassified ATA/ATAPI links.
- Actual dispatcher: SATA-only/NVMe-only/mixed selection, non-target isolation,
  live read-only views and failed inventories PASS.
- Gate G1: `-fstack-usage -Wframe-larger-than=1024 -Werror` passes for AHCI;
  maximum measured frame 848 bytes (`build/ahci-stack.su`).
- Actual legacy PCI paths: four threads/32,000 forced-interleaving transactions
  PASS; no cross-device accesses, PCI RW1C status preserved.
- Existing installer control/indexed leases, replacement/source/storage
  exclusions, fixed physical fingerprint and inspection profiles PASS.
- Actual GPT/root/EXT4/JBD2 admission using a completed installed fixture:
  inventory/namespaces/truncation/flush/replay failures, success and freeze-taint
  **8/8 PASS**. Hardware is mocked in this gate. Host result directory:
  `build/root-phase4/sata-admission-b2vzlp61`.

Guest campaign uses private regular-file fixtures in this isolated source
snapshot, relative to the repository:

`.codex-remote-attachments/ext4-phase9/guest-workspace-bhkoz8uj`

No physical disk was attached. The runner shares existing installer/root
Linux-oracle helpers and validates every QEMU argv.

- SATA-only installation under BIOS and UEFI; source-media removal; SMP=1/4
  login/sudo, file writes/sync/SHA-256, clean shutdown and Linux FAT/EXT4 audits:
  **4 positive boots PASS**. Snapshot result directories `sata-qhb_wodz` and
  `sata-ix4d3kgm` retain these case passes and a subsequent harness assertion
  failure when starting the mixed case.
- Mixed SATA/NVMe: select SATA disk 2, preserve unselected NVMe bytes, remove
  installer media, SMP=1/4 root I/O/shutdown/Linux audits, reject duplicate root
  PARTUUID across transports without writes. BIOS/UEFI PASS: **4 positive boots
  and 2 rejection boots**. Manifests: `sata-wsshv3ss`, `sata-e2if3ug8`.
- Final timeout/power/classification revision: refresh only boot kernel/readiness on copies of
  the installed fixture and repeat BIOS/UEFI x SMP=1/4 root I/O/shutdown/Linux
  audits. SATA **4/4 PASS**, `sata-_1k_m5q_`. Earlier timeout/power/classification
  results are retained in `sata-szw0srm1`, `sata-g5vfj603`, `sata-jvxwv867`. Initial port admission
  changed after installation; command framing and installer authority did not.
- Final-kernel NVMe-only regression: BIOS/UEFI x SMP=1/4 root I/O, shutdown and
  Linux audits **4/4 PASS**, `sata-qfo9h4yt`. Source media is absent. The
  installed filesystem is transport independent.

All snapshot result directories above are below its `build/root-phase4/`.
Representative commands, executed there:

```
python3 scripts/test_sata.py --accel kvm --firmware bios --mixed-only
python3 scripts/test_sata.py --accel kvm --firmware uefi --mixed-only --media build/root-phase4/media-6yb9xry3
python3 scripts/test_sata.py --accel kvm --installed build/root-phase4/sata-wsshv3ss/bios-mixed/target.img
python3 scripts/test_sata.py --accel kvm --installed build/root-phase4/sata-wsshv3ss/bios-mixed/target.img --installed-transport nvme
```

`make test-sata` builds media and runs the full BIOS/UEFI TCG campaign. KVM was
used for recorded installation acceptance; TCG installation is not claimed.

Retained debug failures: a shared build correctly rejected concurrently changed
GUI artifacts in its preservation audit, so verification moved into the snapshot.
The original root harness expected numeric-only `id` output; it now accepts
account-name decoration while still checking numeric UID/GID. Prompt redraw
newlines are tolerated. The mixed helper originally permitted USB source only;
ISO source is now allowed with the same exact disk/argv checks. These harness
failures were not kernel crashes or accepted storage passes.

The NVMe-only regression additionally exposed a real initialization issue:
UEFI can leave an unused optical port at SSTS=0x113, SIG=0xffffffff, TFD=0x7f.
Waiting for its signature before installing a receive FIS buffer blocked the
combined inventory. Initialization now enables receive-only DMA into the private
classifier page before waiting/resetting for that signature. The device is
classified as ATAPI and receives no ATA/packet/storage commands. This follows
the generic initial-FIS path, with no QEMU or vendor whitelist. The observed
failure is retained in snapshot `sata-oaxe1cb5` and subsequent debug directories;
an earlier passing matrix is `sata-rtat0pee`; the final matrix is below.

A later parallel DrvFS-backed NVMe rerun hit the existing NVMe FLUSH timeout
(`sata-mfohpr25`). This remains a failed I/O case; it is not counted as a pass.
The runner now places active data disks in native Linux temporary storage,
as the canonical acceptance runners do, then preserves artifacts under
`guests/` in its result directory. The final native-storage matrix is
`sata-qfo9h4yt`, **4/4 PASS**; NVMe driver deadlines were not changed. Physical
timing/performance implications of the DrvFS failure are not established.

The release and final verified kernel have byte-identical PT_LOAD memory images;
ELF debug paths differ. Whole-file hashes and comparison are recorded in
`build/sata-load-image.json`. Normal ISO and combined USB media were rebuilt;
the USB producer retains its GPT/FAT/payload-hash/EXT4 audits.

## Hardware follow-up

The Latitude 5510 boot self-test fix is committed as `a24a964`. Its physical
retest and SATA physical acceptance both remain pending. When hardware is
available, capture controller mode/PCI ID and ATA identification, check read-only
inventory, then use an explicitly selected disposable disk for installation,
reboot, persistence and clean-shutdown checks. No physical compatibility,
performance or fault-recovery acceptance is claimed.
