# FortressOS Roadmap and Checkpoint History

This file preserves the detailed roadmap and verification notes migrated from
AGENTS.md at commit `9a3c4b4`. Historical statements describe their checkpoint;
for example, later phases supersede earlier deferred-work notes. They are not
new instructions or proof that every current revision has passed every test.

For binding implementation rules, task routing, current hardware evidence and
next acceptance targets, read [AGENTS.md](AGENTS.md). For audit limits and
technical debt, read [ARCH_REVIEW.md](ARCH_REVIEW.md). Code and public headers
remain the implementation reference. Some old notes (including the default
keyboard layout) are corrected in AGENTS.md; the record below is preserved.

## Phase 9D review follow-up (2026-09-17)

Fixed shutdown synchronization without a writable mount, removed failed-mount
placeholder pointers, and reserved detached mount nodes before dirtying disk.
Truncation now reserves its bitmap scratch space before detachment and checks
all reclamation errors. Inode-initialization flush failure taints immediately,
without rollback writes. Metadata mapping rejection returns EIO; reserved GDT
expansion blocks are excluded from file data. Shutdown synchronization returns
failure on taint or I/O failure and freezes writes after a clean marker.

`wsl -d Ubuntu-24.04 -- make test-ext2` passed all eight sanitizer geometries,
including 14 fresh-image regression scenarios covering mount OOM/write/flush
failure, read-only/no-mount shutdown, allocation ownership, truncation OOM and
reclamation failure, tainted no-I/O behavior, and shutdown freeze/flush failure.
`make test-ext2-write` passed BIOS/UEFI three-boot persistence and both offline
`e2fsck -fn` audits per firmware on disposable clones. These tests do not claim
crash atomicity, torn-sector recovery, or physical writable-disk acceptance.

## Phase 9E Saved File Management & Bug H4 Resolution (2026-09-18)

Implemented directory operations (`mkdir`), file rename/move (`rename`), and deletion
(`unlink`) across VFS and writable ext2, along with user syscalls (`SYS_MKDIR` = 11,
`SYS_UNLINK` = 12, `SYS_RENAME` = 13) and interactive Ring 3 shell commands (`mkdir`, `rm`, `mv`).

- **Directory lifecycle:** Ext2 directory creation allocates dedicated data block and
  initializes standard `.` (self) and `..` (parent) records. Parent `links` count is
  incremented on creation and decremented on removal.
- **Safety checks:** Directory unlinking enforces that directories are empty (only `.` and `..`
  permitted; returns `-VFS_ENOTEMPTY` / `SYSCALL_ENOTEMPTY` otherwise).
- **Directory reparenting:** Cross-directory renames update `..` directory entry in the moved
  directory to point to the new parent, with corresponding link count adjustments.
- **On-disk reclamation:** Unlinked inodes have their data blocks returned to the block bitmap,
  inode marked free in the inode bitmap, block pointers and size cleared, `i_links_count` set
  to 0, and `i_dtime` deletion timestamp recorded.
- **Bug H4 fix:** Corrected Belgian AZERTY layout scancode decoding. Number row scancodes 2..13
  now use `shift ^ s->caps` as Shift-Lock for digits `1234567890`. Shifted lookup takes precedence
  over alphabet table, preventing accented keys (`0x03`, `0x08`, `0x0A`, `0x0B`, `0x28`) from
  falsely generating uppercase letters. Added ISO scancode 86 (`<` / `>`).
- **Verification:** `make test-input` and `make test-console` passed under ASan/UBSan. `make test-ext2`
  passed all 8 geometries. `make test-ext2-write` verified 3-boot persistence across BIOS and
  UEFI with zero `e2fsck -fn` errors. `make test-storage`, `make test-shell`, and `make test-power`


## Phase 9G.1 xHCI Controller & USB Device Enumeration (2026-09-19)

Completed Milestone 9G.1 (xHCI controller initialization, DMA rings, root port discovery/reset, device addressing, descriptor validation, and device configuration) across both QEMU (BIOS and UEFI) and physical bare-metal Dell Latitude 5590 hardware:

- **9G.1a Discovery**: PCI discovery of xHCI controller (`0000:00:14.0`, Intel Sunrise Point-LP `8086:9D2F`, 64-bit non-prefetchable BAR0 at `0xEF330000`).
- **9G.1b Reset & MMIO**: Sized aperture (64 KiB), validated operational registers, verified BIOS-to-OS ownership handoff (extended capability at offset `0x846C`), halted and reset controller (`CNR=0`).
- **9G.1c Rings**: Command Ring and Event Ring with ERST, Link TRB toggle cycle, and synchronous No-Op command verification via Command Completion Events.
- **9G.1d Ports**: Protocol capability mapping (12 USB 2.0 ports, 6 USB 3.0 ports). Root port scan detected 4 connected devices: Port 0x5 (High-Speed), Port 0x7 (Full-Speed), Port 0x9 (High-Speed), Port 0xA (Full-Speed). Bounded port reset and speed negotiation.
- **9G.1e Device Addressing & Configuration**:
  - DCBAA and scratchpad buffers initialized.
  - Multi-port scan loop probes attached USB 2.0 ports and filters non-mass-storage devices.
  - Port 0x5: Internal laptop webcam (`if_cls=0x0E`, USB Video Class) detected, cleanly rejected, and Slot 1 disabled.
  - Port 0x7: Full-speed device cleanly rejected.
  - Port 0x9: Physical Kingston/Phison USB flash drive (`VID=0x13FE`, `PID=0x4200`) detected on Slot 3:
    - Control Transfers on EP0 verified (TRT 16-bit shift fixed).
    - Device Descriptor read: `bMaxPacketSize0=64`, `bcdUSB=0x0200`.
    - Configuration Descriptor parsed (expanded buffer up to 2048 bytes).
    - Interface validated: Class `0x08` (Mass Storage), SubClass `0x06` (SCSI transparent command set), Protocol `0x50` (Bulk-Only Transport).
    - Bulk endpoints identified: Bulk-In EP `0x81` (max packet 512), Bulk-Out EP `0x02` (max packet 512).
    - `SET_CONFIGURATION(1)` command issued and completed successfully.
- **Verification**: `make test-usb-descriptors` passed 100% (8 host ASan/UBSan unit tests + BIOS/UEFI QEMU absent/present matrix). Bare-metal Dell Latitude 5590 boot confirmed working with interactive Ring 3 shell reached.

## Phase 9G.2 Read-Only USB Mass Storage Block Device (2026-09-19)

Completed Milestone 9G.2 (Bulk-Only Transport, SCSI engine, uniform block device registration, and GPT partition discovery) across QEMU (BIOS and UEFI) and physical bare-metal Dell Latitude 5590 hardware:

- **Bulk Transfer Rings**: Configured xHCI transfer rings for Bulk-In (Endpoint ID / DCI 3) and Bulk-Out (DCI 4) via `Configure Endpoint` command (Type 12) with Input Context slot indexing `(dci + 1) * ctx_dwords`.
- **Bulk-Only Transport (BOT)**:
  - 31-byte CBW (`0x43425355` "USBC") submission on Bulk-Out.
  - Data transfer stage on Bulk-In/Bulk-Out with cacheline flushing.
  - 13-byte CSW (`0x53425355` "USBS") reading on Bulk-In with signature, tag matching, and status validation.
- **SCSI Engine**:
  - `INQUIRY` (0x12): Reported Product "USB DISK 2.0".
  - `TEST UNIT READY` (0x00): Automatic `REQUEST SENSE` (0x03) recovery for initial Unit Attention.
  - `READ CAPACITY (10)` (0x25): Dell Kingston USB drive reported 30,320,640 sectors (16 GB / 14.46 GiB), 512 bytes/sector.
  - `READ (10)` (0x28): Verified logical sector reads.
- **Uniform Block Device & GPT Discovery**:
  - Registered block device `sda` via `block_register_usb()`.
  - Sector 0 read verified with Protective MBR signature `0xAA55`.
  - GPT partition table parsed: published `sdap1` (ESP FAT32, 64 MiB) and `sdap2` (Linux FS ext2, 64 MiB).
  - xHCI controller and DMA rings remain active at runtime for block I/O.
- **Verification**: `make test-usb-block` passed 100% (8 host ASan/UBSan unit tests + BIOS/UEFI QEMU absent/present matrix). Bare-metal Dell Latitude 5590 photo confirmed `sda`, `sdap1`, and `sdap2` registration and interactive Ring 3 shell reached.

## Detailed checkpoint roadmap

Recorded implementation sequence and planned work:

```
[Phase 1] Serial & Early Logging (COMPLETE)
    │
    ▼
[Phase 2] GDT & IDT (COMPLETE)
    │
    ▼
[Phase 3] Physical Memory Manager (PMM) (COMPLETE)
    │
    ▼
[Phase 3.5] Foundation Hardening & Freestanding Lib (COMPLETE)
    │
    ▼
[Phase 4A] Virtual Memory Manager (VMM) & 4-Level Paging (COMPLETE)
    │
    ▼
[Phase 4B] Kernel Heap Allocator (COMPLETE)
    │
    ▼
[Phase 5] ACPI Discovery & APIC Timer (COMPLETE)
    │
    ▼
[Phase 6] Kernel Threads & Scheduling
    │
    ▼
[Phase 7] User Space & Ring 3 Syscalls (The First Milestone)
        │
        ▼
[Phase 8] Virtual File System & Interactive Shell
    │
    ▼
[Phase 9A–9D] Storage track, NVMe, GPT, ext2, writable (COMPLETE)
    │
    ▼
[Phase 9E] Program execution, ABI, exit status (COMPLETE)
    │
    ▼
[Phase 9F] Raw disk image packaging (COMPLETE)
    │
    ▼
[Phase 9G] USB storage (COMPLETE through 9G.5b)
    │
    ▼
[Phase 9G.5] USB topology expansion (IN PROGRESS — 9G.5a, 9G.5b complete)
    │
    ▼
[Phase 9H] 32 GiB RAM support (COMPLETE)
    │
    ▼
[Following] Accounts/permissions, then installer
```

### Phase 9G handoff and evidence boundary (2026-09-18)

After flashing `fortress.img` with Rufus, the user reported no `/mnt`. Review
identified the missing runtime path: the kernel has no USB controller or
mass-storage driver, and `/mnt` is mounted from `nvme0n1p1` only by the QEMU
storage acceptance suite. The boot image's ext2 filesystem is partition 2.
The `run-img*` targets attach a separate NVMe fixture, so their shell boot and
any fixture `/mnt` do not prove access to the image's USB data partition.
Phase 9F completion covers image packaging, not physical USB persistence.
The README embedded in that partition describes intended behavior, not proof
of implemented USB access.

Antigravity's next implementation is Phase 9G, beginning with enumeration
and read-only USB access. The staged implementation scope, protected contracts
and acceptance checklist are in [AGENTS.md §2](AGENTS.md#phase-9g-implementation-handoff).
Each stage records its actual results here. The original planning update did
not establish implementation or test acceptance; subsequent evidence follows.

**Scope discipline:** Planning estimate: 9G.1 is expected to be the largest single driver effort since NVMe. Commit 9G.1a through 9G.1e as separate changes, each verified in QEMU before merging. If any checkpoint exceeds two focused sessions without a working artifact meeting its required evidence, stop implementation, document the specific blocker and evidence, and reassess scope before proceeding. Do not begin 9G.2 until 9G.1e produces a valid device descriptor and a validated directly attached BOT mass-storage interface on both QEMU and the Dell; awaiting hardware verification is a recorded blocker, not a pass.

**9G stop condition:** Use two weeks of active implementation effort after 9G.1e acceptance as a provisional review budget for 9G.2 and the read-only mount in 9G.3, not a delivery promise. If no read-only `/mnt` mount works on the Dell by that review point, stop and identify whether the bottleneck is xHCI complexity, hardware divergence or the existing storage stack. Record completed artifacts, failed checks and a revised scope/estimate before resuming. Exclude and record time awaiting hardware access separately. USB mounting/persistence may be explicitly deferred to a follow-up phase; the existing boot-image path remains available, and deferred acceptance must remain marked incomplete.

The handoff now defines inter-stage guarantees in AGENTS.md's fourth table
column, a USB 2.0/direct-attachment/boot-time-only scope, exact read-only SCSI
commands, and bounded event-ring polling compatible with ext2's lock contract.
9G.1 is split into PCI-only discovery (9G.1a), MMIO/reset (9G.1b), No-Op command
completion (9G.1c), port inspection (9G.1d), and descriptor enumeration (9G.1e).
The original handoff starts with 9G.1a and requires bounded state-dump diagnostics before
the first transfer. Known hardware unknowns and their measurement stages are
listed in AGENTS.md. These are planned deliverables, not new driver code.

Each 9G.1 checkpoint also names likely failure symptoms and bounded responses.
Diagnostic printing is thread-context only after locks are released; timeout
paths publish a preallocated snapshot and pending flag. Writable selection is
planned as an explicit boot-menu opt-in using `usb_data=PARTUUID=<guid>` plus
`usb_data_mode=rw`, with a read-only default and duplicate-target rejection.
Labels or marker files alone do not authorize writes. Boot-argument support
and the menu entry are implementation work, not existing functionality.

The consolidated "What 9G does NOT do" list in AGENTS.md excludes SuperSpeed,
external hubs, hot-plug/reconnection, UAS, other USB classes, suspend/resume,
multiple LUNs and runtime host-controller reset recovery. Direct-attached
SuperSpeed should be a separate follow-up after 9G; external hubs remain later
work. Initial reset, root-port management and safe failure remain required.

9G.2 owns logical-sector compatibility; 9G.3 owns larger-device GPT policy and
read-only mount selection; 9G.4 owns the writable path and real device flush.
New `test-usb-*` runners must assert that no extra data disk is attached,
allowing only the USB fixture plus firmware code/vars. `test-img-*` boot evidence
must remain separate from storage acceptance.

Completion requires all of the following:

- Read the intended USB partition at `/mnt` with no NVMe fixture attached;
  preserve shell startup when the stick is absent, unsupported or unreadable.
- Test exact-size images and images copied to larger disposable devices,
  including the backup-GPT location mismatch. Preserve physical disk exclusions
  and require explicit selection/opt-in for writable USB mounting.
- On disposable USB images under BIOS and UEFI: create/save and cleanly shut
  down, reboot/read/overwrite and cleanly shut down, then reboot/read again.
  Check the ext2 partition offline with `e2fsck -fn` after clean shutdowns.
- Exercise transfer failures, timeouts, invalid descriptors and bounds, and
  safe handling of device disappearance without releasing DMA still in use.
- Record separate Dell acceptance: selected USB device, `/mnt` file read,
  save, clean shutdown and persisted contents after reboot. Keep the internal
  NVMe outside this test. QEMU results alone cannot close physical acceptance.

**Post-completion note (2026-09-19):** The scope discipline and stop
conditions above were the plan as issued. Phase 9G completed without
triggering either the per-checkpoint review or the two-week stop condition;
each stage landed with its acceptance evidence recorded. Physical writable
persistence was achieved on the Kingston USB 2.0 stick; the strong durability
path remains QEMU-only pending SuperSpeed support (Phase 9G.5). The handoff
language above is preserved as the historical record.

### Phase 9G.1a — PCI-only xHCI discovery (2026-09-18)

Implemented `pci_report_xhci()` using the existing read-only PCI lookup. It
reports the first class/subclass/interface 0x0c/0x03/0x30 match, segment/BDF,
vendor/device ID and firmware-assigned BAR0 base, width and prefetch flag.
Unsupported headers, absent/unassigned BARs, I/O BARs and unsupported memory
BAR types return with a diagnostic. No controller BAR mapping/sizing, command
register writes, firmware handoff, reset, DMA or USB transfers are performed.
BAR aperture and controller accessibility remain unverified for 9G.1b.

The report appears immediately before shell startup through the serial output
path that also mirrors to the framebuffer. Missing NVMe now skips the NVMe
checks instead of halting, allowing PCI-only tests without a storage fixture.
The existing QEMU identity gate and physical NVMe storage exclusion remain.

`wsl -d Ubuntu-24.04 -- make test-usb-discovery` passed all four cases:
BIOS/UEFI, each with xHCI present and absent. All reached the interactive shell
without NVMe or other data disks. The runner validates its final QEMU arguments,
uses ISO boot and disposable paired OVMF vars, bounds waits and terminates QEMU.
Evidence: `build/usb-discovery-{bios,uefi}-{present,absent}.log` and `.stderr`.
This verifies PCI metadata and continuation, not malformed-BAR injection,
USB-device enumeration, USB I/O or physical hardware behavior.

Regression: `wsl -d Ubuntu-24.04 -- make test-shell test-storage` passed
BIOS/UEFI shell and storage checks plus keyboard-only UEFI 8 GiB without COM1.
The kernel compiled with the existing strict warning/freestanding flags.
Final `wsl -d Ubuntu-24.04 -- make test-usb-discovery img` repeated all four
discovery cases successfully and rebuilt `bin/fortress.img`; the image
builder's MBR/GPT/FAT checks and offline ext2 `e2fsck` verification passed.

**Dell photo evidence (2026-09-18):** user-supplied `Photo 1.jpg` shows
`[USB 9G.1a] First xHCI controller: 0000:00:14.0 vendor=0x8086 device=0x9D2F`
and `BAR0=0xEF330000 memory64 prefetch=no`, followed by the discovery-complete
message, `[BOOT] Interactive shell ready.` and the `fortress>` prompt. COM1 RX
is unavailable, so this also establishes visible framebuffer reporting on
the Dell. QEMU storage fixture tests are shown as skipped.

This confirms physical PCI discovery and boot progression to the shell prompt.
The photo does not show a typed command, so post-change keyboard interaction
is not newly verified. BAR extent, controller MMIO, ownership handoff/reset,
USB enumeration and persistence remain unverified. These addresses/IDs are
observations of this Dell, not constants for the driver. Next: 9G.1b MMIO/reset.

**Dell 9G.1b hardware verification (2026-09-18):** user confirmed that Phase 9G.1b
passed on physical Dell Latitude 5590 hardware:
- Controller BAR0 sized and mapped in dedicated UC/NX virtual window.
- Capability offsets validated against the aperture.
- BIOS-to-OS ownership handoff semaphore successfully negotiated; SMIs disabled.
- Host controller halted and reset via HCRST; CNR cleared to 0.
- Reset readback values (USBCMD, USBSTS, PAGESIZE) validated.
- Virtual window safely unmapped, bus mastering left disabled, no firmware DMA leaked.
- Interactive PS/2 keyboard confirmed functional at the `fortress>` shell prompt.

Next checkpoint: 9G.1c command and event rings.

**Dell 9G.1c hardware verification (2026-09-18):** user confirmed that Phase 9G.1c
passed on physical Dell Latitude 5590 hardware:
- Command Ring and Event Ring DMA pages allocated and bound to `CRCR`, `ERSTSZ`, `ERSTBA`, and `ERDP`.
- PCI Bus Mastering enabled dynamically during transfer execution.
- Controller started (`USBCMD.RS = 1`); hardware posted a Port Status Change Event for attached Port 5 (`ctrl=0x8801`).
- Port status change event consumed and acknowledged via `ERDP`.
- No-Op Command TRB (type 23) executed via Doorbell 0; matching Command Completion Event (`XHCI_COMP_SUCCESS`, type 33) received.
- Controller cleanly halted, bus mastering disabled, DMA frames reclaimed safely without leaks.
- Interactive PS/2 shell confirmed functional.

Next checkpoint: 9G.1d root port inspection and reset.

**Dell 9G.1d hardware verification (2026-09-18):** user confirmed that Phase 9G.1d
passed on physical Dell Latitude 5590 hardware:
- Traversed Supported Protocol capabilities; mapped USB 2.0 and USB 3.x ports.
- Scanned 18 root ports (`PORTSC`), detected connected device on Port 5.
- Verified port power and executed bounded port reset on Port 5.
- Verified port enablement (`PED = 1`) and successfully decoded High-Speed (480 Mbps) speed.
- Selected Port 5 for subsequent device addressing; isolated non-target ports.
- Interactive PS/2 shell confirmed functional.

Next checkpoint: 9G.1e device addressing and descriptor parsing.

---

## Architectural audit and ext2 implementation scope

See `ARCH_REVIEW.md` for implemented checks, supported ext2 format and deferred work.
`make test-ext2` runs the actual ext2/VFS sources under host ASan/UBSan;
`make test-storage` verifies the complete boot suite in BIOS and UEFI, saving logs
in `build/storage-bios.log` and `build/storage-uefi.log`. QEMU uses snapshot disk
writes; raw-sector pattern tests remain disabled by default.

Lock ranks increase on acquisition: scheduler/ext2 (1, mutually exclusive),
heap (2), VMM (3), PMM (4), console (5). Tracking uses bootstrap-CPU storage,
with IRQs disabled before inspecting it. Release must be LIFO. Saved 64-bit
RFLAGS belongs to each caller. Diagnostics use raw UART. This is not SMP-ready.

### NMI transition and physical-boot diagnostics

- MADT type 4 NMI records are validated and applied to the bootstrap CPU's
  LAPIC LINT pins, with processor-ID matching and conflict detection. Undeclared
  pins stay masked. x2APIC/type-10 NMI routing is not implemented.
- `make test-nmi` uses QEMU TCG QMP injection plus hardware GDB breakpoints at
  zero-byte assembly labels. It never patches code, synthesizes INT 2, or widens
  the transition windows. It verifies 5 boundaries x 4 rounds x 2 firmware modes,
  then requires the complete boot suite to finish. Evidence: `build/nmi-*.json`
  and `build/nmi-*.log`. This covers QEMU delivery, not physical NMI injection,
  nested fault/NMI scenarios or SMP.
- Framebuffer logging begins before PMM/GDT tests. COM1 loopback failure disables
  UART output; transmitter waits are bounded so absent hardware cannot hang boot.
- The current PMM explicitly manages RAM below 2 GiB, reserving higher RAM until
  allocator/audit capacity is expanded. Its bitmap is selected within managed RAM.
- `make test-boot-diagnostics`: UEFI, 8 GiB, no COM1, no NVMe fixture; verifies
  progress to PCI discovery and captures `build/boot-8g-no-uart.png`.
- Storage fixture assertions run only against QEMU NVMe vendor/device IDs.
  Both hardware and fixture paths launch /bin/shell after diagnostics. The physical
  NVMe is still not mounted; /mnt is only available on the QEMU ext2 fixture.

### Boot-console scrolling

The console caches character/colour cells in static RAM (512 x 256 cells,
1.5 MiB; viewport capped to this grid). Scrolling never reads framebuffer MMIO.
Only changed cells are rendered, and each scroll advances min(8, max(1, rows/4))
rows so several subsequent log lines need no screen movement. Output remains
synchronous and immediately visible, including before PMM/heap initialization.
Wrapping is deferred until the next printable character; an explicit newline
following a full-width line advances exactly once.

`make test-console` checks pixel output, colour preservation, scroll batching,
zero redraws for blank lines, control characters, one-cell screens and padded
framebuffer bounds under ASan/UBSan. BIOS/UEFI full boot suites also pass.

### Interactive input and shell

`make` includes a separate freestanding C ELF `/bin/shell` in initramfs. Boot
launches it as a normal Ring 3 process after the acceptance suite. Try:

```
help
ls /
ls /bin
cat /etc/motd
cat /docs/readme.txt
echo hello
```

`exit` terminates/reaps the process and launches a fresh shell. Files are read-only;
no editor, command execution/exec, disk installation, accounts or USB HID driver
is provided by this step. Keyboard layout is Belgian AZERTY (Shift/Caps Lock,
Backspace, Enter; arrows and function keys ignored, Caps LED not synchronized).
Line length is bounded to 191 bytes; overflow discards the entire command.
The console supports erasing across wrapped rows. Serial CR/LF and DEL are
normalized; echo and line editing occur in user space, not interrupt handlers.

`SYS_READ(0, buffer, count)` checks all user pages for write permission before
waiting. It returns available bytes as a short read, without waiting for newline.
A scheduler predicate and BLOCKED-list insertion occur under the scheduler lock
with IRQs disabled. Producers publish input before waking readers; a resumed
reader rechecks availability. IRQ exclusion also spans predicate-to-dequeue.
No lock crosses a context switch, no ISR allocates/logs/switches, and the IDT
dispatcher owns each keyboard/UART EOI. Blocked tasks remain visible to process
liveness/wait APIs. This input/scheduler contract is bootstrap-CPU-only; SMP and
concurrent address-space mutation need further synchronization.

Verification:
- `make test-input`: actual decoder/FIFO under ASan/UBSan; modifiers, Pause,
  PrintScreen, extended keys, wraparound and overflow.
- `make test-shell`: BIOS/UEFI PS/2 events and serial RX through real emulated
  devices, stdin pointer checks, Backspace/Shift, file commands and error paths,
  sleeping task/timer progress, descriptors closed, three process restarts with
  stable physical free-page and stack-slot counts. Logs: `build/shell-*.log`.
- The same target tests UEFI 8 GiB with COM1 absent and non-fixture NVMe identity,
  confirming framebuffer `echo hello` and sleeping input on the hardware boot
  path. Screenshot: `build/shell-keyboard-only.png`. Physical Dell interaction
  was subsequently confirmed by the user photo described below.
- Existing BIOS/UEFI storage acceptance and 40 exact-boundary NMI tests pass.

References for the driver/test protocol: Intel EC firmware 8042 documentation
(https://intel.github.io/ecfw-zephyr/reference/kbchost/index.html), QEMU PS/2
implementation (https://github.com/qemu/qemu/blob/master/hw/input/ps2.c), and
QMP input-send-event (https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html).

### Dell Latitude 5590 physical acceptance (2026-09-16)

User-supplied boot photos confirm the shell on a Latitude 5590 (Core i5-8350U,
32 GiB installed RAM, 256 GB NVMe, Intel UHD 620), booted from a Rufus-written
USB. The latest photo shows PS/2 set 2 -> set 1 / IRQ1 ready, COM1 RX unavailable,
and the Ring 3 shell responding to keyboard input. `help` prints the command
list, `ls` lists `docs/`, `etc/`, `bin/`, and `cat etc/motd` prints the welcome
file and returns to the prompt. `cat motd` correctly reports a missing file.
These are manual observations, supplementing the automated QEMU tests.

The welcome file is from the boot initramfs. The photo explicitly reports that
QEMU storage fixture tests were skipped: physical NVMe filesystem mounting,
reads/writes and persistence remain unverified. This photo does not verify
physical NMI injection, every key/modifier, or blocked-reader resource counters.
The PMM still manages only the low 2 GiB despite 32 GiB being installed.

Follow-up integration: `wsl -d Ubuntu-24.04 -- make test-storage test-shell
test-power` passed BIOS/UEFI storage and shell checks, keyboard-only UEFI 8 GiB,
and ordinary read-only-boot shutdown/reboot (QEMU exit code 0). The kernel and
ISO also built with the existing strict compiler flags.

### Phase 9D — Bounded Writable ext2 Filesystem Verification (2026-09-17)

Phase 9D delivers bounded write support on the ext2 block layer, enabling file creation,
truncation, block reclamation, and persistence to NVMe storage.

- **Implementation Details**:
  - Direct block and single-indirect block allocation and writes (`vfs_write`).
  - Directory entry insertion (`vfs_create`).
  - File truncation (`vfs_truncate`) with a 3-stage contract: scratch pre-allocation,
    Stage 2 detachment (inode size/pointers zeroed and flushed first), and Stage 3
    block reclamation with per-block error validation and taint marking.
  - Unsupported structures (double/triple indirect blocks, non-regular files) are
    explicitly pre-rejected with `-EFBIG` / `-EOPNOTSUPP`.
  - Atomic mount staging: `/mnt` VFS node is linked to the hierarchy only after the
    dirty marker is persisted and flushed to disk; mount failure unwinds cleanly.
  - Clean shutdown lifecycle: `ext2_sync_all()` returns `bool`. If the filesystem is
    tainted, it refuses to mark the filesystem clean; on success, it sets `s_state = EXT2_VALID_FS`
    and freezes further writes (`fs->read_only = true`).
  - Line editor in `user/shell.c` expanded to 8 KiB with save-protection guards
    (`editor_save_disabled`) against truncated reads.
  - Explicit write opt-in: writes are disabled by default; enabled only when
    `-fw_cfg name=opt/fortress/write_test,string=1` is provided (or `WRITE_TEST=1`).

- **Verification Environment & Evidence**:
  - **Environment**: WSL2 `Ubuntu-24.04` on Windows 11 host (x86_64, Linux 6.6 kernel).
    QEMU `q35`, 2 GiB RAM, PCIe NVMe controller (`serial=fortress0`), GPT with
    a 1024-byte-block ext2 partition. These prior results were supplied by the user;
    they are not new test runs by the implementation agent.
  - **Automated 3-Boot Persistence Suite (`make test-ext2-write`)**:
    - Ran disposable GPT NVMe fixtures across both legacy BIOS and UEFI
      (paired with OVMF 4M firmware).
    - Boot 1: created `/mnt/written.txt` via Ring 3 shell, saved, verified NVMe flush,
      and clean ACPI S5 shutdown (QEMU exit code 0). Offline `e2fsck -fn` passed with 0 errors.
    - Boot 2: verified persisted multi-line content, truncated and overwrote with
      shorter content, clean shutdown. Offline `e2fsck -fn` passed with 0 errors.
    - Boot 3: verified cross-boot persistence of truncated state with zero stale lines.
  - **Interactive Manual Verification**:
    - Booted via `make run-bios WRITE_TEST=1` in WSL Ubuntu-24.04 (from PowerShell).
    - Verified kernel log: `[ext2] Writable mount complete at /mnt`.
    - Created `/mnt/test.txt` via `edit /mnt/test.txt`, entered multi-line text in append mode,
      saved via `w` (`[EDIT] Saved 58 bytes (2 lines) to /mnt/test.txt`), quit with `q`,
      and verified contents via `cat /mnt/test.txt`.
    - Executed clean shutdown via `poweroff`.
    - User reports persistence on reload. The clean-marker/freeze behavior is
      established by code and automated offline checks, not solely by shutdown output.
  - **Host Fault-Injection Matrix (`make test-ext2`)**:
    - 14 deterministic regression scenarios in `tests/ext2_host.c` under ASan/UBSan,
      covering exact write failure counts, OOM allocations, and shutdown freeze invariants.

### Phase 9E — Program Execution from Shell, Exit Status & System V AMD64 ABI (2026-09-18)

Phase 9E adds the ability for the interactive Ring 3 shell to load, execute, pass arbitrary string arguments to, and wait on standalone user ELF binaries from VFS (`/bin/hello`), while isolating CPU faults, tracking exit status, and adhering strictly to the standard System V AMD64 ELF ABI.

- **Implementation Details**:
  - **System Calls**: `SYS_SPAWN` (nr 9: path, argv pointer -> child PID) and `SYS_WAIT` (nr 10: child PID, status pointer -> 0 on success).
  - **Standard System V AMD64 Process Stack**: In `process_setup_user_stack()`, string arguments are packed at the high end of the initial 4 KiB user stack page (`USER_STACK_TOP_VIRT = 0x00007FFFF0001000ULL`) via HHDM virtual translation (`vmm_phys_to_virt(stack_phys)`). Below the strings, the initial pointer table is written: `[RSP] = argc`, `[RSP+8] = argv[0]`, ..., `argv[argc] = NULL`, `envp[0] = NULL`, `AT_NULL` auxiliary vector pair (`0, 0`). `RSP` is strictly 16-byte aligned (`RSP % 16 == 0`).
  - **Register Initialization & ABI**: At process entry (`user_process_trampoline`), `RDI = argc`, `RSI = argv`, `RDX = 0` (standard `rtld` termination handler), with all other GPRs sanitized to zero. Kernel boot tests using `process_spawn_with_arg()` maintain scalar `RDI` mode selection compatibility for `init.asm` test modes 0..7.
  - **Process Waiting & Reclamation**: `process_wait_child()` uses single-threaded parent predicate `child_done` with `sched_wait_until()`, waking via `sched_wake_all()`. Parent reaps dead resources via `sched_reap_dead()`. Up to 64 active child records are tracked in `g_child_records` under the scheduler spinlock.
  - **Fault Isolation**: Processes faulting on CPU exceptions (e.g. #PF vector 14, #GP vector 13) are recorded with status `128 + vector` by the exception handler, reported to the user as `[PROCESS] Faulted (exception vector <N>)` without bringing down the parent shell or kernel.
  - **Shell Argument Parsing & Status Tracking (`$?`)**: User-space shell command parser tokenizes whitespace-delimited arguments (`run /path [args...]`), passing `argv[]` array to `SYS_SPAWN`. Shell tracks `last_status` updated on every command and child termination. `echo $?` expands to decimal exit code.
  - **Command Chaining**: Shell command parser supports conditional chaining: `&&` executes subsequent command only if previous succeeded (`last_status == 0`), while `||` executes only if previous failed (`last_status != 0`).
  - **Driver Hardening**: In `serial_init()`, receiver FIFO is drained inside loopback mode, followed by a bounded poll for data ready. This eliminates false loopback failures and serial silencing caused by UEFI/OVMF firmware debug noise during boot.

- **Verification Environment & Evidence**:
  - **Automated Shell Integration Suite (`make test-shell`)**:
    - `PASS bios`: Tested `/bin/hello` execution with no arguments (`run /bin/hello`), numeric argument (`run /bin/hello 42`), string argument (`run /bin/hello world`), status query (`echo $?` -> `42` / `0`), command chaining (`&&` executed on success, skipped on failure; `||` executed on failure, skipped on success), negative error paths (`/missing`, `/bin`), and zero-leak resource audit (`free_pages` and `g_stack_slots_bitmap` unchanged across child lifecycles). Log: `build/shell-bios.log`.
    - `PASS uefi`: Validated identical command sequences and resource assertions under UEFI with paired OVMF firmware. Log: `build/shell-uefi.log`.
    - `PASS keyboard-only UEFI 8 GiB`: Validated hardware boot path without COM1 UART.
  - **Subsystem Regression Coverage**:
    - `make test-input`: Passed FIFO and scancode decoding.
    - `make test-console`: Passed cached redraw and scrolling checks.
    - `make test-storage`: Passed BIOS and UEFI GPT and ext2 Ring 3 read/audit tests.
    - `make test-nmi`: Passed 40 exact-boundary NMI delivery cycles across all 5 syscall transitions in BIOS and UEFI.
    - `make test-ext2`: Passed host ASan/UBSan matrix with injected failures across 8 configurations.

### Phase 9G.1e — xHCI Device Addressing & Descriptors (2026-09-18)

Phase 9G.1e implements device slot assignment, device addressing, Default Control Pipe (EP0) transfer ring management, USB descriptor querying and parsing, Mass Storage BOT class validation, and device configuration (`SET_CONFIGURATION(1)`).

- **Implementation Details**:
  - `src/drivers/xhci_dev.h`, `src/drivers/xhci_dev.c`:
    - Clean separation of memory: DCBAA table, scratchpad buffer array (up to 128 pages based on `HCSPARAMS2`), Input Context (32-byte or 64-byte based on `HCCPARAMS1.CSZ`), Output Context, EP0 Transfer Ring (256 TRBs with Link TRB), and bounce buffer.
    - Pre-initialization: `CONFIG.MaxSlotsEn` and `DCBAAP` programmed while the controller is halted, following the xHCI specification Section 4.2.
    - Single running pipeline: controller starts once in `xhci_verify_rings()`, executes No-Op at index 0, verifies root ports in `xhci_discover_and_reset_ports()`, and proceeds directly to device enumeration without halting, preserving controller internal cycle states and dequeue indices.
    - Command execution via Doorbell 0: issues `ENABLE_SLOT` (acquires Slot ID), registers Output Context in DCBAA, sets up Input Context (Slot Context + EP0 Context with speed and port routing), and issues `ADDRESS_DEVICE`.
    - EP0 Control Transfers: Setup Stage TRB (IDT=1), Data Stage TRB (pointing to DMA bounce buffer), and Status Stage TRB (IOC=1). Rings Doorbell for Slot ID (Target=1).
    - Descriptor Parsing:
      - Reads initial 8 bytes of Device Descriptor: extracts and validates `bMaxPacketSize0` (8, 16, 32, 64).
      - Issues `EVALUATE_CONTEXT` if the negotiated packet size differs from the initial speed default.
      - Reads full 18-byte Device Descriptor: captures `idVendor`, `idProduct`, `bNumConfigurations`.
      - Reads 9-byte Configuration Descriptor header, validates `wTotalLength` (9..512), and reads full configuration descriptor.
      - Validates Interface: requires class `0x08` (Mass Storage), subclass `0x06` (SCSI transparent command set), protocol `0x50` (Bulk-Only Transport). Rejects other device classes cleanly.
      - Locates Bulk-In and Bulk-Out endpoints, verifies max packet size (e.g. 512 bytes for High-Speed).
      - Issues standard USB request `SET_CONFIGURATION(1)`.
    - Clean teardown and DMA quarantine: all allocated PMM pages are freed upon successful clean shutdown; if any command or transfer fails/times out, frames are quarantined to prevent DMA memory corruption.

- **Verification Evidence**:
  - **Host ASan/UBSan Unit Test Suite (`python3 scripts/test_xhci_dev_host.py`)**:
    - `PASS`: normal enumeration, descriptor reads, BOT class validation, and SET_CONFIGURATION(1).
    - `PASS`: Enable Slot failure handled cleanly.
    - `PASS`: Address Device failure handled cleanly.
    - `PASS`: Bad descriptor header rejected cleanly.
    - `PASS`: Malformed device descriptor rejected cleanly.
    - `PASS`: Non-mass-storage class device rejected cleanly.
    - `PASS`: Device without bulk endpoints rejected cleanly.
    - `PASS`: SET_CONFIGURATION failure handled cleanly.
  - **QEMU Full Matrix Suite (`make test-usb-descriptors`)**:
    - `PASS usb-descriptors-bios-absent`: boots cleanly without xHCI, shell prompt reached, PS/2 echo responsive.
    - `PASS usb-descriptors-bios-present`: QEMU `qemu-xhci` with `usb-storage` attached to USB 2.0 port. Discovers Port 1, High-Speed (480 Mbps), issues Enable Slot (Slot ID 1), Address Device, reads descriptors (`VID=0x46F4 PID=0x0001 EP0_MAX=64 Bulk-In=0x81 Bulk-Out=0x02`), issues `SET_CONFIGURATION(1)`, and boots to interactive shell with PS/2 echo.
    - `PASS usb-descriptors-uefi-absent`: paired OVMF 4M UEFI firmware boots cleanly without xHCI.
    - `PASS usb-descriptors-uefi-present`: full UEFI boot with `qemu-xhci` and `usb-storage` enumeration verified.

### Phase 9G.3 — Production `/mnt` Mount & Command Line Partition Selection (2026-09-19)

Phase 9G.3 delivers production storage initialization independent of QEMU acceptance fixtures, mounting the persistent ext2 data partition (`sdap2`) read-only at `/mnt` using explicit GPT partition GUID (`PARTUUID`) matching and USB device provenance verification.

- **Implementation Details**:
  - **Limine Command Line Capture**:
    - Instantiated `struct limine_kernel_file_request kernel_file_request` under Base Revision 3 protocol.
    - Deep-copied `kernel_file->cmdline` into kernel-owned storage in `boot_info_t` (`boot_info.cmdline`, up to 512 bytes, null-terminated).
  - **Bounded Command Line & GUID Parser (`src/fs/usb_mount.h`, `src/fs/usb_mount.c`)**:
    - `gpt_str_to_guid()`: converts standard 36-char mixed-endian UUID string `XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX` into `gpt_guid_t`, functioning as exact inverse to `gpt_guid_to_str()`.
    - `usb_mount_parse_cmdline()`: parses whitespace-delimited tokens for `usb_data=PARTUUID=<guid>` and `usb_data_mode=ro|rw` (default `ro`). Non-PARTUUID selection (e.g. filesystem labels or raw disk names) is strictly rejected as malformed.
  - **Storage Initialization & Provenance Verification**:
    - Hooked directly into boot sequence in `kmain()` after `xhci_boot_probe()`.
    - Checks controller initialization state (`usb_is_initialized()`); if absent, cleanly logs diagnostic and leaves `/mnt` unmounted.
    - Checks if `/mnt` is already mounted (preventing conflicts with test fixtures).
    - Filters candidates by USB BOT parent device provenance (`part->parent->name == "sda"`), strictly excluding internal NVMe devices (`nvme0n1`).
    - Resolves candidates against target GUID:
      - 0 matches: logs `Partition PARTUUID=... not found on supported USB storage; /mnt left unmounted`.
      - > 1 matches: logs `Ambiguous candidates: multiple partitions matched PARTUUID=...; /mnt left unmounted`.
      - 1 match: verifies GPT policy (rejects invalid/ambiguity, accepts consistent primary, degraded primary, or backup fallback).
    - Enforces read-only policy for Phase 9G.3 (if `mode=rw` is requested, notes that writable persistence is deferred to 9G.4 and proceeds with read-only mount).
    - Reports device provenance: `[USB 9G.3] Selected USB device: sda, partition: sdap2 (PARTUUID=...)`.
    - Mounts ext2 partition read-only via `ext2_mount(&part->block_dev, "/mnt")` and logs `[USB 9G.3] PASS: Mounted sdap2 read-only at /mnt`.
  - **Dual-Boot Image Builder Update (`scripts/create_boot_img.py`)**:
    - Generates and prints `[IMG] Data partition PARTUUID: <UUID>`.
    - Generates image-specific `limine.conf` with:
      - Default entry: `/FortressOS (UEFI x86_64)` with `kernel_cmdline: usb_data=PARTUUID=<UUID> usb_data_mode=ro`.
      - Writable entry: `/FortressOS (Persistent Storage - Writable: PARTUUID=<UUID>)` with `kernel_cmdline: usb_data=PARTUUID=<UUID> usb_data_mode=rw`.
    - Deploys to all 4 standard ESP configuration locations.

- **Verification Evidence**:
  - **Host ASan/UBSan Unit Test Suite (`scripts/test_usb_mount_host.py`)**:
    - `PASS`: `gpt_str_to_guid` and `gpt_guid_to_str` bidirectional roundtrip test.
    - `PASS`: `usb_mount_parse_cmdline` with valid RO/RW, default modes, extra arguments, malformed targets, and invalid syntax.
    - `PASS`: `usb_mount_production_storage` candidate selection: 0 matches, non-USB parent rejection, ambiguous clones rejection, GPT policy rejection, degraded primary acceptance, and RW fallback to RO.
  - **QEMU Full Matrix Integration Suite (`make test-usb-mount`)**:
    - `PASS usb-mount-bios-absent`: boots cleanly without xHCI, shell prompt reached and responsive to keyboard echo.
    - `PASS usb-mount-bios-present`: boots raw disk image `bin/fortress.img` as an emulated USB flash drive under legacy BIOS. Discovers xHCI, addresses BOT device on Slot 1 Port 1, registers `sda`, parses GPT (`sdap1`, `sdap2`), reads Limine cmdline, mounts `sdap2` read-only at `/mnt`, drops to interactive shell, verifies `ls /mnt` lists `README.txt`, and `cat /mnt/README.txt` prints persistent storage banner.
    - `PASS usb-mount-uefi-absent`: paired OVMF 4M UEFI firmware boots cleanly without xHCI, shell prompt reached and responsive.
    - `PASS usb-mount-uefi-present`: boots raw disk image `bin/fortress.img` as an emulated USB flash drive under UEFI firmware. Fully mounts `sdap2` at `/mnt`, and verifies `ls /mnt` and `cat /mnt/README.txt` via QMP.
  - **Bare-Metal Dell Latitude 5590 Hardware Acceptance (2026-09-19)**:
    - Booted from physical USB flash drive flashed with Rufus.
    - Discovered xHCI controller 8086:9D2F, enumerated Kingston/Phison flash drive on Port 9, registered `sda` (30,320,640 sectors).
    - Verified Sector 0 MBR signature `0xAA55`.
    - GPT parsed with both Primary and Backup valid and consistent; published `sdap1` (ESP FAT32) and `sdap2` (ext2 data).
    - Kernel command line read from Limine (`usb_data=PARTUUID=79C710... usb_data_mode=ro`).
    - Matched `sdap2` against target PARTUUID with USB BOT parent provenance (`sda`).
    - Successfully mounted `sdap2` read-only at `/mnt` (`[USB 9G.3] PASS: Mounted sdap2 read-only at /mnt`).
    - Interactive Ring 3 shell prompt reached and responsive. Photographic evidence confirmed.


### Phase 9G.4 — USB Writable Persistence & Durability Classification (2026-09-19)

Phase 9G.4 completes the USB storage stack: `/mnt` mounts read-write on real hardware, files written from the shell persist across a full power cycle, and durability is classified per-device with an explicit disclosure when the device cannot be classified strongly. The work spans three sub-problems — BOT stall recovery, SCSI cache-policy discovery, and mount eligibility — each verified independently before the whole path was exercised end-to-end.

**What was built**

*BOT stall recovery (Commit 1b).*
- `xhci_bot_endpoint_reset()` issues Stop Endpoint, Reset Endpoint, Set TR Dequeue Pointer (with DCS bit set), and CLEAR_FEATURE(ENDPOINT_HALT) in sequence.
- `xhci_bot_transfer()` attempts a single bounded endpoint reset on a stall before latching the device offline.
- `latched_offline` is distinct from `transport_failed`; both prevent further BOT submissions and retain DMA allocations until reboot.

*SCSI cache-policy discovery (Commit 2).*
- `MODE SENSE(6)` and `MODE SENSE(10)` caching page `0x08` support, requesting current values only. `MODE SELECT` is not implemented and device cache settings are not modified.
- Fallback sequence: `MODE SENSE(6)` first, then `MODE SENSE(10)` if the page is not found or the command is rejected. Transport failures stop the probe; command rejections do not.
- `xhci_scsi_probe_cache_policy()` parses the mode header, block descriptor length, page code, and page length independently, validates each against transferred byte count, and reads `WCE` / `RCD` / write-protect. Malformed, truncated, missing, or conflicting reports mean unknown.
- A separate `SYNCHRONIZE CACHE(10)` probe with `IMMED=0` records whether the device supports durable flushing. Command rejection is captured as `command_failed`, not `transport_failed`.

*Durability classification (Commit 3).*
- `xhci_bot_probe_durability()` runs after block registration and before mount. It stores one of: `SYNC_BACKED`, `WRITE_THROUGH`, `ASSUMED_WRITE_THROUGH`, `READ_ONLY`, or `UNKNOWN` (probe not completed).
- Classification rules: explicit `WCE=0` → `WRITE_THROUGH`; working sync → `SYNC_BACKED`; `WCE=1` with failed sync → `READ_ONLY`; no page and no sync with healthy transport → `ASSUMED_WRITE_THROUGH`.
- `xhci_bot_flush_barrier()` is mode-aware: `SYNC_BACKED` requires the command to succeed; `WRITE_THROUGH` succeeds immediately; `ASSUMED_WRITE_THROUGH` attempts sync and succeeds even if the device rejects, failing only on transport loss. Modes latch to `READ_ONLY` on transport failure and are not silently changed.
- The `[USB DURABILITY]` boot dump prints the raw MODE SENSE attempts, WCE/RCD, sync result, classification, and mount eligibility. When classification is `ASSUMED_WRITE_THROUGH`, a three-line disclosure states that the device does not report cache policy, that write-through is assumed matching Linux and Windows, and that power-loss during writes may lose data.

*Mount eligibility & normal sync (Commit 4).*
- `usb_mount_production_storage()` permits RW for `SYNC_BACKED`, `WRITE_THROUGH`, and `ASSUMED_WRITE_THROUGH`; RO otherwise. It still requires explicit `usb_data_mode=rw`, a matching PARTUUID, USB parent provenance, a strictly consistent or degraded-primary GPT policy, and a successful flush preflight.
- `usb_mount_sync()` flushes the mounted block device without marking the ext2 filesystem clean. It is deliberately separate from `ext2_sync_all()` (which sets `EXT2_VALID_FS` and freezes writes for shutdown).

**Verification Evidence**

*Host ASan/UBSan unit suites:*
- `python3 scripts/test_xhci_bot_host.py`: stall recovery paths, MODE SENSE(6/10) parsing matrices (legal short replies, WCE on/off, write-protect, wrong page/subpage, bad lengths, zero sense, conflicting responses), and the full durability policy table.
- `python3 scripts/test_usb_mount_host.py`: mount eligibility across all durability modes, degraded GPT, missing write/flush callbacks, ext2 RW failure fallback, and `usb_mount_sync()` success/failure.

*QEMU three-boot persistence:*
- `make test-usb-persistence` passed BIOS and paired-OVMF UEFI three-boot create/read/overwrite/delete cycles on disposable 130 MiB USB images, with offline `e2fsck -fn` returning zero after every clean shutdown. The runner validates final QEMU argv (only the disposable USB data device, with read-only firmware and disposable vars permitted). These are clean-shutdown tests with substring content assertions; they do not establish physical power-loss resilience.

**Dell 9G.4 hardware verification (2026-09-19):** user confirmed that Phase 9G.4 passed on physical Dell Latitude 5590 hardware with the Kingston USB DISK 2.0 (VID `0x13FE` PID `0x4200`, 30,320,640 sectors, 512 bytes/sector):
- `MODE SENSE(6) page 0x08: not found` and `MODE SENSE(10) page 0x08: not found`.
- `SYNCHRONIZE CACHE test: failed/unsupported`; command-failed CSW, sense `0/0/0`.
- `Classification: ASSUMED_WRITE_THROUGH`, disclosure printed at boot.
- `Mount mode: read-write`; `[USB 9G.4] PASS: Mounted sdap2 read-write at /mnt`.
- A file written via the editor on the Dell survived a full power cycle (power off, stick physically removed, reinserted, rebooted). Photographic evidence recorded.
- `e2fsck -fn /dev/sda2` on the stick from Linux reported 0 errors on two consecutive runs: one with the device mounted (kernel-cached view, warning noted), one after `umount` (raw on-disk view). File and block counts stable (14 files, 2091/65536 blocks).

The SanDisk USB 3.x stick was also tested and correctly identified as a SuperSpeed device on Port 0x12, then skipped by 9G's scope. This is a correct scope exclusion, not a failure. Strong durability paths (`WRITE_THROUGH`, `SYNC_BACKED`) remain QEMU-verified only.

**What 9G.4 does NOT establish**

- **Strong durability on physical hardware.** No tested stick reports a caching page or accepts `SYNCHRONIZE CACHE`. The `WRITE_THROUGH` and `SYNC_BACKED` paths are QEMU-verified only.
- **Physical power-loss tolerance.** The `ASSUMED_WRITE_THROUGH` disclosure states the boundary explicitly: clean shutdown is assumed durable; power-loss during writes may lose data. Abrupt-stop tests are simulated (mock volatile
  cache, injected flush failures), not physical.
- **USB 3.x devices.** A SanDisk USB 3.x stick was tested on the Dell and correctly identified as a SuperSpeed device on Port 0x12, then skipped by 9G's scope. SuperSpeed support is required to reach the BOT layer with this device.

**Why the strong path is unverified on hardware**

The SanDisk was tested specifically to exercise `WRITE_THROUGH` or `SYNC_BACKED`. It did not enumerate as a USB 2.0 mass-storage device because it is USB 3.x, and 9G's scope explicitly excludes SuperSpeed. This makes SuperSpeed support a prerequisite for physical verification of the strong durability paths — not a convenience, a dependency. That observation motivates Phase 9G.5. µ
**Update (2026-09-20):** SuperSpeed was implemented as Phase 9G.5b, and the strong path was verified on the SanDisk the same day —> classification `SYNC_BACKED` on physical hardware. See §9G.5b.

### Phase 9G.5 — USB Topology Expansion (IN PROGRESS; 9G.5a, 9G.5b complete)

SuperSpeed support, multiple-controller enumeration, and hub support. Sequenced as three sub-projects, each with its own acceptance criteria and its own hardware target.

### Phase 9G.5a — Multiple xHCI Controllers (COMPLETE, 2026-09-19)

FortressOS now enumerates and initializes every xHCI controller the platform exposes, instead of stopping at the first match. On machines with a single controller the behaviour is unchanged; on machines with two, a mass-storage device on either controller is reachable and mountable.

Delivered in three commits:

- **Commit 1 — `xhci_controller_t` struct and array.** The six controller-scoped statics in `xhci.c` (`s_rings_io`, `s_dma`, `s_dev_dma`, `s_bot_rings`, `s_flush_error`, `g_dump_record`) moved into a single `xhci_controller_t` type, held in `s_controllers[XHCI_MAX_CONTROLLERS]`. Only index 0 was used; no PCI
  collection, no loop, no per-controller initialization. External signatures, call graph, and log strings unchanged.

- **Commit 2 — bounded enumeration in PCI discovery.**
  `pci_find_all_devices()` added to `pci.c`, iterating the same topology as the existing `pci_find_device()` but collecting all matches up to a caller-supplied maximum. `pci_report_xhci()` now reports every controller as `xHCI controller N/M: BDF=..., vendor=..., device=...`. Only controller 1 is still initialized. Test runners updated to assert the new log format.

- **Commit 3 — per-controller init loop and active-device selection.**
  `xhci_boot_probe()` now enumerates all controllers and runs the existing init sequence for each via a new private helper `xhci_init_one_controller()`. A new file-scope pointer `s_active_usb_controller` tracks the controller whose mass-storage device is currently registered as `sda`. Per-controller MMIO windows replace the single shared `XHCI_PROBE_VIRT` mapping. A private `xhci_dump_controller_state()` enables per-controller diagnostics without routing through the active pointer. Only the first successfully registered device becomes active; subsequent mass-storage devices are logged as `Mass-storage device found on controller N, but only one active device is supported` and left unregistered.

**Verification:**

- **QEMU:** dual-controller boot with a single stick on the second controller; all five USB suites pass under BIOS and UEFI; three-boot persistence passes with zero filesystem errors.
- **Dell Latitude 5590 (one xHCI controller at `0000:00:14.0`):**
  behaviour identical to commit 2; single `1/1` line; the rest of the boot log unchanged; Kingston mounts RW with `ASSUMED_WRITE_THROUGH`.
- **Dell Latitude 5530 (two xHCI controllers at `0000:00:14.0` and `0000:00:14.2`):** both controllers enumerated and initialized. With the Kingston plugged into a controller-2 port, the device is found on controller 2, registered as `sda`, and mounted read-write at `/mnt`. SuperSpeed device on Port 0x10 correctly skipped as unsupported. USB hub on Port 0x1 correctly rejected as class 0x09. Prior to commit 3, controller 2 was invisible and `/mnt` was never mounted.

**Not established:** the guard path for a second simultaneously attached mass-storage device. The guard logic (`s_active_usb_controller` check before registration) is in the code and structurally verified, but a two-stick boot was not photographed with the guard message visible in the log.

**Deferred:** the "no scrollback on boot" limitation of the framebuffer console means the head of the boot log (the `1/2` and `2/2` lines) is not photographable from hardware without a kernel-log buffer. This is independent of 9G.5a and tracked as a future work item (`dmesg`-style log capture).

### Phase 9G.5b — SuperSpeed enumeration and data transfer (COMPLETE, 2026-09-20)

SuperSpeed (USB 3.x) devices on USB 3.0 ports now enumerate fully, complete BOT transactions, register as block devices, and mount their ext2 partitions read-write on real hardware.

Root cause of the previous SuperSpeed transfer failure: the configuration descriptor walk in xhci_dev.c advanced `off` only inside the endpoint branch, so the loop hung on the first non-endpoint descriptor after the BOT interface. When the hang was fixed, the walk continued into the same interface's UAS alternate setting (protocol 0x62) and its endpoints overwrote the BOT endpoints (0x81 IN, 0x02 OUT) recorded earlier. The driver then sent BOT CBWs to UAS endpoints; the device stalled on the wire; the first transfer returned completion code 0x4 (USB Transaction Error).

Two fixes:
- `off += len` moved out of the endpoint branch so it advances on every descriptor, not just endpoints.
- `found_bot_if` now clears on every non-BOT interface, so a later alternate setting cannot overwrite the endpoints recorded for the BOT interface.

Verified on Dell Latitude 5590 with SanDisk USB 3.2 Gen 1 (VID 0x0781, PID
0x5588) on Port 0x12:
- Device enumerates as SuperSpeed (5 Gbps).
- Configuration descriptor walk shows Interface 0 Alternate 0 (proto 0x50, BOT) with endpoints 0x81 IN and 0x02 OUT, followed by Interface 0 Alternate 1 (proto 0x62, UAS) with the same endpoint addresses plus two others. Only the BOT endpoints are retained.
- SCSI INQUIRY: "SanDisk" / "3.2 Gen 1".
- READ CAPACITY: 241,385,472 sectors × 512 bytes = 117.86 GiB.
- GPT parsed, ext2 partition mounted read-write at /mnt.
- MODE SENSE(6) page 0x08: WCE=1.
- SYNCHRONIZE CACHE test: passed.
- Classification: SYNC_BACKED.
- Mount mode: read-write.

**Not verified:**
- The BOT stall recovery path is not yet invoked from the transfer path. On a device that stalls `SYNCHRONIZE CACHE` or another command, the driver would currently classify the device as `READ_ONLY` rather than treating the stall as a command failure. This does not affect the SanDisk, which does not stall on the commands the driver issues. Tracked as a follow-up
- Other SuperSpeed devices. SuperSpeedPlus (10 Gbps). Other host controllers.

**9G.5c — Strong durability on the SanDisk.** Once the SanDisk enumerates, its MODE SENSE and SYNCHRONIZE CACHE behavior can be read. Success criterion is a classification other than `ASSUMED_WRITE_THROUGH` — either `WRITE_THROUGH` (explicit `WCE=0`) or  SYNC_BACKED` (working flush). If the SanDisk also reports no cache policy, the strong path remains QEMU-only and that is documented as a device-class finding, not a driver defect.

**9G.5d — Persistence on the SanDisk.** Same three-boot test as the Kingston, with `e2fsck` clean after each clean shutdown. This closes the "verified on two independent devices" claim for the persistence path.

**9G.5b debugging update (2026-09-20):** Fixed a descriptor-walk regression in the working SuperSpeed changes: `off += len` was nested inside the bulk-endpoint branch, so parsing never advanced past the configuration descriptor. The host enumeration runner reproduced a 15-second timeout before the fix. After moving advancement outside the branch, `wsl -d Ubuntu-24.04 -- make test-usb-descriptors` passed the ASan/UBSan host suite (including a video-class webcam rejection with successful mocked Disable Slot and no SET_CONFIGURATION) and QEMU BIOS/UEFI present/absent checks with interactive shell startup. `wsl -d Ubuntu-24.04 -- make` rebuilt and verified `bin/fortress.img`. These checks do not establish Dell webcam recovery or SanDisk SuperSpeed bulk acceptance; physical retesting remains required.

**9G.5e — Hubs (deferred).** USB 2.0 and USB 3.x hub support, recursive enumeration, downstream port power sequencing. No hot-plug. Deferred until there is a specific device reachable only through a hub. A USB-C docking hub is available for testing when the sub-project begins.

### Phase 9H — 32 GiB RAM support (2026-09-20)

FortressOS now uses the Dell 5590's full 32 GiB of installed RAM. Previously
the PMM bitmap was capped at 2 GiB (64 KiB bitmap); the kernel could not
allocate frames above that ceiling.

**What was built**

- PMM bitmap `PMM_BITMAP_CAPACITY_BYTES` extended from 64 KiB to 1 MiB,
  covering 32 GiB of physical RAM.
- `pmm_high_memory_probe()` added as a diagnostic: allocates one frame
  above 2/4/16/30 GiB via `pmm_alloc_page_above()`, writes a pattern
  through `vmm_phys_to_virt`, reads back, verifies. Temporary; removed
  after acceptance.
- Two-stage PMM init:
  - `pmm_init` initializes the bitmap for 32 GiB but sets
    `g_alloc_ceiling = 1 GiB`, restricting allocation to safe low memory
    during VMM construction.
  - `vmm_init` builds the kernel PML4, maps the HHDM for all 32 GiB of
    usable RAM, allocates page-table frames from the capped region
    (always reachable via Limine's HHDM), switches CR3, then calls
    `pmm_unlock_high_memory()`.
  - After the CR3 switch, `phys_to_virt` reaches any physical page, so
    PMM can hand out frames across the full 32 GiB.
- PMM audit message and the "memory map exceeds bitmap" warning now
  derive their numbers from `PMM_BITMAP_MAX_RAM_BYTES`.

**Why it was necessary**

Limine's HHDM on the Dell 5590 covers only physical `[0, ~2.5 GiB)`.
Measured by direct read at `hhdm_offset + phys`:

    [HHDM-PROBE] Limine HHDM offset: 0xFFFF800000000000
    0x100000 -> 0x1
    0x40000000 -> 0x0
    0x60000000 -> 0x0
    0x70000000 -> 0x0
    0x78000000 -> 0x0
    0x80000000 -> 0x26
    CPU EXCEPTION KERNEL PANIC
    Faulting Linear Address (CR2): 0xFFFF8000A0000000

`0x80000000` (2 GiB) succeeds; `0xA0000000` (2.5 GiB) raises #PF.
Without the two-stage init, the VMM's own page-table allocations would
fault as soon as PMM handed out a frame above Limine's coverage. The
1 GiB ceiling keeps all early-boot allocations within Limine's window
until the kernel's own HHDM takes over.

**Evidence**

- Boot log (`/mnt/boot.log`, preserved on USB, 2026-09-20, Dell 5590):

    [WARN] PMM: memory map reports RAM above bitmap capacity; clamping to 32 GiB
    [ OK ] PMM initialized:
           Total Physical RAM:  32768 MiB (8388608 frames)
           Usable Free RAM:     31873 MiB (8159717 frames)
           Used/Reserved RAM:   894 MiB (228891 frames)
           Bitmap Location:     Phys 0x100000 (1024 KiB)
    [ OK ] PMM audit passed (bitmap reserved, frame 0 guarded, 32 GiB capacity verified)
    ...
    [ OK ] CR3 switch survived! Kernel running on independent 4-level page tables.
    [PMM] High-memory allocation unlocked
    [PROBE] PMM total: 32 GiB, free: 31 GiB
    Allocated phys 0x80000000 — HHDM readback PASS
    Allocated phys 0x100000000 — HHDM readback PASS
    Allocated phys 0x400000000 — HHDM readback PASS
    Allocated phys 0x780000000 — HHDM readback PASS

- Memory map top range: `[0x100000000 - 0x82E7EC000] Type: Usable RAM
  (30121904 KiB)`, ending at ~32.72 GiB. The bitmap covers 32 GiB; the
  last 0.72 GiB is clamped and warned.

**What this does NOT do**

- The 0.72 GiB above the bitmap ceiling is reserved, not usable. Raising
  `PMM_BITMAP_CAPACITY_BYTES` to 2 MiB would cover 64 GiB and eliminate
  the clamp.
- No user-space API for requesting large memory. Process stacks remain
  4 KiB. This change is a prerequisite, not a feature.
- SMP initialization is unchanged. The two-stage ordering assumes
  bootstrap-CPU-only execution during `vmm_init`.
