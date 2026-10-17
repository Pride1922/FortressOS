# FortressOS — PTY Subsystem Contract Specification

**Status:** Proposed Architecture & Verification Contract (Phase 6 Prerequisite)  
**Authors:** FortressOS Core Team  
**Date:** 2026-10-17  
**References:** [`PROTECTED.md`](../../PROTECTED.md), [`docs/plans/CRYPTO_SSH_PLAN.md`](CRYPTO_SSH_PLAN.md), [`docs/plans/SSH_PHASE0_CONTRACTS.md`](SSH_PHASE0_CONTRACTS.md)

---

## 1. Overview & Purpose

FortressOS currently has a single global `/dev/tty` character node backed by the physical keyboard/UART driver (`src/drivers/input.c`) and single-console job control (`g_terminal.controlling_sid`).

Interactive remote SSH sessions (RFC 4254) require per-session terminal isolation:
1. **Independent Terminal Instances:** Input/output streams from remote clients must not cross into the local console or other SSH sessions.
2. **Standard Terminal Semantics:** Interactive programs (`/bin/nano`, `/bin/sh`) require `isatty(fd) == 1`, window size (`cols`, `rows`), and signal delivery (Ctrl+C &rarr; `SIGINT` to the session's foreground process group).
3. **Non-Blocking Draining:** The session bridge worker requires non-blocking drain capabilities (`EAGAIN` when empty) to interleave socket and terminal I/O without deadlock.
4. **Multi-CPU Correctness:** PTY operations must be safe to call from any CPU (BSP or AP), unlike `SYS_INPUT_READ` which is restricted to the BSP.

---

## 2. PTY Topology & Lifecycle

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
 |                      | - in_buffer (ring)  |                      |
 |                      | - out_buffer (ring) |                      |
 |                      | - termios / winsize |                      |
 |                      | - sid / fg_pgid     |                      |
 |                      | - rank-2 spinlock   |                      |
 |                      +---------------------+                      |
 +-------------------------------------------------------------------+
```

### 2.1 Syscall Allocation & Creation

A new syscall `SYS_OPENPT` (or `SYS_PTY_OPEN`) creates a connected PTY pair:
```c
int64_t sys_openpt(int flags);
/* Returns master_fd on success, negative error on failure */
```
Alternatively, an atomic pair creation syscall:
```c
int64_t sys_pty_pair(uintptr_t u_fds /* int fds[2]: master, slave */, int flags);
```
- **File Descriptors:** Two file descriptors are allocated:
  - `master_fd`: Held by the privileged or session bridge process (`sshd`).
  - `slave_fd`: Passed to `SYS_SPAWN_EXT` actions as stdin (0), stdout (1), and stderr (2) of the spawned shell.
- **Flags:** `VFS_O_NONBLOCK` may be passed to create the master endpoint in non-blocking mode, or toggled via `SYS_FCNTL(fd, F_SETFL, O_NONBLOCK)`.

---

## 3. Read/Write Semantics & Edge Cases

Each PTY pair contains two circular ring buffers (e.g. 4 KiB each):
- **Master &rarr; Slave Ring (`in_buffer`):** Keystrokes/data from SSH client &rarr; shell stdin.
- **Slave &rarr; Master Ring (`out_buffer`):** Output from shell stdout/stderr &rarr; SSH client.

### 3.1 PTY Master Read Semantics (`master_fd`)
- **Data Available:** Copies up to `count` bytes from `out_buffer` to caller buffer. Returns number of bytes transferred (> 0).
- **Buffer Empty & Slave Open:**
  - If `master_fd` has `O_NONBLOCK` set: Returns immediately with `-SYSCALL_EAGAIN`.
  - If `master_fd` is blocking: Blocks until slave writes data or closes.
- **Slave Closed (All slave descriptors closed):**
  - If `out_buffer` has remaining unread data: Returns buffered data.
  - Once `out_buffer` is empty: Returns `0` (**EOF**).

### 3.2 PTY Master Write Semantics (`master_fd`)
- **Space Available:** Writes up to `count` bytes into `in_buffer`. Returns number of bytes transferred. Wakes any blocking slave readers.
- **Buffer Full & Slave Open:**
  - If non-blocking: Returns `-SYSCALL_EAGAIN` (or short-write).
  - If blocking: Waits for slave to read.
- **Slave Closed:**
  - Returns `-SYSCALL_EIO` (or sends `SIGPIPE` / returns `-SYSCALL_EPIPE`).

### 3.3 PTY Slave Read Semantics (`slave_fd`)
- **Data Available:** Copies data from `in_buffer`.
- **Buffer Empty & Master Open:**
  - Blocks on `sched_wait_until` until master writes data or closes.
- **Master Closed (Master descriptor closed / SSH disconnect):**
  - If `in_buffer` is empty: Returns `0` (**EOF**).
  - Also delivers `SIGHUP` to the session's foreground process group.

### 3.4 PTY Slave Write Semantics (`slave_fd`)
- **Space Available:** Writes into `out_buffer`. Wakes master readers.
- **Buffer Full & Master Open:**
  - Blocks on `sched_wait_until` until master reads space.
- **Master Closed:**
  - Returns `-SYSCALL_EIO` or `-SYSCALL_EPIPE` (delivers `SIGPIPE` to writer).

---

## 4. Terminal Identity & Control Semantics

### 4.1 `isatty` Support
- **Current Behavior:** `sys_termctl(TERM_ISATTY)` returns `1` only if `file->node == vfs_get_terminal_node()`.
- **Contract Extension:** `sys_termctl(TERM_ISATTY)` returns `1` if `file->node == vfs_get_terminal_node()` **OR** `file->node->type == VFS_STREAM && file->node->fs_private_is_pty_slave`.
- **Result:** `/bin/nano`, `/bin/sh`, and other userland utilities correctly detect standard input/output as an interactive terminal.

### 4.2 Terminal Attributes & Echo
- Each `pty_t` maintains its own `terminal_attrs_t`:
  - `ISIG`: When enabled, ASCII `0x03` (Ctrl+C) generates `SIGINT` to the PTY's `fg_pgid`; `0x1a` (Ctrl+Z) generates `SIGTSTP`.
  - `ECHO`: When enabled, bytes written to master are echoed into the slave's output buffer unless raw mode is requested.
  - `ICANON`: Canonical (line-buffered) vs. raw mode.

### 4.3 Window Size (`cols`, `rows`)
- The master can update window dimensions via `SYS_TERMCTL(TERM_SET_WINSIZE)`:
  - Updates `cols` and `rows` in `pty_t`.
  - Sends `SIGWINCH` to the PTY's foreground process group (`fg_pgid`).

---

## 5. Kernel Invariants & Lock Hierarchy

To comply with [`PROTECTED.md`](../../PROTECTED.md) §L1–L4 and SMP rules:
1. **Lock Rank:** Each PTY pair has a private `spinlock_t lock`. The lock is Rank 2 (Resource Lock), identical to pipe locks.
2. **IRQ Safety:** All PTY lock acquisitions must use `spin_lock_irqsave` / `spin_unlock_irqrestore`.
3. **No Lock Nesting:** The PTY lock must never be acquired while holding any scheduler lock (Rank 0 or 1).
4. **Multi-CPU Isolation:** PTY locks are strictly per-instance. Thread execution on CPU $N$ accessing PTY $A$ will never contend with CPU $M$ accessing PTY $B$ or the console.
5. **Memory Reclamation:** The underlying `pty_t` structure is ref-counted: it is freed only when both master and slave `vfs_node_t` reference counts reach zero.

---

## 6. Synergistic Syscall Extensions

To enable non-blocking drain without inventing new unreviewed polling subsystems:
1. **`SYS_FCNTL` `F_SETFL` / `F_GETFL`:**
   - Support `F_SETFL` with `VFS_O_NONBLOCK` (flag `0x0800` / bitmask).
   - Toggles `file->flags & VFS_O_NONBLOCK`.
2. **`vfs_read` Non-Blocking Propagation:**
   - Pass `file->flags` into stream read callbacks, or allow `read` to check if `VFS_O_NONBLOCK` is active on the file. If active and buffer is empty, return `-VFS_EAGAIN`.
