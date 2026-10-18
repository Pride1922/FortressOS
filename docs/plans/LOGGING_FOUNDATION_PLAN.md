# FortressOS — Persistent Logging Foundation Plan (Three Pillars Architecture)

## 1. Executive Summary

FortressOS logging is structured into three decoupled layers ("The Three Pillars") that separate raw kernel diagnostics, daemon output streaming, and high-level system event auditing. This architecture ensures zero blast radius: logging failures or storage exhaustion never block critical system daemons, require no heavyweight syslog IPC daemons or sockets, and maintain bounded, deterministic execution.

```
+-------------------------------------------------------------------------------+
|                               THREE PILLARS OF LOGGING                        |
+-------------------------------------------------------------------------------+
| Pillar 1: Kernel Boot Dump    | Pillar 2: Service Capture     | Pillar 3: Global Timeline     |
| Destination: /var/log/boot.log| Destination: /var/log/watchtower/ Destination: /var/log/messages|
| Producer: /bin/dmesg          | Producer: Watchtower Pipes    | Producer: /bin/logger & Daemons|
| Lifecycle: Early boot service | Lifecycle: Supervised runtime | Lifecycle: Event-driven audit  |
+-------------------------------------------------------------------------------+
```

---

## 2. Architectural Pillars

### Pillar 1: Kernel Boot Dump (`/var/log/boot.log`)
- **Purpose**: Persist an uncorrupted snapshot of early hardware initialization (PCIe, ACPI, PMM/VMM, NVMe, USB, network link) before daemon activity overwrites the fixed-capacity kernel circular ring buffer.
- **Mechanism**: Executed via `/bin/dmesg /var/log/boot.log`.
- **Forensic Retention**: Before overwriting `/var/log/boot.log`, the previous boot snapshot is rotated to `/var/log/boot.log.prev` to preserve diagnostic records across crash or reboot cycles.
- **Supervisor Integration**: Configured as an early supervisor service (`Order=01`, `Type=simple`, `Restart=no`). The kernel does not execute userspace tools directly during `kmain`.

### Pillar 2: Service Capture (`/var/log/watchtower/<service>.log`)
- **Purpose**: Aggregate stdout and stderr from supervised background daemons (`sshd`, `dhcpc`, `crond`) without embedding custom logging IPC or wire protocols into daemon code.
- **Mechanism**: Watchtower creates anonymous pipe pairs (`SYS_PIPE`), redirects child stdout/stderr (fds 1 and 2) to the write end via `SPAWN_FD_ACTION_DUP2`, and sets the read end non-blocking (`VFS_O_NONBLOCK`).
- **Backpressure & Draining**: Pipe buffers are bounded (4–16 KiB). Supervisor event loop drains child pipes prior to heavy operations to minimize writer stalls. Append failures increment `dropped_bytes` counters rather than blocking.
- **Ownership**: Watchtower is the **sole owner** of `/var/log/watchtower/*.log` rotation. External tools must not compete for these files.

### Pillar 3: Global Audit Timeline (`/var/log/messages`)
- **Purpose**: Chronological, human-readable audit trail of significant administrative and lifecycle events (user login, privilege elevation, network configuration, supervisor actions).
- **Mechanism**: Written via `/bin/logger` CLI / pipeline tool and shared in-daemon append routines.
- **Access Control & Permissions**: File mode `0640` (`root:wheel`). Unprivileged processes cannot directly append to or truncate the audit log.
- **Privilege Separation**: In privilege-separated daemons (such as `sshd`), audit messages are emitted solely by the privileged monitor process (`uid=0`), never by unprivileged session workers.

---

## 3. Core Technical Invariants & Failure-Handling

### 3.1 Append Atomicity & Short-Write Semantics
- **Single `SYS_WRITE` Call**: Every record is fully formatted in a static/stack buffer (`<= 512` bytes) and emitted in a single `write()` call using `VFS_O_APPEND`.
- **Filesystem Guarantee**: Ext2 and runfs serialize authoritative EOF pointer adjustment and cluster allocation under filesystem exclusion (`ext2_lock`). Records under cluster size do not interleave.
- **Short-Write Policy**:
  - If `write()` returns `< 0` (e.g. `ENOSPC`, `EROFS`, `EIO`): Log diagnostic to stderr and exit 1.
  - If `0 <= written < len`: Storage exhausted mid-record. **Do not retry the remaining fragment** (retrying against an append file can interleave with concurrent writers). **Do not replay the full record to fallback** (produces duplicate prefixes). Mark failure, report truncation to stderr, and exit 1.

### 3.2 Safe Fallback Destination Hierarchy
- **Runtime Fallback**:
  1. Primary: `/var/log/messages` (writable persistent root).
  2. Fallback: `/run/messages` (RAM-backed `runfs`). Used automatically if `/var/log` is read-only (Live ISO/USB media) or missing.
- **Policy Boundary**: Do **not** silently fall back to persistent storage paths (`/mnt/.fortress/...`). Mount targets and storage persistence require explicit configuration and permission enforcement.

### 3.3 Log Injection & Input Sanitization
- Unsanitized inputs (usernames, command arguments, network identifiers) could inject spoofed log lines via carriage returns or newlines.
- All strings processed by `/bin/logger` or daemon loggers must be strictly sanitized:
  - `\n` and `\r` are replaced with space or escape sequence `\n`.
  - Non-printable ASCII bytes (`< 0x20` or `>= 0x7f`) are escaped as `\xHH`.
  - Record length is hard-capped at 512 bytes including timestamp and newline.

### 3.4 Timestamping Invariants
- Timestamps are retrieved via `SYS_CLOCK_GETTIME` with `CLOCK_REALTIME`.
- **Calendar Time**: Converted to UTC datetime using deterministic leap-year arithmetic:
  `[YYYY-MM-DD HH:MM:SS UTC] <tag>: <message>`
- **Monotonic Fallback**: If RTC/calendar time is uninitialized (epoch year < 2026), format monotonic uptime explicitly to avoid deceptive `1970-01-01` stamps:
  `[   12.345s UPTIME] <tag>: <message>`

### 3.5 Rotation Concurrency & Ownership
- **Watchtower Logs**: Managed exclusively by Watchtower internal rotation (1 MiB threshold, 3 generations). Excluded from `/etc/logrotate.conf`.
- **Global & Boot Logs**: Managed by `/sbin/logrotate` via `/etc/crontab`.
- **Elimination of the Rotation Race Window**:
  - When rotating `active -> active.1`, `logrotate` must recreate the active path **immediately after renaming**, *before* executing `/bin/gzip`.
  - The recreation must use `VFS_O_CREAT | VFS_O_EXCL` so that concurrent writers opening between rename and create do not have their newly appended records truncated by `O_TRUNC`.

---

## 4. Phased Implementation Roadmap

### Phase 1: `/bin/logger` Utility & Host Test Suite
- Implement `user/tools/logger.c` and `user/tools/logger.h`:
  - CLI syntax: `/bin/logger [-t tag] [message...]`
  - Stdin streaming mode when no message arguments are provided.
  - Strict input sanitization (control characters and newlines escaped).
  - Single atomic `SYS_WRITE` buffer (`<= 512` bytes).
  - UTC timestamp formatter with monotonic uptime fallback.
  - Destination fallback (`/var/log/messages` -> `/run/messages`).
- Host test suite in `tests/logger_host.c`:
  - Unit tests under ASan/UBSan (`make test-logger-host`).
  - Covers CLI options, tag formatting, stdin pipe processing, sanitization, bounds truncation, and destination fallback.
- Makefile integration: compile `tool-logger.elf` and package `/bin/logger` into `initramfs.tar`.

### Phase 2: Logrotate Concurrency Fix & Reopen Safety
- Hardening `user/logrotate/logrotate.c`:
  - Recreate active file immediately after rename before gzip invocation.
  - Protect against concurrent writer truncation.
- Update `/etc/logrotate.conf` to target `/var/log/messages` and `/var/log/boot.log` only (explicit paths, no conflicting Watchtower service wildcards).
- Schedule rotation in `/etc/crontab` (`60 /sbin/logrotate /etc/logrotate.conf`).

### Phase 3: Watchtower Boot Snapshot Service (`boot-log.service`)
- Add boot snapshot service `/etc/watchtower/boot-log.service`:
  ```ini
  Exec=/bin/dmesg /var/log/boot.log
  Order=01
  Type=simple
  Restart=no
  ```
- Implement pre-rotation (`/var/log/boot.log` -> `/var/log/boot.log.prev`) to preserve previous boot diagnostics.
- Update Watchtower supervisor tests to verify oneshot execution under `Type=simple`.

### Phase 4: Daemon Event Logging Wiring
- **SSHD**:
  - Add structured logging calls in privileged monitor (`user/sshd/monitor.c`):
    - `logger -t sshd "Accepted publickey for <user> from <ip>"`
    - `logger -t sshd "Failed publickey for <user> from <ip>"`
- **DHCPC**:
  - Add lease event logging in `user/dhcpc.c`:
    - `logger -t dhcpc "Bound lease <ip> via <gateway>"`
    - `logger -t dhcpc "Renewed lease <ip>"`

### Phase 5: Verification & Multi-Firmware Acceptance
- Host unit & integration test suites (`test-logger-host`, `test-logrotate-host`, `test-watchtower-host`).
- BIOS and UEFI QEMU acceptance:
  - Concurrent loggers running under multi-core SMP.
  - Boot dump presence and persistence across reboot cycles.
  - Active logrotate shifts under concurrent logging load.
  - Storage exhaustion handling on full and read-only media.
