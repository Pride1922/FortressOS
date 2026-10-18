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
   - In-daemon logging returns `-1` on failure; it **never terminates, crashes, or stalls the daemon**. Diagnostic reporting of logging failures is strictly rate-limited to avoid flooding or blocking the stderr capture pipe.

---

## 3. Detailed Architectural Pillars

### Pillar 1: Kernel Boot Dump (`/var/log/boot.log`)
- **Purpose**: Persist a forensic snapshot of early hardware initialization (PCIe, ACPI, PMM/VMM, NVMe, USB, network link) before daemon runtime activity overwrites the fixed-capacity kernel circular ring buffer.
- **Scope & Limitations**: Captures the *retained* kernel buffer at snapshot time; messages dropped prior to ring expansion or during very early boot are not recoverable.
- **Single-Owner Retention**:
   - **Startup ALONE owns boot-log retention**. External tools (`logrotate`) must **not** manage or rotate `/var/log/boot.log`.
   - Before taking a new snapshot, the existing `/var/log/boot.log` is rotated to `/var/log/boot.log.prev`.
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
- **Descriptor Reopen Protocol**:
   - Watchtower holds an open write descriptor (`wt_log_fd`, dup'd to fd 2) for its own log.
   - Upon internal log rotation (`watchtower.log -> watchtower.log.1`), Watchtower must explicitly close and reopen `/var/log/watchtower/watchtower.log` so subsequent writes do not continue into the renamed archive.

### Pillar 3: Global Audit Timeline (`/var/log/messages`)
- **Purpose**: Sequential, human-readable audit trail of significant administrative events (logins, privilege drops, lease changes, daemon startup/shutdown).
- **Dual Producers**:
   - **/bin/logger CLI**: Standalone tool for shell scripts, administrative commands, and piped streams (`dmesg | grep NET | logger -t kernel`). Exits 1 on failure.
   - **In-Daemon C Helper (`timeline_log`)**: In-process direct append for `sshd` privileged monitor and `dhcpc`. Returns failure code on error; never exits daemon.
- **Permissions & Security**:
   - File mode `0640` (`root:wheel`).
   - Unprivileged processes cannot directly append to or truncate the audit log.
   - In privilege-separated daemons (`sshd`), audit messages are emitted solely by the privileged monitor process (`uid=0`), never by unprivileged session workers.
   - Safe destination validation: Rejects world-writable directories and symlink targets.

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

### 4.3 Logrotate Active-File Concurrency Fix
- **The Race Window**:
  - Renaming `active -> active.1` leaves the active path momentarily absent until recreation.
  - Recreating with `VFS_O_TRUNC` after compression will wipe out any records written by loggers that opened the file during the compression delay.
- **Hardened Rotation Protocol**:
  1. Rename `active -> active.1`.
  2. **Immediately** recreate `active` using `open(active, VFS_O_CREAT | VFS_O_WRONLY, orig_mode & 07777)` and close it *before* spawning `/bin/gzip`.
  3. Because `VFS_O_EXCL` is not defined in FortressOS VFS, `logrotate` inspects descriptor and file metadata; if another writer already created the path, `logrotate` does not truncate it.
  4. Run `/sbin/logrotate` under single-instance exclusion (via crond serialized scheduling or lockfile).
  5. Writers opening per-record (`open -> write -> close`) acquire the new file descriptor immediately after step 2.

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
  - Move active file recreation to immediately after `rename()`, prior to `/bin/gzip`.
  - Remove `O_TRUNC` from recreation step.
- Update `user/watchtower/watchtower_main.c`:
  - Reopen `wt_log_fd` after `wt_rotate_log()`.
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
