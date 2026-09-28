# Shell S8 Phase 4 — complete

Complete and user-confirmed 2026-09-28: bounded job table, `&`, foreground and
background launch, terminal handoff, and idle-prompt reaping with draft/cursor
preservation. Host tests and BIOS/UEFI SMP=1 launch and real-shell idle gates
pass. The READER stall was a fixture error; no kernel changes were needed.
Only Phase 5 (`jobs`/`fg`/`bg`/`kill %n`) and Phase 6 (`SIGPIPE`) remain in S8.

## Implementation surface

- `user/shell/lexer.*` and `parser.*`: background pipeline syntax, `CMD_OP_BG`
  and `parse_tree_t.background`, with unsupported syntax rejection.
- `user/shell/jobs.*`: eight BSS job slots, bounded member/command metadata,
  member state aggregation, waitpid drainage, notifications and reclamation.
- `user/shell/program.*` and `pipeline.*`: version-2 staged group launch,
  masked SIGCHLD registration, foreground handoff before release, background
  release without handoff, and cancellation on launch failure.
- `user/shell.c`: flag-only SIGCHLD handler, initialization and reaping before
  prompting and after command execution.
- `user/shell/ui.c`: unconditional timeout/EINTR prompt drain and repaint using
  the existing editor state; `jobs.c` performs notification-aware output and GC.
- `tests/s8_jobs_host.c`, `scripts/test_s8_jobs_host.py` and Makefile targets:
  actual shell code with mocked syscalls. `pipeline.c` uses the
  `SHELL_TERMATTR_HOST_TEST` call4 seam, following the existing
  `SHELL_IO_HOST_TEST` approach in `io.c`.
- `tests/s8_jobs_user.c` and `scripts/test_s8_jobs.py`: disposable Ring 3 ABI
  fixture, replacing `/bin/shell` only inside a temporary test ISO.

## READER diagnosis and fix

The requested first-action `S` pipe probe arrived, followed by `B` after
`dup2(0,31)`. Additional parent markers reached `RECEIVED` and `REAPED` before
the timeout. Thus the child was scheduled, recreated fd 31, delivered its pipe
markers, read the UART byte and was reaped. The remaining call was the parent's
`TCSETPGRP(31, own)` reclaim. The diagnostic log is retained locally as
`build/s8-jobs-reclaim-probe.log`.

The fixture parent had default SIGTTOU. After handing the terminal away it was
a background caller; `input.c`'s documented control-access check stops such a
caller on reclaim. Installing ignored SIGTTOU, as the real shell and proven
Phase 3 fixture already do, resolved the stall. This is expected terminal
behavior, not a staged-child scheduling defect.

The final fixture retains the diagnostic milestones. It requires successful
pipe writes, the exact UART byte `J`, successful child exit status, and the
exact `SYSCALL_EIO` result for an ignored-SIGTTIN background reader. The stale
extra `B` in NOHANDOFF was removed; its parent reads `N` from fd 3. A blocking
pipe read replaces the arbitrary spin delay. FDOWN checks terminal descriptors
0/1/2/31, CLOEXEC on 31, and EBADF on 3..30. PASS parks for runner teardown,
matching the Phase 3 convention.

`SYS_INPUT_READ` takes `(pointer, count, timeout_ms)`, unlike fd-based
`SYS_READ(fd, pointer, count)`. The reader retries INPUT_LOST, EINTR and bounded
timeouts, with a finite attempt limit. GROUP_CANCEL aborts staged child records;
PARTIAL correctly asserts ECHILD rather than waiting for nonexistent exits.

Fixture buffers, argument/options structures and status storage are static.
A GCC `-fstack-usage` build using the Makefile's fixture flags reports 56 bytes
for `shell_main`, 16 for `spawn_staged`, and 8 each for `check`, `text` and
`reap`. These small frames preserve the 512-byte stack discipline; they are
compiler frame measurements, not a general signal-stack proof.

## Evidence

### Earlier agent-run launch checks

Executed in WSL Ubuntu-24.04 from `/mnt/c/Sources/FortressOS`:

- `make test-s8-jobs-host`: PASS, all 14 host cases with ASan/UBSan, covering
  parser, capacity, state aggregation, launch ordering, masked registration,
  failure unwinding and notifications. Syscalls are mocked; no IRQ/SMP claim.
- `make test-s8-jobs`: PASS BIOS and UEFI, SMP=1, AP-count checks passed.
  REGISTER runs 32 fast-exit cycles; PARTIAL runs eight two-member cancellation
  cycles; READER, NOHANDOFF and FDOWN all pass. Logs:
  `build/s8-jobs-bios-1.log` and `build/s8-jobs-uefi-1.log`, plus `.stderr`.
  QEMU uses TCG, q35, 2 GiB, a disposable ISO, no data disks, and paired OVMF
  code/disposable vars for UEFI. Final argv preflight and bounded teardown remain.

The prior-session handoff reports passing `make`, `test-shell-host`,
`test-pipeline-host`, `test-shell-s7`, `test-s8-terminal`, `test-s8-stops`,
`test-s8-signals`, `test-nmi`, `test-shell`, `test-shell-s6` and
`test-shell-s6-resources`. Those are inherited reports, not fresh passes from
this test-fixture follow-up. No physical hardware or cross-core acceptance is
claimed here.

### Closed idle-prompt acceptance gate (2026-09-28)

The previously identified gap was that `shell_read_line()` continued on input
timeout/EINTR without draining child records. The editor now calls
`jobs_reap_prompt()` unconditionally on both paths, including when no SIGCHLD
flag is set. This closes the signal-before-sleep gap using the existing 100 ms
input timeout. No signal masking, kernel code or launch-registration changes
were made; the handler still only writes its scalar flag.

The prompt-specific reaper uses the same WAITPID drain as normal shell reaping,
retries EINTR, and starts a new output line only when the first Done/Stopped
notification is ready. It returns whether output occurred, then GC frees
notified completed slots. The UI calls its existing `paint()` only when needed;
it never initializes or clears the BSS editor during notification. Text, cursor,
history/search/paste state remain owned by the editor. Existing escape-timeout
and Ctrl+C behavior remain: a pending prompt interrupt cancels the edit normally.
Empty timeout scans produce no new output. Direct calls fit the existing UI
module layering; no callback framework was added.

### Real-shell idle test coverage

`make test-s8-jobs-idle` builds a test-only `/bin/idle-delay` helper and adds it
to a disposable ISO alongside the **unchanged real shell**. The helper waits
12 billion QEMU TSC ticks before exiting zero; this is a test delay, not a
portable sleep API or a fixed wall-clock claim. It does not read the terminal
(which would trigger background SIGTTIN/EIO). The runner rejects a helper that
finishes before the prompt/edit setup, bounds every wait, and tears QEMU down.
It follows the Phase 3 UART/QMP runner pattern with no data disks, final argv
preflight and paired read-only OVMF code/disposable vars.

The BIOS/UEFI runner (default SMP=1, optional SMP=4/8 with AP-count checks):

1. Launches `/bin/idle-delay &`, observes its job/PID and prompt, then sends no
   input until that job's Done notification and new prompt appear.
2. Launches another delayed job, types `echo ac` without Enter, and moves the
   cursor left via QMP. With no further input, it requires Done followed by a
   repaint of `fortress> echo ac`.
3. Checks empty idle scans stay quiet, then inserts `b` via QMP and presses
   Enter. Exact `abc` command output proves both the text and mid-line cursor
   survived. It rejects shell restart.

Logs are `build/s8-jobs-idle-{bios,uefi}-N.log` with matching `.stderr`.
This covers real shell execution and real UART/PS2 input, not cross-core pipe
wakeups or physical hardware. The helper is never added to production initramfs.

`make test-s8-jobs-host` now also includes a prompt-drain case: empty scans,
EINTR retry, notification separation, immediate GC and silent rescanning, with
unchanged masks. This brings the suite to 15 cases; the new case does not run
the UI input loop. The QEMU test is the edit-preservation evidence.

### Compilation evidence

Agent compilation/link check passed:
`make build/shell.elf build/s8_jobs_delay_user.elf`. GCC stack-usage reports
64 bytes for `shell_read_line`, 32 for `jobs_reap_prompt`, 64 for the shared
reaper, and 192 for `paint`. Drain and paint execute sequentially; no large
editor/notification buffers were added to the Ring 3 stack.

### User-confirmed final acceptance (2026-09-28)

The user reported all tests passing and supplied the result summary below.
These are user-run results, not fresh agent executions. The idle-prompt gate
is closed; the earlier acceptance-pending notes are superseded.

| Command / target | Reported result |
| --- | --- |
| `make test-s8-jobs-idle` | PASS BIOS + UEFI, SMP=1: real-shell idle Done notification and draft/cursor preservation; Phase 4 gate |
| `make test-s8-jobs` | PASS BIOS + UEFI, SMP=1: Ring 3 launch/handoff |
| `make test-s8-jobs-host` | PASS all 15 cases, including prompt drain; mocked syscalls with ASan/UBSan |
| `make test-shell` | PASS BIOS + UEFI and no-UART 8 GiB |
| `make test-shell-s7` | PASS BIOS + UEFI; pipeline execution intact |
| `make test-shell-host` | PASS full suite |
| `make test-pipeline-host` | PASS |
| `make test-s8-terminal` | PASS BIOS + UEFI |
| `make test-nmi` | PASS BIOS + UEFI, full SYSRET and sigreturn suite |
| `make` (clean build) | PASS ISO + image; reported e2fsck zero errors and GPT verified |

SMP=4/8 idle-runner support is not a claim of execution at those CPU counts.
No new physical-hardware or cross-core pipe acceptance is claimed. Phase 5
builtins and Phase 6 SIGPIPE remain separate.

## Earlier regression-runner repairs

The inherited working tree includes three test repairs:

1. `scripts/test_shell.py` adds `-Isrc/arch/x86_64` to the host offsets helper.
2. Its snapshot path adds a bounded retry to find the blocked shell after the
   prompt, instead of assuming the first sample sees it asleep. It re-walks
   under a QEMU stop for the assertions. The helper resumes before the final
   stop, so this does not eliminate every possible sampling race.
3. `scripts/test_shell_no_uart.py` removes an obsolete framebuffer assertion
   for a diagnostic suppressed during quiet boot; keyboard echo and blocked
   reader checks remain.
