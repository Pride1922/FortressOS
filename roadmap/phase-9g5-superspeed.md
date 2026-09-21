# Phase 9G.5 — USB Topology Expansion

Status: 9G.5a and 9G.5b COMPLETE; 9G.5c/d/e open. See `AGENTS.md` status
table for current summary; this file holds the detailed implementation notes
and evidence.

SuperSpeed support, multiple-controller enumeration, and hub support.
Sequenced as three sub-projects, each with its own acceptance criteria and
its own hardware target.

## Phase 9G.5a — Multiple xHCI Controllers (COMPLETE, 2026-09-19)

FortressOS now enumerates and initializes every xHCI controller the
platform exposes, instead of stopping at the first match. On machines with a
single controller the behaviour is unchanged; on machines with two, a
mass-storage device on either controller is reachable and mountable.

Delivered in three commits:

- **Commit 1 — `xhci_controller_t` struct and array.** The six
  controller-scoped statics in `xhci.c` (`s_rings_io`, `s_dma`, `s_dev_dma`,
  `s_bot_rings`, `s_flush_error`, `g_dump_record`) moved into a single
  `xhci_controller_t` type, held in `s_controllers[XHCI_MAX_CONTROLLERS]`.
  Only index 0 was used; no PCI collection, no loop, no per-controller
  initialization. External signatures, call graph, and log strings
  unchanged.
- **Commit 2 — bounded enumeration in PCI discovery.**
  `pci_find_all_devices()` added to `pci.c`, iterating the same topology as
  the existing `pci_find_device()` but collecting all matches up to a
  caller-supplied maximum. `pci_report_xhci()` now reports every controller
  as `xHCI controller N/M: BDF=..., vendor=..., device=...`. Only
  controller 1 is still initialized. Test runners updated to assert the new
  log format.
- **Commit 3 — per-controller init loop and active-device selection.**
  `xhci_boot_probe()` now enumerates all controllers and runs the existing
  init sequence for each via a new private helper
  `xhci_init_one_controller()`. A new file-scope pointer
  `s_active_usb_controller` tracks the controller whose mass-storage device
  is currently registered as `sda`. Per-controller MMIO windows replace the
  single shared `XHCI_PROBE_VIRT` mapping. A private
  `xhci_dump_controller_state()` enables per-controller diagnostics without
  routing through the active pointer. Only the first successfully
  registered device becomes active; subsequent mass-storage devices are
  logged as
  `Mass-storage device found on controller N, but only one active device is supported`
  and left unregistered.

### Verification

- **QEMU:** dual-controller boot with a single stick on the second
  controller; all five USB suites pass under BIOS and UEFI; three-boot
  persistence passes with zero filesystem errors.
- **Dell Latitude 5590 (one xHCI controller at `0000:00:14.0`):** behaviour
  identical to commit 2; single `1/1` line; the rest of the boot log
  unchanged; Kingston mounts RW with `ASSUMED_WRITE_THROUGH`.
- **Dell Latitude 5530 (two xHCI controllers at `0000:00:14.0` and
  `0000:00:14.2`):** both controllers enumerated and initialized. With the
  Kingston plugged into a controller-2 port, the device is found on
  controller 2, registered as `sda`, and mounted read-write at `/mnt`.
  SuperSpeed device on Port 0x10 correctly skipped as unsupported. USB hub
  on Port 0x1 correctly rejected as class 0x09. Prior to commit 3,
  controller 2 was invisible and `/mnt` was never mounted.

**Not established:** the guard path for a second simultaneously attached
mass-storage device. The guard logic (`s_active_usb_controller` check
before registration) is in the code and structurally verified, but a
two-stick boot was not photographed with the guard message visible in the
log.

**Deferred:** the "no scrollback on boot" limitation of the framebuffer
console means the head of the boot log (the `1/2` and `2/2` lines) is not
photographable from hardware without a kernel-log buffer. This is
independent of 9G.5a and tracked as a future work item (`dmesg`-style log
capture).

## Phase 9G.5b — SuperSpeed enumeration and data transfer (COMPLETE, 2026-09-20)

SuperSpeed (USB 3.x) devices on USB 3.0 ports now enumerate fully, complete
BOT transactions, register as block devices, and mount their ext2
partitions read-write on real hardware.

### Root cause of the previous SuperSpeed transfer failure

The configuration descriptor walk in `xhci_dev.c` advanced `off` only
inside the endpoint branch, so the loop hung on the first non-endpoint
descriptor after the BOT interface. When the hang was fixed, the walk
continued into the same interface's UAS alternate setting (protocol 0x62)
and its endpoints overwrote the BOT endpoints (0x81 IN, 0x02 OUT) recorded
earlier. The driver then sent BOT CBWs to UAS endpoints; the device stalled
on the wire; the first transfer returned completion code 0x4 (USB
Transaction Error).

### Two fixes

- `off += len` moved out of the endpoint branch so it advances on every
  descriptor, not just endpoints.
- `found_bot_if` now clears on every non-BOT interface, so a later
  alternate setting cannot overwrite the endpoints recorded for the BOT
  interface.

### Verified on Dell Latitude 5590

SanDisk USB 3.2 Gen 1 (VID 0x0781, PID 0x5588) on Port 0x12:

- Device enumerates as SuperSpeed (5 Gbps).
- Configuration descriptor walk shows Interface 0 Alternate 0 (proto 0x50,
  BOT) with endpoints 0x81 IN and 0x02 OUT, followed by Interface 0
  Alternate 1 (proto 0x62, UAS) with the same endpoint addresses plus two
  others. Only the BOT endpoints are retained.
- SCSI INQUIRY: "SanDisk" / "3.2 Gen 1".
- READ CAPACITY: 241,385,472 sectors × 512 bytes = 117.86 GiB.
- GPT parsed, ext2 partition mounted read-write at /mnt.
- MODE SENSE(6) page 0x08: WCE=1.
- SYNCHRONIZE CACHE test: passed.
- Classification: SYNC_BACKED.
- Mount mode: read-write.

This closes the strong-durability gap noted in `phase-9g4-usb-durability.md`
— `SYNC_BACKED` is now verified on physical hardware, not QEMU-only.

### Not verified

- The BOT stall recovery path is not yet invoked from the transfer path. On
  a device that stalls `SYNCHRONIZE CACHE` or another command, the driver
  would currently classify the device as `READ_ONLY` rather than treating
  the stall as a command failure. This does not affect the SanDisk, which
  does not stall on the commands the driver issues. Tracked as a follow-up.
- Other SuperSpeed devices. SuperSpeedPlus (10 Gbps). Other host
  controllers.

### Debugging update (2026-09-20)

Fixed a descriptor-walk regression in the working SuperSpeed changes:
`off += len` was nested inside the bulk-endpoint branch, so parsing never
advanced past the configuration descriptor. The host enumeration runner
reproduced a 15-second timeout before the fix. After moving advancement
outside the branch, `wsl -d Ubuntu-24.04 -- make test-usb-descriptors`
passed the ASan/UBSan host suite (including a video-class webcam rejection
with successful mocked Disable Slot and no SET_CONFIGURATION) and QEMU
BIOS/UEFI present/absent checks with interactive shell startup.
`wsl -d Ubuntu-24.04 -- make` rebuilt and verified `bin/fortress.img`. These
checks do not establish Dell webcam recovery or SanDisk SuperSpeed bulk
acceptance beyond what's recorded above; physical retesting remains
required for any further change to this path.

## Open sub-projects

### 9G.5c — Strong durability on the SanDisk

Once the SanDisk enumerates, its MODE SENSE and SYNCHRONIZE CACHE behavior
can be read. Success criterion is a classification other than
`ASSUMED_WRITE_THROUGH` — either `WRITE_THROUGH` (explicit `WCE=0`) or
`SYNC_BACKED` (working flush). **Note: this was effectively achieved by
9G.5b** — the SanDisk classified `SYNC_BACKED` on first physical test. If
this needs re-verification as its own checkpoint (e.g. on a different
SuperSpeed device), record that separately; don't assume 9G.5b's result
generalizes to other USB 3.x devices.

### 9G.5d — Persistence on the SanDisk

Same three-boot test as the Kingston, with `e2fsck` clean after each clean
shutdown. This closes the "verified on two independent devices" claim for
the persistence path. Not yet done.

### 9G.5e — Hubs (deferred)

USB 2.0 and USB 3.x hub support, recursive enumeration, downstream port
power sequencing. No hot-plug. Deferred until there is a specific device
reachable only through a hub. A USB-C docking hub is available for testing
when the sub-project begins.

## Hardware facts recorded during this phase

- **H10:** SanDisk USB 3.2 Gen 1 (VID 0x0781, PID 0x5588). Enumerates as a
  SuperSpeed device on Port 0x12 (Dell 5590). Reports two alternates on
  Interface 0: Alternate 0 with protocol 0x50 (BOT) and endpoints 0x81
  IN / 0x02 OUT; Alternate 1 with protocol 0x62 (UAS) and endpoints
  0x81 / 0x02 / 0x83 / 0x04. Only the BOT alternate's endpoints are used.
  `MODE SENSE(6)` page 0x08 reports `WCE=1`; `SYNCHRONIZE CACHE` succeeds.
  Classified `SYNC_BACKED`. Mounts read-write at `/mnt`.
- **H12:** Dell Latitude 5530 presents **two** xHCI controllers:
  `0000:00:14.0` and `0000:00:14.2`, both matching class 0x0C subclass 0x03
  progif 0x30. Verified via 9G.5a: both controllers initialized; a Kingston
  USB 2.0 stick is reachable on either controller depending on physical
  port; the first controller has no attached devices in the default
  configuration. Do not assume controller index maps to physical port
  group.
