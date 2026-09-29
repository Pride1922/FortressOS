# Shell S8 Phase 6 — SIGPIPE (complete)

Complete and user-accepted 2026-09-29. The kernel and tool changes below were
implemented 2026-09-29 (commit `72631ed`); the user then ran the QEMU regression
bar and supplied a Dell Latitude 5590 physical observation, both recorded in
Evidence. The user explicitly authorized the kernel change after the reading-pass
report. With Phase 6 closed, Shell S8 (Phases 1–6) is complete and earlier phase
results are unchanged.

The agent executed no builds, host suites, QEMU runs or hardware tests for this
record; every result below is user-run or user-reported, and the Dell observation
is manual. A first-run fixture defect recorded under Notes was repaired in the
test harness before the passing gate; it established no kernel finding.

## Implementation

`SYS_WRITE` publishes SIGPIPE to the current writer's positive PID after
`vfs_write` returns `-VFS_EPIPE`, with an explicit no-lock-held assertion.
The pipe's rank-2 lock is already released before the rank-1 process lock is
acquired. Selector zero is deliberately not used: it would signal the group.
No signal ABI, scheduler, IRQ, assembly transition or frame-layout change was
needed. Direct kernel VFS writes retain their error-only contract.

The existing syscall return path stores EPIPE in the return frame before
processing signals. Default SIGPIPE terminates with signal metadata (legacy
status 141); caught handlers return through existing sigreturn with EPIPE;
ignored signals return EPIPE without publication; blocked signals remain pending
until unblocked. Ordinary child exit and SIGCHLD bookkeeping remain unchanged.

Pipe writes larger than PIPE_BUF return the available positive prefix immediately.
They do not loop after copying. Reader closure discovered on a subsequent write
produces EPIPE/SIGPIPE; a successful prefix is never replaced by an error.
Zero-length writes and rejected user buffers do not publish SIGPIPE. Capacity,
atomic-write threshold, backpressure, EOF and endpoint lifetime are unchanged.

Stream tools and builtin stages now return ordinary status 1, quietly, if they
survive SIGPIPE and receive EPIPE. Default signal termination supplies 141;
tools no longer manufacture that status. The shell already decodes signaled
wait statuses. A pipeline still uses its final stage's status: an upstream
SIGPIPE followed by successful head yields `$? = 0`, while a final stage killed
by SIGPIPE yields 141. Historical S7 evidence records the previous behavior.

## Coverage

- `make test-s8-sigpipe-host`: extended actual pipe/VFS/SYS_WRITE and process-table
  code with single-threaded allocator, descriptor, user-range, lock and scheduler
  adapters; actual publication/action selection for all four dispositions,
  positive partial writes, blocked pending state, writer-only targeting,
  immediate closure and closure during a full-pipe wait. The lock adapter rejects
  nested locks. Also runs existing signal, stream-tool and builtin-runner host
  suites, with EPIPE expectations updated. No real IRQ, scheduling or handler
  frame claim is made for host coverage.
- `make test-s8-sigpipe SMP=1`: new disposable ISO retaining the real shell and
  adding `/bin/sigpipe-probe`; BIOS and UEFI, no data disks, paired read-only OVMF
  code/disposable vars and final argv preflight. Ring 3 cases include caught
  EPIPE restored through sigreturn, ignored survival, blocked coalescing and
  caught/default delivery on unblock, exact 17-byte short write, zero-length and
  invalid-pointer cases, early reader closure, stopped producer close/continue,
  infinite producer with real `head -n 1`, actual upstream signal metadata,
  real cat/builtin default versus ignored dispositions, shell status and prompt
  recovery. The parent shares the writer's group, detecting accidental group
  signaling. Logs: `build/s8-sigpipe-{bios,uefi}-{SMP}.log` and `.stderr`.
- Existing `test-pipe` Ring 3 fixture explicitly ignores SIGPIPE for its raw-EPIPE
  assertions; kernel-side VFS checks remain unchanged. The S7 test producer no
  longer synthesizes 141, so default termination must provide it.

## Evidence

### User-run QEMU regression bar (2026-09-29)

The user ran these from WSL Ubuntu-24.04 at `/mnt/c/Sources/FortressOS`. They are
user-run results, not fresh agent executions.

| Target | Reported result |
| --- | --- |
| `make` | PASS clean build; image verified (MBR, GPT CRC, ESP, ext2 0 errors) |
| `make test-s8-sigpipe-host` | PASS: host publication, dispositions, partial writes and the tool regressions |
| `make test-s8-sigpipe SMP=1` | PASS BIOS + UEFI: real-shell SIGPIPE and status gate |
| `make test-s8-signals` | PASS |
| `make test-nmi` | PASS |
| `make test-pipe` | PASS |
| `make test-pipe-host` | PASS |
| `make test-shell-s7` | PASS |
| `make test-s8-jobs` | PASS BIOS + UEFI |
| `make test-s8-jobs-idle` | PASS BIOS + UEFI |
| `make test-s8-jobctl` | PASS BIOS + UEFI |
| `make test-s8-stops` | PASS BIOS + UEFI |
| `make test-s8-terminal` | PASS BIOS + UEFI |
| `make test-shell` | PASS BIOS + UEFI and no-UART 8 GiB |
| `make test-shell-host` | PASS |
| `make test-pipeline-host` | PASS |

The SIGPIPE gate ran at the default `SMP=1` under both firmware modes. It uses a
disposable ISO retaining the real shell plus `/bin/sigpipe-probe`, no data disks,
paired read-only OVMF code and disposable vars, and a final argv preflight; the
S7 regression retains its own disposable NVMe fixture scope. No `SMP=4/8`
SIGPIPE execution is claimed, and these QEMU results do not stand in for physical
acceptance.

## Dell Latitude 5590 physical acceptance

Manual user observation, 2026-09-29, on the Dell Latitude 5590. The agent neither
observed nor reproduced it; evidence is the user's report and its photo.

```sh
cat /bin/shell /bin/shell /bin/shell /bin/shell /bin/shell | head -c 1
```

Observed: upstream `cat` reported `[4]+ Terminated(signal 13)`; `echo $?`
returned `0` (the pipeline's final-stage status, from `head`); the prompt
recovered; the shell survived.

This confirms real SIGPIPE delivery on hardware, correct pipeline status
semantics when an upstream stage is terminated, shell survival, and that the
shell reports actual signal metadata rather than a synthesized 141. Evidence
boundary: one manual observation of the default-termination disposition only —
not automated capture, not a transcript, and not a per-case matrix. The four-way
disposition matrix (default, caught, ignored, blocked-pending) is covered by the
QEMU `test-s8-sigpipe` gate, which remains the primary logic evidence; no
physical acceptance is claimed for the other three dispositions or for SMP
counts. Artifact: Dell screen photo, 2026-09-29.

Two known image gaps, not SIGPIPE defects and not exercised by this command:
`yes` is absent from the production initramfs, and `/dev` does not exist because
FortressOS has no device filesystem.

## Notes

- Resolved first-run fixture failure (test harness, not kernel). The first
  `make test-s8-sigpipe SMP=1` run reached the BIOS shell but failed at probe line
  45, before any SIGPIPE coverage: the fixture passed non-null `fd_actions` with
  zero `action_count`, which `SYS_SPAWN_EXT` rejects with `EINVAL`. The fixture
  now passes a null pointer when there are no actions. The defect established
  neither a kernel nor a SIGPIPE failure, and the subsequent run recorded above
  passed after the repair.

## Scope and remaining boundary

Phase 6 completes S8's SIGPIPE delivery and closes the S8 milestone. No signal
ABI, scheduler, IRQ, assembly or interrupt-frame layout change was introduced,
and direct kernel VFS writes retain their error-only contract.

`SMP=4/8` is supported by the new runner and it checks boot AP counts, but all
pipe peers remain BSP-pinned. No cross-core pipe execution or cross-core wakeup
claim is made, and there is no physical acceptance beyond the single
default-termination observation recorded above.
