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

#### 4.3.1 The Delaycompress Protocol & Residual Writer Risk
- **Mechanism**:
  - Rotating shifts the active file to `messages.1` and leaves `messages.1` **uncompressed**.
  - Compression is deferred to the subsequent cycle when `messages.1` is shifted to `messages.2` and compressed into `messages.2.gz`.
- **Residual Risk & Bounded Reality**:
  - Delaycompress drastically narrows the open-descriptor race compared to immediate compression, but **it does not theoretically eliminate it**:
    - If a writer process opens `messages` with `VFS_O_APPEND`, gets preempted/suspended by the scheduler (or paused across two rapid manual rotations), and only executes its `SYS_WRITE` after two full rotation cycles, it will append data to `messages.2` while `/bin/gzip` is reading or replacing it.
  - **Explicit Guarantee**: Delayed compression is a **best-effort protection** against transient writer preemption. FortressOS does not implement whole-file advisory or mandatory byte-range locking. Loss-free archival is guaranteed only for writers that complete their single-record append within one rotation cycle.
  - A dedicated host test (`tests/logrotate_host.c`) verifies writer interleaving: a paused writer holding an open descriptor across one rotation appends safely to `messages.1`, and a writer held across two rotations is asserted under best-effort residual risk behavior.

#### 4.3.2 Atomic Single-Instance Exclusion (`SYS_MKDIR`)
- **The Problem with Ordinary Files**:
  - In FortressOS VFS, `SYS_OPEN` with `VFS_O_CREAT` does not support `O_EXCL`. Two processes attempting to open a lockfile simultaneously can both succeed, leading to corrupt or concurrent rotation shifts.
- **Atomic Exclusion Mechanism**:
  - Exclusion is enforced using **atomic directory creation via `SYS_MKDIR`**:
    - `sys_call(SYS_MKDIR, "/run/logrotate.lock", 0700)` maps to `vfs_create_common(..., VFS_DIRECTORY, ...)` which executes under filesystem spinlock (`run_lock` or `ext2_lock`).
    - If the directory already exists, `SYS_MKDIR` returns the userspace ABI error `SYSCALL_EEXIST` (`-15`).
  - **Ownership & PID Tracking**:
    - Upon successful directory creation, `logrotate` writes its PID to `/run/logrotate.lock/pid`.
  - **Stale Lock Policy (Fail-Closed, No Automatic Removal, Explicit CLI Switch)**:
    - Automatic stale-lock removal introduces destructive races: if two recovering processes observe a dead PID, or if a creator process is delayed between `mkdir` and writing its PID, an aggressive recoverer can remove a newly acquired lock owned by a live process.
    - **Contract**: Acquisition fails closed on `SYSCALL_EEXIST` (`-15`). `logrotate` prints a diagnostic warning to stderr (`"logrotate: locked by /run/logrotate.lock (pid %ld); skipping\n"`).
    - **Explicit Mode Switch (`--skip-if-locked`)**:
      - Because an executable cannot reliably infer whether it was invoked interactively or by `crond`:
        - Scheduled cron jobs invoke `/sbin/logrotate --skip-if-locked /etc/logrotate.conf`. When `--skip-if-locked` is passed and `SYS_MKDIR` returns `SYSCALL_EEXIST`, `logrotate` exits `0` (clean skip without supervisor/cron failure).
        - Direct administrative invocations (omitting `--skip-if-locked`) exit nonzero (`1`) on lock collision.
    - **No automatic deletion**: Because `/run` is an ephemeral in-memory filesystem (`runfs`), abandoned locks from unhandled system panics are automatically cleared on reboot. Explicit administrative recovery is performed by manually removing `/run/logrotate.lock` via `/bin/rm -rf` or shell.
  - **Clean Release**:
    - On exit (normal completion or error), the lock creator unlinks `/run/logrotate.lock/pid` and removes `/run/logrotate.lock` with `SYS_UNLINK`.

#### 4.3.3 Exact Rotation Sequencing & Boundary Conditions
The rotation engine supports all valid configuration combinations (`rotate 0`, `rotate 1`, `rotate >= 2`, with or without `compress`).

**Recreation Timing & Active Path Availability**:
- Between `rename(active, active.1)` and `open(active, O_CREAT)`, the active path is not present in directory lookups.
- However, FortressOS writers (`logger` or daemons) open the audit file with `VFS_O_CREAT | VFS_O_WRONLY | VFS_O_APPEND`. If a writer initiates an append during this brief window, its `open` simply recreates the active file directly and appends successfully, without requiring assistance from `logrotate`.
- If creation fails because the filesystem is read-only (`EROFS`) or the parent directory is missing (`ENOENT`), the writer falls back to `/run/messages`. If storage is exhausted (`ENOSPC`), the write fails closed and does **not** fall back (as defined in Section 4.1).

**Step Sequence by Archive Count & Mode**:
1. **Acquire Exclusion**: Atomically create `/run/logrotate.lock`. On `SYSCALL_EEXIST`, exit 0 if `--skip-if-locked`, else exit 1.
2. **Handle `rotate 0`**:
   - If `entry->rotate == 0`:
     - Truncate or unlink `active`.
     - Recreate `active` (empty) with `orig_mode & 07777`.
     - Release lock and return 0.
3. **Handle `rotate 1`**:
   - Unlink oldest archive (`active.1.gz` and `active.1`).
   - Rename active file: `active -> active.1`.
   - **Immediately recreate active file**: Open `active` with `VFS_O_CREAT | VFS_O_WRONLY` (`orig_mode & 07777`, **no** `O_TRUNC`) and close.
   - **Compression Policy for `rotate 1`**:
     - Immediate compression on `rotate 1` would eliminate the one-cycle grace period and reintroduce the writer-vs-compressor race on `active.1`.
     - Therefore, `active.1` is retained **uncompressed** even if `compress` is configured, guaranteeing that in-flight open descriptors write safely without corrupting a concurrent gzip process.
     - Release lock and return 0.
4. **General Case (`rotate >= 2`) with `compress` (Delaycompress)**:
   - **Step 4a (Unlink Oldest)**: Unlink `active.N.gz` and `active.N`.
   - **Step 4b (Shift Older Compressed Archives)**:
     - For `i = N - 1` down to `2`: rename `active.i.gz -> active.(i+1).gz` and `active.i -> active.(i+1)`.
   - **Step 4c (Shift and Compress Previous Shift)**:
     - If `active.1` exists:
       - Rename `active.1 -> active.2`.
       - Spawn `/bin/gzip -f active.2` (bounded timeout, e.g. 5 seconds).
       - Wait for exit. On timeout, send `SIGKILL`, await confirmed child exit via `waitpid`, and leave `active.2` uncompressed.
   - **Step 4d (Shift Active File)**:
     - Rename `active -> active.1`.
   - **Step 4e (Recreate Active File)**:
     - Open `active` with `VFS_O_CREAT | VFS_O_WRONLY` (`orig_mode & 07777`, **no** `O_TRUNC`) and close.
   - **Step 4f**: `active.1` remains uncompressed during this cycle.
5. **General Case (`rotate >= 2`) without `compress`**:
   - Unlink `active.N`.
   - Shift archives uncompressed: `active.i -> active.(i+1)` for `i = N - 1` down to `1`.
   - Rename `active -> active.1`.
   - Recreate `active` with `orig_mode & 07777` and close.
6. **Release Exclusion**: Remove `/run/logrotate.lock/pid` and `/run/logrotate.lock`.

#### 4.3.4 Asynchronous Compression in Watchtower
- **Confirmed Exit Before Cleanup or Reuse**:
  - Watchtower tracks `wt_compress_pid` and `wt_compress_archive`.
  - While `wt_compress_pid > 0`, supervisor rotation ticks **must not initiate new compression jobs**, and must not shift, rename, or unlink `wt_compress_archive`.
  - On each supervisor tick, check child exit via `sys_call(SYS_WAITPID, wt_compress_pid, &status, WNOHANG)`.
  - **Timeout Handling**:
    - If elapsed time exceeds 5 seconds, send `sys_call(SYS_KILL, wt_compress_pid, SIGKILL)`.
    - **Do NOT immediately unlink output paths**: The child process may still be executing syscalls until scheduled. Keep the compression slot reserved.
    - Only after `SYS_WAITPID` subsequently reaps the dead child PID and confirms process exit does Watchtower unlink the partial `.gz` file, mark the archive uncompressed, and clear `wt_compress_pid = 0`.
- **Capacity & Concurrency Cap**:
  - Exactly **one** asynchronous compression child is permitted across the entire supervisor at any time.

#### 4.3.5 Consistent RAM Log Retention & Runfs Safeguards
- **Consistent 2 KiB Threshold**:
  - In `runfs`, each vnode reserves a fixed 4 KiB pool entry (`RUNFS_BYTES = 4096u`) regardless of file length.
  - To prevent individual log files from exhausting node capacity or triggering `ENOSPC` during drain appends:
    - The active log file rotation threshold is strictly set to **2 KiB** (`WT_LOG_MAX_SIZE_RAM = 2048`).
    - Incoming pipe drain chunks check remaining headroom against this 2 KiB ceiling: `2048 - current_size`. If a chunk exceeds remaining headroom, it is truncated to fit, and discarded bytes are explicitly added to `service->dropped_bytes`.
  - **Uncompressed RAM Retention**:
    - Archives in `/run/watchtower/` are retained **uncompressed** (`watchtower.log.1`, `watchtower.log.2`).
    - Gzip stream headers (10 bytes), block dynamic trees, and deflate trailers on small 2 KiB buffers introduce overhead without substantial compression benefit, and spawn unnecessary processes in ephemeral memory.
    - Retention in RAM is capped at **2 generations of 2 KiB**, guaranteeing that total **retained log content** for a service log never exceeds `6 KiB` (active + 2 generations × <= 2 KiB content), avoiding file capacity exhaustion.

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
  - Adopt exact rotation sequences for `rotate 0`, `rotate 1`, and `rotate >= 2` with/without `compress`:
    1. Acquire exclusion via atomic `SYS_MKDIR("/run/logrotate.lock", 0700)`. Fail closed on `SYSCALL_EEXIST` (`-15`). If `--skip-if-locked` is specified, exit 0; otherwise exit 1. No racy automatic stale deletion.
    2. Handle archive shifting per boundary rules (`rotate 0` clears active; `rotate 1` shifts active -> .1 uncompressed; `rotate >= 2` shifts archives).
    3. Under `delaycompress`: compress previous shift `.2 -> .2.gz` and wait for confirmed child exit before rotating active.
    4. Rename active `messages -> messages.1`.
    5. **Immediately** recreate active file (`VFS_O_CREAT | VFS_O_WRONLY`, original mode, no `O_TRUNC`) before releasing exclusion.
    6. Cleanly release exclusion directory.
  - Document residual risk for writers held across >= 2 rotations.
  - Update `tests/logrotate_host.c`:
    - Assert `delaycompress` behavior (active -> .1 uncompressed, .2 compressed to .2.gz).
    - Assert boundary conditions: `rotate 0`, `rotate 1` (uncompressed retention), and uncompressed rotation.
    - Assert atomic `SYS_MKDIR` lockdir collision (`SYSCALL_EEXIST`), PID recording, `--skip-if-locked` exit 0 vs CLI exit 1, and clean release.
    - Assert writer paused across 1 rotation (survives cleanly in .1) vs writer paused across 2 rotations (residual risk behavior).
- Update `user/watchtower/watchtower_main.c`:
  - After `wt_rotate_log()`, open new active log file and call `SYS_DUP2(new_fd, 2)` so supervisor diagnostics and panics direct to the new log.
  - Run log compression asynchronously:
    - Track `wt_compress_pid` and `wt_compress_archive`.
    - Prevent concurrent compression jobs or shifts on conflicting paths.
    - On timeout (5s), send `SIGKILL` but await confirmed child exit via `SYS_WAITPID` before unlinking partial `.gz` output and releasing the slot.
  - Pipe drain bounded budget (max 4 iterations of 4 KiB chunks per tick).
  - RAM log adaptation: Retain 2 uncompressed generations of 2 KiB in `/run/watchtower/` (avoid gzip expansion over 4 KiB), and truncate oversized incoming chunks to fit available node capacity, accounting discarded bytes to `dropped_bytes`.
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

### Phase 4: Multi-Firmware Verification & Host Integration (COMPLETE)
- Host test coverage (100% PASS under ASan/UBSan):
  - `make test-logger-host` (9/9 PASS): input sanitization, UTC/uptime formatting, truncation, untrusted path rejection, short-write handling, CLI parsing, stdin streaming.
  - `make test-timeline-log-host` (9/9 PASS): formatting, monotonic uptime fallback, control char sanitization, 512B truncation, fallback to /run/messages on ENOENT/EROFS, fail-closed on EACCES, untrusted rejection, short-write handling, rate-limiting.
  - `make test-boot-snapshot-host` (11/11 PASS): primary /var/log, pre-rotation to .prev, fallback to /run on ENOENT/EROFS, runfs 4 KiB clamping, EPERM rejection, symlink/untrusted rejection.
  - `make test-logrotate-host` (14/14 PASS): delaycompress lifecycle, boundary rotate 0/1/uncompressed, atomic SYS_MKDIR lock collision, --skip-if-locked, reserved path rejection (/var/log/watchtower, /run/watchtower, boot.log).
- Multi-firmware QEMU testing (`make test-logging`, 100% PASS on BIOS & UEFI at SMP=1 and SMP=4):
  - Pillar 1: Verified boot snapshot service (`boot-log.service`), creation of `/run/boot.log` with kernel messages, pre-rotation to `/run/boot.log.prev` on successive run.
  - Pillar 2: Verified Watchtower supervisor diagnostics in `/run/watchtower/watchtower.log` and child daemon pipe redirection.
  - Pillar 3: Verified `/bin/logger` CLI direct and piped messages, timestamp formatting, input sanitization escaping control characters, and strict DAC permissions rejection (unprivileged `operator` receiving `permission denied` on `0640` `/run/messages`).
  - Logrotate: Verified `/sbin/logrotate` execution with `--skip-if-locked /etc/logrotate.conf`, atomic `/run/logrotate.lock` exclusion collision, and active rotation lifecycle shifting `.1 -> .2` and recreating active logs.
  - SMP Concurrency: Verified multi-core Ring 3 concurrent logging stress at SMP=4 without record corruption or interleaving.
