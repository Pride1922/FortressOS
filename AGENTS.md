# FortressOS - AI Agent & Developer Architecture Guide

## 1. Read This First

Read [`PROTECTED.md`](PROTECTED.md) first, every session, regardless of task scope — it's the short boundary list this guide's contracts expand on.

Before editing, read [§4 invariants](#4-coding-standards-and-invariants), the subsystem's public header, and the matching [§7 recipe](#7-how-to-add). Read [§9 protected contracts](#9-do-not-touch-without-discussion) before changing boot, synchronization, address-space ownership or test isolation.

| Change touches | Read next |
| --- | --- |
| Locking, scheduling, blocking | L1–L4, S3; [spinlock.h](src/include/spinlock.h), [thread.h](src/kernel/thread.h) |
| Syscalls or user pointers | S1–S4; [syscall.h](src/kernel/syscall.h), [vmm.h](src/mm/vmm.h); §7.1 |
| Interrupts or input | I1–I3; [idt.h](src/arch/x86_64/idt.h), [ioapic.h](src/drivers/ioapic.h); §7.2 |
| Storage or VFS | M1–M4; [block.h](src/drivers/block.h), [vfs.h](src/fs/vfs.h), [ext2.h](src/fs/ext2.h); §7.3–7.4 |
| Tests or user programs | [Makefile](Makefile), §3 and §7.5–7.6 |
| Hardware assumptions or workarounds | [§8 evidence](#8-hardware-facts-and-verification-boundaries), then the driver |

Use `rg` / `rg --files` to locate implementations and callers. Trace indirect calls too; a textual search alone does not establish locking or IRQ safety. This guide states contracts; headers/code define implemented APIs. If they conflict, identify the discrepancy before changing behavior or claiming support.

## 2. Project Overview and Current Work

FortressOS is a freestanding C11/NASM x86_64 kernel using Limine v8, base revision 3, with UEFI and BIOS boot. Kernel virtual base: `0xffffffff80000000`; HHDM offset comes from boot metadata. Hardware subsystems have explicit, separate APIs. Early COM1 and framebuffer diagnostics must work before the heap is available.

History lives in [docs/roadmap/README.md](docs/roadmap/README.md); qualifications and technical debt in [ARCH_REVIEW.md](ARCH_REVIEW.md). Keep new implementation instructions here, checkpoint history there, and verification claims tied to actual evidence.

| Checkpoint | Status / acceptance |
| --- | --- |
| Latest: Phase 9G.5b SuperSpeed enumeration & BOT transport | COMPLETE (2026-09-20). SuperSpeed (USB 3.x) mass storage works end-to-end on physical hardware (Dell 5590 + SanDisk USB 3.2 Gen 1, port and multi-controller variants). Full detail and evidence: [docs/roadmap/phase-9g5-superspeed.md](docs/roadmap/phase-9g5-superspeed.md). |
| Phase 9E Saved File Management & H4 AZERTY Fix | COMPLETE. Directory ops (`mkdir`/`rename`/`unlink`), on-disk inode/block reclamation, Belgian AZERTY scancode fix (Bug H4). Full detail: [docs/roadmap/phase-9e-exec-and-files.md](docs/roadmap/phase-9e-exec-and-files.md). |
| Phase 9D Bounded writable ext2 | COMPLETE. Explicit opt-in writable mount, allocation/truncation ordering, emergency read-only remount, 3-boot BIOS/UEFI persistence. Full detail: [docs/roadmap/phase-9d-writable-ext2.md](docs/roadmap/phase-9d-writable-ext2.md). |
| Phase 9C.5 Power & layout | COMPLETE. ACPI S5 shutdown/reset and US/AZERTY switching, verified on Dell 5590. Full detail: [docs/roadmap/subsystems.md](docs/roadmap/subsystems.md) ("Dell Latitude 5590 physical acceptance"). |
| Phase 9G.1 xHCI Controller & Enumeration | COMPLETE (2026-09-19). PCI discovery, MMIO/reset, Command/Event rings, root ports, device addressing & configuration; verified on QEMU and bare-metal Dell 5590. Full detail: [docs/roadmap/phase-9g1-xhci-enumeration.md](docs/roadmap/phase-9g1-xhci-enumeration.md). |
| Phase 9G.2 Read-only USB block device | COMPLETE (2026-09-19). Bulk-Only Transport, SCSI engine, block device registration, GPT partition parsing; verified on QEMU and bare-metal Dell 5590. Full detail: [docs/roadmap/phase-9g2-usb-block.md](docs/roadmap/phase-9g2-usb-block.md). |
| Phase 9G.3 Production `/mnt` mount | COMPLETE (2026-09-19). Bounded cmdline parsing, PARTUUID-based partition selection, read-only production mount; verified on QEMU and Dell 5590 hardware. Full detail: [docs/roadmap/phase-9g3-usb-mount.md](docs/roadmap/phase-9g3-usb-mount.md). |
| Phase 9G.4 USB writable persistence & durability classification | COMPLETE (2026-09-19). BOT stall recovery, four-tier durability classification, explicit writable opt-in; `/mnt` read-write persistence confirmed on physical USB. Full detail: [docs/roadmap/phase-9g4-usb-durability.md](docs/roadmap/phase-9g4-usb-durability.md). |
| Phase 9H RAM capacity | COMPLETE (2026-09-20). PMM extended to cover 32 GiB, two-stage PMM/VMM init to stay within Limine's HHDM coverage until the kernel PML4 is active. Verified on Dell 5590 (32 GiB) with a write-readback probe. Full detail: [docs/roadmap/phase-9h-ram.md](docs/roadmap/phase-9h-ram.md). |
| Current: multi-core (SMP) support | Pieces 1–3 implemented; Pieces 1–2 verified in QEMU and on Dell 5590 (UEFI, 8 CPUs); Piece 3 (Lock discipline) verified in QEMU, ready for Dell 5590 verification ([details](docs/roadmap/smp-piece3-lock-discipline.md)). APs remain parked; only BSP schedules or uses subsystem locks. Full design, sequencing, and binding invariants (SM IDs) in [`SMP_DESIGN.md`](SMP_DESIGN.md). Piece 4 (SMP Scheduler) is next. Smaller open items not blocking SMP: 9G.5c/d (strong durability on a second device class, persistence on the SanDisk), introspection syscalls + `sysinfo`/`top`/`ps`, persistent rootfs with `/paradise`, shell improvements, MicroPython. |

### Phase 9G implementation handoff

Phase 9G is complete through 9G.5b. The staged plan, per-stage acceptance evidence, and hardware observations are recorded in [docs/roadmap/README.md](docs/roadmap/README.md) (see the 9G phase files). The driver handles USB 2.0 and USB 3.x direct-attached mass storage on any enumerated xHCI controller; USB 3.x devices enumerate as SuperSpeed and complete BOT transport. Hubs, hot-plug, UAS, and non-mass-storage classes remain out of scope. See the "What 9G does NOT do" list below.

#### What 9G does NOT do

- No USB 3.x hub support (SuperSpeed devices on root ports work; devices behind a SuperSpeed hub do not). No SuperSpeedPlus (10 Gbps) verification. No streams.
- No external USB hubs; xHCI root-port management remains required.
- No hot-plug enumeration, reconnection or removal recovery beyond safe failure.
- No UAS, USB keyboards, mice, audio or other non-mass-storage classes.
- No USB power management, suspend or resume.
- No multiple-LUN support: access LUN 0 only.
- No recovery/re-enumeration after a runtime host-controller reset. Initial reset and bounded BOT transport recovery are still required; controller failure leaves storage unavailable until reboot, with DMA safely contained.

#### Explicit USB selection and writable opt-in

Boot arguments are `usb_data=PARTUUID=<unique-partition-guid>` and `usb_data_mode=ro|rw` (default `ro`). Parsing is bounded, and the kernel keeps its own copy of the boot command line under the existing boot-metadata contract. The image builder reports the generated data partition GUID; an explicit writable boot-menu entry displays that target and passes both arguments. The default entry is read-only.

"User-selected test USB" means the user deliberately chooses that configured target and writable entry. Require exactly one matching partition on a supported USB BOT device. A matching filesystem label (`FORTRESS_DATA`), GPT name (`Fortress Persistent Data`) or marker file alone is never write authorization. No first-disk or first-matching-label fallback. Duplicate GUIDs (including two clones of the same image), missing/malformed selection, unsupported media or failed eligibility checks must never produce a writable mount. Without a valid unique target, leave `/mnt` unmounted and explain why; with a selected target but failed RW eligibility, allow only the documented read-only fallback.

The explicit opt-in does not override GPT ambiguity/degraded-mode policy, ext2 validation, write/flush capability checks or the internal NVMe exclusion. Log the selected USB identity, partition GUID and actual mount mode. Tests cover no selection, RO default, explicit RW, wrong GUID, duplicate clones and failed flush capability. Do not auto-enable RW merely because an image was flashed.

#### Bounded first implementation

- USB 2.0 Full-Speed/High-Speed devices on xHCI USB 2.0 ports only. Identify port protocol capabilities rather than assuming port numbers. SuperSpeed slots, streams, USB 3.x port state machines and low-speed storage are out of scope. Test media must actually negotiate a supported speed.
- Devices must be attached at initialization. No hot-plug discovery or reconnection support; reject hubs (class 0x09) with `hub not supported`. Still consume/acknowledge port-status events while polling so they cannot clog the event ring. Unexpected removal must fail safely, not hang or release DMA memory still owned by the controller.
- Bootstrap endpoint zero according to negotiated speed, read the first **8 bytes** of the device descriptor (bMaxPacketSize0 is at byte offset 7), validate/update endpoint-zero packet size and fetch full descriptors. A one-byte read cannot supply bMaxPacketSize0. Validate device and interface descriptors; class may be declared on the interface. Reference: [USB-IF USB 2.0 specification](https://www.usb.org/document-library/usb-20-specification).
- 9G.2 commands: INQUIRY (0x12), TEST UNIT READY (0x00), READ CAPACITY(10) (0x25), READ(10) (0x28), REQUEST SENSE (0x03). Bound retries and implement BOT stall/reset recovery. Initially support LUN 0 only; document GET MAX LUN handling and reject unsupported configurations. Reject UAS explicitly. Defer READ(12/16), MODE SENSE(6/10), REPORT LUNS and larger-capacity command sets; reject READ CAPACITY(10)'s overflow sentinel and unrepresentable LBAs. WRITE(10) (0x2a) and SYNCHRONIZE CACHE(10) (0x35) belong to 9G.4.
- In 9G.2, bulk completion **polls the event ring with a bounded timeout**: no USB completion IRQ dependency, sleeps, re-enabling IF or waiting on another thread. This follows ext2's IRQ-save lock contract (L1 and §9). A timeout propagates an I/O error and initiates bounded quiescence or DMA quarantine; it does not permit immediate reuse/free of active buffers.
- 9G.2 owns 512/4096-byte sector integration tests against GPT and ext2; reject other sizes explicitly. The current boot image is laid out in 512-byte LBAs: do not reinterpret it as a 4096-byte-sector image. Use separately generated matching-geometry fixtures for 4096-byte tests.

#### 9G.1 checkpoints and debugging

| Checkpoint | Required evidence | Known failure modes and response |
| --- | --- | --- |
| 9G.1a PCI discovery only | Implemented: report the first matching xHCI BDF, vendor/device and assigned BAR metadata near shell startup. BIOS/UEFI present/absent QEMU checks pass. Dell photo: 0000:00:14.0, 8086:9D2F, BAR0 0xEF330000, memory64, non-prefetchable; shell prompt reached. BAR extent and controller MMIO remain unverified; never hardcode these observed values. | No controller or invalid/unsupported BAR: report unavailable and return without probing an unvalidated MMIO address. |
| 9G.1b MMIO and reset | Verified in QEMU and Dell hardware: sized UC/NX aperture, capability/offset validation, bounded handoff/halt/reset, captured register diagnostics. Controller remains stopped with PCI mastering/decode disabled; no DMA buffers or rings. PS/2 input responsive. | No legacy handoff capability means no semaphore to wait for; stuck ownership, halt, HCRST or not-ready state must time out, record the failing register and disable this controller path. |
| 9G.1c Rings | Verified in QEMU and Dell hardware: command/event rings, Link TRB toggle cycle, port status event consumption, and No-Op Command Completion Event verified. Bounded event polling with interrupts disabled. | No completion before deadline or unexpected completion code/command pointer: record TRB and ring positions, fail the checkpoint, and quiesce/quarantine DMA rather than proceeding. |
| 9G.1d Ports | Verified in QEMU and Dell hardware: protocol mapping, root port inspection, USB 2.0 port reset, and speed negotiation verified. SuperSpeed attachments isolated. | Connected but unpowered: check power-switching capability and perform bounded supported power/reset sequencing; unresolved state fails that port. SuperSpeed attachment is logged as unsupported and skipped. |
| 9G.1e Descriptors | Verified in QEMU (both BIOS & UEFI), host ASan/UBSan, and Dell Latitude 5590 hardware (2026-09-19): DCBAA/scratchpad initialization, Enable Slot, Address Device, EP0 Control Transfers, Device Descriptor, Configuration Descriptor parsing, non-storage port filtering (Port 5 webcam 0x0E, Port 7 rejected), BOT validation, Bulk-In EP 0x81 (max 512), Bulk-Out EP 0x02 (max 512) on Slot 0x3 Port 0x9 (VID 0x13FE, PID 0x4200), and SET_CONFIGURATION(1) verified. Interactive shell prompt reached. | Short/all-0xFF response, invalid bLength (including zero), descriptor type other than DEVICE (1), or invalid packet size: reject before further parsing, record the reason, and do not publish a device. |

Before the first transfer, add a bounded `usb_dump_state()` diagnostic callable **only from thread context, with no subsystem or console lock held**, using the normal console/serial path. Timeout/error paths never call it: they copy bounded already-available state into a preallocated diagnostic record and publish a pending flag without allocation, logging or acquiring another lock. A thread consumes that record after transfer/FS locks have been released; use the existing IRQ-excluded publication discipline and preserve the record until consumed. Do not dereference stale controller/DMA pointers when printing. The record covers controller run/halt state, port state, software command enqueue/event dequeue positions and cycle bits, and last submitted/completed TRBs. Distinguish software bookkeeping from controller-owned positions that cannot be read directly; do not invent a hardware producer index. Capture QEMU serial logs and comparable Dell framebuffer diagnostics. Never print inside an ordinary IRQ handler or recursively acquire console locks. Use gated snapshots on failure/on demand rather than unconditional per-transfer logging. Existing QEMU launch recipes provide xHCI/USB attachment, but no event ring debugger or USB acceptance runner is implemented yet.


#### Known unknowns to record during bring-up

- Does the Dell expose a BIOS/OS ownership semaphore, and what handoff is needed?
- What controller state does firmware leave, and does bounded reset succeed?
- Does the selected stick negotiate Full-Speed, High-Speed or unsupported SuperSpeed, and is its actual topology directly attached?
- Does it expose BOT or UAS, which LUNs, and 512- or 4096-byte logical sectors?
- Does the stick support the required cache synchronization semantics?

These are measurements for 9G.1/9G.2 (flush capability for 9G.4), not assumed hardware facts. Record observed values with the device and test environment.

Implementation constraints and verification:

- Read §4, §7.2–7.5 and §9, plus `pci.h`, `block.h`, `gpt.h`, `ext2.h`, `vfs.h`, VMM and synchronization headers before changing the related code. Preserve lock ranks, bounded IRQ work, DMA ownership/quiescence and the internal physical NVMe exclusion. No automatic formatting or raw-pattern tests on hardware; only the deliberately selected USB data partition may become writable under the explicit mount policy.
- Trace ext2-to-block calls before choosing USB completion handling: ext2 holds its ranked IRQ-save lock around I/O. Do not introduce sleeping or interrupt-dependent waits beneath that lock. Any synchronization redesign must follow §9 discussion requirements.
- Test the 130 MiB image on a larger disposable device as well as at exact image size. The GPT backup remains at the image boundary after a raw copy, while the current parser probes the device's last sector. 9G.3 owns this policy: accept a fully validated primary header AND partition array in degraded read-only mode when the end-of-device backup is absent/invalid; log the declared backup LBA and actual last LBA as a possible raw-copy size mismatch. Do not label an unverified mismatch definitively a raw copy. Preserve rejection of two valid but conflicting GPTs and the existing validated read-only backup fallback. Reject when neither copy validates. Never silently repair/resize disks. 9G.4 must explicitly resolve writable eligibility for the as-flashed layout and test it; RO acceptance alone does not authorize writes or partition-table repair.
- New USB persistence runners must use disposable copies and omit the NVMe fixture so `/mnt` cannot accidentally come from it. The existing `run-img*` targets attach a separate NVMe fixture and prove boot only. Name new USB tests `test-usb-*`; existing storage tests retain their NVMe fixture scope, and future `test-img-*` tests prove image boot only. Add a Makefile/runner preflight assertion over the final QEMU arguments: only the disposable USB data disk is allowed, with paired read-only OVMF code and disposable OVMF vars as firmware exceptions. Reject extra data disks, including NVMe and injected `-drive`/`-blockdev` backends or extra arguments. Add appropriate host failure tests and bounded QEMU targets following §7.5; run relevant existing ext2, storage, shell and power regressions.
- Preserve historical results and label new evidence by command, firmware, image/device and result. QEMU USB success does not establish Dell USB acceptance. Update this status and the matching file under `docs/roadmap/` after each completed stage.

## 3. Build, Run, Debug and Verify

Run Linux tools in WSL `Ubuntu-24.04` at `/mnt/c/Sources/FortressOS` (or a Linux checkout). Prerequisites: GCC/binutils, NASM, make, xorriso, git, QEMU x86, OVMF, Python 3, e2fsprogs; GDB for interactive debugging. No hosted runtime in the OS.

```bash
sudo apt-get install -y build-essential nasm xorriso qemu-system-x86 ovmf git curl e2fsprogs python3 gdb mtools dosfstools
make                         # bin/fortress.elf, bin/initramfs.tar, bin/fortress.iso, bin/fortress.img
make run                     # QEMU q35, 2 GiB, COM1, paired OVMF when available (ISO)
make run-bios                # Legacy BIOS (ISO)
make run-img                 # Boot raw disk image (bin/fortress.img) under UEFI
make run-img-bios            # Boot raw disk image (bin/fortress.img) under legacy BIOS
make run-img-usb             # Boot raw disk image emulated as a USB flash drive (UEFI)
make debug                   # Frozen QEMU, GDB port 1234
gdb bin/fortress.elf -ex "target remote :1234" -ex "break _start" -ex "continue"

# Flash raw disk image to physical USB drive for bare-metal testing (e.g. Dell Latitude 5590):
# sudo dd if=bin/fortress.img of=/dev/sdX bs=4M status=progress conv=fdatasync
```

From PowerShell: `wsl -d Ubuntu-24.04 -- make` (workspace is the current directory). `make clean` removes build/ISO outputs; `make distclean` also removes downloaded Limine/OVMF. Use only when needed. `make` fetches missing Limine dependencies.

| Target | Scope / evidence |
| --- | --- |
| `make test-input` | Host ASan/UBSan: decoder, modifiers and bounded FIFO |
| `make test-usb-discovery` | 9G.1a BIOS/UEFI PCI discovery with/without xHCI, shell startup without NVMe; ISO boot only, no data disk. No USB transfers or persistence claimed. |
| `make test-console` | Host ASan/UBSan: pixel output, wrapping, scrolling and bounds |
| `make test-ext2` | Host ASan/UBSan: actual ext2/VFS, malformed images, I/O/OOM paths |
| `make test-ext2-write` | QEMU ext2 file creation, editor save, host `e2fsck -fn` integrity, and cross-boot persistence on disposable NVMe GPT fixture |
| `make test-storage` | BIOS/UEFI GPT/ext2, Ring 3 reads, allocation-set audits; `build/storage-*.log` |
| `make test-shell` | BIOS/UEFI IRQ1/IRQ4 interaction, sleeping readers, restart counts; also UEFI 8 GiB without COM1 |
| `make test-nmi` | 7 exact syscall/SWAPGS boundaries × 4 rounds × 2 firmware modes (BSP); `build/nmi-*.json` and `.log` |
| `make test-smp-percpu` | BIOS/UEFI with 1/4/8 CPUs: CPU-local GS/GDT/TSS/stacks, AP #DF, real NMI delivery on every CPU, unchanged BSP IST/TSS/GDT and shell startup; snapshot NVMe fixture |
| `make test-boot-diagnostics` | UEFI 8 GiB, no COM1; progress to PCI discovery and framebuffer capture |
| `make test-power` | QEMU shutdown/reboot command tests; physical ACPI S5 confirmed separately on Dell 5590 (see §8 H7), not by this target |
| `make test-usb-descriptors` | 9G.1e host ASan/UBSan + QEMU BIOS/UEFI descriptor parsing, BOT class validation, device configuration |
| `make test-usb-block` | 9G.2 BOT host tests + QEMU sector read/GPT registration, no NVMe fixture |
| `make test-usb-mount` | 9G.3 mount policy host tests + QEMU BIOS/UEFI PARTUUID selection and read-only mount |
| `make test-usb-persistence` | 9G.4 QEMU BIOS/UEFI three-boot create/read/overwrite/delete with offline `e2fsck -fn` on disposable 130 MiB images |
| `make test-xhci-bot-host` | Host ASan/UBSan: BOT stall recovery, MODE SENSE parsing, durability policy table |
| `make test-usb-mount-host` | Host ASan/UBSan: mount eligibility, durability modes, sync path |

Choose tests relevant to the change, then required integration coverage. Report commands actually run and their limits; an existing test target is not a new pass. QEMU storage tests use disposable fixtures/snapshots; never point raw-write tests at a real disk. `build/nvme_raw.img` and `build/nvme_gpt.img` serve different tests.

## 4. Coding Standards and Invariants

### Freestanding and ABI rules

- Kernel/user code: project headers or compiler freestanding headers only; no hosted `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<unistd.h>` or `<sys/...>`. Host tools/tests are separate and may use their host runtime.
- Keep Makefile strict warnings (`-Wall -Wextra -Werror`), freestanding flags, `-mno-red-zone`, and disabled x87/MMX/SSE. SIMD state management is not implemented. Kernel uses `-fPIE`; the standalone C shell overrides with `-fno-pie`.
- NASM: `[bits 64]`, `default rel`. SysV C arguments: RDI, RSI, RDX, RCX, R8, R9; preserve RBX/RBP/R12–R15/RSP; align RSP to 16 bytes **before** every call.
- Inline assembly needs correct operands/clobbers and `volatile` for hardware effects; include `"memory"` when required. Use explicit fixed-width types, alignment and overflow-safe bounds before dereferencing or computing offsets.

### Locking and lifecycle

| ID | Binding invariant | How to check |
| --- | --- | --- |
| L1 | Acquire increasing ranks: sched **or** ext2 (1) → heap (2) → VMM (3) → PMM (4) → console (5). Sched/ext2 cannot nest. Release LIFO. | Trace nested calls from `spin_lock_irqsave`; verify `SPINLOCK_RANKED` values and `spin_debug_selftest`. Violations panic/deadlock, not warnings. |
| L2 | No spinlock across `switch_context`. Keep IRQs disabled through target TSS.RSP0, CR3 and stack exchange until saved incoming flags restore. | Check every switch site for `spin_unlock_noirq` and `spin_debug_assert_unheld`; exercise preemption and sleeping reads. |
| L3 | Locks are non-recursive; `_unlocked` helpers avoid reacquisition. Saved 64-bit RFLAGS belongs to the caller. Tracking is bootstrap-CPU-only. | Inspect public-to-public calls, error exits and saved flags; never share an IRQ-save token. |
| L4 | Detach dead tasks under sched lock; free outside it, on another stack and CR3. | Inspect `sched_reap_dead` assertions and all unwind paths; run lifecycle/reclamation tests. |

### Syscalls and user memory

| ID | Binding invariant | How to check |
| --- | --- | --- |
| S1 | ABI: RAX number/result; RDI/RSI/RDX/R10/R8/R9 arguments. Fast entry clobbers RCX/R11. Normal dispatch writes `frame->rax`; return uses the existing stub. | Compare `syscall.h`, dispatch and Ring 3 callers; preserve assembly frame layout. |
| S2 | Validate every user range via `vmm_validate_user_range` before access; kernel-written buffers require `write_req=true`. Bound sizes and strings. | Every syscall case that dereferences a user pointer calls `vmm_validate_user_range` first. Grep for `frame->rdi`/`rsi`/`rdx` and confirm each is preceded by a validate call. Trace every pointer, page crossing and arithmetic operation; test unmapped, read-only, kernel, zero-length and overflow cases. |
| S3 | Fast entry masks IF and switches from user RSP before any stack access. | Preserved in `syscall_entry.asm` (SWAPGS, RSP switch, IF masked by SFMASK). |
| S3a | Blocking stdin does NOT enable IF before sleeping. Predicate check, BLOCKED insertion and dequeue are IRQ-excluded. | Follow `input_read` → `sched_wait_until`; verify IRQ state on resume and a blocked reader with advancing timer. |
| S3b | Sleep releases all locks before switching. Wake rechecks predicate. | Check every switch site for `spin_unlock_noirq` and verify wait predicate is checked in loop. |
| S3c | Do not invent a `wait_queue_sleep` API — see `input_read` / `sched_wait_until`. | Use existing `sched_wait_until` / `sched_wake_all` primitives; do not add generic ad-hoc blocking helpers. |
| S4 | Validate canonical lower-half RIP/RSP (strictly below `0x0000800000000000`, at least one page); sanitize return RFLAGS before SYSRET. | Keep IOPL/NT/TF/VM stripped and IF/bit 1 forced; run hostile-state cases and `make test-nmi` for entry/exit changes. Canonical does not mean mapped. |

### Interrupts and deferred work

| ID | Binding invariant | How to check |
| --- | --- | --- |
| I1 | Ordinary device IRQ handlers do bounded draining/queue publication/wakeup only: no allocation, blocking, context switch or normal logging. Timer preemption is a deliberate exception. | Every `idt_register_hardware_handler` callback must not call `kmalloc`, `vmm_map`, `sched_wake_all` (except the timer path), or `console_*`/`serial_*`. Grep the handler body. |
| I2 | Exactly one EOI owner. `idt_register_hardware_handler` makes the dispatcher own EOI. Timer uses `idt_register_handler` and issues EOI **before** scheduling. Spurious APIC IRQ gets no EOI. | Every `lapic_eoi()` call site is either inside the IDT dispatcher (via `g_needs_eoi`) or in `apic_timer_handler` before scheduling. No other caller. Check registration and handler together; never convert timer registration blindly. |
| I3 | ISR publication precedes wakeup; processing occurs in a runnable thread. Timer/idle schedules it; no universal deferred-work-at-next-tick API exists. NMI remains lockless/non-scheduling and uses raw UART. | Trace producer/consumer and wake races; inspect NMI transitive calls for subsystem locks or console output. |

### Memory, ownership and storage boundaries

| ID | Binding invariant | How to check |
| --- | --- | --- |
| M1 | Never dereference raw physical addresses. Use runtime HHDM translation for mapped RAM; map MMIO explicitly with the driver's required cache/NX flags. | No `(void *)phys_addr` or `*(phys_addr)` cast appears outside the HHDM translate helper. Grep for casts to `void *` and confirm each is either an HHDM translation (`vmm_phys_to_virt`) or an explicit MMIO map. |
| M2 | HHDM offset is boot-provided, never a constant. Use kernel-owned boot metadata after handoff. | Inspect `boot_info` and `vmm_phys_to_virt`; reject missing Limine responses before reading fields. |
| M3 | VMM owns tables; caller owns data frames. Destroy refuses kernel/active CR3, never frees shared higher half; `free_user_frames=true` requires exclusively owned, singly mapped leaf frames. | Read `vmm.h` ownership/prevalidation contract; check rollback and exact allocation-set/table audits, not just equal counts. |
| M4 | Bound all block/partition/parser arithmetic and hardware waits; publish only fully validated state. Never free DMA memory while a controller may still use it. | Inspect lower-layer dispatch on rejected requests, NVMe quiesce/quarantine, GPT staging, ext2 malformed-input tests and rollback. |

Thread stacks have a 4 KiB lower guard and 16 KiB usable space. A guard catches contiguous downward exhaustion, not every large frame skip. Ring 0 #PF (IST=0) uses active RSP; Ring 3 privilege transitions use TSS.RSP0. #DF uses IST1 and NMI uses IST2. These diagnostic stacks do not guarantee survival if their mappings, TSS, IDT, handler or diagnostic path is damaged. Keep that qualification.

## 5. File Map

```
FortressOS/
├── .gitignore               # Ignores build outputs, ISOs, and external bootloader binaries
├── AGENTS.md                # Task routing, binding invariants, recipes and evidence
├── PROTECTED.md             # Short "do not touch without discussion" boundary list
├── ROADMAP.md               # Stub redirecting to docs/roadmap/README.md
├── ARCH_REVIEW.md           # Architecture audit, limits and technical debt
├── SMP_DESIGN.md            # Multi-core design: six pieces, binding invariants (SM IDs)
├── docs/
│   └── roadmap/             # Per-phase implementation notes and verification evidence
├── scripts/                 # Host/QEMU verification and disposable disk fixtures
├── tests/                   # Host tests and mocks
├── Makefile                 # Automated compilation, bootloader fetch, ISO packaging, and QEMU run
├── limine.conf              # Limine bootloader configuration menu and kernel path
├── linker.ld                # x86_64 higher-half linker script (4KiB section alignment, Limine markers)
├── src/
│   ├── arch/
│   │   └── x86_64/
│   │       ├── boot.asm         # Early assembly crt0 entry stub, aligns stack, invokes kmain
│   │       ├── gdt.h            # GDT, TSS, and segment selector structures
│   │       ├── apic.c           # Local APIC and APIC Timer initialization & MMIO access
│   │       ├── apic.h           # LAPIC registers, offsets, MSRs, and timer prototypes
│   │       ├── context.asm      # Low-level switch_context and thread_trampoline assembly stubs
│   │       ├── gdt.c            # GDT, TSS RSP0, IST1 (#DF) and IST2 (NMI)
│   │       ├── gdt_flush.asm    # lgdt, segment reloads (CS/DS/SS/ES), and ltr
│   │       ├── idt.h            # IDT descriptor, interrupt_frame_t, and IRQ handler registry
│   │       ├── idt.c            # IDT table setup, exception diagnostics, and IRQ dispatch
│   │       ├── interrupts.asm   # Exception/IRQ stubs and register preservation
│   │       ├── msr.h            # MSR read/write inlines, register addresses, and bit flags
│   │       └── syscall_entry.asm# Low-level fast syscall entry stub and sysretq dispatcher
│   ├── drivers/
│   │   ├── acpi.c           # RSDP, RSDT/XSDT validation, and MADT parsing
│   │   ├── acpi.h           # ACPI table headers, RSDP, and MADT structure definitions
│   │   ├── block.c          # Abstract block device subsystem & device registry
│   │   ├── block.h          # block_dev_t descriptor, sector operations, and registration API
│   │   ├── console.c/.h     # Cached framebuffer text console
│   │   ├── font.h           # Embedded 8x16 font
│   │   ├── input.c/.h       # IRQ input and blocking stdin
│   │   ├── input_buffer.h   # Bounded FIFO
│   │   ├── keyboard.c/.h    # Translated scancodes and layout tables
│   │   ├── power.c/.h       # ACPI shutdown and reset fallbacks
│   │   ├── ioapic.c         # I/O APIC discovery, MMIO registers, and redirection table masking
│   │   ├── ioapic.h         # I/O APIC controller definitions and routing prototypes
│   │   ├── nvme.c           # PCIe NVMe storage driver, Admin/IO queues, dynamic doorbells
│   │   ├── nvme.h           # NVMe register structures, SQE/CQE, and sector read/write/flush API
│   │   ├── pci.c            # PCI configuration access (ECAM MCFG & legacy 0xCF8/0xCFC)
│   │   ├── pci.h            # PCI device descriptors, class codes, and configuration prototypes
│   │   ├── pic.c            # 8259 PIC masking and disable logic
│   │   ├── pic.h            # 8259 PIC port definitions and mask queries
│   │   ├── serial.c         # UART 16550 COM1 port I/O driver (115200 8N1)
│   │   ├── serial.h         # Serial driver headers and port I/O inlines (inb, outb, io_wait)
│   │   ├── xhci.c           # xHCI controller: PCI, MMIO, reset, command/event rings, root ports
│   │   ├── xhci.h           # Controller/ring/port structures and public probe API
│   │   ├── xhci_trb.h       # Transfer Request Block layout, TRB types, completion codes, ERST
│   │   ├── xhci_dev.c       # Device addressing: DCBAA, scratchpads, slot/EP contexts, descriptors
│   │   ├── xhci_dev.h       # Context structures, USB descriptor layout, BOT device report
│   │   ├── xhci_bot.c       # USB Mass Storage BOT transport, SCSI engine, block device adapter
│   │   └── xhci_bot.h       # CBW/CSW, SCSI opcodes, durability classification state machine
│   ├── fs/
│   │   ├── ext2.c/.h        # ext2 mount, file/directory operations (read-only and bounded-writable)
│   │   ├── gpt.c            # GPT partition table parser, Protective MBR, and bounded partition devices
│   │   ├── gpt.h            # GPT header, partition entry structures, and GUID definitions
│   │   ├── usb_mount.c/.h   # Production /mnt selection: PARTUUID match, provenance, mount policy
│   │   ├── tarfs.c          # Read-only USTAR archive parser for initramfs
│   │   ├── tarfs.h          # USTAR tar format headers
│   │   ├── vfs.c            # Virtual File System tree, lookup, file descriptors, stat/readdir, mkdir/unlink/rename
│   │   └── vfs.h            # VFS node structures, file handle descriptors, and public API
│   ├── include/
│   │   ├── boot_info.h      # Kernel-owned boot information and memory map snapshot
│   │   ├── limine.h         # Official Limine bootloader protocol specification
│   │   ├── spinlock.h       # IRQ-save locks and rank contract
│   │   ├── string.h         # Freestanding memory and string manipulation prototypes
│   │   └── types.h          # Standard freestanding primitive types (uint8_t, size_t, bool)
│   ├── kernel/
│   │   ├── boot_info.c      # Boot metadata deep-copying and verification
│   │   ├── elf.c            # Strict ELF64 executable validation, mapping, and loading
│   │   ├── elf.h            # ELF64 header, program header, limits, and loader API
│   │   ├── embedded_init.asm# Embedded user init ELF binary blob via incbin
│   │   ├── main.c           # Kernel entry point (kmain), validates Limine tags, memory & FB
│   │   ├── spinlock.c       # Bootstrap-CPU lock discipline checks
│   │   ├── syscall.c        # System call dispatcher, range validation, and handlers
│   │   ├── syscall.h        # System call numbers, ABI register mappings, and error codes
│   │   ├── thread.c         # Preemptive scheduler, run/wait queues and process lifecycle
│   │   └── thread.h         # TCB structure, thread_state_t, and scheduler prototypes
│   ├── lib/
│   │   ├── crc32.c/.h       # GPT CRC32
│   │   └── string.c         # Freestanding memset, memcpy, memmove, memcmp, strlen
│   └── mm/
│       ├── heap.c           # Dynamic kernel heap allocator with boundary tags and free list
│       ├── heap.h           # Heap public prototypes, block structures, and alignment macros
│       ├── pmm.c            # Physical Memory Manager bitmap frame allocator
│       ├── pmm.h            # PMM public prototypes, page macros, and metrics
│       ├── vmm.c            # Virtual Memory Manager 4-level paging and CR3 management
│       └── vmm.h            # VMM public prototypes, PTE flags, and query APIs
└── user/                    # Standalone programs, outside src/
    ├── init.asm             # Standalone ELF64 user init program (Ring 3 execution test)
    ├── hello.asm            # Standalone hello program
    ├── shell.c              # Interactive Ring 3 shell
    ├── shell_start.asm      # Shell entry and ABI alignment
    ├── shell.ld             # Shell ELF segment layout
    └── linker.ld            # Assembly test programs, page-separated segments
```

## 6. Limine Notes

- Base revision 3; verify `LIMINE_BASE_REVISION_SUPPORTED` and non-NULL responses.
- Keep requests between `.requests_start_marker` / `.requests_end_marker` and linker `KEEP` directives. Read [boot_info.h](src/include/boot_info.h) for snapshots.
- Initramfs module backing memory stays reserved (`KERNEL_AND_MODULES`); tarfs nodes reference it directly. Copying metadata does not copy module contents.

## 7. How to Add

### 7.1 A syscall

Canonical examples: `SYS_STAT` in [syscall.c](src/kernel/syscall.c); blocking `SYS_READ` in [input.c](src/drivers/input.c).

1. Read `syscall.h`, caller code and S1–S4; choose an unused number and update ABI docs.
2. Add the handler and dispatch case; define argument bounds and negative errors.
3. Validate every user buffer/string before access; request writable pages for outputs.
4. For blocking: follow `input_read`/`sched_wait_until` exactly (see S3/S3a/S3b). Do not paraphrase the contract — read the invariant.
5. Return through dispatch/`frame->rax`; terminal process/power operations use their existing non-returning lifecycle.
6. Add Ring 3 success/error/boundary coverage in a suitable test program; extend the appropriate BIOS/UEFI runner (often `test-shell`).
7. Audit acquired resources on failure/exit; use PMM bitmap/table/mapping and heap checks where ownership changes. Run NMI tests if entry/exit changes.

### 7.2 An IRQ handler

Canonical example: `keyboard_irq` / `serial_irq` in [input.c](src/drivers/input.c); timer is the explicit I1/I2 exception.

1. Read `idt.h`, `ioapic.h`, device source and I1–I3; choose a nonconflicting vector.
2. Initialize bounded device buffers and register the handler before enabling delivery.
3. Use `idt_register_hardware_handler` for ordinary device IRQs; omit EOI in its body. Exception: spurious APIC vector is registered via a path that suppresses EOI (`idt_register_handler` without EOI). If your handler must not acknowledge, use that path and document why.
4. Route ISA through `ioapic_route_isa` to respect MADT overrides; serialize IOAPIC access with IRQs disabled as its header requires.
5. Drain a bounded batch, publish queue/flag state, wake waiters; defer substantial work to a thread. No printing/allocating/switching in the device handler.
6. Test real device delivery, repeated events, overflow and sleep/wake races; verify timer progress and firmware coverage. Direct calls alone do not prove routing.

### 7.3 A block device

Canonical examples: [block.c](src/drivers/block.c), [nvme.c](src/drivers/nvme.c), bounded partition adapter in [gpt.c](src/fs/gpt.c), USB BOT adapter and registration in [xhci_bot.c](src/drivers/xhci_bot.c).

1. Read `block.h`, M1/M4 and the driver lifecycle before allocating MMIO/DMA resources.
2. Provide actual `sector_size`/`sector_count`; check multiplication/addition overflow before capacity/range use.
3. Implement bounded sector callbacks and applicable flush; read-only devices leave write/flush NULL. A device whose write path may not be usable yet (e.g. USB durability not classified) must still expose read callbacks and leave write/flush NULL until eligibility is established.
4. Validate partition-relative bounds before parent dispatch; validate all entries before registry publication.
5. Register only fully initialized devices; unregister and unwind on failure, respecting references and DMA quiescence/quarantine. A device that has latched into an unrecoverable error state must not be re-registered without a documented recovery path.
6. Test first/last/out-of-range I/O, failure cleanup and registry state using mocks/disposable images; never enable raw patterns on hardware or GPT fixtures. USB-specific tests use disposable 130 MiB images under `test-usb-*` runners that must assert no other data disk is attached.

### 7.4 A VFS node or file operation

Canonical examples: [vfs.c](src/fs/vfs.c), [tarfs.c](src/fs/tarfs.c), [ext2.c](src/fs/ext2.c), mount policy in [usb_mount.c](src/fs/usb_mount.c).

1. Read `vfs.h`/filesystem headers: shared `vfs_node_t` differs from `file_t` with its own open offset/reference count.
2. Define backing-store lifetime, node ownership and supported operations. ext2 read-only mounts persist for boot lifetime; writable mounts must be explicitly opted in, tainted on unrecoverable I/O or flush failure, and frozen by clean shutdown before the backing device is released.
3. Bound paths, names, directory records and read sizes; return the existing errors/EOF semantics. Distinguish read-only policy (`-EROFS`) from tainted failure state (`-EIO`); do not collapse them into a single error.
4. Wire callbacks and per-process descriptors without sharing offsets between independent opens. If a mount is conditional on external state (durability classification, PARTUUID match, provenance), reject or fall back to read-only before the first filesystem write — never dirty the superblock and then discover the mount was ineligible.
5. Close/unwind references on failure and exit; keep `fd_close_all`/reaper cleanup valid. A tainted or read-only mount must still allow reads through the same paths.
6. Test dual opens, short reads/EOF, directory iteration, malformed backing data and Ring 3 writable-output validation. For conditional writable mounts, test each eligibility outcome (eligible, degraded, ineligible, unknown) and assert zero filesystem writes occur on any rejected path. Use host tests plus relevant boot tests.

### 7.5 A test / make target

Canonical examples: [test_ext2.py](scripts/test_ext2.py), [test_shell.py](scripts/test_shell.py), [test_nmi_transitions.py](scripts/test_nmi_transitions.py), [test_usb_persistence.py](scripts/test_usb_persistence.py).

1. Choose host sanitizer coverage for parsers/pure logic, QEMU for CPU/device delivery, manual hardware evidence for physical claims. Do not let a QEMU pass stand in for a physical acceptance criterion.
2. Test behavior and failure boundaries using actual subsystem code; label mocks explicitly. USB tests must distinguish "enumerated" from "usable" from "persisted" — each is a separate acceptance claim.
3. Add a `test-X` Makefile target with real build/fixture dependencies; keep destructive tests explicitly gated. USB persistence tests use disposable image copies, never physical disks or the shared build fixtures.
4. Bound waits, use disposable/snapshot disks, pair OVMF code/vars, capture logs and always terminate test QEMU. For multi-boot runners, validate the final QEMU argv on every boot — assert that only the intended data disk is attached, so `/mnt` cannot accidentally come from an unexpected device.
5. Assert specific results (not just a boot banner); include resource ownership checks when allocating/reclaiming, and offline verification (e.g. `e2fsck -fn` on a disposable copy) where an independent implementation can validate the outcome. Substring checks are not byte-for-byte comparisons; state which one a given test performs.
6. Record exact invocation, result and evidence boundary in the task report/roadmap; update this routing table only if needed. When a new QEMU target is added, note the firmware modes it covers and whether it attaches any fixture data disk.

### 7.6 A user program in initramfs

Canonical examples: [shell.c](user/shell.c), [shell_start.asm](user/shell_start.asm), [shell.ld](user/shell.ld), Makefile `USER_SHELL_ELF`.

1. Place sources under root `user/`, read the syscall ABI, and use freestanding flags with no red zone/SIMD/host runtime.
2. Provide a correctly aligned entry stub and loader-supported static ELF64 with page-separated permission segments.
3. Add explicit ELF source/header/linker dependencies and an initramfs archive dependency.
4. Copy the ELF to staging `/bin/<name>` and regenerate USTAR; preserve strict tar format constraints.
5. Launch through the existing `process_spawn` lifecycle/test harness. Merely adding `/bin/foo` does not add a shell exec command; no exec syscall exists yet.
6. Verify Ring 3 execution, syscall results, faults/exit and deferred resource reclamation in BIOS/UEFI.

### 7.7 A USB mass-storage driver addition

Canonical examples: [xhci.c](src/drivers/xhci.c) (controller), [xhci_dev.c](src/drivers/xhci_dev.c) (device and descriptors), [xhci_bot.c](src/drivers/xhci_bot.c) (BOT transport and SCSI), [xhci_trb.h](src/drivers/xhci_trb.h) (TRB layout).

The USB stack is split across three source files, each with a distinct responsibility. Keep additions inside the correct file.

BOT stall recovery now handles a matching data/CSW STALL with bounded Reset
Endpoint, EP0 CLEAR_FEATURE(ENDPOINT_HALT), and Set TR Dequeue Pointer. Retry
CSW once; never replay a stalled data OUT payload. Enumeration hands the EP0
producer index/cycle to the selected BOT device; runtime recovery must continue
that ring, never reuse index zero or borrow the event-ring cycle. A CBW stall,
timeout, malformed CSW, mismatched completion or second CSW stall sets
`transport_failed`; a failed recovery step also sets `latched_offline`. Both
block further BOT submissions. A recovered STALL alone is not command failure:
only a valid failed CSW sets `command_failed`. This implementation does not add
full BOT class-reset recovery or change writable mount authorization.

- **`xhci.c` / `xhci.h`** — controller-level only. PCI discovery, BAR mapping, BIOS-to-OS ownership handoff, halt/reset, command ring, event ring, ERST, root-port protocol mapping and reset. It knows nothing about USB classes or SCSI. It exposes the controller state and the DMA ring structures that the next layer uses.
- **`xhci_dev.c` / `xhci_dev.h`** — device-level. DCBAA and scratchpad setup, `Enable Slot`, slot and endpoint context construction, `Address Device`, EP0 control transfers, standard descriptor parsing (device, configuration, interface, endpoint), and **class filtering**. This file decides whether an enumerated device is a BOT mass-storage device (class `0x08`, subclass `0x06`, protocol `0x50`) or something to reject and disable. It does not transfer sector data.
- **`xhci_bot.c` / `xhci_bot.h`** — transport and SCSI. CBW/CSW exchange over the bulk endpoints, the SCSI command set (`INQUIRY`, `TEST UNIT READY`, `REQUEST SENSE`, `READ CAPACITY`, `READ`, `WRITE`, `SYNCHRONIZE CACHE`, `MODE SENSE`), durability classification, and the block-device callbacks registered with `block.c`. It assumes the endpoints and slot ID that `xhci_dev.c` produced; it does not re-validate the device class.

Additions follow the boundaries: a new SCSI opcode goes in `xhci_bot.c`; a new descriptor type goes in `xhci_dev.c`; a new controller register or port capability goes in `xhci.c`. If you find yourself needing to violate the split, that is a sign the interface between two layers is wrong and should be discussed before changing it.

1. **Read [L1](AGENTS.md#locking-and-lifecycle), [M1](AGENTS.md#memory-ownership-and-storage-boundaries), and [M4](AGENTS.md#memory-ownership-and-storage-boundaries) first.** USB transfers are block I/O; the same rules that apply to NVMe apply here.
2. **Bulk completion polls the event ring with a bounded timeout — no IRQ dependency, no sleeps, no enabling of IF.** This is required because `ext2` holds its ranked IRQ-save lock across block I/O (L1). A USB transfer that slept, enabled interrupts, or waited on another thread while that lock was held would deadlock or violate the scheduler contract. Completion is observed by polling the event ring's producer position until the expected completion event arrives or a bounded deadline elapses. Bounded per-transfer timeout and a bounded total recovery sequence; both must have limits. A timeout propagates an I/O error upward and initiates DMA quiescence — it does not permit immediate reuse or freeing of buffers the controller might still be reading.
3. **Never free, reuse, or reissue into DMA memory whose completion state is uncertain (M4).** If a bulk transfer times out, if the CSW is malformed or the tag does not match, if the completion event indicates a stall you could not recover, or if the controller state is ambiguous for any reason: latch the device offline, stop issuing further commands on it, and retain the DMA allocations until reboot. Runtime recovery of an unrecoverable USB device is not supported. The alternative — freeing a buffer the controller is still reading — produces silent memory corruption that will manifest in an unrelated subsystem. The `latched_offline` flag on the transport state is distinct from `transport_failed`; both prevent further BOT submissions and both retain DMA. Document which failure classes set which flag.
4. **Class filtering is in `xhci_dev.c`, not `xhci_bot.c`.** The decision "this is not a BOT mass-storage device, disable its slot and move on" belongs to the descriptor-parsing layer. By the time `xhci_bot.c` sees a device, the class and protocol have already been validated, the bulk-in and bulk-out endpoints have already been identified, and `SET_CONFIGURATION` has already been issued. Adding class checks to `xhci_bot.c` would duplicate the filter, and the two copies would drift. If a new device class needs to be rejected earlier (e.g. hubs, UAS), the check goes in `xhci_dev.c` alongside the existing filter.
5. **Register only fully initialized block devices; unwind on failure (7.3).** A USB stick that fails descriptor validation, endpoint configuration, or durability probe does not get registered with `block.c`. If registration succeeds but a later operation latches the device, the block device may remain registered but must return errors for all subsequent I/O rather than silently accepting commands whose results are unknown.
6. **Test with mocks and disposable images; never enable raw-write tests on hardware or GPT fixtures (7.5).** Host-side BOT tests use mocked transfer completions to exercise result classification, stall recovery, and the durability policy table. QEMU tests use disposable 130 MiB images under `test-usb-*` runners, which validate the final QEMU argv to assert no other data disk is attached. Physical USB testing uses only the deliberately selected stick and the `usb_data_mode=rw` boot entry; the default boot entry remains read-only.
7. **Do not whitelist by VID:PID and do not assume removable flash has no volatile cache.** Durability is established by what the device reports (caching page, `SYNCHRONIZE CACHE` acceptance) or by explicit fallback classification, not by vendor identity. A new device that behaves differently is a fact to record, not a case to special-case.

## 8. Hardware Facts and Verification Boundaries

Preserve empirically-derived and spec-derived workarounds with their evidence labels. Do not remove one merely to match a datasheet; investigate discrepancies and record device/firmware/repro. Observation, implemented behavior and unverified claims are distinct. Do not turn an example or a source-code comment into physical acceptance evidence.

| ID | Evidence / constraint |
| --- | --- |
| H1 | **Dell 5590 photo:** keyboard input and IRQ1 initialization reported working. **Code:** `keyboard_init` clears translation while selecting/querying set 2, then sets bit 6 to deliver translated set 1. Preserve the sequence; it is not proof of the firmware's initial bit value. |
| H2 | **Code:** NVMe doorbells use CAP.DSTRD-derived stride (`4 << DSTRD`) and dynamic mapping extent. No physical DSTRD measurement is established here; never hardcode QEMU's value. |
| H3 | **5590 photo:** ECAM segment 0 buses 0..127, base `0xF0000000`. Parse MCFG, never assume this address/range or apply it to another Dell. See H12 for 5530 controller topology. |
| H4 | **Code + user report (2026-09-16, fixed 2026-09-18):** `layout azerty` selects Belgian AZERTY (Punt). Top number row uses `shift ^ s->caps` as Shift-Lock for digits `1234567890`. Shifted table takes priority over `a..z` matching, resolving bug where scancodes 0x03, 0x08, 0x0A, 0x0B (`é è ç à`) and 0x28 (`ù/%`) emitted uppercase `'E'`, `'C'`, `'A'`, `'U'` instead of digits and `%`. Scancode 86 (0x56) added for ISO `<` / `>`. AltGr absent; arrows/function keys ignored; Caps LED unsynchronized. |
| H5 | **2026-09-20, revised:** Limine's HHDM on the Dell 5590 covers physical `[0, ~2.5 GiB)` only. Measured by direct read at `hhdm_offset + phys`: `0x80000000` succeeds, `0xA0000000` raises #PF at CR2=`0xFFFF8000A0000000`. FortressOS works around this with a two-stage PMM: allocation is capped at 1 GiB until `vmm_init` loads the kernel PML4 with a full 32 GiB HHDM, then `pmm_unlock_high_memory()` clears the cap. PMM bitmap is 1 MiB (32 GiB coverage); the memory map's top range ends at `0x82E7EC000` (~32.72 GiB), which is clamped and warned. Verified on the Dell: `dmesg` reports `Total Physical RAM: 32768 MiB (8388608 frames)`, `Usable Free RAM: 31873 MiB`; write-readback probe passed at 2, 4, 16, and 30 GiB; boot log preserved at `/mnt/boot.log`. |
| H6 | The internal physical NVMe filesystem is not mounted by the kernel. `/mnt` is provided by the USB data partition when a supported stick is attached and selected; see H9. Initramfs file reads prove neither physical disk I/O nor persistence. |
| H6a | **Phase 9F code + user report (2026-09-18):** the raw image includes an ext2 data partition; the user flashed it with Rufus and reported no `/mnt`. Kernel USB storage support is absent. USB boot and image verification do not establish USB partition mounting or persistence; Phase 9G supplies that missing path. |
| H7 | **Code:** ACPI FADT/DSDT S5 and reset fallbacks now exist (`power.c`); this is limited parsing, not a general AML interpreter. Port `0x604` is a QEMU mechanism. Physical ACPI S5 shutdown and multi-tier reset confirmed functional on Dell 5590. |
| H8 | **Recorded QEMU evidence:** 40 exact-boundary NMIs on IST2; no proof of physical NMI injection, nested-fault completeness, SWAPGS or SMP safety. |
| H9 | Kingston USB DISK 2.0 (VID 0x13FE, PID 0x4200) on Dell 5590. Reports no SCSI caching page on MODE SENSE(6) or MODE SENSE(10); rejects SYNCHRONIZE CACHE(10). Classified ASSUMED_WRITE_THROUGH. Files persist across power cycle with clean shutdown. e2fsck -fn clean. Not a claim of power-loss tolerance. |
| H10 | SanDisk USB 3.2 Gen 1 (VID 0x0781, PID 0x5588). Enumerates as a SuperSpeed device on Port 0x12 (Dell 5590). Reports two alternates on Interface 0: Alternate 0 with protocol 0x50 (BOT) and endpoints 0x81 IN / 0x02 OUT; Alternate 1 with protocol 0x62 (UAS) and endpoints 0x81 / 0x02 / 0x83 / 0x04. Only the BOT alternate's endpoints are used. `MODE SENSE(6)` page 0x08 reports `WCE=1`; `SYNCHRONIZE CACHE` succeeds. Classified `SYNC_BACKED`. Mounts read-write at `/mnt`. |
| H11 | Dell 5590 xHCI controller at 0000:00:14.0 (Intel Sunrise Point-LP, 8086:9D2F), 64 KiB BAR at 0xEF330000, 12 USB 2.0 ports and 6 USB 3.0 ports. Legacy handoff extended capability at offset 0x846C. These are observations of one machine, not constants. |
| H12 | Dell Latitude 5530 presents **two** xHCI controllers: `0000:00:14.0` and `0000:00:14.2`, both matching class 0x0C subclass 0x03 progif 0x30. Verified via 9G.5a: both controllers initialized; a Kingston USB 2.0 stick is reachable on either controller depending on physical port; the first controller has no attached devices in the default configuration. Do not assume controller index maps to physical port group. |

### Dell Latitude 5590 physical acceptance (2026-09-16 & 2026-09-18)

User-supplied testing confirms hardware operation on a Latitude 5590 (Core i5-8350U, 32 GiB installed RAM, 256 GB NVMe, Intel UHD 620), booted from USB:

- **2026-09-16:** PS/2 set 2 -> set 1 / IRQ1, COM1 RX absent, interactive shell `help`, `ls`, `cat etc/motd`, and missing-file error handling verified.
- **2026-09-18:**
  - **Belgian AZERTY (Bug H4):** Shift-Lock on top number row with Caps Lock ON verified producing digits `1234567890`. Accented keys unshifted produce base characters without falsely emitting uppercase letters. European ISO `<` / `>` key (scancode 0x56) verified.
  - **System V AMD64 ELF ABI:** `run /bin/hello testing ...` verified passing command-line arguments across the user/kernel boundary with proper 16-byte stack alignment.
  - **Visuals:** Limine splash wallpaper and kernel boot logo / emblem verified.
  - **Power:** ACPI S5 shutdown and multi-tier reset confirmed functional.

These are manual hardware observations, supplementing the automated QEMU and host test suites. Cached text scrolling avoids framebuffer reads; keep early/no-UART output working. Detailed console, NMI and input test notes are in [docs/roadmap/subsystems.md](docs/roadmap/subsystems.md).

## 9. Do Not Touch Without Discussion

Short version: [`PROTECTED.md`](PROTECTED.md). This section is the full detail behind that boundary list.

Discuss intentional changes to these contracts before implementation unless the current task already explicitly authorizes them. Routine edits preserving them need no extra approval. Preserve the behavior, not arbitrary lines of code.

- Limine request markers and linker `KEEP` placement.
- Boot/entry stack alignment (System V ABI, 16-byte before call).
- Interrupt-frame layout (register preservation order in `interrupts.asm` and `syscall_entry.asm`).
- Syscall transition windows (no stack writes before RSP switch, canonical RIP/RSP validation before SYSRET).
- Dependency ordering of subsystem initialization in `kmain`. This includes the two-stage PMM/VMM ordering: `pmm_init` caps allocation at 1 GiB → `vmm_init` builds the kernel PML4 with the full HHDM and switches CR3 → `pmm_unlock_high_memory()` lifts the cap. Any future reordering must preserve the invariant that no allocation reaches above Limine's HHDM coverage until the kernel PML4 is active and CR3 points at it.
- Lock ranks, no-lock-across-switch rule, IRQ-excluded sleep/wakeup and CR3/stack ownership.
- Evidence-backed hardware workarounds (including any `empirical: Dell` comments): read their evidence first.
- `ENABLE_*` raw-write gates, disposable fixture separation, hardware storage exclusions and DMA quarantine.
- Shared kernel PML4 ownership, boot-module backing lifetime, and current single-CPU assumptions.
- xHCI DMA ring state, BOT completion polling discipline (no sleeps/IRQ-enables under ext2 lock), DMA quarantine on failure, and USB class filtering (only 0x08/0x06/0x50 accepted as BOT mass storage).
