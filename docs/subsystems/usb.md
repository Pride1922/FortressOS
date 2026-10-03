# USB Subsystem Annex (Phase 9G)

This annex documents the current status, hardware facts, verification evidence, and scope boundaries of the FortressOS USB subsystem. Binding contracts, locking rules (L1–L4), memory ownership (M1, M4), and block device standards live in [`AGENTS.md`](../../AGENTS.md) (§4 and §9). For driver addition guidelines, see [`AGENTS.md` §7.7](../../AGENTS.md#77-a-usb-mass-storage-driver-addition).

---

## 1. Subsystem Status and Overview

| Subsystem | Status | Detail |
| --- | --- | --- |
| **Phase 9G.5b SuperSpeed enumeration & BOT transport** | **COMPLETE** (2026-09-20) | SuperSpeed (USB 3.x) mass storage works end-to-end on physical hardware (Dell 5590 + SanDisk USB 3.2 Gen 1, port and multi-controller variants). Full detail and evidence: [`docs/roadmap/phase-9g5-superspeed.md`](../roadmap/phase-9g5-superspeed.md). |
| **Phase 9G.1 xHCI Controller & Enumeration** | **COMPLETE** (2026-09-19) | PCI discovery, MMIO/reset, Command/Event rings, root ports, device addressing & configuration; verified on QEMU and bare-metal Dell 5590. Full detail: [`docs/roadmap/phase-9g1-xhci-enumeration.md`](../roadmap/phase-9g1-xhci-enumeration.md). |
| **Phase 9G.2 Read-only USB block device** | **COMPLETE** (2026-09-19) | Bulk-Only Transport, SCSI engine, block device registration, GPT partition parsing; verified on QEMU and bare-metal Dell 5590. Full detail: [`docs/roadmap/phase-9g2-usb-block.md`](../roadmap/phase-9g2-usb-block.md). |
| **Phase 9G.3 Production `/mnt` mount** | **COMPLETE** (2026-09-19) | Bounded cmdline parsing, PARTUUID-based partition selection, read-only production mount; verified on QEMU and Dell 5590 hardware. Full detail: [`docs/roadmap/phase-9g3-usb-mount.md`](../roadmap/phase-9g3-usb-mount.md). |
| **Phase 9G.4 USB writable persistence & durability classification** | **COMPLETE** (2026-09-19) | BOT stall recovery, four-tier durability classification, explicit writable opt-in; `/mnt` read-write persistence confirmed on physical USB. Full detail: [`docs/roadmap/phase-9g4-usb-durability.md`](../roadmap/phase-9g4-usb-durability.md). |

---

## 2. Phase 9G Implementation Handoff

Phase 9G is complete through 9G.5b. The staged plan, per-stage acceptance evidence, and hardware observations are recorded in [`docs/roadmap/README.md`](../roadmap/README.md) (see the 9G phase files). The driver handles USB 2.0 and USB 3.x direct-attached mass storage on any enumerated xHCI controller; USB 3.x devices enumerate as SuperSpeed and complete BOT transport. Hubs, hot-plug, UAS, and non-mass-storage classes remain out of scope. See the "What 9G does NOT do" list below.

### What 9G does NOT do

2026-10-03 transport follow-up: bounded synchronous runs use the existing 4 KiB
BOT bounce page and short completions use fine PIT polling. BOT/GPT sanitizer
tests, EXT4 BIOS/UEFI SMP=1/4 persistence and ext2 USB persistence regressions
PASS. Existing durability, locking and quarantine contracts remain intact;
Dell manual retest PASS: 6–7 seconds/MiB, 16 MiB in 1m52s, matching hashes, reboot persistence, responsive quiet background commands and clean Linux Mint fsck (exit 0). See [performance evidence](../roadmap/ext4-usb-performance.md).

- No USB 3.x hub support (SuperSpeed devices on root ports work; devices behind a SuperSpeed hub do not). No SuperSpeedPlus (10 Gbps) verification. No streams.
- No external USB hubs; xHCI root-port management remains required.
- No hot-plug enumeration, reconnection or removal recovery beyond safe failure.
- No UAS, USB keyboards, mice, audio or other non-mass-storage classes.
- No USB power management, suspend or resume.
- No multiple-LUN support: access LUN 0 only.
- No recovery/re-enumeration after a runtime host-controller reset. Initial reset and bounded BOT transport recovery are still required; controller failure leaves storage unavailable until reboot, with DMA safely contained.

### Explicit USB selection and writable opt-in

Boot arguments are `usb_data=PARTUUID=<unique-partition-guid>` and `usb_data_mode=ro|rw` (default `ro`). Parsing is bounded, and the kernel keeps its own copy of the boot command line under the existing boot-metadata contract. The image builder reports the generated data partition GUID; an explicit writable boot-menu entry displays that target and passes both arguments. The default entry is read-only.

"User-selected test USB" means the user deliberately chooses that configured target and writable entry. Require exactly one matching partition on a supported USB BOT device. A matching filesystem label (`FORTRESS_DATA`), GPT name (`Fortress Persistent Data`) or marker file alone is never write authorization. No first-disk or first-matching-label fallback. Duplicate GUIDs (including two clones of the same image), missing/malformed selection, unsupported media or failed eligibility checks must never produce a writable mount. Without a valid unique target, leave `/mnt` unmounted and explain why; with a selected target but failed RW eligibility, allow only the documented read-only fallback.

The explicit opt-in does not override GPT ambiguity/degraded-mode policy, ext2 validation, write/flush capability checks or the internal NVMe exclusion. Log the selected USB identity, partition GUID and actual mount mode. Tests cover no selection, RO default, explicit RW, wrong GUID, duplicate clones and failed flush capability. Do not auto-enable RW merely because an image was flashed.

### Bounded first implementation

- USB 2.0 Full-Speed/High-Speed devices on xHCI USB 2.0 ports only. Identify port protocol capabilities rather than assuming port numbers. SuperSpeed slots, streams, USB 3.x port state machines and low-speed storage are out of scope. Test media must actually negotiate a supported speed.
- Devices must be attached at initialization. No hot-plug discovery or reconnection support; reject hubs (class 0x09) with `hub not supported`. Still consume/acknowledge port-status events while polling so they cannot clog the event ring. Unexpected removal must fail safely, not hang or release DMA memory still owned by the controller.
- Bootstrap endpoint zero according to negotiated speed, read the first **8 bytes** of the device descriptor (bMaxPacketSize0 is at byte offset 7), validate/update endpoint-zero packet size and fetch full descriptors. A one-byte read cannot supply bMaxPacketSize0. Validate device and interface descriptors; class may be declared on the interface. Reference: [USB-IF USB 2.0 specification](https://www.usb.org/document-library/usb-20-specification).
- 9G.2 commands: INQUIRY (0x12), TEST UNIT READY (0x00), READ CAPACITY(10) (0x25), READ(10) (0x28), REQUEST SENSE (0x03). Bound retries and implement BOT stall/reset recovery. Initially support LUN 0 only; document GET MAX LUN handling and reject unsupported configurations. Reject UAS explicitly. Defer READ(12/16), MODE SENSE(6/10), REPORT LUNS and larger-capacity command sets; reject READ CAPACITY(10)'s overflow sentinel and unrepresentable LBAs. WRITE(10) (0x2a) and SYNCHRONIZE CACHE(10) (0x35) belong to 9G.4.
- In 9G.2, bulk completion **polls the event ring with a bounded timeout**: no USB completion IRQ dependency, sleeps, re-enabling IF or waiting on another thread. This follows ext2's IRQ-save lock contract (L1 and §9). A timeout propagates an I/O error and initiates bounded quiescence or DMA quarantine; it does not permit immediate reuse/free of active buffers.
- 9G.2 owns 512/4096-byte sector integration tests against GPT and ext2; reject other sizes explicitly. The current boot image is laid out in 512-byte LBAs: do not reinterpret it as a 4096-byte-sector image. Use separately generated matching-geometry fixtures for 4096-byte tests.

### 9G.1 checkpoints and debugging

| Checkpoint | Required evidence | Known failure modes and response |
| --- | --- | --- |
| 9G.1a PCI discovery only | Implemented: report the first matching xHCI BDF, vendor/device and assigned BAR metadata near shell startup. BIOS/UEFI present/absent QEMU checks pass. Dell photo: 0000:00:14.0, 8086:9D2F, BAR0 0xEF330000, memory64, non-prefetchable; shell prompt reached. BAR extent and controller MMIO remain unverified; never hardcode these observed values. | No controller or invalid/unsupported BAR: report unavailable and return without probing an unvalidated MMIO address. |
| 9G.1b MMIO and reset | Verified in QEMU and Dell hardware: sized UC/NX aperture, capability/offset validation, bounded handoff/halt/reset, captured register diagnostics. Controller remains stopped with PCI mastering/decode disabled; no DMA buffers or rings. PS/2 input responsive. | No legacy handoff capability means no semaphore to wait for; stuck ownership, halt, HCRST or not-ready state must time out, record the failing register and disable this controller path. |
| 9G.1c Rings | Verified in QEMU and Dell hardware: command/event rings, Link TRB toggle cycle, port status event consumption, and No-Op Command Completion Event verified. Bounded event polling with interrupts disabled. | No completion before deadline or unexpected completion code/command pointer: record TRB and ring positions, fail the checkpoint, and quiesce/quarantine DMA rather than proceeding. |
| 9G.1d Ports | Verified in QEMU and Dell hardware: protocol mapping, root port inspection, USB 2.0 port reset, and speed negotiation verified. SuperSpeed attachments isolated. | Connected but unpowered: check power-switching capability and perform bounded supported power/reset sequencing; unresolved state fails that port. SuperSpeed attachment is logged as unsupported and skipped. |
| 9G.1e Descriptors | Verified in QEMU (both BIOS & UEFI), host ASan/UBSan, and Dell Latitude 5590 hardware (2026-09-19): DCBAA/scratchpad initialization, Enable Slot, Address Device, EP0 Control Transfers, Device Descriptor, Configuration Descriptor parsing, non-storage port filtering (Port 5 webcam 0x0E, Port 7 rejected), BOT validation, Bulk-In EP 0x81 (max 512), Bulk-Out EP 0x02 (max 512) on Slot 0x3 Port 0x9 (VID 0x13FE, PID 0x4200), and SET_CONFIGURATION(1) verified. Interactive shell prompt reached. | Short/all-0xFF response, invalid bLength (including zero), descriptor type other than DEVICE (1), or invalid packet size: reject before further parsing, record the reason, and do not publish a device. |

Before the first transfer, add a bounded `usb_dump_state()` diagnostic callable **only from thread context, with no subsystem or console lock held**, using the normal console/serial path. Timeout/error paths never call it: they copy bounded already-available state into a preallocated diagnostic record and publish a pending flag without allocation, logging or acquiring another lock. A thread consumes that record after transfer/FS locks have been released; use the existing IRQ-excluded publication discipline and preserve the record until consumed. Do not dereference stale controller/DMA pointers when printing. The record covers controller run/halt state, port state, software command enqueue/event dequeue positions and cycle bits, and last submitted/completed TRBs. Distinguish software bookkeeping from controller-owned positions that cannot be read directly; do not invent a hardware producer index. Capture QEMU serial logs and comparable Dell framebuffer diagnostics. Never print inside an ordinary IRQ handler or recursively acquire console locks. Use gated snapshots on failure/on demand rather than unconditional per-transfer logging. Existing QEMU launch recipes provide xHCI/USB attachment, but no event ring debugger or USB acceptance runner is implemented yet.

### Known unknowns to record during bring-up

- Does the Dell expose a BIOS/OS ownership semaphore, and what handoff is needed?
- What controller state does firmware leave, and does bounded reset succeed?
- Does the selected stick negotiate Full-Speed, High-Speed or unsupported SuperSpeed, and is its actual topology directly attached?
- Does it expose BOT or UAS, which LUNs, and 512- or 4096-byte logical sectors?
- Does the stick support the required cache synchronization semantics?

These are measurements for 9G.1/9G.2 (flush capability for 9G.4), not assumed hardware facts. Record observed values with the device and test environment.

### Implementation constraints and verification

- Read §4, §7.2–7.5 and §9 of [`AGENTS.md`](../../AGENTS.md), plus `pci.h`, `block.h`, `gpt.h`, `ext2.h`, `vfs.h`, VMM and synchronization headers before changing the related code. Preserve lock ranks, bounded IRQ work, DMA ownership/quiescence and the internal physical NVMe exclusion. No automatic formatting or raw-pattern tests on hardware; only the deliberately selected USB data partition may become writable under the explicit mount policy.
- Trace ext2-to-block calls before choosing USB completion handling: ext2 holds its ranked IRQ-save lock around I/O. Do not introduce sleeping or interrupt-dependent waits beneath that lock. Any synchronization redesign must follow §9 discussion requirements.
- Test the 130 MiB image on a larger disposable device as well as at exact image size. The GPT backup remains at the image boundary after a raw copy, while the current parser probes the device's last sector. 9G.3 owns this policy: accept a fully validated primary header AND partition array in degraded read-only mode when the end-of-device backup is absent/invalid; log the declared backup LBA and actual last LBA as a possible raw-copy size mismatch. Do not label an unverified mismatch definitively a raw copy. Preserve rejection of two valid but conflicting GPTs and the existing validated read-only backup fallback. Reject when neither copy validates. Never silently repair/resize disks. 9G.4 must explicitly resolve writable eligibility for the as-flashed layout and test it; RO acceptance alone does not authorize writes or partition-table repair.
- New USB persistence runners must use disposable copies and omit the NVMe fixture so `/mnt` cannot accidentally come from it. The existing `run-img*` targets attach a separate NVMe fixture and prove boot only. Name new USB tests `test-usb-*`; existing storage tests retain their NVMe fixture scope, and future `test-img-*` tests prove image boot only. Add a Makefile/runner preflight assertion over the final QEMU arguments: only the disposable USB data disk is allowed, with paired read-only OVMF code and disposable OVMF vars as firmware exceptions. Reject extra data disks, including NVMe and injected `-drive`/`-blockdev` backends or extra arguments. Add appropriate host failure tests and bounded QEMU targets following §7.5; run relevant existing ext2, storage, shell and power regressions.
- Preserve historical results and label new evidence by command, firmware, image/device and result. QEMU USB success does not establish Dell USB acceptance. Update this status and the matching file under `docs/roadmap/` after each completed stage.

---

## 3. Hardware Facts and Verification Boundaries

| ID | Evidence / constraint |
| --- | --- |
| H9 | Kingston USB DISK 2.0 (VID 0x13FE, PID 0x4200) on Dell 5590. Reports no SCSI caching page on MODE SENSE(6) or MODE SENSE(10); rejects SYNCHRONIZE CACHE(10). Classified ASSUMED_WRITE_THROUGH. Files persist across power cycle with clean shutdown. e2fsck -fn clean. Not a claim of power-loss tolerance. |
| H10 | SanDisk USB 3.2 Gen 1 (VID 0x0781, PID 0x5588). Enumerates as a SuperSpeed device on Port 0x12 (Dell 5590). Reports two alternates on Interface 0: Alternate 0 with protocol 0x50 (BOT) and endpoints 0x81 IN / 0x02 OUT; Alternate 1 with protocol 0x62 (UAS) and endpoints 0x81 / 0x02 / 0x83 / 0x04. Only the BOT alternate's endpoints are used. `MODE SENSE(6)` page 0x08 reports `WCE=1`; `SYNCHRONIZE CACHE` succeeds. Classified `SYNC_BACKED`. Mounts read-write at `/mnt`. |
| H11 | Dell 5590 xHCI controller at 0000:00:14.0 (Intel Sunrise Point-LP, 8086:9D2F), 64 KiB BAR at 0xEF330000, 12 USB 2.0 ports and 6 USB 3.0 ports. Legacy handoff extended capability at offset 0x846C. These are observations of one machine, not constants. |
| H12 | Dell Latitude 5530 presents **two** xHCI controllers: `0000:00:14.0` and `0000:00:14.2`, both matching class 0x0C subclass 0x03 progif 0x30. Verified via 9G.5a: both controllers initialized; a Kingston USB 2.0 stick is reachable on either controller depending on physical port; the first controller has no attached devices in the default configuration. Do not assume controller index maps to physical port group. |

---

## 4. Test Targets and Verification Notes

| Target | Scope / evidence |
| --- | --- |
| `make test-usb-discovery` | 9G.1a BIOS/UEFI PCI discovery with/without xHCI, shell startup without NVMe; ISO boot only, no data disk. No USB transfers or persistence claimed. |
| `make test-usb-descriptors` | 9G.1e host ASan/UBSan + QEMU BIOS/UEFI descriptor parsing, BOT class validation, device configuration |
| `make test-usb-block` | 9G.2 BOT host tests + QEMU sector read/GPT registration, no NVMe fixture |
| `make test-usb-mount` | 9G.3 mount policy host tests + QEMU BIOS/UEFI PARTUUID selection and read-only mount |
| `make test-usb-persistence` | 9G.4 QEMU BIOS/UEFI three-boot create/read/overwrite/delete with offline `e2fsck -fn` on disposable 130 MiB images |
| `make test-xhci-bot-host` | Host ASan/UBSan: BOT stall recovery, MODE SENSE parsing, durability policy table |
| `make test-usb-mount-host` | Host ASan/UBSan: mount eligibility, durability modes, sync path |

---

## 5. Architectural References

- Phase 9G.1 Enumeration: [`docs/roadmap/phase-9g1-xhci-enumeration.md`](../roadmap/phase-9g1-xhci-enumeration.md)
- Phase 9G.2 Read-only Block: [`docs/roadmap/phase-9g2-usb-block.md`](../roadmap/phase-9g2-usb-block.md)
- Phase 9G.3 Mount Policy: [`docs/roadmap/phase-9g3-usb-mount.md`](../roadmap/phase-9g3-usb-mount.md)
- Phase 9G.4 Durability & RW: [`docs/roadmap/phase-9g4-usb-durability.md`](../roadmap/phase-9g4-usb-durability.md)
- Phase 9G.5 SuperSpeed: [`docs/roadmap/phase-9g5-superspeed.md`](../roadmap/phase-9g5-superspeed.md)

E4-A SanDisk/Dell 5590 [physical acceptance](../roadmap/ext4-phase5-acceptance.md): all seven Phase-5 checklist items user-confirmed PASS, 2026-10-03.
