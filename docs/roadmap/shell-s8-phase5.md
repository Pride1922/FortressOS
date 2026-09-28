# Shell S8 Phase 5 — complete

Complete and user-confirmed 2026-09-28. The BIOS/UEFI real-shell Phase 5 gate,
host suites and listed regressions pass. Only Phase 6 (`SIGPIPE` and integrated
acceptance) remains in S8. The earlier pending notes below describe debugging
checkpoints and are superseded by this final acceptance record.

## User-confirmed final acceptance (2026-09-28)

The user reported the following results. These are user-run results, not new
agent test executions.

| Target | Reported result |
| --- | --- |
| `test-s8-jobctl` | PASS BIOS + UEFI, SMP=1; Phase 5 real-shell gate |
| `test-s8-jobctl-host` | PASS; aggregate jobs and orphan metadata suites |
| `test-s8-jobs` | PASS |
| `test-s8-jobs-idle` | PASS |
| `test-s8-jobs-host` | PASS |
| `test-s8-stops` | PASS |
| `test-s8-stops-host` | PASS |
| `test-s8-groups-host` | PASS |
| `test-s8-signals` | PASS |
| `test-s8-terminal` | PASS |
| `test-shell` | PASS |
| `test-shell-s7` | PASS |
| `test-nmi` | PASS |
| `make` | PASS clean build; image verified |

No additional firmware/CPU matrix is inferred for the regression targets from
this report. Optional SMP=4/8 runner support is not execution evidence. No new
physical-hardware or cross-core pipe-wakeup acceptance is claimed.

## Shell behavior

- Parent-only `jobs`, `fg [spec]`, `bg [spec]`, and `kill spec [signal]` are
  registered in `builtins.*` and dispatched through `jobctl.*`. Pipeline and
  background builtin stages reject them using the existing child-safe table.
- Specs are ordinary expanded words, parsed by `jobs_resolve`: `%n` (1–9999),
  `%+`, `%-`, and `%%` as an alias for current. Omitted fg/bg arguments select
  current. Malformed and missing jobs have distinct diagnostics. IDs remain
  stable until removal and wrap without colliding with live records.
- The user approved this selection rule: most recently stopped jobs first,
  then most recently backgrounded; previous is the next job in that order.
  Listing adds `+`/`-` markers and bounded command text. Completed notifications
  are collected as before. Signal termination is distinct from normal exit
  with the same numeric shell status.
- `kill` defaults to TERM and targets the entire negative PGID. It accepts
  supported numeric signals (including probe 0) and uppercase HUP, INT, KILL,
  PIPE, TERM, CHLD, CONT, STOP, TSTP, TTIN, TTOU, optionally prefixed `SIG`.
  The syntax is `kill %n TERM`, not the broader POSIX kill option grammar.

## Shared foreground controller

`pipeline_foreground()` serves staged launches and `fg`: save shell attributes,
transfer terminal ownership, restore saved job attributes on resume, release
the staged group or send CONT, wait until every member is done or every
remaining member is stopped, save stopped-job attributes, reclaim the terminal,
and restore the shell attributes. It drains all child reports between waits,
including after EINTR, so background jobs can be reported while a foreground
job is active. Failed staged handoff cancels the group and restores the mask;
failed resume leaves the job tracked.

Single external commands now also use a staged group and this controller,
allowing Ctrl+Z to stop a one-stage command without stopping the shell.
SIGCHLD remains masked around registration and is restored after release.
The flag-only handler and unconditional idle timeout/EINTR drain are unchanged.
Job storage, selection order, command scratch and terminal snapshots are BSS.

## Exit and orphan cleanup

Normal shell exit/EOF sends HUP then CONT to each owned live group, drains
during at most ten 100 ms timed input reads, then sends KILL to survivors and
collects them. Input/EINTR can shorten this grace period. The final collection
uses the existing blocking wait and depends on KILL waking stopped/blocked
tasks; the real-shell fixture covers a full pipe and HUP-ignoring peers.

The user explicitly authorized the missing kernel orphan-cleanup prerequisite.
`process_table.c` now detects stopped groups without a live same-session parent
outside the group and publishes KILL to all live members. It checks both parent
exit and later STOP, covering either ordering. Bootstrap roots are exempt;
another live parent still anchors a group. The process lock alone protects
these bounded metadata scans; no scheduler lock or delivery is entered beneath
it. Owner schedulers perform the existing wake/unwind path. Running orphan
identities remain registered until teardown. Existing staged-child cancellation
and child-reservation release remain in place. Full POSIX orphan-group semantics
are deferred.

## Prepared tests

| Target | Coverage |
| --- | --- |
| `make test-s8-jobs-host` | Existing 15 cases plus two job-control cases: spec bounds, selection/reused slots, rendered markers, group signaling, shared foreground ordering, saved attributes, errors and signal-vs-exit status; mocked syscalls with ASan/UBSan |
| `make test-s8-orphans-host` | Actual process metadata with pthread lock adapters: stopped/parent-exit orderings, whole-group KILL, retained identities, running orphans, unrelated group and second-parent anchor; no scheduler/IRQ claim |
| `make test-s8-jobctl-host` | Both host targets above |
| `make test-s8-jobctl` | Real shell under BIOS and UEFI, default SMP=1: multi-stage stop/bg/fg/kill, defaults/errors, background drain during fg, one-stage stop and attribute restoration, TTIN/read after fg, prompt controls, kernel parent death, staged cancellation, full-pipe/stopped exit cleanup and idle completion |

The new QMP/UART runner adds `/bin/jobctl-probe` and `/bin/idle-delay` only to
a disposable ISO. Final QEMU argv preflight rejects data disks, UEFI uses
read-only OVMF code and disposable vars, waits are bounded, and teardown always
terminates QEMU. Optional SMP=4/8 checks AP counts; current pipe/input peers
remain BSP-pinned. Logs: `build/s8-jobctl-{bios,uefi}-N.log` and `.stderr`.
The pipe fixture reports 64 KiB written before the runner stops its group.

## Earlier compilation evidence and test handoff

Agent checks in WSL Ubuntu-24.04 passed: shell/kernel/helper compilation and
linking (`make build/shell.elf bin/fortress.elf build/s8_jobctl_user.elf
build/s8_jobs_delay_user.elf`), and separate GCC compilation/linking of both
new/extended host fixtures with strict warnings and ASan/UBSan instrumentation.
The host binaries were **not executed**. Python runner syntax and whitespace
checks were also performed. At that checkpoint no Phase 5 host or QEMU pass
was claimed; final user-run acceptance is recorded above.

GCC stack-usage measurements: `pipeline_foreground` 112 bytes,
`pipeline_run_program` 80 bytes, and the test helper's `shell_main` 40 bytes.
These are individual compiler frames, not a general signal-stack proof.

User-run acceptance commands:

```sh
make test-s8-jobctl-host
make test-s8-jobctl
make test-s8-jobs test-s8-jobs-idle
make test-shell-host test-pipeline-host
make test-shell-s7 test-s8-terminal test-s8-stops test-s8-groups-host
make test-shell test-nmi
```

This was the requested runtime handoff. The user's final results above close
the Phase 5 gate; the debugging history below records how failures were resolved.

## First real-shell runner failure

The user reported `make test-s8-jobctl` failing on the first stop after 25
Ctrl+Z retries. The BIOS log reached `jobs`/Running and the next prompt, but
contained no echo of the submitted `fg` command. Terminal signal ingress
flushes ordinary queued input even when the shell ignores TSTP; sending Ctrl+Z
before command consumption can therefore erase `fg` itself. Repeated control
keys cannot repair a discarded command.

The loop fixture now periodically reports `JOBCTL FOREGROUND` only after
`TCGETPGRP(2)` equals its own PGID. The runner waits for that acknowledgement
before sending one Ctrl+Z, then waits for the actual Stopped report. Each stage
retains its deadline. This replaces timing/retry assumptions with an observed
handoff and preserves the no-input background-completion gate during `fg`.
The default-argument `fg` case and resuming before default TERM are also retained.
Helper compilation/linking and Python syntax checks passed; the corrected
QEMU run remains for the user. No shell or kernel changes were needed for this
runner correction.

## Foreground stop diagnosis after the handoff acknowledgement

The next user log did echo `fg %1` and both foreground acknowledgements, yet
neither PS/2 Ctrl+Z nor UART 0x1a stopped the job. The earlier queued-input race
does not explain this later failure. Source inspection found the launch policy
bug: `shell_ui_init()` installed SIG_IGN for SIGTSTP; the documented exec-like
inheritance in `process_record_attach_signals()` preserves ignored dispositions.
The loop helper never changed this disposition. `take_action()` excludes the
inherited ignored bit, so even a correctly published TSTP cannot stop it.

Minimal production fix: the shell catches TSTP with a no-op handler instead.
The prompt still does not stop, and spawn resets that caught handler to DFL in
the child. SIGTTOU stays ignored for terminal reclaim. No kernel change.

The existing foreground marker proves group ownership. The probe now also
requires TERM_ISIG, vsusp=26 and SIGTSTP=DFL before emitting it. Source tracing
shows UART ingress passes each byte separately and preserves 0x1a, then targets
the retained foreground group; the failed log itself does not establish IRQ
receipt or event publication. The attrs test that clears ISIG runs later, and
`layout us` changes only keyboard layout. Compilation/linking passed; runtime
verification of this correction remains pending the user's run.

The subsequent BIOS run passed the multi-stage stop/bg/fg cycle and group TERM
cleanup, then failed an incorrect `kill %+` error assertion: the log showed
the delayed job still Running, so selecting and terminating it was correct.
The runner's missing wait for that job's Done notification during foreground
execution is restored. An explicit empty active-job-table assertion now guards
the missing-current-job case. Python syntax validation passed; the complete
BIOS/UEFI gate remains pending.

The next supplied output and both saved firmware logs reached shell restart,
the final group-absence PASS, and idle Done plus the repainted prompt, but the
user still reported a runner failure. The supplied excerpt omitted the traceback.
Review found a final-wait race: taking a new log-length cursor after launch can
skip an already-arrived Done. The runner now retains the matched launch-prompt
cursor and requires Done followed by a new prompt, as the Phase 4 idle runner
does. Exceptions are saved to `build/s8-jobctl-{bios,uefi}-N.failure` for exact
diagnosis. Python syntax/whitespace checks passed; no rerun or full acceptance
is claimed from the incomplete failure excerpt.

The next saved BIOS `.failure` identifies a timeout at the third foreground
acknowledgement (runner line 179). The probe's `was_fg` edge detection could
miss the entire intervening bg interval between TSC polls, suppressing every
later marker while foreground. Removed that latch: the helper periodically
acknowledges current foreground ownership, still checking ISIG, vsusp and
default TSTP each time. This is a fixture-only correction; compilation/linking
passed, with runtime acceptance still pending.
