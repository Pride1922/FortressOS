# Shell S7 Phase 2: VFS Stream Lifecycle & Blocking Scheduler Integration

Implemented and verified on 2026-09-26. Architecture: [S7 plan](../plans/S7_PLAN.md).

## Summary of Implementation

Phase 2 completes blocking I/O and process lifecycle wiring for anonymous kernel pipes:

1. **Blocking Scheduler Integration (Zero Locks Held Across Sleep):**
   - In `src/fs/pipe.c`, `pipe_read` and `pipe_write` evaluate ring buffer availability under `pipe->lock`.
   - When blocking is required, `pipe->lock` is released via `spin_unlock_irqrestore(&pipe->lock, flags)` before invoking `sched_wait_until()`.
   - Reader predicate `pipe_read_ready` checks atomic mirror `pipe->data_bytes > 0 || pipe->writers == 0`.
   - Writer predicate `pipe_wait_write_predicate` checks atomic mirror `pipe->space_bytes >= required_space || pipe->readers == 0`. This threshold-aware waiter eliminates the partial-space busy-spin bug for writes where `count <= PIPE_BUF` and `0 < space < count`.
   - Wakeups via `sched_wake_all(pipe)` are issued exclusively **after** releasing `pipe->lock`, preserving lock hierarchy L1–L4 and preventing wait-queue contention under subsystem spinlocks.

2. **3-Phase CLOEXEC Spawn Lifecycle:**
   - In `src/kernel/thread.c:process_spawn_internal`:
     - **Phase A (Clone with Flags):** `fd_clone_table` copies all parent file descriptors into the child, preserving `FD_FLAG_CLOEXEC`.
     - **Phase B (Action Processing):** Ordered spawn actions (`SPAWN_FD_ACTION_*`) execute. `SPAWN_FD_ACTION_DUP2(src_fd, dst_fd)` duplicates `src_fd` (which may be CLOEXEC) to `dst_fd`, clearing `FD_FLAG_CLOEXEC` on `dst_fd`.
     - **Phase C (Post-Action Sweep):** The kernel sweeps `child->fd_flags` and closes any remaining non-NULL descriptors that still have `FD_FLAG_CLOEXEC` set.
   - This ensures child pipeline stages inherit only their designated standard streams (stdin/stdout) without leaking intermediate pipe file descriptors or requiring manual `CLOSE` actions.
   - S6 acceptance audit Contract B.2 (`docs/plans/S6_AUDIT.md`) is formally updated and verified.

3. **Kernel Lifecycle Self-Test:**
   - Implemented `test_pipe_kernel_lifecycle()` in `src/kernel/main.c`, placed alongside `test_smp_ext2_concurrent_append()` in `kmain`.
   - Validates pipe creation, blocking read/write between threads/endpoints, EOF detection on writer close, and `SYSCALL_EPIPE` detection on reader close before starting the user shell.

## Verification Matrix & Gate Status

| Gate | Description | Result | Evidence |
| :--- | :--- | :--- | :--- |
| **P2.1** | Host pipe unit suite | **PASS** | `make test-pipe-host` passes with ASan/UBSan & leak check: wraparound, atomic limits, blocking state transitions, threshold predicate correctness. |
| **P2.2** | Kernel lifecycle self-test | **PASS** | Verified in QEMU under both BIOS and UEFI boot paths. |
| **P2.3** | CLOEXEC sweep assertion | **PASS** | Verified via Ring 3 test runner `tests/pipe_user.c` (`make test-pipe`). |
| **P2.4** | Multi-core SMP integration | **DEFERRED** | Deferred to Phase 6 per architectural plan. |
| **P2.5** | S6 resources regression | **PASS** | `make test-shell-s6-resources` passes under BIOS and UEFI (1 and 4 CPUs): child descriptor limit, parent table exhaustion, process table bounds, zero thread leaks. |
| **P2.6** | S6 audit Contract B.2 update | **CONFIRMED** | Landed in `docs/plans/S6_AUDIT.md` lines 172–182. |
