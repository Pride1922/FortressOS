# Shell S7 Phase 2: VFS Stream Lifecycle & Blocking Scheduler Integration

**Status: COMPLETE (2026-09-26).** Architecture: [S7 plan](../plans/S7_PLAN.md).
Phases 1–3 are complete; [Phase 4 preparation](../plans/S7_PHASE4.md) is next.

## Summary of Implementation

Phase 2 completes blocking I/O and process lifecycle wiring for anonymous kernel pipes:

1. **Blocking Scheduler Integration (Zero Locks Held Across Sleep):**
   - In `src/fs/pipe.c`, `pipe_read` and `pipe_write` evaluate ring buffer availability under `pipe->lock`.
   - When blocking is required, `pipe->lock` is released via `spin_unlock_irqrestore(&pipe->lock, flags)` before invoking `sched_wait_until()`.
   - Reader predicate `pipe_read_ready` checks atomic mirror `pipe->data_bytes > 0 || pipe->writers == 0`.
   - Writer predicate `pipe_write_ready` takes `pipe_wait_write_t` and checks `space_bytes >= needed_space || readers == 0`. `needed_space` is the requested count for writes up to `PIPE_BUF`, otherwise 1. This prevents a small atomic write from repeatedly resuming with insufficient space.
   - Wakeups via `sched_wake_all(pipe)` are issued exclusively **after** releasing `pipe->lock`, preserving lock hierarchy L1–L4 and preventing wait-queue contention under subsystem spinlocks.

2. **3-Phase CLOEXEC Spawn Lifecycle:**
   - In `src/kernel/thread.c:process_spawn_internal`:
     - **Phase A (Clone with Flags):** `fd_clone_table` copies all parent file descriptors into the child, preserving `FD_FLAG_CLOEXEC`.
     - **Phase B (Action Processing):** Ordered spawn actions (`SPAWN_FD_ACTION_*`) execute. `SPAWN_FD_ACTION_DUP2(src_fd, dst_fd)` duplicates `src_fd` (which may be CLOEXEC) to `dst_fd`, clearing `FD_FLAG_CLOEXEC` on `dst_fd`.
     - **Phase C (Post-Action Sweep):** The kernel sweeps `child->fd_flags` and closes any remaining non-NULL descriptors that still have `FD_FLAG_CLOEXEC` set.
   - CLOEXEC pipe sources remain available to actions and are then swept. Unrelated non-CLOEXEC descriptors still inherit normally. Same-fd spawn DUP2 clears CLOEXEC; ordinary `SYS_DUP2(fd, fd)` remains a no-op.
   - Endpoint close releases the pipe lock, wakes waiters, then drops `active_endpoints`. The last release frees both anonymous nodes, the control structure and all 16 PMM frames.
   - S6 acceptance audit Contract B.2 (`docs/plans/S6_AUDIT.md`) is formally updated and verified.

3. **Kernel Lifecycle Self-Test:**
   - Implemented `test_pipe_kernel_lifecycle()` in `src/kernel/main.c`, placed alongside `test_smp_ext2_concurrent_append()` in `kmain`.
   - Enabled only by `opt/fortress/pipe_test`; `make test-pipe` supplies this QEMU fw_cfg key. It runs before input and shell startup.
   - Checks blocked read, writer-close EOF, 256 KiB of payload in 32 observed writer block/wake cycles (plus 64 KiB prefill), blocked and immediate `-VFS_EPIPE`, production clone/dup/sweep helpers, and heap/stack-slot reclamation.

## Verification Matrix & Gate Status

| Gate | Description | Result | Evidence |
| :--- | :--- | :--- | :--- |
| **P2.1** | Host pipe unit suite | **PASS** | `make test-pipe-host` passes with ASan/UBSan & leak check: wraparound, atomic limits, blocking state transitions, threshold predicate correctness. |
| **P2.2** | Kernel lifecycle self-test | **PASS** | Verified in QEMU under both BIOS and UEFI boot paths. |
| **P2.3** | CLOEXEC sweep assertion | **PASS** | Verified via Ring 3 test runner `tests/pipe_user.c` (`make test-pipe`). |
| **P2.4** | Multi-core SMP integration | **DEFERRED** | Deferred to Phase 6 per architectural plan. |
| **P2.5** | S6 resources regression | **PASS** | `make test-shell-s6-resources` passes under BIOS and UEFI (1 and 4 CPUs): child descriptor limit, parent table exhaustion, process table bounds, zero thread leaks. |
| **P2.6** | S6 audit Contract B.2 update | **CONFIRMED** | Landed in `docs/plans/S6_AUDIT.md` lines 172–182. |

## Evidence boundaries and handoff

The host suite uses controlled scheduler/allocator adapters; it checks predicates,
lock-free wake/wait call sites and cleanup, not real context switches. The kernel
and Ring 3 tests run on the BSP under BIOS/UEFI. Logs are `build/pipe-bios.log`
and `build/pipe-uefi.log`. Ring 3 coverage includes CLOEXEC sources, same-fd DUP2,
no-action sweep, streaming through spawned children, and failed-action rollback.
The consolidated `make test-host` regression passed during Phase 2 development.

`sched_wake_all` currently visits the calling CPU's wait queue. Pipe peers must
remain on the same CPU; SMP boot/resource regression does not prove cross-core
pipe wakeup. Phase 6 must address that limitation before claiming distributed
pipe execution. Physical pipe acceptance remains pending.

Phase 3 completion and the later regression results are recorded separately in
[shell-s7-phase3.md](shell-s7-phase3.md); they are not fresh Phase 2 test runs.
