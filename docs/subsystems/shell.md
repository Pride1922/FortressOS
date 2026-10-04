# Shell & Userland Subsystem Annex (S0–S9)

This annex documents the current status, hardware facts, verification evidence, and scope boundaries of the FortressOS interactive Ring 3 shell, process management, signals, job control, and system utilities. Binding contracts, locking rules (L1–L4, `g_process_lock`), syscall standards (S1–S4), and userland rules live in [`AGENTS.md`](../../AGENTS.md) (§4, §7.1, §7.6, and §9).

---

## 1. Subsystem Status and Overview

| Subsystem | Status | Detail |
| --- | --- | --- |
| **Shell S9** | **COMPLETE** (2026-09-30) | All phases verified. Phase 0 metadata prerequisites and cross-CPU spawn int 0x80 fix; Phase 1 `SYS_PROCINFO` and `/bin/ps` verified on QEMU and Dell 5590; Phase 2 `SYS_SYSINFO` and `/bin/sysinfo` verified on QEMU (BIOS/UEFI, SMP=1/4) with managed-RAM total and BSP monotonic timebase (multi-writer APIC scaling bug eliminated), plus Dell 5590 and Dell 5500 hardware acceptance; Phase 3 `/bin/top` live view verified on QEMU (BIOS/UEFI, SMP=1/4) and bare-metal Dell hardware (live console redraw, monotonic 100 Hz uptime, CPU% deltas, PS/2 'q' exit; all three tools ps/sysinfo/top verified live on physical hardware). Details: [`docs/roadmap/shell-s9-phase3.md`](../roadmap/shell-s9-phase3.md). |
| **Shell S8** | **COMPLETE** (2026-09-29) | User-accepted: Phases 1–6 verified. Phase 1 process identity, child records and the wait ABI; Phases 2A/2B signal infrastructure, default delivery, custom handlers, `sigreturn` and an NMI gate; Phase 2C stop/continue and `SIGCHLD`; the group-lifetime prerequisite; Phase 3 single-terminal foreground ownership (fd-based `SYS_TCSETPGRP`/`SYS_TCGETPGRP`, TTIN/TTOU, deferred ingress worker); Phase 4 job table, `&`, foreground/background launch, terminal handoff and idle-prompt reaping; Phase 5 `jobs`/`fg`/`bg`/`kill %n`, terminal restore and exit/orphan cleanup; Phase 6 SIGPIPE published on a write to a closed pipe, the default/caught/ignored/blocked disposition matrix and removal of tool-side 141 synthesis. QEMU verified under BIOS and UEFI at `SMP=1`, plus a manual Dell Latitude 5590 observation of default SIGPIPE termination ([Phase 6](../roadmap/shell-s8-phase6.md)); pipe/input peers remain BSP-pinned, so no cross-core execution is claimed. Full detail: [Phase 1](../roadmap/shell-s8-phase1.md), [Phase 2C](../roadmap/shell-s8-phase2c.md), [group lifetime](../roadmap/shell-s8-group-lifetime.md), [Phase 3](../roadmap/shell-s8-phase3.md), [Phase 4](../roadmap/shell-s8-phase4.md), [Phase 5](../roadmap/shell-s8-phase5.md). |
| **Shell S7** | **COMPLETE** (2026-09-27) | User-confirmed: Phases 1–6 verified. Blocking 64 KiB pipes, CLOEXEC cleanup, grouped pipelines, redirections, negation, cooperative teardown, stream utilities and builtin stages via `/bin/sh-builtin`. Fixed envp delivery with `mov rdx, rbp` in the trampoline, preserving argv and scheduler cleanup. QEMU BIOS/UEFI verified with `SMP=1/4/8`, AP counts confirmed in boot logs; Dell Latitude 5590 accepted in both RO and RW mount modes. Working pipelines: `echo hello \| wc -l`, `cat file \| head -n 5 \| wc -l`. Peers remain BSP-pinned; cross-core execution deferred. Details: [Phase 5B](../roadmap/shell-s7-phase5b.md), [Phase 6](../roadmap/shell-s7-phase6.md). |
| **Shell S6** | **COMPLETE** (2026-09-26) | Redirections, uniform descriptor architecture, resource bounds, RO/tainted storage assertions, documented single-threaded process rules (concurrent non-append shared-file_t I/O deferred to S7 pipelines), and physical Dell Latitude 5590 hardware acceptance. Phase 1–3 complete: negative error codes for dup/dup2, stream node EOF bypass (`VFS_STREAM`), numeric parser bounds, atomic acquire-release refcounting on `file_t`, atomic EOF append serialization under `ext2_lock` (`ext2_write(..., &offset, append, ...)`), host sequential verification, and true multi-core SMP concurrent append integration suite (`make test-smp-append`) verified under QEMU `-smp 4` (BIOS & UEFI) with offline `e2fsck -fn` integrity audits. Phase 4A–4D complete: child process file redirection via `SYS_SPAWN_EXT`, target expansion, ambiguous redirect rejection, scoped parent builtin redirection (`save/apply/restore`), redirection-only empty commands (`> file`), `SYS_FCNTL` (`F_DUPFD_CLOEXEC`, `F_GETFD`, `F_SETFD`), retained UI terminal handle (`g_term_fd` on FD 31 with `FD_FLAG_CLOEXEC`), `/bin/dual_stream` lexical duplication ordering, stderr append/truncation/closure, stdin `cat`, and non-recursive short-write error returns. Phase B complete: resource exhaustion suite (`make test-shell-s6-resources`) verified under BIOS & UEFI (1 and 4 CPUs) for child descriptor limit, parent fd table exhaustion, process table capacity, GDB scheduler inspection confirming 0 partial/runnable threads published on failed spawn, and prompt recovery. Phase C complete: RO and tainted ext2 storage assertions verified under BIOS and UEFI; distinct error strings (`Read-only filesystem.` and `I/O error.`), execution suppression on failed redirection setup, bit-for-bit file preservation, status propagation (`$? == 1`, `||` recovery, `&&` halt), and prompt recovery. Phase D complete: physical Dell Latitude 5590 acceptance verified on SanDisk USB 3.2 Gen 1 (RO Pass 1, RW Pass 2, offline host `e2fsck -fn` 0 errors, bit-for-bit SHA-256 match on all created files). Pipelines belong to S7. Full detail: [`docs/plans/S6_AUDIT.md`](../plans/S6_AUDIT.md). |
| **Shell S5** | **COMPLETE** (2026-09-25) | Environment, variables, parameter expansion, aliases, and globbing. Flat variables (scoped for S9), SYS_SPAWN_EXT ABI, stack budget assertion with 512B floor, top-down string packing, builtins (set, unset, export, env, alias, unalias), 5-stage expansion pipeline (tilde, parameter, word splitting, globbing, quote removal). Verified on BIOS and UEFI. Full detail: [`docs/roadmap/shell-s5.md`](../roadmap/shell-s5.md). |
| **Shell S3–S4** | **COMPLETE** (2026-09-25) | Quote-aware parser, comments, `;`/`&&`/`||`/`!`, multi-line continuation, process CWD (`SYS_GETCWD`/`SYS_CHDIR`), relative path normalization, `cd`/`pwd`, direct execution (`/bin/hello`, `./tool`, bare `hello`), Tab completion, configurable prompt with status indicator, length-framed persistent history (`/mnt/.fortress/history`). Verified on BIOS and UEFI. Full detail: [`docs/roadmap/shell-s3-s4.md`](../roadmap/shell-s3-s4.md). |
| **Shell S0–S2** | **COMPLETE** | Implemented: raw/timed terminal input, cursor/erase support, long-line editing, Ctrl/AltGr, RAM history/search. Automated evidence: [`docs/roadmap/shell-s0-s2.md`](../roadmap/shell-s0-s2.md). |

---

## 2. Narrative Milestones and Execution Boundaries

### Shell S8 Summary and Execution Boundary

Shell S8 Phases 1–6 are complete and user-accepted; Phase 6 closed the milestone on 2026-09-29. Phase 1 covers process identity, child records and the wait ABI, and Phases 2A/2B add signal infrastructure, default delivery, custom handlers, `sigreturn` and the NMI gate, including BIOS/UEFI sigreturn NMI coverage. Phase 2C adds stop/continue and durable child notification, and the separate group-lifetime prerequisite retains PGID identities, generations and bounded references. Phase 3 adds single-terminal foreground ownership, fd-based `SYS_TCSETPGRP`/`SYS_TCGETPGRP`, versioned input attributes, TTIN/TTOU enforcement and the BSP deferred ingress signal worker; Phase 4 the job table, `&`, foreground/background launch, terminal handoff and idle-prompt reaping; Phase 5 `jobs`/`fg`/`bg`/`kill %n`, terminal restore and exit/orphan cleanup; Phase 6 SIGPIPE published on a write to a closed pipe, the default/caught/ignored/blocked disposition matrix, and removal of the tool-side 141 synthesis. Evidence: [Phase 1](../roadmap/shell-s8-phase1.md), [Phase 2C](../roadmap/shell-s8-phase2c.md), [group lifetime](../roadmap/shell-s8-group-lifetime.md), [Phase 3](../roadmap/shell-s8-phase3.md), [Phase 4](../roadmap/shell-s8-phase4.md), [Phase 5](../roadmap/shell-s8-phase5.md) and [Phase 6](../roadmap/shell-s8-phase6.md). QEMU acceptance for the final phase was at `SMP=1` under BIOS and UEFI, with one manual Dell default-termination observation; pipe/input peers stay BSP-pinned, so no cross-core execution is claimed.

### Shell S7 Summary and Execution Boundary

Shell S7 Phases 1–6 are complete. See [Phase 5A utilities](../roadmap/shell-s7-phase5a.md), [Phase 5B builtin stages and envp diagnosis](../roadmap/shell-s7-phase5b.md), and [Phase 6 QEMU/Dell acceptance](../roadmap/shell-s7-phase6.md). Pipe peers remain on the BSP even in multi-CPU runs; cross-core wake channels remain future work.

---

## 3. Hardware Facts and Verification Boundaries

| ID | Evidence / constraint |
| --- | --- |
| H1 | **Dell 5590 photo:** keyboard input and IRQ1 initialization reported working. **Code:** `keyboard_init` clears translation while selecting/querying set 2, then sets bit 6 to deliver translated set 1. Preserve the sequence; it is not proof of the firmware's initial bit value. |
| H4 | **Code + user report (2026-09-16, fixed 2026-09-18):** `layout azerty` selects Belgian AZERTY (Punt). Top number row uses `shift ^ s->caps` as Shift-Lock for digits `1234567890`. Shifted table takes priority over `a..z` matching, resolving bug where scancodes 0x03, 0x08, 0x0A, 0x0B (`é è ç à`) and 0x28 (`ù/%`) emitted uppercase `'E'`, `'C'`, `'A'`, `'U'` instead of digits and `%`. Scancode 86 (0x56) added for ISO `<` / `>`. AltGr absent; historically arrows/function keys ignored; shell S1 adds arrows, Ctrl and AltGr (Dell verification pending), while function keys remain ignored and Caps LED unsynchronized. |

---

## 4. Physical Hardware Acceptance

### Dell bare-metal acceptance: Shell S9 introspection suite (2026-09-30)

User-supplied testing confirms physical bare-metal hardware operation on Dell hardware booted via UEFI from USB:
- `/bin/ps`, `/bin/sysinfo`, and `/bin/top` all operational in Ring 3.
- `/bin/top` live interactive framebuffer redraw confirmed with 100 Hz monotonic uptime advance, CPU% deltas, and clean PS/2 'q' exit returning to the shell prompt.

### Dell Latitude 5500 physical acceptance (2026-09-30)

User-supplied testing confirms hardware operation on a second physical machine, a Latitude 5500 (8 GiB installed RAM, 8 logical CPUs), booted via UEFI from USB: all 8 CPUs brought online, interactive Ring 3 shell reached, and `/bin/ps` and `/bin/sysinfo` operational with accurate ~8 GiB DRAM and monotonic 100 Hz uptime.

### Dell Latitude 5590 physical acceptance (2026-09-16 & 2026-09-18)

User-supplied testing confirms hardware operation on a Latitude 5590 (Core i5-8350U, 32 GiB installed RAM, 256 GB NVMe, Intel UHD 620), booted from USB:

- **2026-09-16:** PS/2 set 2 -> set 1 / IRQ1, COM1 RX absent, interactive shell `help`, `ls`, `cat etc/motd`, and missing-file error handling verified.
- **2026-09-18:**
  - **Belgian AZERTY (Bug H4):** Shift-Lock on top number row with Caps Lock ON verified producing digits `1234567890`. Accented keys unshifted produce base characters without falsely emitting uppercase letters. European ISO `<` / `>` key (scancode 0x56) verified.
  - **System V AMD64 ELF ABI:** `run /bin/hello testing ...` verified passing command-line arguments across the user/kernel boundary with proper 16-byte stack alignment.

---

## 5. Test Targets and Verification Notes

| Target | Scope / evidence |
| --- | --- |
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
| `make test-pipe` | S7 Phases 1–2 BIOS/UEFI (1 CPU), disposable test ISO with no data disks; kernel blocking/backpressure/close tests and Ring 3 pointer validation, CLOEXEC spawn actions, fd exhaustion, EOF/EPIPE. See [Phase 2 evidence](../roadmap/shell-s7-phase2.md). Cross-core wakeups remain deferred. |
| `make test-shell-host` | Consolidated ASan/UBSan: keyboard/queue, framebuffer terminal and actual shell editor/history logic |
| `make test-pipeline-host` | S7 Phase 4 actual parser/expander/executor with mocked syscalls: grouping, preflight, fd ownership, action order and failure injection. Also included in `test-shell-host`; Phase 4 accepted by the user; per-command results are not separately recorded. |
| `make test-stream-tools-host` | Phase 5A actual tools with ASan/UBSan and mocked syscalls: binary copying, short I/O, tail bounds/draining, CLI, descriptor cleanup and injected wc overflow. Included in `test-host`; user-reported pass 2026-09-27, not rerun by the agent. |
| `make test-shell-s7 SMP=N` | User-confirmed BIOS/UEFI passes with `SMP=1`, `4`, `8`, AP counts checked in boot logs. Disposable ISO/NVMe copy, streaming/status/cleanup and offline byte/e2fsck checks; pipe peers remain BSP-pinned. See [Phase 6](../roadmap/shell-s7-phase6.md). |
| `make test-shell-s6` | Shell S6 Phase 4A–4D under QEMU (BIOS & UEFI): child/parent redirection, dual-stream lexical ordering, stdin, stderr append/truncation/closure, expansion, failed setup and prompt recovery. |
| `make test-shell-s6-resources` | Shell S6 Phase B resource limits under QEMU (BIOS & UEFI, 1 & 4 CPUs): child descriptor limit (32), parent fd table exhaustion, process table capacity, GDB scheduler inspection confirming 0 partial/runnable threads published on abort, prompt recovery. |
| `make test-shell-integration` | Existing shell integration extended with cursor/screen-state, history/search/paste, timeout/log separation and no-UART coverage; snapshot NVMe fixture for normal runs |
| `make test-input` | Host ASan/UBSan: decoder, modifiers and bounded FIFO |
| `make test-shell` | BIOS/UEFI IRQ1/IRQ4 interaction, sleeping readers, restart counts; also UEFI 8 GiB without COM1 |

---

## 6. Diagnostic & Troubleshooting Utilities

| Utility | Invocation | Pipeline Safe | Description |
| --- | --- | --- | --- |
| `dmesg` | `dmesg [-n N \| tail [N]] [<lines>] [path]` | **Yes** (`child_safe = true`) | Inspects the kernel 64 KiB ring buffer (`SYS_DMESG`). Available as a shell builtin and standalone ELF tool `/bin/dmesg`. Supports direct tailing (`dmesg -n 13`, `dmesg tail 20`, `dmesg 15`), disk persistence (`dmesg -n 25 /mnt/error.log`), and child-safe pipeline integration (`dmesg \| tail`, `dmesg \| wc -l`) for rapid live troubleshooting of driver, storage sync, and network events. |

---

## 7. Interactive Terminal & Prompt Resilience

### Screen Generation Tracking & Automatic Prompt Redraw
Background jobs (e.g. `wget &`, background compiler or download workers) writing to `/dev/tty` previously clobbered the interactive prompt, requiring `wget -q` or pressing Enter to restore visual context.

FortressOS implements display generation tracking and reactive prompt restoration:
1. **Kernel Generation Counter (`console_inc_generation`):**
   - The kernel maintains an atomic generation counter incremented whenever bytes are written to the terminal (`terminal_write` in VFS and `console_terminal_write`).
   - The current generation is reported via `SYS_TERMCTL(TERM_GET, &term)`.
2. **Interactive Redraw Loop (`user/shell/ui.c`):**
   - When idling at the prompt in `shell_read_line()`, the shell blocks on input with 100 ms timeouts.
   - On each timeout or interrupt, the shell checks `SYS_TERMCTL(TERM_GET, &cur_term)`.
   - If `cur_term.generation != term.generation`, external output has occurred on the display. The editor emits `\n` to clear past the background text, restores the prompt, and repaints the active draft line and cursor position without user keypress.
   - Prompt painting itself synchronizes `term.generation` to avoid spurious redraw loops.


