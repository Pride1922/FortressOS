# FortressOS — Readiness Primitive (`sys_poll`) Architectural Plan (Issue #17)

**Status:** DRAFT / PROPOSED  
**Date:** 2026-10-25  
**Issue:** [#17](https://github.com/Pride1922/FortressOS/issues/17) (`Readiness primitive (select / poll): Multiplex without deadlock for nc, wget, etc.`)  
**Scope:** Kernel I/O multiplexing syscall (`SYS_POLL`), subsystem readiness engine (pipes, PTYs, TCP/UDP sockets, files), bounded sleep integration, and userspace adoption in `nc` (and network tooling).

---

## 1. Executive Summary & Problem Statement

### 1.1 The Deadlock Hazard
FortressOS currently lacks an I/O readiness multiplexing primitive (`poll` or `select`). Single-threaded Ring 3 processes needing to handle bidirectional data streams (such as `nc`, network proxies, or interactive daemons) are forced into blocking on a single file descriptor at a time.

In [`user/nc.c`](../../user/nc.c), this leads to an explicit "finite serial half-duplex" restriction:
1. `nc` reads all data from standard input to EOF, sends it, half-closes the connection (`SHUT_WR`), and only then reads the response from the socket.
2. In listener mode (`nc -l`), terminal stdin is skipped entirely to prevent blocking.
3. If a remote peer sends a response before consuming the entire request (or expects an interactive exchange), `nc` and the peer deadlock indefinitely as TCP window buffers fill up.
4. If `nc` blocks on reading the socket, the user cannot provide keyboard input; if it blocks on keyboard input, incoming network packets sit unread.

### 1.2 Objective
Deliver a bounded, lock-safe, memory-safe `sys_poll` primitive conforming to the standard `poll(2)` model:
* Allows a process to monitor up to `MAX_PROCESS_FDS` (32) file descriptors concurrently.
* Returns immediately when requested events are ready (`POLLIN`, `POLLOUT`, `POLLERR`, `POLLHUP`, `POLLNVAL`).
* Supports non-blocking queries (`timeout = 0`), bounded millisecond deadlines (`timeout > 0`), and interruptible indefinite sleep (`timeout < 0`).
* Integrates cleanly with FortressOS's existing scheduler sleep channels without lock inversions or busy-waiting.
* Modernizes `user/nc.c` to support true full-duplex interactive sessions without deadlocks.

---

## 2. Protected Invariants & Constraints

1. **Lock Ranking (`PROTECTED.md` §Lock ranks):**
   - Readiness checks must never acquire Rank-1 locks simultaneously (e.g., cannot hold pipe lock while acquiring socket table lock).
   - Readiness checks are non-blocking and release all subsystem locks before any scheduling sleep.
   - Never hold a spinlock across `switch_context` or `sched_wait_until`.
2. **BSP Network Owner Invariant (`PROTECTED.md` §Networking contracts):**
   - Sockets must be inspected without mutating socket state on application cores. Socket readiness reads are query-only and thread-safe.
3. **Memory & Stack Bounds (Gate G1):**
   - Syscall stack usage must not exceed $\le 1024$ bytes.
   - Max file descriptors per poll: fixed array on kernel stack `struct pollfd k_fds[MAX_PROCESS_FDS]` ($32 \times 8 = 256$ bytes). Zero heap allocation on the syscall path.
4. **Validation (AGENTS.md S1–S4):**
   - User buffer `struct pollfd *` must be validated with `vmm_validate_user_range(pml4, ptr, size, PTE_WRITABLE)` before reading or writing.
5. **Signal Interruption:**
   - Sleeping poll calls must wake immediately and return `-EINTR` (`-VFS_EINTR`) when a signal is pending (`process_signal_interrupt()`).

---

## 3. ABI Design

### 3.1 Syscall Definition
Assign the next sequential syscall number in [`src/include/syscall_abi.h`](../../src/include/syscall_abi.h):
```c
#define SYS_POLL 88 /* (struct pollfd *fds, uint64_t nfds, int64_t timeout_ms) -> ready_count or -errno */
```

### 3.2 Structures & Event Flags
In `src/include/syscall_abi.h` (and `user/poll.h`):
```c
struct pollfd {
    int   fd;       /* File descriptor */
    short events;   /* Requested events */
    short revents;  /* Returned events */
};

#define POLLIN   0x0001 /* Ready to read */
#define POLLPRI  0x0002 /* Exceptional condition (out-of-band) */
#define POLLOUT  0x0004 /* Ready to write without blocking */
#define POLLERR  0x0008 /* Error condition */
#define POLLHUP  0x0010 /* Hang up / peer closed connection */
#define POLLNVAL 0x0020 /* Invalid request: fd not open */
```

---

## 4. Phase-by-Phase Implementation Plan

### Phase 0: ABI Definitions & Userspace Header
- Add `SYS_POLL` (88) and `struct pollfd` definitions to `src/include/syscall_abi.h`.
- Create `user/poll.h` exporting `poll(struct pollfd *fds, uint64_t nfds, int64_t timeout_ms)` wrapper.
- Verify userland symbol linkage and headers.

### Phase 1: Subsystem Readiness Query Protocol
Implement non-blocking readiness query functions for each file type:
1. **Pipes (`src/fs/pipe.c`)**:
   - `pipe_poll(vfs_node_t *node, short events) -> short revents`:
     - `POLLIN`: `p->count > 0`. If `p->writers == 0`, append `POLLHUP` (and `POLLIN` if `p->count > 0`).
     - `POLLOUT`: `(PIPE_CAPACITY - p->count) > 0`. If `p->readers == 0`, append `POLLERR`.
2. **PTYs (`src/drivers/pty.c`)**:
   - `pty_slave_poll(pty_t *p, short events) -> short revents`:
     - `POLLIN`: `p->in_count > 0`. If `!p->master_open`, return `POLLHUP`.
     - `POLLOUT`: `(PTY_OUT_CAPACITY - p->out_count) > 0`.
   - `pty_master_poll(pty_t *p, short events) -> short revents`:
     - `POLLIN`: `p->out_count > 0`. If `!p->slave_open`, return `POLLHUP`.
     - `POLLOUT`: `(PTY_IN_CAPACITY - p->in_count) > 0`.
3. **Sockets (`src/net/net_socket.c`, `src/net/net_tcp.c`, `src/net/net_udp.c`)**:
   - `net_socket_poll(int fd, short events) -> short revents`:
     - Stream (TCP):
       - `POLLIN`: data buffered in RX ring (`rx_head != rx_tail`), or connection in `CLOSE_WAIT` / `CLOSED` / `TIME_WAIT` (sets `POLLHUP | POLLIN`), or listen socket has pending connections.
       - `POLLOUT`: connection in `ESTABLISHED` state and TX buffer has space.
     - Datagram (UDP):
       - `POLLIN`: datagram queued in UDP socket RX queue.
       - `POLLOUT`: always writable (within standard MTU).
4. **Regular Files & Devices (`ext4`, `tarfs`, `devfs`)**:
   - Always immediately ready: `POLLIN | POLLOUT`.

### Phase 2: Kernel Syscall Implementation (`sys_poll`)
- Implement `sys_poll` in `src/kernel/syscall.c`:
  1. Validate arguments: `nfds <= MAX_PROCESS_FDS`, validate user pointer memory with write permission.
  2. Copy `user_fds` to `struct pollfd k_fds[MAX_PROCESS_FDS]` on the kernel stack (measured $\le 256$ B).
  3. Scan loop:
     - Check each descriptor's readiness using the subsystem query helpers.
     - Accumulate `revents` for each entry and count active descriptors.
  4. Immediate return conditions:
     - If count > 0, copy back `k_fds` to user buffer and return count.
     - If `timeout_ms == 0`, copy back and return 0.
     - If signal interrupt pending, return `-SYSCALL_EINTR`.
  5. Sleep condition:
     - Calculate deadline in BSP ticks using `apic_timer_get_bsp_ticks()`.
     - Put thread to sleep using `sched_wait_until(time_get_sleep_channel(), poll_ready_predicate, &poll_ctx)` with timeout tick evaluation.
     - On wakeup from timer tick or driver wake, re-evaluate readiness.

### Phase 3: Host Unit Testing & Adversarial Gates
- Create `tests/poll_host.c` running under host ASan/UBSan:
  - Test 1: Immediate non-blocking poll on regular files / empty fds (`timeout = 0`).
  - Test 2: Pipe write -> read event notification (`POLLIN`).
  - Test 3: Pipe close -> hangup notification (`POLLHUP`).
  - Test 4: PTY master/slave bidirectional readiness.
  - Test 5: Timeout expiration when no descriptors become ready (`timeout = 50ms`).
  - Test 6: Invalid descriptor detection (`POLLNVAL`).
  - Test 7: Signal interruption during sleep (`EINTR`).
  - Test 8: Stack frame verification asserting $\le 1024$ bytes.
- Add target to `Makefile`: `make test-poll-host`.

### Phase 4: Modernize `user/nc.c`
- Rewrite `user/nc.c` main loop using `SYS_POLL`:
  - Set up `struct pollfd fds[2]`:
    - `fds[0] = { .fd = 0, .events = POLLIN }` (stdin)
    - `fds[1] = { .fd = sock, .events = POLLIN }` (network socket)
  - Event loop:
    - If `fds[0].revents & POLLIN`: read up to 4096 bytes from stdin and transmit to socket. On EOF, shutdown write channel (`SYS_SHUTDOWN(sock, NET_SHUT_WR)`) and stop monitoring stdin (`fds[0].fd = -1`).
    - If `fds[1].revents & POLLIN`: read incoming socket data and write to stdout (fd 1). On socket EOF/HUP, exit cleanly.
  - Fix listener mode (`nc -l`):
    - Do NOT skip stdin! Enable full bidirectional communication in listener mode.
  - Update `print_usage()` text in `user/nc.c`: remove the "Deadlock Hazard" warning.
  - Update `scripts/test_shell.py` to match the updated `nc --help` text.

### Phase 5: Multi-Firmware QEMU Acceptance Testing
- Write automated test script `scripts/test_poll_nc.py`:
  - Test A: Bidirectional interactive dialogue (client sends message, waits for response, sends follow-up without closing connection).
  - Test B: High-volume concurrent duplex streaming (preventing buffer lockup).
  - Test C: Listener mode interactive session.
  - Test D: Multi-firmware execution across BIOS and UEFI under `SMP=1` and `SMP=4`.
- Add `make test-poll-nc` target.

---

## 5. Verification Matrix & Exit Criteria

| Gate | Check | Target |
| --- | --- | --- |
| **G1: Stack Budget** | Max kernel stack frame | $\le 1024$ bytes (Gate G1 compliant) |
| **G2: Memory Safety** | Valgrind / ASan / UBSan | 0 leaks, 0 undefined behaviors |
| **G3: Lock Ranking** | Spinlock debug invariants | Zero lock rank inversions, zero locks held during sleep |
| **G4: Host Test Suite** | `make test-poll-host` | 100% PASS |
| **G5: Interactive `nc`** | `make test-poll-nc` | 100% PASS across BIOS/UEFI × SMP=1/4 |
