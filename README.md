# FortressOS

**A 64-bit operating system built from the kernel up.**

FortressOS is a freestanding C11 and x86-64 assembly kernel with isolated user processes, an interactive shell, and a writable ext2 filesystem. It boots through [Limine](https://github.com/limine-bootloader/limine) under UEFI or legacy BIOS, runs in QEMU, and has been tested on real Dell Latitude 5590 and 5530 hardware.

It is an independent kernel, not a Linux distribution. The project is still under active development: its emphasis is on explicit ownership, bounded failure paths, and tests that distinguish emulator results from hardware observations.

> This README is a human-facing overview and may lag implementation by a
> commit or two. For authoritative, currently-accurate status, implementation
> contracts, and what's safe to change, read [`AGENTS.md`](AGENTS.md) and
> [`PROTECTED.md`](PROTECTED.md) — those are kept in sync with the code, this
> file is kept in sync with those.

**Current milestone:** Shell Milestone S6 is **complete and verified on physical hardware** (Dell Latitude 5590) with uniform file descriptors (0–31), full redirection (`<`, `>`, `>>`, `2>&1`, `n>&-`), atomic multi-core append serialization, and RO/tainted storage assertions. Multi-core (SMP) execution is complete across all six pieces (Pieces 1–5, 6A–6D) on 8 CPU cores with 32 GiB RAM. A modern quiet boot UX delivers a graphical Tokyo Night splash screen, live centered kernel status ticker, automatic un-mute on panic/failure, clean shell handoff, and Pride1922 branding. Next milestone: **Shell S7 (Pipes and Stream Utilities)**.

## What works today

| Area | Implemented capabilities |
| --- | --- |
| Boot & Visual UX | Quiet graphical boot splash with custom FortressOS logo (Tokyo Night color palette) and live centered status ticker. Raw boot diagnostics suppressed on framebuffer by default (100% preserved in COM1 serial and in-memory `dmesg`). Automatic console un-mute and context dump on `[FAIL]` or `[PANIC]`. Clean screen transition into Ring 3 shell. Limine menu supports standard quiet boot and `FortressOS (Verbose Debug)` fallback. |
| Multi-Core (SMP) | 8-core concurrent execution verified on bare metal. AP discovery via Limine/ACPI MADT (Piece 1); per-CPU GS base, GDT, TSS, and IST stacks (Piece 2); strict rank-checked lock discipline with contention telemetry and panic isolation (Piece 3); distributed multi-core preemptive scheduler with per-CPU runqueues and dual-lock work-stealing (Piece 4); APIC ICR cross-core IPIs and broadcast synchronous TLB shootdowns (Piece 5); and full multi-core memory architecture with contention deadlock breaking, `op_refs`, `sched_refs`, and deferred address-space reaping (Piece 6). |
| Memory | Physical page allocator covering 32 GiB RAM with two-stage boot initialization (Phase 9H / Piece 6A); concurrent PMM allocation safety verified across 320,000 cycles under 622k+ contention events (Piece 6B); contention-safe TLB shootdown and CR3 reload (Piece 6C); per-process address spaces with transient operation references (`op_refs`), scheduler references (`sched_refs`), hardware active CPU masks, and guaranteed zero-leak deferred destruction (Piece 6D). |
| CPU and scheduling | GDT/IDT per CPU, exception diagnostics, dedicated double-fault/NMI stacks, ACPI discovery, APIC timer preemption (100 Hz), per-CPU runqueues, and work-stealing across online cores. |
| User programs | Ring 3 execution, ELF loading, fast `syscall` MSRs configured across all cores, System V AMD64 argument passing, validated syscalls, cross-core child waiting, exit status propagation, and deferred process reclamation. |
| Storage | PCI discovery, NVMe reads/writes/flush, xHCI + USB Mass Storage BOT (USB 2.0 and USB 3.x SuperSpeed), validated GPT partitions, and bounded read/write ext2 support. Multi-core concurrent append serialization verified under QEMU `-smp 4` and Dell Latitude 5590 hardware with offline `e2fsck -fn` audits. Write persistence is verified on QEMU NVMe fixtures and on two independent physical USB devices. |
| USB | Multiple xHCI controllers enumerated and initialized; device enumeration and descriptor parsing; BOT/SCSI reads and writes; durability classification with per-device policy; explicit writable opt-in. Both USB 2.0 and directly-attached USB 3.x (SuperSpeed) devices are supported; external hubs and hot-plug are not. |
| Files | Read, create, write, truncate, make directories, rename/move, and delete. Initramfs provides boot-time programs; ext2 provides persistent storage. |
| Shell (Milestones S0–S6) | Modular Ring 3 shell (`user/shell/`) featuring 4096-byte line editing, horizontal viewport, cursor movement, Ctrl shortcuts, RAM history, incremental `Ctrl+R` search, bracketed paste review, raw/timed input (`SYS_INPUT_READ`), terminal mode control (`SYS_TERMCTL`), Belgian AZERTY AltGr operator decoding, working directories (`cd`/`pwd`), logic chaining (`;`, `&&`, `||`, `!`), parameter expansion (`$VAR`, `${VAR}`, `$?`), aliases (`alias`/`unalias`), globbing (`*`, `?`, `[...]`), uniform descriptors (0–31), redirections (`<`, `>`, `>>`, `2>&1`, `n>&-`), retained UI terminal handle (fd 31 with CLOEXEC), `version` builtin, and persistent history (`/mnt/.fortress/history`). |
| Power and platform | BIOS/UEFI boot images, ACPI S5 shutdown, and reset fallbacks. Shutdown and reboot verified on bare-metal Dell Latitude 5590. |

User-process fault isolation and resource reclamation have targeted tests; this is not a claim of complete security isolation. ext2 writes support direct and single-indirect blocks, with explicit rejection of unsupported structures. The filesystem does not promise crash-atomic updates or recovery from arbitrary power loss.

USB durability is classified per device and disclosed in the boot log. A device that reports its caching page and accepts `SYNCHRONIZE CACHE` gets the strong guarantee; a device that reports neither is mounted write-through on the assumption that it behaves like every other consumer stick, with an explicit warning that power loss during writes may lose data. Physical power-loss tolerance is not claimed for any device, even a `SYNC_BACKED` one — clean shutdown is what's verified.

## Try it in QEMU

Build on Linux, or use WSL **Ubuntu-24.04** on Windows. Install the tools:

```bash
sudo apt-get update
sudo apt-get install -y build-essential nasm xorriso qemu-system-x86 ovmf \
    git curl e2fsprogs python3 gdb mtools dosfstools

git clone https://github.com/Pride1922/FortressOS.git
cd FortressOS
make
make run
```

`make` fetches missing Limine dependencies and produces:

| Artifact | Purpose |
| --- | --- |
| `bin/fortress.elf` | Kernel ELF |
| `bin/initramfs.tar` | Boot-time files and user programs |
| `bin/fortress.iso` | Bootable ISO |
| `bin/fortress.img` | 130 MiB raw GPT disk image with a 64 MiB FAT32 EFI partition and a 64 MiB ext2 data partition |

Useful launch targets:

```bash
make run-bios       # ISO, legacy BIOS
make run-img        # Raw image, UEFI when firmware is available
make run-img-bios   # Raw image, legacy BIOS
make run-img-usb    # Raw image presented as USB storage, UEFI when available
make debug         # Start paused for GDB on port 1234
```

The `run-img*` targets attach a separate NVMe test disk whose ext2 partition supplies `/mnt` — that is the QEMU storage fixture, not the USB path. To exercise the real USB code path in QEMU, use the USB-specific runners below.

### Booting the raw image from a USB stick

`bin/fortress.img` can be flashed to a USB stick and booted on real hardware:

```bash
sudo dd if=bin/fortress.img of=/dev/sdX bs=4M status=progress conv=fdatasync
```

The image carries two partitions: a FAT32 EFI System Partition with Limine, the kernel, and the initramfs, and a 64 MiB ext2 data partition. The default boot entry mounts the data partition read-only. A second boot entry opts into a writable mount of the same partition, matched by `PARTUUID`.

On the Dell Latitude 5590, both a Kingston USB DISK 2.0 (VID `0x13FE`, PID `0x4200`, USB 2.0) and a SanDisk USB 3.2 Gen 1 (VID `0x0781`, PID `0x5588`, SuperSpeed) enumerate and mount their ext2 partitions read-write. Files written from the shell persist across a full power cycle; `e2fsck -fn` on the unmounted stick reports 0 errors. The SanDisk classifies as `SYNC_BACKED` — the strongest durability tier — verified on physical hardware.

To try saving files on the disposable QEMU NVMe fixture (not the USB path):

```bash
make run-bios WRITE_TEST=1
```

Use `shutdown` or `poweroff` in the shell to flush the filesystem and finish a clean session.

## Boot Experience & Visual Design

FortressOS features a distraction-free, modern boot experience inspired by the Tokyo Night color palette:
- **Quiet Graphical Splash:** The framebuffer displays a centered, custom-rendered FortressOS shield logo on a `#1A1B26` backdrop with a live centered status ticker updating kernel boot milestones in real time.
- **Diagnostic Preservation:** Raw boot logs are suppressed on the screen by default to maintain a clean aesthetic, but 100% of diagnostic output is continuously captured in the 64 KiB in-memory `dmesg` buffer and mirrored to COM1 serial (UART).
- **Auto-Unmute Crash Protection:** If the kernel encounters any assertion failure (`[FAIL]`) or panic (`[PANIC]`), quiet mode is automatically disabled and the full diagnostic context is dumped to the screen for immediate troubleshooting.
- **Limine Bootloader Modes:**
  - `FortressOS` (default): Quiet graphical boot with live status ticker and clean handoff to the Ring 3 shell.
  - `FortressOS (Verbose Debug)`: Streams full scrolling kernel diagnostics directly to the display.
- **Clean Shell Handoff:** Once hardware and filesystem initialization completes, the screen clears smoothly and presents the interactive Ring 3 shell with Pride1922 branding.

## At the shell

```text
version               # print kernel version, architecture, and Pride1922 branding
help                  # view available commands and builtins
pwd                   # display current working directory
cd /mnt/notes         # navigate filesystem
export FOO="bar"      # set environment variable
echo $FOO             # variable expansion
alias ll="ls -l"      # define command alias
echo "data" > out.txt # redirect stdout to file
echo "more" >> out.txt# append stdout to file
cat < out.txt         # redirect stdin from file
ls /missing 2> err.log# redirect stderr
history               # display in-memory command history
terminal local        # select output mode: local (full screen), serial, mirror, or plain
layout azerty         # Belgian AZERTY with AltGr (| \ {} [] ~)
layout us             # US QWERTY
run /bin/hello world  # execute user program
echo $?               # exit status of last command
```

### Interactive line editing & shortcuts

- **Navigation:** Left / Right arrows move cursor; Home (`Ctrl+A`) / End (`Ctrl+E`) jump to line boundaries.
- **Editing:** Backspace and Delete remove characters; `Ctrl+D` deletes at cursor (or exits the shell on an empty line).
- **Kill buffer:** `Ctrl+W` erases previous word; `Ctrl+U` erases to start of line; `Ctrl+K` erases to end of line; `Ctrl+Y` yanks (pastes) the last erased text.
- **History & Search:** Up / Down arrows recall commands and restore unfinished drafts; `Ctrl+R` begins reverse incremental search (Enter accepts for editing, second Enter executes; Escape or `Ctrl+G` cancels).
- **Tab Completion:** Tab completes builtin names, executables in `/bin`, and filesystem paths with smart prefix matching.
- **Control & Logic:** `Ctrl+L` clears screen and repaints; `Ctrl+C` cancels current input; command chaining with `;`, `&&`, `||`, and `!`.
- **Safety:** Command lines support up to 4096 bytes with a horizontal scrolling viewport. Input loss or overflow displays `[lost/full: Ctrl+C]` and refuses to execute partial input. Bracketed paste converts newlines to spaces and requires two Enter presses to execute (`[paste: Enter twice]`).

With the USB stick's data partition mounted read-write:

```text
mkdir /mnt/notes
cd /mnt/notes
echo "Hello from FortressOS" > hello.txt
cat hello.txt
echo "Appended log entry" >> hello.txt
edit hello.txt
mv hello.txt saved.txt
sync
shutdown
```

After reboot, `cat /mnt/notes/saved.txt` returns the exact preserved file.

## Real hardware: what has been verified

Manual testing on a **Dell Latitude 5590** (Core i5-8350U, 32 GiB installed RAM, 256 GB NVMe, Intel UHD 620) has confirmed USB boot, the framebuffer console, PS/2 keyboard input, Belgian AZERTY behavior, shell interaction, argument passing to `/bin/hello`, ACPI shutdown, reboot, and full use of the machine's 32 GiB of RAM (verified with a write-readback probe at 2, 4, 16, and 30 GiB, and a full-capacity boot log).

USB storage is verified on physical hardware across two device classes. The kernel discovers xHCI controllers, addresses both a USB 2.0 stick (Kingston) and a USB 3.x SuperSpeed stick (SanDisk), parses GPT on each, mounts their ext2 partitions read-write, and persists files written from the editor across a full power cycle. `e2fsck -fn` on the unmounted stick from Linux reports 0 errors. A **Dell Latitude 5530**, which exposes two independent xHCI controllers, has also been verified: both controllers initialize, and a stick is reachable and mountable on either one.

The internal physical NVMe is deliberately excluded from the USB storage mount path by parent-device provenance. Adding an ext2 partition to the laptop's SSD does not enable automatic mounting.

**What is not yet verified on hardware:**
- **Physical power-loss tolerance.** Even a `SYNC_BACKED` device's guarantee is about a completed flush, not about surviving power loss mid-write. Only clean-shutdown persistence is verified on any device.
- **NVMe write persistence on physical hardware.** NVMe read/write/flush and ext2 writable-mount persistence are verified against QEMU fixtures; the equivalent three-boot `e2fsck`-clean test has not yet been run against the Dell's internal NVMe.

## USB storage and durability

The USB path is a self-contained driver set under `src/drivers/xhci*` and `src/fs/usb_mount.c`. It targets xHCI controllers only; EHCI/UHCI/OHCI are out of scope.

**Supported today:**
- Directly attached USB 2.0 (Full-Speed/High-Speed) and USB 3.x (SuperSpeed) mass-storage devices, on any enumerated xHCI controller.
- BOT transport with LUN 0 only.
- Read, write, and `SYNCHRONIZE CACHE` where the device supports them.
- Partition selection by `PARTUUID` with USB parent-device provenance.
- Explicit `usb_data_mode=rw` opt-in; the default boot entry is read-only.

**Not supported:**
- External hubs (class 0x09; rejected) — recursive/hub-attached enumeration is deferred (Phase 9G.5e) until a specific device needs it.
- Hot-plug or reconnection.
- UAS, or any device class other than mass storage.
- SuperSpeedPlus (10 Gbps) — untested.

**Durability classification** is per-device and printed in the boot log:

- `SYNC_BACKED` — the device accepts `SYNCHRONIZE CACHE`; every flush must complete. Verified on physical hardware (SanDisk USB 3.x).
- `WRITE_THROUGH` — `MODE SENSE` reports `WCE=0`; the device reports no write cache.
- `ASSUMED_WRITE_THROUGH` — the device reports neither, but is mounted RW with an explicit disclosure that power-loss during writes may lose data. Where the Kingston USB 2.0 stick lands.
- `READ_ONLY` — the device reports a write cache it cannot flush, or is in an error state.

The classification is what gates writable mount: only devices that can either prove their durability or accept the fallback disclosure get RW. A device that reports `WCE=1` and rejects `SYNCHRONIZE CACHE` is refused.

## Multi-Core (SMP) Architecture

Multi-core execution is complete and verified on bare metal (Dell Latitude 5590, 8 CPUs, 32 GiB RAM). It was designed and sequenced in six pieces — architectural specification in [`docs/plans/SMP_DESIGN.md`](docs/plans/SMP_DESIGN.md):

1. [**AP discovery and boot**](docs/roadmap/smp-piece1-ap-discovery.md) — Limine SMP protocol cross-checked with ACPI MADT; boots every core to a parked state.
2. [**Per-CPU storage**](docs/roadmap/smp-piece2-percpu.md) — GS-base CPU locals, per-CPU GDTs, TSS selectors, and IST1/IST2 stacks with SWAPGS and NMI isolation.
3. [**Lock discipline under real concurrency**](docs/roadmap/smp-piece3-lock-discipline.md) — Hierarchical rank checks (L1), contention telemetry, atomic spinlocks, and panic isolation.
4. [**SMP Scheduler & Work-Stealing**](docs/roadmap/smp-piece4-scheduler.md) — Per-CPU runqueues, 100 Hz APIC timer preemption, task migration, and dual-lock work-stealing across online cores.
5. [**IPIs and TLB shootdown**](docs/roadmap/smp-piece5-ipi.md) — APIC ICR messaging, synchronous broadcast TLB invalidation barriers, remote core wakeups, and real VMM unmap synchronization.
6. [**Multi-core memory architecture**](docs/roadmap/smp-piece6-memory.md) —
   - **6A**: 1 GiB boot ceiling and two-stage PMM/VMM initialization.
   - **6B**: PMM multi-core safety (320,000 cycles across 8 CPUs under 622k+ contention events, 0 duplicate claims, exact post-quiescence state equality).
   - **6C**: Contention-safe TLB shootdown with polled local servicing breaking circular deadlocks under interrupts-disabled contention, plus full TLB CR3 reload across all APs.
   - **6D**: Address-space lifetime discipline (`op_refs`, `sched_refs`, active CPU masks, deferred destruction queue, and atomic wait/exit coordination verified across 100 process cycles with zero leaks).

**Next milestones:**
- **Shell Milestone S7 (Pipes & Stream Utilities):** Kernel anonymous pipes (`SYS_PIPE`), multi-stage pipelines (`cmd1 | cmd2 | ... | cmdN`), POSIX auto-CLOEXEC descriptor cleanup, simulated exit 141 on broken pipes, and transparent stream filter tools (`cat`, `head`, `tail`, `wc`). Planned in [`docs/plans/S7_PLAN.md`](docs/plans/S7_PLAN.md).
- **Shell Milestone S8 (Jobs, Signals & Process Groups):** Background jobs (`&`), job control (`jobs`, `fg`, `bg`), process group terminal ownership, and signal handling (`SIGINT`, `SIGTSTP`, `kill`).
- **Shell Milestone S9 (Scripting & Control Flow):** Script execution, shell functions, parameter expansion sub-stages, and control structures.
- Introspection syscalls and utilities (`sysinfo`, `top`, `ps`).
- Persistent rootfs integration (`/paradise`).
- MicroPython port.
- Accounts, permissions, and installer.

Larger follow-ups: a journaling filesystem (ext4 or similar) and networking.

See [`docs/roadmap/`](docs/roadmap/README.md) for checkpoint history and hardware evidence, [`docs/plans/`](docs/plans/README.md) for architectural plans, and [`AGENTS.md`](AGENTS.md) for implementation contracts and invariants.

## Testing

The project combines host sanitizer tests, QEMU integration tests, offline filesystem checks, and manual hardware observations.

| Command | Coverage |
| --- | --- |
| `make test-smp-percpu` | BIOS/UEFI 1/4/8 CPUs: GS base, GDT/TSS, stack guards, NMI delivery on all CPUs |
| `make test-smp-append` | True multi-core SMP concurrent append verification under QEMU `-smp 4` (BIOS & UEFI): atomic serialization under `ext2_lock`, 200 records intact, 0 lost/corrupt, clean S5 shutdown, offline `e2fsck -fn` audit |
| `make test-shell-s6` | Shell S6 Phase 4A–4D under QEMU (BIOS & UEFI): child/parent redirection, dual-stream lexical ordering, stdin, stderr append/truncation/closure, expansion, failed setup and prompt recovery |
| `make test-shell-s6-resources` | Shell S6 resource exhaustion under BIOS & UEFI (1 and 4 CPUs): child descriptor limit, process table capacity, 0 uncollected threads, prompt recovery |
| `make test-vmm-host` | ASan/UBSan: VMM space registry, lifecycle states, transient `op_refs`, context switch tracking, deferred destruction queue with 0 leaks |
| `make test-pmm-boot-host` | ASan/UBSan: PMM boot ceiling, capped OOM, contiguous boundary, and unlock gates |
| `make test-smp-memory-boot` | BIOS/UEFI 1/4/8 CPUs: boot memory ceiling, readiness/CR3, and high-memory unlock |
| `make test-smp-vmm` | BIOS/UEFI 1/4/8 CPUs: 100 user process spawn/exit cycles across cores, deferred destruction, table frame and page leak checks |
| `make test-input` | Keyboard decoding, modifiers, and bounded input FIFO |
| `make test-usb-discovery` | PCI-only xHCI detection/absence and shell startup in BIOS/UEFI, without an NVMe fixture |
| `make test-usb-descriptors` | Device addressing, descriptor parsing, BOT class validation, and `SET_CONFIGURATION` in QEMU BIOS/UEFI |
| `make test-usb-block` | BOT transport, SCSI reads, and GPT registration in QEMU BIOS/UEFI, no NVMe fixture |
| `make test-usb-mount` | PARTUUID selection and read-only `/mnt` mount in QEMU BIOS/UEFI |
| `make test-usb-persistence` | QEMU BIOS/UEFI three-boot create/read/overwrite with offline `e2fsck -fn` on disposable 130 MiB images |
| `make test-xhci-bot-host` | BOT stall recovery, MODE SENSE parsing, and durability policy under ASan/UBSan |
| `make test-usb-mount-host` | Mount eligibility, durability modes, and sync path under ASan/UBSan |
| `make test-console` | Framebuffer rendering, wrapping, scrolling, and bounds |
| `make test-ext2` | Actual ext2/VFS code under ASan/UBSan, malformed data and injected failures |
| `make test-ext2-write` | BIOS/UEFI multi-boot persistence on disposable NVMe images, with offline `e2fsck -fn` |
| `make test-storage` | BIOS/UEFI GPT/ext2, user-space reads, and allocation audits |
| `make test-shell-host` | Consolidated ASan/UBSan: keyboard/queue, framebuffer CSI terminal parser, and line editor/search/history limits |
| `make test-shell-integration` | BIOS/UEFI shell interaction, cursor/screen-state, history/search/paste, and no-UART coverage |
| `make test-shell` | Shell interaction, input wakeups, process execution, and reclamation |
| `make test-nmi` | NMI injection at exact syscall transition boundaries in QEMU |
| `make test-boot-diagnostics` | UEFI boot with 8 GiB RAM and no COM1 |
| `make test-power` | QEMU shutdown and reboot commands |


Recorded test results and their limits live in [`docs/roadmap/`](docs/roadmap/README.md). A listed test target is not a claim that every revision has passed it, and QEMU success is not physical-hardware acceptance. Storage tests must use disposable images, never an existing physical disk.

## Finding your way around

```text
src/arch/x86_64/   CPU setup, SMP bring-up, APIC/IPI, interrupts, context switching, syscall entry
src/kernel/       Boot, scheduler, processes, ELF loader, syscalls, lock discipline
src/mm/           Physical memory (PMM), paging (VMM), heap
src/drivers/      Console, input, PCI, NVMe, xHCI, USB BOT, power
src/fs/           VFS, tar initramfs, GPT, ext2, USB mount policy
src/include/      Shared kernel and user ABI definitions (syscalls, terminal)
user/             Freestanding user programs (init, hello) and shell entry
user/shell/       Modular shell engine (line editing, builtins, UI, RAM history, file editor)
tests/            Host tests and mocks
scripts/          Image creation and QEMU verification
docs/plans/       Architecture specifications and staged implementation plans
docs/roadmap/     Checkpoint implementation history and hardware verification evidence
```

Before changing kernel code, read [`PROTECTED.md`](PROTECTED.md) first, then [`AGENTS.md`](AGENTS.md). For supported formats, architectural limits, and technical debt, see [`ARCH_REVIEW.md`](ARCH_REVIEW.md). For design plans, see [`docs/plans/`](docs/plans/README.md). Contributions should state what changed, which tests were run, and whether the evidence comes from host tests, QEMU, or physical hardware.
