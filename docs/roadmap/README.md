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
`AGENTS.md`'s status table and [`docs/subsystems/`](../subsystems/README.md) for current, up-to-date status per phase.

## Phase files

| File | Covers |
| --- | --- |
| [ext4-phase8-6.md](ext4-phase8-6.md) | Complete bounded fixture integration audit, runtime home-image coverage, shared write offsets, durable-truncate cache coherence; host fault/staging and BIOS/UEFI SMP=1/4 recovery/lifecycle gates. Production journaled RW remains disabled. |
| [ext4-phase8-5.md](ext4-phase8-5.md) | Explicit disposable journal mounts with validated recovery before publication, real VFS lifetimes, SYS_SYNC and shutdown clean-state transactions; production journaled RW remains disabled. |
| [ext4-phase8-4.md](ext4-phase8-4.md) | Exclusive traditional orphan/shrink recovery, bounded in-place reclamation and revokes, open-unlink handle lifetimes and disposable host/Linux verification; production journaled RW remains disabled. |
| [ext4-metadata-cache.md](ext4-metadata-cache.md) | Bounded coherent RW metadata cache, validation on hits and commit/failure containment; Dell physical acceptance PASS, 16 MiB 10.15 → 7.06 s, reboot/Mint hashes and unmounted fsck exit 0. |
| [ext4-phase7.md](ext4-phase7.md) | Ordered JBD2 writer/checkpoint workbench, credits/states/circular reuse, 12 geometry/placement cases and Linux replay/fsck; production VFS journaling remains Phase 8. |
| [ext4-phase6.md](ext4-phase6.md) | Bounded JBD2 recovery workbench, explicit replay admission, host crash/restart model, 12 geometry/source cases and 48 zero-write rejections; production journal mounts remain disabled. |
| [ext4-phase5.md](ext4-phase5.md) / [Dell checklist](ext4-phase5-dell.md) | Selected USB EXT4 mount/sync/shutdown integration, separate non-journaled image; automated validation verified; Dell performance/hash/reboot/fsck manually PASS, all Phase-5 physical checklist items user-confirmed PASS (2026-10-03). |
| [ext4-usb-performance.md](ext4-usb-performance.md) | Bounded 4 KiB BOT runs, fine polling and zero staging; host and BIOS/UEFI USB gates PASS, Dell manual performance/hash/reboot/fsck PASS. |
| [net-traceroute.md](net-traceroute.md) | Finite numeric ICMP traceroute, approved probe ABI and shared ping mailbox; host sanitizer and QEMU 4/4 PASS, Dell acceptance pending. |
| [download-checksums.md](download-checksums.md) | Streaming MD5/SHA-256 tools and manifests; host sanitizer/4 GiB streams and BIOS/UEFI 32/32 PASS, Dell acceptance pending. |
| [net-config.md](net-config.md) | COMPLETE (2026-10-02): NET-3 runtime network configuration (/bin/ifconfig, /bin/ifup, /mnt/.fortress/network.conf, DNS fallback); host ASan/UBSan PASS, BIOS/UEFI QEMU 10/10 PASS, Dell Latitude 5590 physical acceptance user-confirmed PASS. |
| [net-link-recovery.md](net-link-recovery.md) | COMPLETE (2026-10-02): Cold cable waiting, deferred activation, and retained-ring link recovery; host/QEMU 8/8 PASS, Dell Latitude 5590 physical acceptance 6/6 PASS (autonomous PHY renegotiation confirmed). |
| [net2-step7.md](net2-step7.md) | COMPLETE: DNS/nslookup/hostname nc, approved TCP deadlines, host/QEMU evidence and Dell 5590 physical DNS user-confirmed PASS; NET-2 7/7 complete. |
| [net2-step6.md](net2-step6.md) | Dell 5590 physical TCP A–D user-confirmed PASS; Step 6 closed, NET-2 6/7 complete. |
| [net2-step6-caseC.md](net2-step6-caseC.md) | Confirmed stdin invocation issue; terminal-aware nc listener and BIOS/UEFI tty/null/file/pipe data+FIN tests. Physical retry accepted. |
| [net2-step5.md](net2-step5.md) | Finite serial nc, independent golden-vector TCP socket peer/audit, persistent failure artifacts and complete backend matrix. |
| [net2-step4.md](net2-step4.md) | Bounded listener/half-open queues, ACCEPT adoption/rollback proof, finite Ring 3 server and five-case capture/lifecycle evidence. |
| [net2-step3.md](net2-step3.md) | Numeric TCP client/worker/socket integration, reservation-free wait argument, frozen client ABI, quiet time and independent live-peer evidence. |
| [net2-step2.md](net2-step2.md) | Pure bounded TCP transport, lifecycle pool and deterministic fake-network sanitizer gate; live integration pending. |
| [net2-step1.md](net2-step1.md) | NET-2 started: reviewed design baseline, pure TCP codec and host sanitizer gate; live TCP transport not enabled. |
| [net-i219-5530.md](net-i219-5530.md) | 8086:1A1E discovery/general PCH registration; user-reported physical driver PASS on 5530; unchanged 5590 gates and passing regressions. |
| [net-phase5b.md](net-phase5b.md) | User-reported physical UDP PASS on 5590; independent capture/application artifact audit pending. |
| [net-phase5a.md](net-phase5a.md) | COMPLETE host/QEMU: bounded BSP-only UDP sockets, syscalls 38–41, Ring 3 tools, binary/ABI/fd fixtures and 10 QEMU cases. 5590 UDP is user-reported PASS; capture audit pending; includes Windows peer/capture procedure. |
| [net-phase4b.md](net-phase4b.md) | COMPLETE (2026-10-01), physical LAN-peer gate accepted: Dell Latitude 5590 I219-LM at `net=192.168.0.168/24,192.168.0.1`; guest→gateway ping 4/4 (0% loss, reported 20 ms RTT) with usable shell, guest→Windows 11 (`192.168.0.222`) after allowing inbound ICMP, second-host Wireshark screenshot with four matched request/reply pairs (sequences 1–4, all 74 bytes: frames 923/924, 931/932, 942/943, 947/948), and reverse Windows→guest ping 4/4 (TTL 64; RTT min 3/max 17/avg 9 ms). Manual/user-supplied, not an automated pass; no pcap supplied, so no independent payload/checksum verification or saved artifacts; guest RTT includes polling/scheduling effects; no sustained-load/idle-CPU/cross-core claim. Phase 5a host/QEMU verified; 5590 UDP user-reported PASS; capture audit pending |
| [net-phase4a.md](net-phase4a.md) | COMPLETE (2026-10-01): IPv4/ICMP, finite worker-owned ping mailbox and real Ring 3 `/bin/ping`; host/QEMU evidence, concrete ABI and owner-exit fallback. Physical 4b accepted separately — see [net-phase4b.md](net-phase4b.md). |
| [net-phase3.md](net-phase3.md) | COMPLETE (2026-10-01): Ethernet/ARP stack dispatch, reply-only ARP cache learning, bounded `net=` config, exact-once RX recycling, and a BSP-pinned worker that sleeps until the next BSP tick. The user explicitly authorized one extra BSP timer wake target (`net_timer_tick()` alongside `input_timer_tick()`, waking `&g_net_poll_channel`) with no scheduler/lock-rank/`sched_wait_until` change. Evidence: `make test-net-eth-host` (Phase 3 ASan/UBSan, mocked NIC/scheduler/ticks), `make test-net-eth` 8/8 QEMU (BIOS/UEFI × e1000/e1000e × {user SLIRP, socket injection}), plus Phase 0 `make test-net-host` 103/103 and `make test-net-rings` 4/4 raw-ring regressions. No physical acceptance, sustained load, measured idle CPU or cross-core delivery claimed |
| [net-phase2b.md](net-phase2b.md) | I219 SPT/CNP MAC takeover/DMA and fatal containment implemented; host mocks PASS. **Physically accepted on the Dell Latitude 5590 (2026-09-30, manual observation + cable-side capture):** RX completed (`DD`/`EOP`, `errors=0`, `length=60`, `RDH` 0→3) and TX completed (`[NET 2b] TX PASS: 60-byte 88B5 frame, DD observed`), with the 60-byte `0x88b5` broadcast frame independently captured on the wire by a second host (Wireshark frame #437, source `C8:F7:50:0E:35:80`). Root cause of the earlier TX stall was the lost upper `TCTL` bits (`0x0103F0FA` vs. the working Linux e1000e `0x3103F0FA`), fixed alongside `FEXTNVM11` bit 13, the SPT `CTRL_EXT`/`TARC`/`IOSFPC` workarounds and PHY link-up. `make test-net-rings` 4/4 BIOS/UEFI × e1000/e1000e SMP=1 and `make test-net-rings-host`/`make test-net-i219-host` PASS under sanitizers. The file's own in-progress notes are the pre-acceptance checkpoint, superseded by the recorded acceptance |
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
[NET NET-1]  Ethernet/ARP, IPv4/ICMP ping, BSP worker       (Phases 0–4 COMPLETE: host + QEMU; 4b physical LAN-peer accepted; 5 UDP next)
[Following]  Accounts/permissions, then installer           (NEXT)
```

E4-A: [Dell 5590 physical acceptance](ext4-phase5-acceptance.md), all seven Phase-5 items PASS, 2026-10-03.

[TCP application poll hints](net-tcp-poll-hints.md): avoid tick-only delay for queued application ACK/window updates, 2026-10-04; physical throughput retest pending.

[TCP performance fix: four-step plan](../plans/TCP_PERFORMANCE_FIX_PLAN.md): reader handoff, window-update service, bounded active RX wait, and automated/Dell acceptance; 2026-10-04.

[Blocking shell supervisor](kernel-supervisor-wait.md): approved scope extension removes runnable HLT waiter; BIOS/UEFI SMP=1/4 blocked-state and restart checks PASS; Dell throughput pending.
