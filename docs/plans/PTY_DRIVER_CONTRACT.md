# FortressOS — PTY Subsystem Contract Specification

**Status:** Pinned Architecture & Verification Contract (Phase 6 Prerequisite)  
**Authors:** FortressOS Core Team  
**Date:** 2026-10-17  
**References:** [`PROTECTED.md`](../../PROTECTED.md), [`docs/plans/CRYPTO_SSH_PLAN.md`](CRYPTO_SSH_PLAN.md), [`docs/plans/SSH_PHASE0_CONTRACTS.md`](SSH_PHASE0_CONTRACTS.md)

---

## 1. Overview & Purpose

FortressOS currently has a single global `/dev/tty` character node backed by physical keyboard/UART drivers (`src/drivers/input.c`) and single-console job control (`g_terminal.controlling_sid`).

Interactive remote SSH sessions (RFC 4254) require per-session terminal isolation:
1. **Independent Terminal Instances:** Input/output streams from remote clients must not cross into the local console or other SSH sessions.
2. **Standard Terminal Semantics:** Interactive programs (`/bin/nano`, `/bin/sh`) require `isatty(fd) == 1`, window size (`cols`, `rows`), and signal delivery (Ctrl+C &rarr; `SIGINT` to the session's foreground process group).
3. **Non-Blocking Draining:** The session bridge worker requires non-blocking drain capabilities (`EAGAIN` when empty, partial short-writes on full) to interleave socket and terminal I/O without deadlock.
4. **Multi-CPU Correctness:** PTY operations must be safe to call from any CPU (BSP or AP), unlike `SYS_INPUT_READ` which is restricted to the BSP.

---

## 2. PTY Topology & Data Structures

```
 +-------------------------------------------------------------------+
 | User Space                                                        |
 |                                                                   |
 |  +-----------------------+              +-----------------------+ |
 |  |  sshd session bridge  |              |      User Shell       | |
 |  |     (pty_bridge)      |              |      (/bin/sh)        | |
 |  +-----------+-----------+              +-----------+-----------+ |
 |              |                                      |             |
 |   master_fd  | (read/write)             stdin/stdout| (read/write)|
 |              |  non-blocking               stderr   |  blocking   |
 +--------------|--------------------------------------|-------------+
 | Kernel VFS   |                                      |             |
 |              v                                      v             |
 |       +--------------+                      +---------------+     |
 |       |  PTY Master  |                      |   PTY Slave   |     |
 |       |   vfs_node   |                      |   vfs_node    |     |
 |       +-------+------+                      +-------+-------+     |
 |               |                                     |             |
 |               +-----------------+-------------------+             |
 |                                 |                                 |
 |                      +----------v----------+                      |
 |                      |        pty_t        |                      |
 |                      |  (in-kernel state)  |                      |
 |                      | - in_buffer (4 KiB) |                      |
 |                      | - out_buffer(16 KiB)|                      |
 |                      | - attrs (termios)   |                      |
 |                      | - winsize (cols/rows|                      |
 |                      | - controlling_sid   |                      |
 |                      | - fg_pgid           |                      |
 |                      | - rank-2 spinlock   |                      |
 |                      +---------------------+                      |
 +-------------------------------------------------------------------+
```

### 2.1 Syscall: `SYS_PTY_PAIR`
```c
int64_t sys_pty_pair(uintptr_t u_fds /* int fds[2]: master, slave */, int flags);
```
- **Atomic Creation & Fail-Closed Unwind:**
  - Allocates both `master_fd` and `slave_fd` atomically without requiring dynamic `/dev/pts` directory nodes.
  - If allocating any descriptor or node fails (e.g. `EMFILE`, `ENOMEM`), the syscall immediately rolls back all partial allocations, frees `pty_t`, closes allocated descriptors, and returns the error. Zero leaked memory or descriptors.
- **Node Differentiation:**
  - Master and slave nodes use function pointer discrimination:
    - `is_pty_master(node)`: `node->read == pty_master_read`
    - `is_pty_slave(node)`: `node->read == pty_slave_read`
- **Flags:** `flags & VFS_O_NONBLOCK` applies directly to `master_fd`'s `file->flags`.

---

## 3. Session & Job Control Ownership (Pinned Rules)

### 3.1 Race-Free Session Creation: `SPAWN_SETSID` + `SPAWN_STAGED`
- `SPAWN_V2_FLAGS` in [`src/include/syscall_abi.h`](../../src/include/syscall_abi.h) is extended with:
  ```c
  #define SPAWN_SETSID 4u
  ```
- **Staged Binding Protocol (Zero-Race Guarantee):**
  1. The bridge invokes `SYS_SPAWN_EXT` with `flags = SPAWN_SETSID | SPAWN_STAGED`. The child process is fully constructed with `sid = child_pid` and `pgid = child_pid`, but **remains in staged state (not scheduled)**.
  2. The bridge invokes `sys_termattr(master_fd, PTY_SET_SESSION, &child_pid, sizeof(uint64_t))`. Because the child is staged, it cannot exit before this call.
  3. The kernel sets `pty->controlling_sid = child_pid` and `pty->fg_pgid = child_pid` under `pty->lock`.
  4. The bridge releases the staged child to run via `SYS_PROCESS_RELEASE(child_pid, GROUP_RELEASE)`. If binding fails, it cancels the child via `GROUP_CANCEL` and closes the PTY.

### 3.2 `SYS_TCGETPGRP` / `SYS_TCSETPGRP` on PTY Slaves
- **`SYS_TCGETPGRP(slave_fd)`:**
  - Validates `caller->sid == pty->controlling_sid`. Returns `(int64_t)pty->fg_pgid`.
- **`SYS_TCSETPGRP(slave_fd, pgid)`:**
  - Validates `caller->sid == pty->controlling_sid`.
  - **Background Process Guard (`SIGTTOU`):** If `caller->pgid != pty->fg_pgid` (background process attempting to change foreground group), the kernel delivers `SIGTTOU` to `caller->pgid` unless `SIGTTOU` is blocked or ignored by the caller (standard POSIX exception). If delivered, returns `-SYSCALL_EINTR`.
  - Validates `pgid` is an active process group belonging to `pty->controlling_sid` (via `process_group_acquire(pty->controlling_sid, pgid, &ref)`).
  - Updates `pty->fg_pgid = pgid` under `pty->lock`.
  - Any attempt by an outside session to alter `pgid` returns `-SYSCALL_ENOTTY`.

---

## 4. Signal Delivery Target & Timing (Pinned Rules)

1. **Keystroke Signals (`VINTR` / `VSUSP`):**
   - When ASCII `0x03` (Ctrl+C) or `0x1a` (Ctrl+Z) is received on master and `ISIG` is active:
   - Target: `pty->fg_pgid` (**foreground process group only**).
   - Snapshot `fg_pgid` under `pty->lock`, release `pty->lock`, deliver signal via `process_signal_send_kernel(0, (int64_t)fg_pgid, sig)`.
2. **Master Disconnect (`SIGHUP`):**
   - When `master_fd` is closed:
   - Target: `pty->controlling_sid` (**entire session**).
   - Delivered **immediately** upon master close, terminating background and stopped processes in the session (unless immune via `SIG_IGN`).
3. **Slave EOF & Closed Descriptor Timing:**
   - Slave `read()` continues to drain any bytes remaining in `in_buffer`.
   - Only once `in_buffer` is completely empty does slave read return `0` (**EOF**).
   - If the child's `SIGHUP` handler closes `slave_fd`, subsequent syscalls on that closed descriptor return `-SYSCALL_EBADF` (cleanup precedence).

---

## 5. Driver Buffer & Line-Discipline Echo Rules

### 5.1 Buffer Capacities
- **Input Buffer (`in_buffer`):** **4 KiB** ring buffer (keystrokes / client stdin &rarr; slave).
- **Output Buffer (`out_buffer`):** **16 KiB** ring buffer (slave stdout/stderr &rarr; client). Ample headroom for full-screen curses/nano redraws.

### 5.2 Line-Discipline Echo
- Echo is performed by the driver in master write:
  - If `pty->attrs.echo` is set and canonical mode is active (`ICANON`), printable characters written to master are copied into `out_buffer` immediately.
  - Default attributes at creation: `ICANON | ECHO | ISIG` enabled.
  - When an application (e.g. `/bin/nano`) disables `ECHO` / enables raw mode via `SYS_TERMATTR(slave_fd, TERM_SET, &attrs)`, driver echo is suppressed (0 bytes copied to `out_buffer`). Keystrokes pass solely to `in_buffer`.

---

## 6. Read/Write Semantics & Short-Write Rules

### 6.1 Master Read (`master_fd`)
- **Data in `out_buffer`:** Returns `n = min(count, available)` bytes (> 0).
- **`out_buffer` Empty & Slave Open:**
  - If `file->flags & VFS_O_NONBLOCK`: returns immediately with `-SYSCALL_EAGAIN`.
  - If blocking: waits via `sched_wait_until`.
- **Slave Closed:** Returns remaining buffered data; once empty, returns `0` (**EOF**).

### 6.2 Master Write (`master_fd`)
- **Space in `in_buffer`:**
  - Non-blocking write is **strictly short-write**: transfers `n = min(count, space)` bytes (> 0) and returns `n`.
  - If `space == 0`: returns `-SYSCALL_EAGAIN`.
- **Slave Closed:** Returns `-SYSCALL_EIO`.

### 6.3 Slave Read & Write (`slave_fd`)
- **Read:** Blocks until data in `in_buffer`. Returns `0` (**EOF**) once master is closed and buffer is empty.
- **Write:** Writes to `out_buffer`. If full, blocks until master reads. Returns `-SYSCALL_EIO` if master closed.

---

## 7. Window Dimensions & Terminal Identity

### 7.1 Window Dimensions (`SYS_TERMATTR`)
- Window dimensions are managed via `SYS_TERMATTR(fd, op, ptr, sizeof(winsize_t))`:
  - `TERM_SET_WINSIZE`: Can be called by master before or after child spawn. Sets `pty->cols` and `pty->rows`.
    - **`SIGWINCH` Delivery Target:**
      - If `pty->fg_pgid != 0`: delivers `SIGWINCH` to `pty->fg_pgid`.
      - Else if `pty->controlling_sid != 0`: delivers `SIGWINCH` to `pty->controlling_sid` (session leader).
      - Else: silently stores dimensions without error (dimensions ready for first `TERM_GET_WINSIZE`).
  - `TERM_GET_WINSIZE`: Called by slave (or nano) to query current viewport dimensions.

### 7.2 Terminal Identity (`isatty`)
- In [`src/kernel/syscall.c`](../../src/kernel/syscall.c), `sys_termctl(TERM_ISATTY, fd, 0)` evaluates:
  ```c
  return (file->node == vfs_get_terminal_node() ||
          file->node->read == pty_slave_read ||
          file->node->read == pty_master_read) ? 1 : 0;
  ```
- Both slave and master return `1` (true). `/bin/nano` correctly recognizes the terminal.

---

## 8. Non-Blocking Control: `SYS_FCNTL` `F_GETFL` & `F_SETFL`

- [`sys_fcntl`](../../src/kernel/syscall.c) is extended with:
  - `F_GETFL` (cmd 3): Returns `file->flags`.
  - `F_SETFL` (cmd 4): Supports toggling `VFS_O_NONBLOCK` (`0x0800`).
- **Single Source of Truth:** `file->flags & VFS_O_NONBLOCK` is authoritative for both open-time initialization and subsequent fcntl modifications.
- **Non-Interrupting Semantics:** Calling `F_SETFL` does not interrupt or awaken an existing blocked read in `sched_wait_until`; the updated flag applies to subsequent read calls.

---

## 9. Kernel Invariants & Lock Hierarchy

- **Rank 2 Lock:** Each `pty_t` has a private `spinlock_t lock` (Rank 2 Resource Lock, identical to pipes).
- **IRQ Safety:** All acquisitions use `spin_lock_irqsave` / `spin_unlock_irqrestore`.
- **No Lock Nesting:** The PTY lock is never held while acquiring any scheduler or process lock. Signals are dispatched after releasing `pty->lock`.
- **Multi-CPU Safe:** Independent instances across CPUs. PTY operations on CPU $A$ never contend with PTY operations on CPU $B$.
- **Ref-Counting & Cleanup:** `pty_t` is ref-counted by master and slave `vfs_node_t` descriptors. When both reach zero, buffer storage and `pty_t` are freed to heap.
