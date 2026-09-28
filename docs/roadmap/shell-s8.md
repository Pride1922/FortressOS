# Shell S8 — jobs, signals and process groups

Per-phase implementation and evidence log for [S8_PLAN.md](../plans/S8_PLAN.md).
Checkpoints that already have their own handoff records remain there:
[Phase 1](shell-s8-phase1.md), [Phase 2B NMI gate](shell-s8-phase2b-nmi.md),
[Phase 2C](shell-s8-phase2c.md),
[group-lifetime prerequisite](shell-s8-group-lifetime.md) and
[Phase 3 terminal ownership](shell-s8-phase3.md).

Record the exact command, firmware, CPU count and result as each phase
completes. An implementation handoff is not a pass, a user-reported result is
labelled as such, and QEMU results never stand in for physical acceptance.

## Phase 3 — Terminal foreground ownership

Implemented 2026-09-28 (commit `2aa9e75`), with runtime acceptance reported by
the user. The agent compiled and linked the changed kernel/shell/Ring 3
artifacts and syntax-checked the runners, but executed no host fixture and no
QEMU boot; the results in Evidence are user-reported.

### Implementation

`input.c` owns one static kernel terminal object: the controlling session ID,
`fg_pgid`, the owned foreground group handle, versioned input settings
(`TERM_ISIG`, `VINTR` 3, `VSUSP` 26), the existing 256-byte input FIFO and a
16-entry ingress-signal queue. It reuses the existing input buffer and
IRQ-exclusion protocol rather than adding a second byte queue or lock. The boot
shell is already a session/group leader; `kmain` assigns its terminal
(`input_terminal_bootstrap`) before preemption and the APIC timer are enabled.
Shell restart resets the input settings and byte queue, releases the previous
foreground reference and assigns the new leader. Already queued events keep
their original group references and are not retargeted.

`SYS_TCSETPGRP` (30) `(fd, pgid)` and `SYS_TCGETPGRP` (31) `(fd)` resolve the
caller's actual terminal file node through the descriptor and validate its
controlling session: invalid descriptors return `EBADF`, nonterminal or
wrong-session handles `ENOTTY`, invalid PGIDs `EINVAL`, absent groups `ESRCH`
and cross-session targets `EPERM`. Handoff retains the new group before
releasing the old one, then marks readers for a timer wakeup. FD 31 (the
retained CLOEXEC UI handle) receives exactly the same ownership checks as any
other terminal descriptor. Background output stays allowed; TOSTOP is not
implemented.

The common input-read path checks ownership before sleeping and after every
wake, before dequeuing, so its wait predicate observes foreground changes
without acquiring a process lock under the scheduler lock. Background reads
publish `SIGTTIN` to the caller's group; ignored or blocked `SIGTTIN` returns
`EIO` without consuming data. Default stops park through the existing signal
boundary and recheck after continuation; caught signals return `EINTR`. Pipes
and files never pass through this terminal check, and direct `SYS_INPUT_READ`
is checked too. A background `tcsetpgrp`, `SYS_TERMATTR` `TERM_SET` or
`SYS_TERMCTL` `TERM_SET` publishes `SIGTTOU`: blocked or ignored `SIGTTOU`
permits the mutation, a default stop retries after continuation, and a caught
signal returns `EINTR` with no mutation. Every ownership and input-control call
rejects non-BSP callers with `EOPNOTSUPP`, and legacy terminal VFS reads
translate the input error into a VFS error before syscall translation.

Decoded keyboard sequences and normalized UART bytes both enter one `ingress`
path. ISIG control bytes are consumed at arrival: they flush pending ordinary
input and acknowledge earlier byte-loss state, even when the signal queue
overflows, and never flush output. ISIG off preserves literal control bytes,
and a keyboard escape sequence enters as a whole (accepted or dropped
atomically). Queue entries own a clone of the foreground group handle at
arrival and coalesce only on identical slot, generation and signal; a full
queue drops the newest event and increments the diagnostic counter. Producers
never acquire the process lock, scan groups, allocate, log, schedule or invoke
a handler, and EOI ownership is unchanged. A BSP-pinned `terminal-signals`
kernel thread is created before input IRQs are enabled; the timer wakes it when
events or unreported drops are pending, and it publishes through
`process_group_signal` from thread context under the established IRQ-excluded
`sched_wait_until` protocol, releases each popped reference and yields after a
bounded batch. It works with no process reading the terminal and never holds a
spinlock across a context switch.

### Test handoff

```sh
make test-s8-terminal-host
make test-s8-terminal SMP=1
make test-nmi
make test-s8-groups-host test-s8-process-host test-s8-signals-host test-s8-stops-host
make test-s8-process test-s8-signals test-s8-stops
make test-shell-host test-pipeline-host test-pipe-host
make test-shell test-pipe test-shell-s7
```

`test-s8-terminal-host` links the actual terminal/input and group logic with
explicit IRQ, device and scheduler adapters under ASan/UBSan: defaults,
fd/session/CPU validation, invalid attributes, blocked/ignored reads without
data consumption, simulated stop and handoff, ownership recheck after a wake,
background mutation, literal bytes, targets captured through handoff,
coalescing, overflow, atomic escape sequences, UART CRLF, timer wake
publication and exact group-reference cleanup. It does not prove real IRQ
exclusion or scheduling.

`test-s8-terminal` builds a disposable ISO, attaches no data disk and rejects
extra backends in the final argv preflight; UEFI uses read-only OVMF code plus
disposable variables. BIOS and UEFI each drive real QMP PS/2 key events and UART
socket bytes, require every fixture marker, check the boot-log AP count against
the requested CPU count, bound each stage and the whole run, and always
terminate QEMU. Ring 3 covers fd/pointer/size errors, ignored and blocked
`SIGTTIN` through FD 31 and `SYS_INPUT_READ`, background pipe input, default
TTIN stop with foreground resume, TTOU stops for all three mutation APIs,
two-member no-reader foreground groups killed by UART and keyboard Ctrl+C,
keyboard Ctrl+Z stopping both members, kill-without-CONT cleanup, FD 31 reclaim
with stdin redirected, CLOEXEC, and UART/keyboard ISIG-off bytes. Logs:
`build/s8-terminal-{bios,uefi}-{1,4,8}.log` with matching `.stderr` files.

### Evidence

- `make test-s8-terminal-host` — pass (user report).
- `make test-s8-terminal SMP=1` — pass under BIOS and UEFI (user report), with
  real UART and PS/2 input and the boot-log AP count verified. `SMP=4` and
  `SMP=8` were not reported.
- `make test-nmi` — pass on three consecutive runs (user report), after the
  runner fix in Notes below; coverage is the legacy INT 0x80 recovery check, the
  seven SYSRET boundaries and the sigreturn fixture under BIOS and UEFI.
- Regressions (user report): `test-s8-groups-host`, `test-s8-process-host`,
  `test-s8-signals-host`, `test-s8-stops-host`, `test-s8-process`,
  `test-s8-signals`, `test-s8-stops`, `test-shell-host`, `test-pipeline-host`,
  `test-pipe-host`, `test-shell`, `test-pipe` and `test-shell-s7` — pass.
- Not claimed: physical keyboard/UART acceptance, storage acceptance, `SMP=4`
  or `SMP=8` terminal runs, and worker-starvation or IRQ-handoff-race
  determinism (overflow and rapid handoff are covered by the host fixture only).

### Notes

**`step` → `resume_to` NMI runner fix.** The user reported a repeatable BIOS
sigreturn failure at the post-IRET CS/SS assertion, following the
syscall-origin before-SWAPGS pass. The failing probe observed the Ring 3
boundary by single-stepping from the debugger stop, but a debugger single-step
stop after `IRETQ` need not be the user boundary: an interrupt can intervene
once IF is restored. `signal_probe` now arms a hardware breakpoint on the
expected user RIP and observes it before its first instruction
(`remote.resume_to(original[17])`), and prints expected/actual RIP, RSP,
selectors, RFLAGS and GS plus QEMU `info registers` when a check fails. Every
selector, RIP/RSP, RFLAGS, GPR and GS assertion remains mandatory, and exact NMI
injection and kernel-frame preservation checks are unchanged. No kernel or
return-path assembly was modified for this follow-up; the change is runner-only.

**Fixture adjustments.** The signal and stop fixtures now keep their
terminal-blocked cases in the foreground group, so they still exercise the
original blocked-input regression instead of accidentally testing background
TTIN. The shell debugger test walks the blocked list to the actual `shell`
thread (the terminal worker also sleeps there) and compares its wait channel
against `input_wait_channel_debug`, because the FIFO now lives inside the
terminal object; `test_shell.py` reads the `tcb_t` `name`/`next` offsets for
that walk.

**Scope boundaries.** Phase 3 adds no jobs, background launch, `fg`, `bg`,
`jobs` or stopped-job management; those remain Phases 4–5, and ordinary shell
launches still share the shell group. Ignored dispositions still inherit under
the existing spawn contract, and Phase 4 must reset child job-control
dispositions as S8_PLAN.md requires. No `setsid`, PTY or multiple terminals, no
TOSTOP, no automatic syscall restart and no cross-core blocking channels: pipe
and input peers stay BSP-pinned even in SMP runs. The Phase 3 fixture performs
its own grouping and handoff, so its acceptance is not a claim that the
interactive shell already implements job control.

**Files changed (commit `2aa9e75`, 22 files).** Kernel, syscall surface and VFS:
`src/drivers/input.c`, `src/drivers/input.h`, `src/include/terminal.h`,
`src/include/syscall_abi.h`, `src/kernel/syscall.c`, `src/kernel/main.c`,
`src/fs/vfs.c`. User shell: `user/shell/ui.c`, `user/shell/program.c`. Tests and
runners: `tests/s8_terminal_user.c`, `tests/s8_terminal_host.c`,
`tests/s8_signal_user.c`, `tests/s8_stop_user.c`, `scripts/test_s8_terminal.py`,
`scripts/test_s8_terminal_host.py`, `scripts/test_nmi_transitions.py`,
`scripts/test_shell.py`, `Makefile`. Documentation:
`docs/roadmap/shell-s8-phase3.md`, `docs/roadmap/README.md`,
`docs/roadmap/shell-s8-group-lifetime.md`, `AGENTS.md`. The annotated per-file
list is in [Phase 3](shell-s8-phase3.md).
