# FortressOS — Persistent Logging Foundation Plan (Three Pillars Architecture)

## 1. Executive Summary

FortressOS logging is structured into three decoupled layers ("The Three Pillars") that separate raw kernel diagnostics, daemon output streaming, and high-level administrative audit events. 

This architecture balances deterministic, bounded execution with pragmatic Unix lifecycle semantics. Rather than embedding heavyweight syslog daemons or complex socket IPC, FortressOS leverages direct filesystem operations, pipe redirection in the service supervisor, and bounded single-append writes.

```
+-----------------------------------------------------------------------------------------+
|                                  THREE PILLARS OF LOGGING                               |
+-----------------------------------------------------------------------------------------+
| Pillar 1: Kernel Boot Dump      | Pillar 2: Service Capture       | Pillar 3: Global Timeline   |
| Destination: /var/log/boot.log  | Destination: /var/log/watchtower/ Destination: /var/log/messages|
| Producer: /bin/dmesg            | Producer: Watchtower Pipes      | Producer: /bin/logger &     |
| Owner: Startup service alone    | Owner: Watchtower supervisor    |           In-daemon helper  |
| Lifecycle: Early boot snapshot  | Lifecycle: Supervised runtime   | Owner: /sbin/logrotate      |
+-----------------------------------------------------------------------------------------+
```

---

## 2. Blocking Policy & Failure Domain Decisions

A fundamental architectural trade-off exists between strict process isolation and synchronous storage reliability:

1. **Daemon Stderr/Stdout Pipes (Pillar 2)**:
   - **Current Reality**: Watchtower sets only the *read* end of child pipes (`pipe_fds[0]`) to `VFS_O_NONBLOCK`. The *write* end (`pipe_fds[1]`, inherited as child stdout/stderr) is **blocking**.
   - **Policy Decision (Best-Effort Synchronous Logging)**:
     - Under ordinary workloads, Watchtower drains child pipes on each iteration of its supervisory loop.
     - If child output exceeds pipe capacity (4–16 KiB) while Watchtower is executing filesystem writes or slow gzip operations, the producing daemon will block on `write(2, ...)` until the pipe is drained.
     - Setting child write ends non-blocking would require all daemons to implement partial-write buffering, `EAGAIN` handling, and non-blocking retry loops.
     - **Architectural Guarantee**: Logging provides *best-effort synchronous capture with explicit backpressure*. To mitigate supervisor latency, Watchtower prioritizes pipe draining before disk syncs and must not block the supervisor loop on child compression.

2. **Audit Event Timeline (Pillar 3)**:
   - Timeline logging performs synchronous file append operations.
   - Daemons use an **in-process C helper function** (`timeline_log()`), never fork/exec-ing `/bin/logger` on critical authentication or lease renewal paths.
   - **Concurrency and Stalling Contract**: Logging failure never terminates the daemon; synchronous append and diagnostic writes may delay it. Rate-limiting stderr diagnostics mitigates backpressure on the supervisor capture pipe, but synchronous storage I/O inherently blocks until the kernel filesystem write returns.

---

## 3. Detailed Architectural Pillars

### Pillar 1: Kernel Boot Dump (`/var/log/boot.log`)
- **Purpose**: Persist a forensic snapshot of early hardware initialization (PCIe, ACPI, PMM/VMM, NVMe, USB, network link) before daemon runtime activity overwrites the fixed-capacity kernel circular ring buffer.
- **Scope & Limitations**: Captures the *retained* kernel buffer at snapshot time; messages dropped prior to ring expansion or during very early boot are not recoverable.
- **Single-Owner Retention**:
   - **Startup ALONE owns boot-log retention**. External tools (`logrotate`) must **not** manage or rotate `/var/log/boot.log`.
   - Before taking a new snapshot, the boot snapshot utility pre-rotates the existing `/var/log/boot.log` to `/var/log/boot.log.prev`.
   - If `/var/log` is unavailable or read-only (Live media), write to `/run/boot.log` (subject to runfs 4 KiB limits) or skip persistent storage.
- **Execution Mechanism**:
   - Configured in Watchtower as `/etc/watchtower/boot-log.service`:
     ```ini
     Exec=/bin/boot-snapshot
     Order=01
     Type=simple
     Restart=no
     ```
   - Note on ordering: `Order=01` orders launch sequence; it does not block subsequent service launches until completion under `Type=simple`.

### Pillar 2: Service Capture (`/var/log/watchtower/<service>.log`)
- **Purpose**: Capture background daemon stdout and stderr without embedding custom logging protocols or IPC code into daemon binaries.
- **Single Ownership**:
   - **Watchtower is the sole owner** of `/var/log/watchtower/*.log`.
   - External `/sbin/logrotate` is explicitly barred from managing Watchtower logs to eliminate competing rotation operations.
- **Descriptor Reopen Protocol & Stderr Replacement**:
   - Watchtower redirects its own stderr (fd 2) to `/var/log/watchtower/watchtower.log` at startup (`sys_call(SYS_DUP2, wt_log_fd, 2)`).
   - Upon internal log rotation (`watchtower.log -> watchtower.log.1`), Watchtower must immediately open the new active log file with `VFS_O_CREAT | VFS_O_WRONLY | VFS_O_APPEND` and call `sys_call(SYS_DUP2, new_fd, 2)` **before** initiating compression.
   - This guarantees subsequent panic writes or diagnostics do not write to the rotated archive.
- **Asynchronous Compression & Drain Budgets**:
   - Compressing rotated logs via `/bin/gzip` is spawned asynchronously.
   - Watchtower tracks the compression child PID without blocking the supervisor loop, reaping it with `SYS_WAITPID(pid, &st, WNOHANG)`.
   - Pipe draining runs with bounded read budgets per service per tick (e.g., maximum 4 iterations of 4 KiB) to prevent a runaway daemon from starving supervision.
- **Emergency RAM Threshold Adaptation**:
   - If operating in RAM fallback (`/run/watchtower/`), the rotation threshold must adapt down from `1 MiB` (`WT_LOG_MAX_SIZE`) to `2 KiB` (`RUNFS_BYTES / 2 = 2048`) because `runfs` files hard-cap at 4 KiB.

### Pillar 3: Global Audit Timeline (`/var/log/messages`)
- **Purpose**: Sequential, human-readable audit trail of significant administrative events (logins, privilege drops, lease changes, daemon startup/shutdown).
- **Dual Producers**:
   - **/bin/logger CLI**: Standalone tool for shell scripts, administrative commands, and piped streams (`dmesg | grep NET | logger -t kernel`). Exits 1 on failure.
   - **In-Daemon C Helper (`timeline_log`)**: In-process direct append for `sshd` privileged monitor and `dhcpc`. Returns failure code on error; never exits daemon.
- **Directory Provisioning & Permissions**:
   - `/var/log` directory is provisioned with ownership `root:wheel` (`uid=0, gid=10`) and mode `0750` or `0755`.
   - File mode `0640` (`root:wheel`).
   - Untrusted validation: Reject logging if `/var/log` or `/run` is world-writable, owned by a non-root user, or if the log path is a symlink (`SYS_LSTAT` + `VFS_S_IFLNK`).
   - In privilege-separated daemons (`sshd`), audit messages are emitted solely by the privileged monitor process (`uid=0`), never by unprivileged session workers.

---

## 4. Technical Invariants & Concurrency Hardening

### 4.1 Append Serialization & Short-Write Semantics
- **Filesystem Guarantee**:
  - `Ext2` and `runfs` serialize authoritative EOF selection and writes under filesystem exclusion (`ext2_lock` and `run_lock`).
  - Records do not interleave on successful append writes. Timeline order represents write-lock acquisition order, not strict global clock chronology across multi-core CPUs.
- **Single `SYS_WRITE` Call**:
  - Formatted in a static/stack buffer bounded to `<= 512` bytes:
    `[YYYY-MM-DD HH:MM:SS UTC] tag: message\n`
  - Issued via exactly one `SYS_WRITE` call with `VFS_O_APPEND`.
- **Short-Write Policy**:
  - `written == len`: Success.
  - `written < 0`: Fail closed, report error, return/exit nonzero.
  - `0 <= written < len`: Storage exhausted mid-write (`ENOSPC`). **Do not retry the fragment** (retrying against an append file can interleave with concurrent writers). **Do not replay full record to fallback** (produces duplicate prefixes). Fail closed, log truncation notice to stderr, and return/exit nonzero.

### 4.2 RAM Fallback & `runfs` Capacity Policy
- **Runfs Reality**: [`src/fs/runfs.c#L7`](file:///c:/Sources/FortressOS/src/fs/runfs.c#L7) enforces `RUNFS_BYTES = 4096u` per file and 64 total nodes across the filesystem.
- **Policy**:
  - `/run/messages` holds at most 4 KiB (~8 to 10 maximum-sized 512B records).
  - `/run/messages` is strictly an emergency ephemeral buffer for early boot or live read-only media.
  - When 4 KiB is reached, subsequent appends return `ENOSPC`/`EFBIG`.
  - Failures to append to `/run/messages` drop records and report dropped count; `runfs` is not enlarged globally.
- **Fallback by Error Class**:
  - `ENOENT` / `EROFS` on primary `/var/log/messages`: Fall back to `/run/messages`.
  - `EACCES` / `EPERM`: Fail closed immediately; do **not** fall back (prevents bypassing permission boundaries).
  - No silent fallback to `/mnt/.fortress/...`.

### 4.3 Logrotate Concurrency, Delaycompress, & Atomic Exclusion
- **The Writer vs Compressor Race**:
  - In a concurrent system, a writer (`logger` or daemon) may open `messages` with `VFS_O_APPEND` just before `logrotate` shifts the file (`messages -> messages.1`).
  - The writer still holds a valid open file descriptor pointing to the vnode now named `messages.1`.
  - If `logrotate` immediately compresses `messages.1` with `/bin/gzip`, any writes in-flight or delayed by scheduler preemption will append data to `messages.1` *while* gzip is reading or unlinking it, resulting in truncated records or gzip corruption.
- **The Delaycompress Protocol**:
  - To eliminate the compression race without complex kernel file locking:
    1. Unlink oldest retained archive (`.N.gz`).
    2. Shift compressed archives: `.N-1.gz -> .N.gz` down to `.2.gz -> .3.gz`.
    3. Compress the previous cycle's uncompressed shift: `messages.1 -> messages.2.gz` via `/bin/gzip -f messages.2` (after renaming `.1 -> .2`).
    4. Rename active file: `messages -> messages.1`.
    5. **Immediately recreate active file**: `open(path, VFS_O_CREAT | VFS_O_WRONLY, orig_mode & 07777)` (without `VFS_O_TRUNC`) and close.
    6. `messages.1` remains **uncompressed** for the duration of the current cycle. Any delayed writes from pre-existing open file descriptors complete safely into `messages.1`.
    7. On the *subsequent* rotation cycle, all pre-existing writers have long since closed their descriptors, allowing safe compression into `.2.gz`.
- **Atomic Exclusion**:
  - Multiple concurrent `logrotate` instances (e.g. manual CLI invocation while `crond` triggers scheduled rotation) must not interleave.
  - Exclusion is enforced via an atomic lockfile `/run/logrotate.pid`:
    - Open with `VFS_O_CREAT | VFS_O_WRONLY` and inspect PID, or fail if locked.
    - If stale PID is detected (process not alive via `kill(pid, 0)`), break lock cleanly.
    - Release lockfile upon exit.

### 4.4 Timestamp Formatting & Edge Cases
- Timestamps are retrieved via `SYS_CLOCK_GETTIME` with `CLOCK_REALTIME`.
- **Formatting**:
  - Valid UTC date: `[YYYY-MM-DD HH:MM:SS UTC] <tag>: <message>`
- **Edge Cases**:
  - Clock syscall failure or epoch year < 2026: Format monotonic uptime explicitly:
    `[   12.345s UPTIME] <tag>: <message>`
  - Implausible future year (> 2100): Treat as invalid clock and fall back to monotonic uptime.
  - Long input lines: Hard-truncated to 512 bytes with `...` indicator.
  - Unterminated input: Stdin lines lacking a trailing newline are explicitly appended with `\n`.
  - Log sanitization: All characters `< 0x20` and `>= 0x7f` are escaped as `\xHH` (or `\r`/`\n` replaced with space) to prevent log injection / ANSI terminal manipulation attacks.

---

## 5. Staged Implementation Plan

```
+-----------------------------------------------------------------------------------------+
| Phase 0: Prerequisite Fixes & Policies                                                 |
| - Concurrency fix in logrotate (immediate active recreate, no post-gzip O_TRUNC)        |
| - Watchtower descriptor reopen protocol after internal rotation                        |
| - Runfs 4 KiB capacity policy documentation                                             |
+-----------------------------------------------------------------------------------------+
                                           |
                                           v
+-----------------------------------------------------------------------------------------+
| Phase 1: /bin/logger CLI & Streaming Tool                                               |
| - user/tools/logger.c: args + stdin stream, sanitization, UTC/uptime formatting         |
| - tests/logger_host.c: ASan/UBSan unit test suite (make test-logger-host)               |
| - Makefile: package /bin/logger into initramfs                                          |
+-----------------------------------------------------------------------------------------+
                                           |
                                           v
+-----------------------------------------------------------------------------------------+
| Phase 2: In-Process Daemon Logging Helper & Wiring                                      |
| - Shared timeline_log() helper avoiding process launch overhead                         |
| - Wire sshd privileged monitor (auth success/failure)                                   |
| - Wire dhcpc (lease bound/renewed)                                                      |
+-----------------------------------------------------------------------------------------+
                                           |
                                           v
+-----------------------------------------------------------------------------------------+
| Phase 3: Boot Snapshot Service & Crontab Scheduling                                     |
| - boot-snapshot utility: pre-rotate boot.log -> boot.log.prev, then dmesg               |
| - /etc/watchtower/boot-log.service (Order=01, Type=simple, Restart=no)                 |
| - /etc/crontab: schedule logrotate for /var/log/messages only                           |
+-----------------------------------------------------------------------------------------+
                                           |
                                           v
+-----------------------------------------------------------------------------------------+
| Phase 4: Multi-Firmware Verification & Host Integration                                 |
| - Host tests: pipe saturation, runfs exhaustion, concurrent rotation races              |
| - BIOS & UEFI QEMU acceptance: crash preservation, active rotation load, reboot forensic|
+-----------------------------------------------------------------------------------------+
```

### Phase 0: Prerequisite Fixes & Policies
- Update `user/logrotate/logrotate.c`:
  - Adopt `delaycompress` rotation flow:
    - Shift compressed archives `.N-1.gz -> .N.gz` down to `.2.gz -> .3.gz`.
    - Shift `.1 -> .2` and compress `.2 -> .2.gz`.
    - Rename active `messages -> messages.1`.
    - **Immediately** recreate active file (`VFS_O_CREAT | VFS_O_WRONLY`, original mode, no `O_TRUNC`) before any compression.
    - Leave `messages.1` uncompressed to allow in-flight open descriptors to write safely without data loss or compression race.
  - Add atomic exclusion via `/run/logrotate.pid` to prevent concurrent CLI and crond runs.
  - Update `tests/logrotate_host.c` to assert `delaycompress` behavior and lockfile exclusion.
- Update `user/watchtower/watchtower_main.c`:
  - After `wt_rotate_log()`, open new active log file and call `SYS_DUP2(new_fd, 2)` so supervisor diagnostics and panics direct to the new log.
  - Run log compression asynchronously (track child PID, non-blocking waitpid reaping) to prevent supervisor blocking.
  - Adapt rotation threshold from 1 MiB to 2 KiB when running from `/run/watchtower/` (`runfs`).
- Explicitly reserve `/var/log/watchtower/*.log` for Watchtower internal rotation.

### Phase 1: `/bin/logger` CLI & Streaming Tool
- Implement `user/tools/logger.c` and `user/tools/logger.h`:
  - Arguments: `/bin/logger [-t tag] [message...]`
  - Streaming: Reads stdin when no positional message is provided.
  - Sanitization: Replaces `\r`/`\n` and escapes control bytes.
  - Buffering: Single stack buffer `<= 512` bytes, single `SYS_WRITE`.
  - Output: `[YYYY-MM-DD HH:MM:SS UTC] tag: message\n` (uptime fallback).
  - Destination: `/var/log/messages` (fallback `/run/messages` on `ENOENT`/`EROFS`).
  - Error: Exits 1 on failure.
- Unit test suite: `tests/logger_host.c` (`make test-logger-host`).
- Package `/bin/logger` into `initramfs.tar`.

### Phase 2: In-Process Daemon Logging Helper & Wiring
- Implement shared in-process helper `timeline_log(const char *tag, const char *fmt, ...)`:
  - Formats in-memory without spawning a child process.
  - Appends to `/var/log/messages`.
  - Non-fatal: Returns -1 on failure, never exits the host daemon.
  - Rate-limited diagnostics to stderr.
- Wire into `user/sshd/monitor.c` (privileged monitor authentication events).
- Wire into `user/dhcpc.c` (lease bound/renewal events).

### Phase 3: Boot Snapshot Service & Crontab Scheduling
- Implement boot snapshot helper `user/tools/boot_snapshot.c`:
  - Rotates `/var/log/boot.log` to `/var/log/boot.log.prev`.
  - Dumps current kernel ring buffer via `sys_call(SYS_DMESG)`.
- Configure `/etc/watchtower/boot-log.service`:
  ```ini
  Exec=/bin/boot-snapshot
  Order=01
  Type=simple
  Restart=no
  ```
- Deploy `/etc/crontab` schedule for `/sbin/logrotate /etc/logrotate.conf`.
- Configure `/etc/logrotate.conf` to rotate `/var/log/messages` only.

### Phase 4: Multi-Firmware Verification & Host Integration
- Host test coverage:
  - Pipe saturation backpressure.
  - Runfs 4 KiB exhaustion handling.
  - Concurrent logger writes during active logrotate shifts.
  - Escaping and input sanitization boundaries.
- Multi-firmware QEMU testing (BIOS & UEFI):
  - Verification of `/var/log/boot.log` and `/var/log/boot.log.prev` across reboot cycles.
  - Concurrency test with multi-core Ring 3 processes writing to `/var/log/messages`.
  - Storage failure injection (full disk and read-only media).
