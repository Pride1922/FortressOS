# Shell S8 Phase 3 — terminal foreground ownership

Implemented 2026-09-28. Compilation/link checks completed; host and QEMU tests
are supplied for user execution and acceptance is pending. The separate
[group-lifetime prerequisite](shell-s8-group-lifetime.md) was reported passing
by the user before this implementation.

## Implementation

`input.c` owns one static terminal containing the controlling SID, foreground
PGID and owned group handle, versioned input settings, the existing 256-byte
FIFO, and a 16-entry signal-event queue. This reuses the existing input buffer
and IRQ exclusion protocol rather than introducing a second byte queue or lock.
The boot shell is already a session/group leader; main assigns its terminal
before enabling preemption. Shell restart resets input settings and the byte
queue, releases the previous foreground reference, and assigns the new leader.
Already queued events keep their original references and are not retargeted.

Shared input settings use `SYS_TERMATTR` (34):
`(fd, TERM_GET/TERM_SET, terminal_attrs_t *, 32)`. GET validates writable memory;
SET validates readable memory and copies before any operation that can stop.
Version 1 supports ISIG, VINTR and VSUSP, defaulting to enabled, 3 and 26.
Control characters must be distinct nonzero 7-bit values; reserved bytes and
unsupported flags are rejected. GET exposes dropped/coalesced event counters;
SET ignores these read-only counters. Output routing via `SYS_TERMCTL` retains
its existing layout and per-process fields.

`SYS_TCSETPGRP` (30) and `SYS_TCGETPGRP` (31) resolve the caller's actual terminal
file node and validate its controlling session. Invalid descriptors return
EBADF, nonterminal/wrong-session handles ENOTTY, invalid PGIDs EINVAL, absent
groups ESRCH and cross-session targets EPERM. Handoff retains the new group
before releasing the old one, then marks readers for timer wakeup. FD 31 has
exactly the same ownership checks as any other terminal descriptor. Background
output remains allowed; TOSTOP is not implemented.

The common input-read path checks ownership before sleeping and after every
wake, before dequeuing. Its wait predicate observes foreground changes without
acquiring a process lock under the scheduler lock. Background reads publish
SIGTTIN to the caller's group; ignored/blocked SIGTTIN returns EIO without
consuming data. Default stops park through the existing signal boundary and
recheck after continuation. Caught signals return EINTR. Pipes/files never pass
through this terminal check. Direct `SYS_INPUT_READ` is checked too.

Background tcsetpgrp, TERMATTR SET and TERMCTL SET publish SIGTTOU; blocked or
ignored SIGTTOU permits the mutation. A default stop retries after continuation;
a caught signal returns EINTR without mutation. All terminal ownership/input
control calls reject non-BSP callers with EOPNOTSUPP. Legacy terminal VFS reads
translate input errors into VFS errors before the syscall translation.

## Ingress, lifetime and wakeup

Both decoded keyboard sequences and normalized UART bytes enter `ingress`.
ISIG control bytes are consumed at arrival. They flush pending ordinary input
and acknowledge earlier byte-loss state, even when the signal queue overflows.
They never flush output. ISIG off preserves literal control bytes. Keyboard
escape sequences enter as a whole and are accepted or dropped atomically.

Queue entries own a clone of the foreground group handle at arrival. Coalescing
requires identical slot, generation and signal; otherwise a full queue drops
the newest event and increments the diagnostic counter. Retain failure is also
counted as a drop. Foreground changes cannot redirect or discard queued events.
Neither producer acquires the process lock, scans groups, allocates, logs,
schedules, or invokes a handler. EOI ownership is unchanged.

A BSP-pinned `terminal-signals` kernel thread is created before input IRQs are
enabled. Timer servicing wakes it when events or unreported drops exist. It
uses the established IRQ-excluded `sched_wait_until` protocol, publishes through
`process_group_signal`, releases each popped reference, reports overflow from
thread context and yields after a bounded batch. It works when no process reads
the terminal. The worker never holds a spinlock across a context switch.

## Shell compatibility and remaining phases

The shell installs a flag-only SIGINT handler, ignores prompt SIGTSTP/SIGTTOU,
and handles input EINTR as edit cancellation. A bounded input wait covers the
signal-before-read race without repainting an idle prompt. Child waits retry
EINTR so a shell signal does not abandon child collection. The existing retained
terminal descriptor and CLOEXEC behavior are preserved.

This does not implement jobs, background launch, fg/bg, shell job handoff or
stopped-job management. Until Phases 4–5, ordinary shell launches still share
the shell group; ignored dispositions still inherit under the existing spawn
contract. Phase 4 must reset child job-control dispositions as specified by the
plan. The Phase 3 fixture explicitly groups/hands off children and restores
their dispositions; its acceptance is not a claim that the interactive shell
already implements job control. No return-path assembly or restorer changes
are included.

## Test handoff

Agent compilation/link checks (no execution): kernel ELF, shell ELF, new
terminal Ring 3 ELF, adjusted signal/stop Ring 3 ELFs, and the host fixture linked
with ASan/UBSan. Python runners were syntax-checked. These checks do not establish
runtime correctness.

Run in WSL Ubuntu-24.04 at `/mnt/c/Sources/FortressOS`:

```sh
make test-s8-terminal-host
make test-s8-terminal SMP=1
make test-s8-terminal SMP=4
make test-s8-terminal SMP=8
make test-s8-groups-host test-s8-process-host test-s8-signals-host test-s8-stops-host
make test-s8-process test-s8-signals test-s8-stops
make test-shell-host test-pipeline-host test-pipe-host
make test-shell test-pipe test-shell-s7
```

The host test uses actual input and group logic with explicit IRQ, device and
scheduler adapters. It covers defaults, fd/session/CPU validation, invalid
attributes, blocked/ignored reads without consumption, simulated stop/handoff,
ownership recheck after a wake, background mutation, literal bytes, captured
targets through handoff, coalescing, overflow, atomic sequences, UART CRLF,
timer wake publication and exact group-reference cleanup. It does not prove
real IRQ exclusion or scheduling. Overflow and rapid handoff are covered here;
the QEMU runner does not force worker starvation or claim deterministic IRQ
handoff-race coverage.

`test-s8-terminal` builds a disposable ISO and attaches no data disk. BIOS and
UEFI each run with the requested CPU count and check boot-log AP counts. UEFI
uses read-only OVMF code plus disposable vars. Final argv preflight rejects
additional backends/devices. The runner uses real QMP PS/2 key events and UART
socket bytes, requires each fixture marker, bounds every stage/whole run, and
always terminates QEMU. Logs: `build/s8-terminal-{bios,uefi}-{1,4,8}.log` and
matching `.stderr` files.

Ring 3 covers fd/pointer/size errors, ignored/blocked SIGTTIN through fd 31 and
SYS_INPUT_READ, background pipe input, default TTIN stop and foreground resume,
TTOU stops for all three mutation APIs, two-member no-reader foreground groups
killed by UART and keyboard Ctrl+C, keyboard Ctrl+Z stopping both members,
kill-without-CONT cleanup, FD 31 reclaim with stdin redirected, CLOEXEC and
UART/keyboard ISIG-off bytes. The earlier signal/stop fixtures now keep their
terminal-blocked cases in the foreground group, preserving the original
blocked-input regression instead of accidentally testing background TTIN.
The existing shell debugger test follows the actual shell in the blocked list
and reads `input_wait_channel_debug`, since the new worker also sleeps and the
FIFO is now embedded in the terminal object.

Physical keyboard/UART acceptance remains separate; QEMU results do not imply
Dell acceptance. No physical storage test is required by this Phase 3 fixture.

## Evidence

User-reported, 2026-09-28. The agent executed no host fixture and no QEMU boot;
only compilation/link, AST and Python syntax checks were performed locally.

- `make test-s8-terminal-host` — pass.
- `make test-s8-terminal SMP=1` — pass under BIOS and UEFI, with real UART and
  PS/2 input and the boot-log AP count verified.
- `make test-nmi` — pass on three consecutive runs, after the `step` →
  `resume_to` runner change recorded below.
- Regressions — all pass: `test-s8-groups-host`, `test-s8-process-host`,
  `test-s8-signals-host`, `test-s8-stops-host`, `test-s8-process`,
  `test-s8-signals`, `test-s8-stops`, `test-shell-host`, `test-pipeline-host`,
  `test-pipe-host`, `test-shell`, `test-pipe` and `test-shell-s7`.

Not claimed by this evidence: physical keyboard/UART acceptance (QEMU PS/2 and
UART results do not imply Dell acceptance), `SMP=4` and `SMP=8` terminal runs,
storage acceptance (the fixture attaches no data disk), and worker-starvation or
IRQ-handoff-race determinism — overflow and rapid handoff are covered by the host
fixture only, and the QEMU runner does not force worker starvation or claim
deterministic IRQ handoff-race coverage.

## NMI regression follow-up

The user reported a repeatable BIOS sigreturn regression-test failure at the
post-IRET CS/SS assertion, following the syscall-origin before-SWAPGS pass.
The failing assertion did not record the actual debugger stop, so the cause
is not established from that traceback. Existing local logs are not evidence
for the failing run or for the subsequent change.

The runner now observes the expected user RIP with a hardware breakpoint before
its first instruction instead of assuming a single-step across IRETQ stops in
Ring 3. All selector, RIP/RSP, RFLAGS, GPR and GS checks remain mandatory; exact
NMI injection and kernel-frame preservation checks are unchanged. Failed return
checks now print expected/actual context and QEMU CPU registers. No kernel or
return assembly changes were made for this follow-up. Python syntax and diff
checks pass; `make test-nmi` rerun remains with the user.

## Files changed

Commit `2aa9e75` — 22 files, +963/−38. Five files are new; the rest are modified.

Kernel terminal, syscall surface and VFS:

- `src/drivers/input.c` — terminal object, fd/session validation, foreground
  read/control enforcement, ingress queue and the `terminal-signals` worker
- `src/drivers/input.h` — terminal API prototypes (`input_terminal_bootstrap`,
  `input_tcgetpgrp`, `input_tcsetpgrp`, `input_termattr`, `input_control_check`)
- `src/include/terminal.h` — `terminal_attrs_t`, `TERM_ISIG` and the
  `SYS_TERMATTR` contract
- `src/include/syscall_abi.h` — `SYS_TCSETPGRP` 30, `SYS_TCGETPGRP` 31,
  `SYS_TERMATTR` 34 and `SYSCALL_ENOTTY`
- `src/kernel/syscall.c` — dispatch for the new terminal calls
- `src/kernel/main.c` — shell terminal bootstrap before preemption is enabled
- `src/fs/vfs.c` — legacy terminal reads translate input errors into VFS errors

User shell:

- `user/shell/ui.c` — flag-only prompt SIGINT handler, ignored prompt
  SIGTSTP/SIGTTOU and the bounded 100 ms input wait with EINTR cancellation
- `user/shell/program.c` — `SYS_WAIT` retries EINTR so a prompt signal does not
  abandon child collection

Test fixtures and runners:

- `tests/s8_terminal_user.c` — Ring 3 terminal/foreground fixture (new)
- `tests/s8_terminal_host.c` — host fixture with IRQ/device/scheduler adapters
  (new)
- `tests/s8_signal_user.c`, `tests/s8_stop_user.c` — blocked-input cases kept in
  the foreground group
- `scripts/test_s8_terminal.py` — BIOS/UEFI UART + PS/2 runner (new)
- `scripts/test_s8_terminal_host.py` — host runner (new)
- `scripts/test_nmi_transitions.py` — `step` → `resume_to` user-boundary
  observation
- `scripts/test_shell.py` — walk the blocked list to the shell thread and use
  `input_wait_channel_debug`
- `Makefile` — `test-s8-terminal-host`, `test-s8-terminal` and the fixture build
  rule

Documentation:

- `docs/roadmap/shell-s8-phase3.md` — this handoff (new)
- `docs/roadmap/README.md` — S8 roadmap index entries
- `docs/roadmap/shell-s8-group-lifetime.md` — prerequisite status updated to
  user-reported passing
- `AGENTS.md` — S8 Phase 3 status and test-target routing
