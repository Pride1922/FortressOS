# FortressOS Roadmap — Index

This directory replaces the old monolithic `ROADMAP.md`. Each file below
covers one phase's implementation notes and verification evidence. Point an
agent at the specific phase file relevant to its task, not this whole
directory — that's the entire point of the split.

For binding implementation rules, task routing, and current hardware
evidence, read `AGENTS.md`. For the always-load guardrail list, read
`PROTECTED.md`. For audit limits and technical debt, read `ARCH_REVIEW.md`.
For design proposals and staged implementation plans, see [`docs/plans/`](../plans/README.md).
Code and public headers remain the implementation reference.

Historical statements in each phase file describe *that* checkpoint; later
phases supersede earlier deferred-work notes. They are not new instructions
and are not proof that every current revision has passed every test — check
`AGENTS.md`'s status table for current, up-to-date status per phase.

## Phase files

| File | Covers |
| --- | --- |
| [net-phase2b.md](net-phase2b.md) | I219 SPT/CNP MAC takeover/DMA and fatal containment implemented; host mocks PASS. Dell TX still stalled, but RX-only run observed 67 descriptor completions (exact peer marker not matched). Added 15D7 reference PLL/K1/PHY FIFO-gap link-up subset with bounded MDIC/page/ownership handling; host sanitizer and four QEMU rings regressions PASS. Next build performs isolated TX trial. Dell TX DD and cable-side `0x88B5` capture pending; physical acceptance not complete |
| [net-phase2a.md](net-phase2a.md) | COMPLETE (2026-09-30): 64-entry legacy TX/RX rings, PMM DMA pages, eth0 raw callbacks and bounded RX recycling, fatal quarantine; exact TX pcap and injected RX byte checks PASS BIOS/UEFI × e1000/e1000e SMP=1, plus host sanitizer ownership/failure coverage. QEMU only; Dell DMA/raw frame is Phase 2b |
| [net-phase1b.md](net-phase1b.md) | COMPLETE (2026-09-30): Networking Phase 1b Dell Latitude 5590 hardware discovery — Intel I219-LM `8086:15D7` at `0000:00:1F.6`, BAR0 `0xEF300000` mapped to `0xFFFFFFFFE2000000`, MAC `C8:F7:50:0E:35:80`, `STATUS` link-down/link-up (`0x40080000` / `0x00080083`), link negotiates so the `NET_PLAN.md` §7.2 PHY/CSME risk is not blocking; device-ID additions (`0x15D7`/`0x15BD`/`0x15BB`). Manual hardware observation, boot-log photos — QEMU `test-net-pci` remains the primary logic evidence |
| [net-phase1a.md](net-phase1a.md) | COMPLETE (2026-09-30): Networking Phase 1a Intel e1000/e1000e PCI discovery (8086:100E/10D3), uncached MMIO mapping (`0xFFFFFFFFE2000000ULL`), MAC address read, link STATUS register, and absent NIC clean fallback; verified across BIOS/UEFI in QEMU |
| [net-phase0.md](net-phase0.md) | COMPLETE (2026-09-30): Networking Phase 0 core abstractions (`net_dev_t`, `pbuf_t`), RFC 1071 ones' complement checksum, Ethernet II, ARP (with 16-entry cache stub), IPv4 codecs and fragment rejection; 103/103 host ASan/UBSan tests pass |
| [shell-s9-phase3.md](shell-s9-phase3.md) | COMPLETE (2026-09-30): /bin/top live view, CPU% delta calculation, stable PID sorting, interactive terminal redraw, QEMU acceptance suites (BIOS/UEFI, SMP=1/4), and bare-metal Dell acceptance — closes Shell S9 milestone |
| [shell-s9-phase2.md](shell-s9-phase2.md) | COMPLETE (2026-09-30): SYS_SYSINFO and /bin/sysinfo, managed-RAM total (prereq 1), BSP monotonic timebase (prereq 2) with multi-writer APIC scaling bug eliminated; verified across BIOS/UEFI on SMP=1 and SMP=4, and Dell Latitude 5500 physical acceptance |
| [shell-s8-phase6.md](shell-s8-phase6.md) | COMPLETE, user-accepted 2026-09-29: writer-only SIGPIPE publication on a write to a closed pipe, default/caught/ignored/blocked disposition handling, tool-side 141 synthesis removed; BIOS/UEFI `SMP=1` host and real-shell gates plus a manual Dell default-termination observation — closes the S8 milestone |
| [shell-s8-phase5.md](shell-s8-phase5.md) | COMPLETE, user-confirmed 2026-09-28: jobs/fg/bg/kill, terminal restore and exit/orphan cleanup; BIOS/UEFI real-shell gate, host suites and regressions pass |
| [shell-s8-phase4.md](shell-s8-phase4.md) | COMPLETE, user-confirmed 2026-09-28: job table, background launch, terminal handoff and idle reaping; 15 host cases plus BIOS/UEFI SMP=1 Ring 3 and real-shell acceptance |
| [shell-s8-phase3.md](shell-s8-phase3.md) | Implemented, acceptance pending: terminal ownership, shared input attributes, foreground read/control enforcement, deferred ingress signals and test handoff |
| [shell-s8-group-lifetime.md](shell-s8-group-lifetime.md) | Separate Phase 3 prerequisite: retained group identities, generations and bounded references; user reported prerequisite tests passing 2026-09-28 |
| [shell-s8-phase2c.md](shell-s8-phase2c.md) | Implemented, runtime acceptance pending: STOPPED scheduling, CONT/KILL resume, durable child transitions, SIGCHLD and test handoff |
| [shell-s7-phase6.md](shell-s7-phase6.md) | COMPLETE: QEMU BIOS/UEFI with 1/4/8 CPUs and Dell RO/RW acceptance; BSP-pinned peers, cross-core G6 deferred |
| [shell-s7-phase5b.md](shell-s7-phase5b.md) | Builtin runner path complete (2026-09-27), envp entry fix, QEMU regression evidence and verification boundaries |
| [shell-s7-phase5a.md](shell-s7-phase5a.md) | Stream utilities and cat/view migration; user accepted successful tests 2026-09-27, builtin stages remain 5B |
| [shell-s7-phase4.md](shell-s7-phase4.md) | External pipeline executor implementation, build checks, test commands and user acceptance (2026-09-27); BSP-only |
| [shell-s7-phase3.md](shell-s7-phase3.md) | Completed flat pipeline grammar, eight-stage limit, temporary execution guard, and user-reported regression evidence |
| [shell-s7-phase2.md](shell-s7-phase2.md) | VFS stream lifecycle, blocking scheduler integration, threshold writer predicate, 3-phase CLOEXEC spawn lifecycle, and BIOS/UEFI verification |
| [shell-s7-phase1.md](shell-s7-phase1.md) | Anonymous pipes, VFS final-close hook, SYS_PIPE ABI, host sanitizer coverage and BIOS/UEFI Ring 3 checks; blocking remains Phase 2 |
| [shell-s0-s2.md](shell-s0-s2.md) | Shell foundation, terminal byte input/output, line editing and RAM history; automated verification and pending Dell checklist |
| [shell-s6.md](shell-s6.md) | Phase 4 and Phase B formal validation across five-gate suite, resource limits, and remaining acceptance boundaries |
| [shell-s5.md](shell-s5.md) | Environment, variables, parameter expansion, aliases, and globbing (flat variables, SYS_SPAWN_EXT ABI, stack budget guard, 5-stage expansion pipeline) |
| [shell-s3-s4.md](shell-s3-s4.md) | Single parser (quotes, escapes, operators), working directories (cd, pwd, relative path resolution), direct execution, completion, prompt customization, and persistent history |
| [phase-9c2-readonly-ext2.md](phase-9c2-readonly-ext2.md) | Read-only ext2 mount at `/mnt` (superseded/extended by Phase 9D for writable support) |
| [phase-9d-writable-ext2.md](phase-9d-writable-ext2.md) | Bounded writable ext2: allocation/truncation ordering, superblock clean/dirty lifecycle, 3-boot persistence |
| [phase-9e-exec-and-files.md](phase-9e-exec-and-files.md) | Program execution (spawn/wait, ABI, exit status, chaining), directory ops (mkdir/rename/unlink), Bug H4 AZERTY fix |
| [phase-9g1-xhci-enumeration.md](phase-9g1-xhci-enumeration.md) | xHCI controller discovery, MMIO/reset, rings, ports, device addressing & descriptors (9G.1a–9G.1e) |
| [phase-9g2-usb-block.md](phase-9g2-usb-block.md) | Bulk-Only Transport, SCSI engine, block device registration, GPT discovery |
| [phase-9g3-usb-mount.md](phase-9g3-usb-mount.md) | Production `/mnt` mount, PARTUUID selection, cmdline parsing |
| [phase-9g4-usb-durability.md](phase-9g4-usb-durability.md) | Writable persistence, BOT stall recovery, durability classification |
| [phase-9g5-superspeed.md](phase-9g5-superspeed.md) | Multi-controller enumeration (9G.5a), SuperSpeed/USB 3.x (9G.5b) |
| [phase-9h-ram.md](phase-9h-ram.md) | 32 GiB RAM support, two-stage PMM/VMM init, HHDM coverage limit |
| [smp-piece1-ap-discovery.md](smp-piece1-ap-discovery.md) | SMP Piece 1: AP discovery via Limine's SMP protocol, cross-checked against ACPI MADT; QEMU/Dell verified |
| [smp-piece2-percpu.md](smp-piece2-percpu.md) | SMP Piece 2: GS state, CPU-local GDT/TSS/stacks, SWAPGS/NMI handling; QEMU and Dell 5590 verified |
| [smp-piece3-lock-discipline.md](smp-piece3-lock-discipline.md) | SMP Piece 3: Lock discipline, per-CPU tracker, atomic contention, AP panic isolation |
| [smp-piece4-scheduler.md](smp-piece4-scheduler.md) | SMP Piece 4: The SMP scheduler, per-CPU runqueues, task migration, and dual-lock work-stealing |
| [smp-piece5-ipi.md](smp-piece5-ipi.md) | SMP Piece 5: Cross-core coordination, APIC ICR messaging, synchronous TLB shootdown (SM14/SM15), remote wakeup |
| [smp-piece6-memory.md](smp-piece6-memory.md) | SMP Piece 6: 6A boot memory readiness, 6B PMM synchronization/multi-core stress, 6C contention-safe TLB shootdown, and 6D address-space lifetime discipline COMPLETE |
| [smp-cross-cpu-spawn.md](smp-cross-cpu-spawn.md) | Cross-CPU user spawn fix (int 0x80 gate ordering, BIOS/UEFI SMP 1/4/8 verified); OPEN: AP user-mode exception routing |
| [subsystems.md](subsystems.md) | Cross-cutting notes not tied to one phase: NMI delivery, boot console, interactive shell/input, general Dell acceptance |

## Checkpoint sequence (for orientation only — not authoritative status)

```
[Phase 1–9C] Boot, memory, threads, syscalls, VFS, storage track (COMPLETE)
[Phase 9D]   Bounded writable ext2                         (COMPLETE)
[Phase 9E]   Program execution, ABI, exit status            (COMPLETE)
[Phase 9F]   Raw disk image packaging                       (COMPLETE)
[Phase 9G]   USB storage, through 9G.5b                     (COMPLETE)
[Phase 9G.5] USB topology expansion                         (9G.5a, 9G.5b complete; 9G.5c/d/e open)
[Phase 9H]   32 GiB RAM support                              (COMPLETE)
[SMP]        Multi-core support                              (Pieces 1-5, 6A, 6B, 6C, 6D COMPLETE)
[Shell S0–S2] Terminal foundation, editing, RAM history      (Implemented, QEMU PASS, Dell pending)
[Shell S3–S5] Quotes, variables, expansion, persistent history (COMPLETE)
[Shell S6]    Redirection & descriptor architecture           (COMPLETE, Dell 5590 verified)
[Shell S7]    Pipes and stream utilities                      (Phases 1–6 COMPLETE; QEMU 1/4/8 CPUs, Dell RO/RW)
[Shell S8]    Jobs, signals and process groups               (COMPLETE; QEMU BIOS/UEFI SMP=1 + Dell SIGPIPE observation)
[Following]  Accounts/permissions, then installer           (NEXT)
```
