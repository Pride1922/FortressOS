# Shell S8 Phase 1 — implementation and verification

Status: COMPLETE, user-confirmed 2026-09-27. The user reports all handoff tests
passing after the S6 resource-runner fix. This is user-reported acceptance;
the agent-observed results below remain separately identified. No additional
test runs or physical hardware acceptance are claimed by this documentation update.

Implemented: global bounded process/child metadata under an ordinary rank-1
process lock, globally allocated process IDs, inherited process groups/sessions,
setpgid/getpgrp, waitpid selectors and encoded normal-exit status, unchanged raw
legacy wait, spawn options v2, staged group release/cancellation, parent-exit
staged cleanup, and persistent sequence-based BSP wait notifications. Scheduler
queues remain per-CPU; VFS spawns and staged launches remain BSP-only. Signals,
stop/continue reports and terminal foreground control belong to later phases.

The registry contains no TCB pointers. Live records survive until reaping;
waitable child status survives independently until collection/parent exit.
The initial kernel-launched user process leads its own session. Parent setpgid
is allowed on staged children; already published exec-like children reject it.
GROUP_CANCEL discards never-run children and their reservations (not waitable
signal deaths). Existing kernel-test exit history remains separate.

## Evidence captured by the agent

- `make -j4 bin/fortress.elf`: passed with strict compiler flags.
- `make test-s8-process-host`: passed ASan/UBSan, including 4,000 concurrent
  metadata lifecycles using a pthread lock adapter. No IRQ/SMP-kernel claim.
- `make test-s8-process`: BIOS and UEFI, SMP=1, passed on disposable ISO with
  no data disks. Logs: `build/s8-process-{bios,uefi}-1.log`.
- `make test-s8-process SMP=4`: both firmware logs contain `S8 USER PASS`.
  Logs: `build/s8-process-{bios,uefi}-4.log`. Runner completion output was not
  collected before interruption; AP counts were not independently audited.
- A host regression command was started but interrupted before its result was
  collected. Do not count those regressions as passed.

At the original handoff, remaining verification was assigned to the user.
Their subsequent all-tests-pass confirmation closes Phase 1 acceptance.

## Verification handoff (WSL Ubuntu-24.04)

```sh
make test-s8-process-host
make test-s8-process SMP=1
make test-s8-process SMP=4
make test-s8-process SMP=8
make test-pipe-host test-shell-host test-stream-tools-host
make test-pipe
make test-shell-s7 SMP=4
make test-shell-s6-resources
make test-smp-vmm
make test-smp-append
make test-shell
```

Check AP counts in the multi-CPU logs. Pipe/input peers remain BSP-affine.
The S8 probe intentionally exhausts stack slots, so the stack-exhaustion warning
and failed-spawn diagnostic immediately preceding `S8 USER PASS` are expected.
The runner must report PASS and no `S8 USER FAIL`; a boot banner alone is not a
pass. New targets use no data disks; existing regression targets retain their
own disposable fixture policies.

## S6 resource regression adaptation (2026-09-27)

Updated the GDB runner to derive child-record layout/capacity from the actual
private process_table.c definition and inspect its global static children array
once through nm -an. No kernel storage was exposed or relocated. The spawn
breakpoint now follows process_spawn_from_vfs_group, which SYS_SPAWN_EXT calls.
All baseline equality, failed-publication, reference and slot assertions remain.

An initial matrix run passed both BIOS variants but encountered an intermittent
UEFI checkpoint assertion. UEFI 1/4 CPU reruns passed. Replaced the checkpoint's
single debugger step with a hardware breakpoint at its actual return address,
asserting bare RET and restored RSP, so an intervening interrupt cannot cause
re-observation of the same end checkpoint.

Final command: `wsl -d Ubuntu-24.04 -- make test-shell-s6-resources` — exit 0.
BIOS and UEFI, each with 1 and 4 CPUs: all 16 cycles passed (4 warm-up, 12 exact
measured baselines per configuration), no failed-child publication, balanced
references, reused stack slot, prompt recovery and clean offline e2fsck.
Evidence: `build/shell-s6-resources-{bios,uefi}-{1,4}cpu.{log,json}`.
This agent run validates the S6 regression; the user's subsequent confirmation
covers the remaining handoff tests.
