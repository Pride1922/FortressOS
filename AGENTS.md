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
| Networking | [docs/subsystems/net.md](docs/subsystems/net.md), then the NET plan and driver |
| USB / storage | [docs/subsystems/usb.md](docs/subsystems/usb.md) / [docs/subsystems/storage.md](docs/subsystems/storage.md) |
| Shell / jobs / signals | [docs/subsystems/shell.md](docs/subsystems/shell.md) |
| Multi-core / SMP | [docs/subsystems/smp.md](docs/subsystems/smp.md) |
| Platform / power / ACPI | [docs/subsystems/platform.md](docs/subsystems/platform.md) |

Use `rg` / `rg --files` to locate implementations and callers. Trace indirect calls too; a textual search alone does not establish locking or IRQ safety. This guide states contracts; headers/code define implemented APIs. If they conflict, identify the discrepancy before changing behavior or claiming support.

## 2. Project Overview and Current Work

FortressOS is a freestanding C11/NASM x86_64 kernel using Limine v8, base revision 3, with UEFI and BIOS boot. Kernel virtual base: `0xffffffff80000000`; HHDM offset comes from boot metadata. Hardware subsystems have explicit, separate APIs. Early COM1 and framebuffer diagnostics must work before the heap is available.

Detailed subsystem status logs, hardware observations, and scope boundaries live in [`docs/subsystems/`](docs/subsystems/README.md). Historical logs live in [docs/roadmap/README.md](docs/roadmap/README.md); qualifications and technical debt in [ARCH_REVIEW.md](ARCH_REVIEW.md). Keep new implementation instructions here, checkpoint history there, and verification claims tied to actual evidence.

| Subsystem | Status | Detail |
| --- | --- | --- |
| NET | IN PROGRESS — Phase 4a complete (QEMU); physical 4b next; Phase 2b/3 physical evidence recorded | [docs/subsystems/net.md](docs/subsystems/net.md) |
| Shell S9 | COMPLETE (2026-09-30) | [docs/subsystems/shell.md](docs/subsystems/shell.md) |
| Shell S8 | COMPLETE (2026-09-29) | [docs/subsystems/shell.md](docs/subsystems/shell.md) |
| Shell S7 / S6 / S5 / S3–S4 / S0–S2 | COMPLETE | [docs/subsystems/shell.md](docs/subsystems/shell.md) |
| SMP | COMPLETE (2026-09-25) | [docs/subsystems/smp.md](docs/subsystems/smp.md) |
| USB (9G) | COMPLETE through 9G.5b | [docs/subsystems/usb.md](docs/subsystems/usb.md) |
| Storage (9D/9E/9H) | COMPLETE | [docs/subsystems/storage.md](docs/subsystems/storage.md) |
| Power & layout (9C.5) | COMPLETE | [docs/subsystems/platform.md](docs/subsystems/platform.md) |

Next open items not blocking any current milestone: system introspection syscalls + `sysinfo`/`top`/`ps`, persistent rootfs with `/paradise`, shell improvements, MicroPython, ext4 (or another journaling filesystem), networking.

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
| `make test-net-icmp-host` | Phase 4a pure ICMP ASan/UBSan: independent checksum vector, all 1473 data lengths, bounds/corruption and IPv4 encoder overflow. PASS 2026-10-01. |
| `make test-net-ipv4-host` | Phase 4a actual IPv4/ICMP stack with NIC/ARP/tick mocks, ASan/UBSan: reply bytes/padding, filters/routes, three-attempt ARP, pending slot, exact token matching, pacing/timeouts/TX failure. PASS 2026-10-01; no hardware claim. |
| `make test-net-ping-host` | Phase 4a actual finite mailbox with pthread/protocol/scheduler adapters, ASan/UBSan: busy/publication/collection, stale tokens/cancellation and abandoned-owner/result expiry. PASS 2026-10-01; no real scheduler claim. |
| `make test-net-icmp` | Phase 4a BIOS/UEFI × e1000/e1000e × user/socket, SMP=1, disposable ISO/OVMF vars, exact argv/no data disks: boot echo probe, real Ring 3 ping 4/4, ABI pointer fixture, independent pcap audit, socket echo/timeout/busy/KILL lease recovery; BIOS/e1000/socket also STOP/CONT expiry. 8/8 PASS 2026-10-01; physical 4b pending. |
| `make test-net-host` | NET Phase 0 host ASan/UBSan: RFC 1071 ones' complement checksum vectors (odd/even lengths, bounds, multi-buffer accumulation), Ethernet II (encode/decode, bounds 60-1514B, runt/oversize rejection, broadcast/MAC filter), ARP (encode/decode request/reply, truncation rejection, 16-entry bounded cache stub), IPv4 (encode/decode, checksum verify/corrupt, fragment rejection, bounds, malformed IHL/len), pbuf_t lifecycle and fuzz/bounds resilience. 103/103 PASS 2026-09-30. |
| `make test-net-rings` | NET Phase 2a: BIOS/UEFI × e1000/e1000e SMP=1, disposable ISO/OVMF vars, no data disks, exact argv preflight; raw TX DD + 60-byte pcap audit, injected raw RX byte check + recycle, shell ready. Includes host ownership/failure sanitizer target. 4/4 PASS 2026-09-30; no physical DMA or worker claim. |
| `make test-net-rings-host` | Actual e1000 driver with mocked PMM/MMIO/PCI/pthread locks under ASan/UBSan: allocation failures, RX pool/recycle/wrap/malformed frames, TX copy/reservation/wrap, completion/reset timeout quarantine. PASS 2026-09-30; no hardware timing/IRQ claim. |
| `make test-net-eth-host` | NET Phase 3 host ASan/UBSan: actual `net.c` stack plus `eth`/`arp` codecs with a mocked NIC, scheduler and BSP ticks. 52 RX inputs recycled exactly once; Ethernet dispatch and ARP request replies, **reply-only** cache learning (request senders not cached), malformed/truncated/oversize and non-matching frames dropped, send-failure and `arp_resolve` outcomes (hit/request/failure); bounded `net=` config (defaults `10.0.2.15/24,10.0.2.2`, prefix 1–30, duplicate/malformed warning fallback, no panic); absent device, failed worker creation and `net_test=rings` raw-test exclusion; three mock idle waits proving the BSP-tick deadline predicate and `sched_wake_all(&g_net_poll_channel)` wake with no idle yield. PASS 2026-10-01; no hardware, IRQ, real-scheduler or idle-CPU claim. |
| `make test-net-eth` | NET Phase 3 QEMU, SMP=1, disposable ISO/OVMF vars, no data disks, exact argv preflight: BIOS/UEFI × e1000/e1000e × {user, socket} = 8 cases. User backend resolves the real SLIRP gateway via ARP; socket backend (no SLIRP gateway) explicitly emulates the gateway reply and, after idle ticks, injects a peer ARP request over loopback UDP to prove the sleeping worker resumes to poll RX. Guest request and reply are audited as exact 60-byte outbound pcap records (`filter-dump` is capture-only, never injection); the real Ring 3 shell prompt (`FortressOS shell (Ring 3)` / `fortress> `) is confirmed while the worker runs. 8/8 PASS 2026-10-01; no physical acceptance, sustained load, measured idle CPU or cross-core delivery claim. |
| `make test-net-pci` | NET Phase 1a QEMU BIOS + UEFI SMP=1: Intel e1000/e1000e PCI discovery (8086:100E/10D3), BAR0 sizing & uncached MMIO mapping (`0xFFFFFFFFE2000000ULL`), valid hardware MAC read (`RAL0`/`RAH0` and `EERD`), link STATUS register, clean fallback report on absent NIC (`-net none`), and argv preflight rejecting unauthorized storage. 4/4 PASS 2026-09-30. |
| `make test-s9-top-host` | S9 Phase 3 host ASan/UBSan: `top` CPU% deltas across PIDs, matching, churn, decreasing counter rejection, zero/reversed interval defense, overflow bounds, sorting, frame rendering, dimension capping, and mock dispatch. PASS 2026-09-30. |
| `make test-s9-top SMP=N` | S9 Phase 3 BIOS/UEFI with real Ring 3 shell and `/bin/top`; disposable ISO, no data disks, argv preflight: redirected one-shot pipeline (`top \| head -n 5`), interactive timed refresh without keystrokes, `q` exit, spinning busy worker (`/bin/hello --spin &`) vs stopped worker (`cat &` + `kill %n STOP`) CPU% deltas, Ctrl-C abort, and Ctrl-Z/bg/fg lifecycle with full redraw. PASS BIOS + UEFI at SMP=1 and SMP=4 2026-09-30. |
| `make test-s9-sysinfo-host` | S9 Phase 2 host ASan/UBSan: `sysinfo_t` conversion, MiB calculation, zero-frequency defense, overflow bounds, S2 `SYSCALL_EFAULT` range validation. PASS 2026-09-30. |
| `make test-s9-sysinfo SMP=N` | S9 Phase 2 BIOS/UEFI with real Ring 3 shell and `/bin/sysinfo`; disposable ISO, no data disks, argv preflight: managed-RAM total/free/used sanity without hole distortion, background process count transition, and SMP=1 vs SMP=4 uptime rate verification (regression guard proving BSP-only 100 Hz timebase). PASS BIOS + UEFI at SMP=1 and SMP=4 2026-09-30. |
| `make test-s8-sigpipe-host` | Phase 6 host, mocked syscalls and adapters, ASan/UBSan: actual pipe/VFS/`SYS_WRITE` and process-table publication for all four dispositions, partial writes, blocked pending state, writer-only targeting and closure cases. User-reported PASS 2026-09-29; no real IRQ, scheduling or handler-frame claim. |
| `make test-s8-sigpipe SMP=N` | Phase 6 BIOS/UEFI with the real shell plus `/bin/sigpipe-probe`; disposable ISO, no data disks: caught/ignored/blocked/default dispositions, short/zero/invalid writes, early closure, stopped producer, `head -n 1` on an infinite producer, shell status and prompt recovery. User-reported PASS BIOS + UEFI at the default `SMP=1` 2026-09-29; `SMP=4/8` is supported by the runner but all pipe peers remain BSP-pinned. |
| `make test-s8-jobctl-host` | Phase 5 aggregate: extended jobs fixture plus orphan metadata fixture, ASan/UBSan. User-reported PASS 2026-09-28. |
| `make test-s8-orphans-host` | Actual process metadata, pthread adapters: parent exit/stop orderings, whole-group KILL, running orphan identities and remaining parent anchors. User-reported PASS via test-s8-jobctl-host 2026-09-28; no scheduler/IRQ claim. |
| `make test-s8-jobctl SMP=N` | Real shell, BIOS/UEFI, default SMP=1 (optional 4/8): stop/bg/fg/kill, terminal attributes/read, prompt controls, orphan/staged/exit cleanup and idle completion; disposable ISO, no data disks. User-reported PASS BIOS + UEFI, SMP=1, 2026-09-28; no SMP=4/8 execution claimed. |
| `make test-s8-jobs-idle` | Real shell, BIOS/UEFI SMP=1: idle `Done` notification and draft/cursor preservation; disposable ISO plus delay helper, no data disks. User-reported PASS 2026-09-28; SMP=4/8 supported but not claimed here. |
| `make test-s8-jobs-host` | Host, mocked syscalls, ASan/UBSan: parser `&`, job table, state machine, launch order, masked window, unwinding and prompt drain. Phase 4's 15 cases user-reported PASS 2026-09-28. Phase 5 adds two job-control cases (specs/selection and shared fg/attributes/status); current 17-case revision user-reported PASS 2026-09-28. No scheduler/IRQ claim. |
| `make test-s8-jobs` | BIOS/UEFI SMP=1, Ring 3 launch/handoff, staged cancellation and descriptor ownership; disposable ISO, no data disks. User-reported PASS 2026-09-28. |
| `make test-s8-terminal-host` | Phase 3 actual input/group logic with IRQ/scheduler adapters: ownership, attributes, retained ingress events, coalescing/overflow and cleanup. User-reported pass 2026-09-28; no IRQ/scheduler claim. |
| `make test-s8-terminal SMP=N` | Phase 3 BIOS/UEFI with SMP=1/4/8, AP-count checks, real UART/PS2, no-reader group signals, TTIN/TTOU, ISIG off and FD 31 reclaim; disposable ISO, no data disks. User-reported pass with `SMP=1` under BIOS and UEFI (AP counts verified) 2026-09-28; `SMP=4`/`SMP=8` terminal runs were not reported. |
| `make test-s8-groups-host` | Separate S8 prerequisite: actual process-group lifetime/reference and signal-publication tests with pthread lock adapters and ASan/UBSan. User-reported pass 2026-09-28 (no per-command details); no IRQ/scheduler claim. |
| `make test-s8-stops-host` | Phase 2C process-table transition/counter/cancellation/CHLD tests with pthread locks; no scheduler/IRQ claim. User-reported pass 2026-09-28. |
| `make test-s8-stops SMP=N` | Phase 2C BIOS/UEFI Ring 3 STOP/CONT/KILL and caught-CHLD fixture, disposable ISO with no data disks. User-reported pass 2026-09-28; children stay BSP-pinned, so this is not cross-core stopped-task acceptance. |
| `make test-pipe-host` | S7 Phases 1–2 ASan/UBSan: real pipe/VFS/sys_pipe with host adapters; wraparound, atomic thresholds, lock-free wait/wake call sites, endpoint lifetime and allocation/fd rollback. No SMP/IRQ claim. |
| `make test-pipe` | S7 Phases 1–2 BIOS/UEFI (1 CPU), disposable test ISO with no data disks; kernel blocking/backpressure/close tests and Ring 3 pointer validation, CLOEXEC spawn actions, fd exhaustion, EOF/EPIPE. See [Phase 2 evidence](docs/roadmap/shell-s7-phase2.md). Cross-core wakeups remain deferred. |
| `make test-shell-host` | Consolidated ASan/UBSan: keyboard/queue, framebuffer terminal and actual shell editor/history logic |
| `make test-pipeline-host` | S7 Phase 4 actual parser/expander/executor with mocked syscalls: grouping, preflight, fd ownership, action order and failure injection. Also included in `test-shell-host`; Phase 4 accepted by the user; per-command results are not separately recorded. |
| `make test-stream-tools-host` | Phase 5A actual tools with ASan/UBSan and mocked syscalls: binary copying, short I/O, tail bounds/draining, CLI, descriptor cleanup and injected wc overflow. Included in `test-host`; user-reported pass 2026-09-27, not rerun by the agent. |
| `make test-shell-s7 SMP=N` | User-confirmed BIOS/UEFI passes with `SMP=1`, `4`, `8`, AP counts checked in boot logs. Disposable ISO/NVMe copy, streaming/status/cleanup and offline byte/e2fsck checks; pipe peers remain BSP-pinned. See [Phase 6](docs/roadmap/shell-s7-phase6.md). |
| `make test-shell-integration` | Existing shell integration extended with cursor/screen-state, history/search/paste, timeout/log separation and no-UART coverage; snapshot NVMe fixture for normal runs |
| `make test-input` | Host ASan/UBSan: decoder, modifiers and bounded FIFO |
| `make test-usb-discovery` | 9G.1a BIOS/UEFI PCI discovery with/without xHCI, shell startup without NVMe; ISO boot only, no data disk. No USB transfers or persistence claimed. |
| `make test-console` | Host ASan/UBSan: pixel output, wrapping, scrolling and bounds |
| `make test-ext2` | Host ASan/UBSan: actual ext2/VFS, malformed images, I/O/OOM paths |
| `make test-ext2-write` | QEMU ext2 file creation, editor save, host `e2fsck -fn` integrity, and cross-boot persistence on disposable NVMe GPT fixture |
| `make test-smp-append` | True multi-core SMP concurrent append verification under QEMU (-smp 4, BIOS & UEFI): independent and shared handles, atomic EOF serialization under `ext2_lock`, 200 records intact, 0 loss/corruption, clean S5 shutdown, offline host `e2fsck -fn` audit. |
| `make test-shell-s6` | Shell S6 Phase 4A–4D under QEMU (BIOS & UEFI): child/parent redirection, dual-stream lexical ordering, stdin, stderr append/truncation/closure, expansion, failed setup and prompt recovery. |
| `make test-shell-s6-resources` | Shell S6 Phase B resource limits under QEMU (BIOS & UEFI, 1 & 4 CPUs): child descriptor limit (32), parent fd table exhaustion, process table capacity, GDB scheduler inspection confirming 0 partial/runnable threads published on abort, prompt recovery. |
| `make test-storage` | BIOS/UEFI GPT/ext2, Ring 3 reads, allocation-set audits; `build/storage-*.log` |
| `make test-shell` | BIOS/UEFI IRQ1/IRQ4 interaction, sleeping readers, restart counts; also UEFI 8 GiB without COM1 |
| `make test-nmi` | BIOS/UEFI, BSP: seven SYSRET boundaries × 4 rounds + kernel test-recovery + four sigreturn boundaries × 2 origins × 4 rounds; `build/nmi-*.json` and `.log`. Runner observes the post-IRET user boundary with a hardware breakpoint (`resume_to`) instead of single-stepping — see the [Phase 3 handoff](docs/roadmap/shell-s8-phase3.md). User-reported pass on three consecutive runs 2026-09-28. |
| `make test-smp-percpu` | BIOS/UEFI with 1/4/8 CPUs: CPU-local GS/GDT/TSS/stacks, AP #DF, real NMI delivery on every CPU, unchanged BSP IST/TSS/GDT and shell startup; snapshot NVMe fixture |
| `make test-pmm-boot-host` | Piece 6A host ASan/UBSan with single-threaded shims: boot ceiling, capped OOM, contiguous boundary, unlock gates and exact cleanup; no SMP exclusion claim |
| `make test-smp-memory-boot` | Piece 6A BIOS/UEFI 1/4/8 CPUs, 2 GiB: explicit test ISO, readiness/CR3, high-memory probe and AP startup; no data disks |
| `make test-vmm-host` | Piece 6D host ASan/UBSan: VMM space registry, lifecycle states, transient op_refs, context switch tracking, deferred destruction queue and drainage with zero leaks |
| `make test-smp-vmm` | Piece 6D BIOS/UEFI 1/4/8 CPUs: 100 user process spawn/exit cycles across cores, deferred destruction, table frame and page leak checks |
| `make test-boot-diagnostics` | UEFI 8 GiB, no COM1; progress to PCI discovery and framebuffer capture |
| `make test-power` | QEMU shutdown/reboot command tests; physical ACPI S5 confirmed separately on Dell 5590 (see §8 H7), not by this target |
| `make test-usb-descriptors` | 9G.1e host ASan/UBSan + QEMU BIOS/UEFI descriptor parsing, BOT class validation, device configuration |
| `make test-usb-block` | 9G.2 BOT host tests + QEMU sector read/GPT registration, no NVMe fixture |
| `make test-usb-mount` | 9G.3 mount policy host tests + QEMU BIOS/UEFI PARTUUID selection and read-only mount |
| `make test-usb-persistence` | 9G.4 QEMU BIOS/UEFI three-boot create/read/overwrite/delete with offline `e2fsck -fn` on disposable 130 MiB images |
| `make test-xhci-bot-host` | Host ASan/UBSan: BOT stall recovery, MODE SENSE parsing, durability policy table |
| `make test-usb-mount-host` | Host ASan/UBSan: mount eligibility, durability modes, sync path |

NET Phase 1b (Dell hardware discovery) deliberately adds **no** target: it is a manual hardware observation recorded in [docs/roadmap/net-phase1b.md](docs/roadmap/net-phase1b.md) and must never be reported as a test pass. `make test-net-pci` stays the primary logic evidence for the driver's discovery/mapping/MAC/STATUS path.

NET Phase 2b (Dell I219-LM DMA/TX/RX) also deliberately adds **no** target: its physical acceptance is a manual hardware observation plus a cable-side capture on a second host, recorded in [docs/roadmap/net-phase2b.md](docs/roadmap/net-phase2b.md), and must never be reported as a test pass. `make test-net-rings` (4/4 BIOS/UEFI × e1000/e1000e) and `make test-net-rings-host` remain the automated logic evidence for the rings/DMA/TX/RX and ownership/failure paths; `make test-net-i219-host` is mocked-only and makes no physical DMA claim.

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
| L1 | Acquire increasing ranks: per-CPU scheduler **or** `ext2_lock` **or** `g_process_lock` (1) → heap (2) → VMM (3) → PMM (4) → console (5). `g_process_lock` owns global process identity/group/session metadata and child reservations/status; it uses ordinary lock kind. Process/ext2 never nest with another rank-1 lock in either order. Only scheduler pairs may nest in increasing address order via `sched_lock_pair`. Release LIFO. | Trace nested calls from `spin_lock_irqsave`; verify `SPINLOCK_RANKED` values and `spin_debug_selftest`, including process↔scheduler/ext2 rejection and the ordered scheduler-pair exception. Violations panic/deadlock, not warnings. |
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
├── SMP_DESIGN.md            # Stub redirecting to docs/plans/SMP_DESIGN.md
├── ARCH_REVIEW.md           # Architecture audit, limits and technical debt
├── docs/
│   ├── plans/               # Architecture specifications and implementation plans (SMP_DESIGN.md, etc.)
│   ├── roadmap/             # Per-phase implementation notes and hardware verification evidence
│   └── subsystems/          # Per-subsystem architecture annexes, status logs, and hardware facts
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
4. Wire callbacks and per-process descriptors without sharing offsets between independent opens. When implementing writes, append mode (`VFS_O_APPEND`) must serialize authoritative EOF determination and writeback under filesystem locking (e.g. `ext_write` under `ext2_lock`), updating `*off` and `node->size` before lock release. Stream nodes (`VFS_STREAM`) bypass file EOF checks and do not advance linear offsets. If a mount is conditional on external state (durability classification, PARTUUID match, provenance), reject or fall back to read-only before the first filesystem write — never dirty the superblock and then discover the mount was ineligible.
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
5. Launch through the existing `process_spawn` lifecycle/test harness. Use `run /bin/foo` through the implemented `SYS_SPAWN`/`SYS_WAIT` ABI; direct command discovery is planned in shell S3.
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

Hardware facts and empirical constraints are maintained in the respective subsystem annexes:

| Subsystem | Hardware facts & empirical constraints | Annex |
| --- | --- | --- |
| Networking | H13 (Intel I219-LM, Dell 5590 BDF/BAR0/MAC, link STATUS, TCTL post-reset, FEXTNVM11, SPT workarounds) | [docs/subsystems/net.md](docs/subsystems/net.md#2-hardware-facts-and-verification-boundaries) |
| Storage & RAM | H2 (NVMe doorbells), H5 (32 GiB RAM / two-stage PMM), H6 (internal NVMe exclusion), H6a (raw image ext2 partition) | [docs/subsystems/storage.md](docs/subsystems/storage.md#3-hardware-facts-and-verification-boundaries) |
| USB | H9 (Kingston USB 2.0 / ASSUMED_WRITE_THROUGH), H10 (SanDisk USB 3.2 Gen 1 SuperSpeed / SYNC_BACKED), H11 (Dell 5590 xHCI controller), H12 (Dell 5530 dual xHCI controllers) | [docs/subsystems/usb.md](docs/subsystems/usb.md#3-hardware-facts-and-verification-boundaries) |
| Shell & Input | H1 (PS/2 keyboard set 2 -> set 1, IRQ1), H4 (Belgian AZERTY layout and scancodes) | [docs/subsystems/shell.md](docs/subsystems/shell.md#3-hardware-facts-and-verification-boundaries) |
| Multi-Core (SMP) | H8 (IST2 exact-boundary NMI delivery; BSP-only vs multi-core) | [docs/subsystems/smp.md](docs/subsystems/smp.md#2-hardware-facts-and-verification-boundaries) |
| Platform & Power | H3 (PCI ECAM segment 0), H7 (ACPI S5 shutdown and multi-tier reset) | [docs/subsystems/platform.md](docs/subsystems/platform.md#2-hardware-facts-and-verification-boundaries) |

### Physical Acceptance Index

Physical bare-metal acceptance observations are detailed in their respective annexes:

- **Dell Latitude 5590:**
  - Networking Phase 2b (I219-LM RX/TX and wire capture): [docs/subsystems/net.md](docs/subsystems/net.md#3-physical-hardware-acceptance)
  - Shell S9 Introspection Suite (`ps`, `sysinfo`, `top` live redraw): [docs/subsystems/shell.md](docs/subsystems/shell.md#4-physical-hardware-acceptance)
  - Interactive Shell, Input & Belgian AZERTY (2026-09-16 & 2026-09-18): [docs/subsystems/shell.md](docs/subsystems/shell.md#dell-latitude-5590-physical-acceptance-2026-09-16--2026-09-18)
  - USB 3.2 SuperSpeed Storage & Durability: [docs/subsystems/usb.md](docs/subsystems/usb.md#3-hardware-facts-and-verification-boundaries)
  - 32 GiB RAM / High-Memory Probe: [docs/subsystems/storage.md](docs/subsystems/storage.md#3-hardware-facts-and-verification-boundaries)
  - SMP 8-core bring-up: [docs/subsystems/smp.md](docs/subsystems/smp.md#3-physical-hardware-acceptance)
  - ACPI S5 Power & Reset: [docs/subsystems/platform.md](docs/subsystems/platform.md#3-physical-hardware-acceptance)
- **Dell Latitude 5500:**
  - 8-CPU bring-up, 8 GiB DRAM, `ps`, `sysinfo`: [docs/subsystems/shell.md](docs/subsystems/shell.md#dell-latitude-5500-physical-acceptance-2026-09-30) and [docs/subsystems/smp.md](docs/subsystems/smp.md#3-physical-hardware-acceptance)

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
