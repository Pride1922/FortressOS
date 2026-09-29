# Shell S8 Phase 6 — SIGPIPE

Implemented 2026-09-29; build and runtime acceptance pending user testing.
The user explicitly authorized the kernel change after the reading-pass report.
No builds, host suites, QEMU runs or hardware tests were executed by the agent.
S8 remains in progress until acceptance. Earlier phase results are unchanged.

First user run of `make test-s8-sigpipe SMP=1` reached the BIOS shell but failed
at probe line 45, before SIGPIPE coverage: the fixture passed non-null
`fd_actions` with zero `action_count`, which SYS_SPAWN_EXT rejects with EINVAL.
The fixture now passes a null pointer for zero actions. Rerun pending; this
failure establishes neither SIGPIPE acceptance nor a SIGPIPE kernel failure.

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

## Added coverage (not executed)

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

`SMP=4/8` is supported by the new runner and checks boot AP counts, but all pipe
peers remain BSP-pinned. No cross-core pipe execution or physical acceptance is
claimed.

## User verification handoff

Run from WSL Ubuntu-24.04 at `/mnt/c/Sources/FortressOS`:

```sh
make
make test-s8-sigpipe-host
make test-s8-sigpipe SMP=1
make test-pipe
make test-shell-s7
make test-s8-signals
make test-nmi
make test-s8-jobs test-s8-jobs-idle test-s8-jobctl
make test-s8-stops test-s8-terminal
```

The SIGPIPE runner has no storage fixture. The existing S7 regression retains
its own disposable NVMe fixture scope. Mark Phase 6/S8 accepted only after the
user supplies results; Dell integrated acceptance remains separate from QEMU.
