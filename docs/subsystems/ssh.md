# Secure Shell Subsystem (SSH) — Architecture, Daemon Lifecycle & Configuration Specification

**Status:** COMPLETE (Phases 0–8; multi-firmware QEMU acceptance 100% PASS across BIOS & UEFI; bare-metal Dell Latitude 5590 verification ready).

This document serves as the comprehensive architectural specification, contract reference, and configuration schema for the FortressOS SSH subsystem.

---

## 1. Overview & Security Architecture

To start SSH from an ordinary local user shell, use `sudo -b /sbin/sshd`.
Sudo authenticates in the foreground and starts sshd in its own session, so
leaving an intermediate root shell does not terminate it. `sudo /sbin/sshd`
also works but occupies the foreground while the daemon runs. Appending `&`
to a password-requiring sudo command stops it when it reads the terminal;
`-b` avoids that problem without bypassing authentication. The existing SSH
configuration and readiness gates still apply. `make test-sshd-sudo` exercises
these paths with disposable QEMU fixtures and an OpenSSH key-exchange oracle.

Nano uses the output PTY's negotiated window size over SSH, including subsequent
window changes, rather than the laptop framebuffer size. It polls resize state
and ignores SIGWINCH, and clips its bars to the available columns to avoid
scrolling the editor offscreen. `make test-nano-ssh` checks a real OpenSSH
password-authenticated session at 100×30 and 60×18 under BIOS/UEFI, visible text
placement and saved-file SHA-256. See [nano SSH evidence](../roadmap/nano-ssh-viewport.md).

FortressOS implements an in-tree, freestanding SSHv2 daemon (`/sbin/sshd`) complying with RFC 4251, RFC 4252, RFC 4253, RFC 4254, and RFC 8731. The daemon is engineered around privilege separation, capability drop boundaries, and strict denial-of-service / resource bounds.

```
                      +---------------------------------------+
                      |       Listen Port 22 (TCP)            |
                      +-------------------+-------------------+
                                          |
                                          v
+-----------------------------------------------------------------------------------+
|  PRIVILEGED MONITOR DAEMON (/sbin/sshd, parent process)                           |
|  - Capabilities retained for SYS_HOSTKEY (72) & shadow password verification      |
|  - Reads authoritative /etc/ssh/sshd_config (or /mnt/.fortress/sshd.conf)         |
|  - Enforces global 2-session concurrency limit BEFORE spawning workers            |
|  - Acts as Host-Key Signing Oracle: single 64-byte Ed25519 signature per session   |
|  - Verifies passwords via constant-time dummy timing mitigation (userdb_t)        |
+-------------------+-------------------------------------------+-------------------+
                    |                                           |
       SYS_PIPE (24)| IPC                                       | SYS_PIPE (24) IPC
                    v                                           v
+---------------------------------------+   +---------------------------------------+
| UNPRIVILEGED PROTOCOL WORKER (Slot 0) |   | UNPRIVILEGED PROTOCOL WORKER (Slot 1) |
| - Dropped all capabilities (CAPSET 0) |   | - Dropped all capabilities (CAPSET 0) |
| - Isolated in independent process     |   | - Isolated in independent process     |
| - Curve25519-SHA256 Key Exchange      |   | - Curve25519-SHA256 Key Exchange      |
| - ChaCha20-Poly1305 AEAD Packet Framer|   | - ChaCha20-Poly1305 AEAD Packet Framer|
| - RFC 4254 Channel Engine & Credits   |   | - RFC 4254 Channel Engine & Credits   |
| - PTY Bridge (PTY_SET_SESSION)        |   | - PTY Bridge (PTY_SET_SESSION)        |
+-------------------+-------------------+   +-------------------+-------------------+
                    |                                           |
                    v                                           v
+---------------------------------------+   +---------------------------------------+
| Ring 3 Session Shell (/bin/sh, nano)  |   | Ring 3 Session Shell (/bin/sh, nano)  |
| - Allocated PTY slave device          |   | - Allocated PTY slave device          |
| - Controlling session & process group |   | - Controlling session & process group |
+---------------------------------------+   +---------------------------------------+
```

---

## 2. Configuration Schema (`sshd_config`)

### 2.1 File Location & Path Precedence

The SSH daemon resolves its configuration using the following strict canonical precedence:
1. **Explicit CLI flag (`-f <path>`)**: Highest priority; loads the specified path directly.
2. **Installed Root (`/etc/ssh/sshd_config`)**: Authoritative persistent configuration for installed systems.
3. **Live Media Fallback (`/mnt/.fortress/sshd.conf`)**: Ephemeral configuration used when booting from live/installation media.
4. **No file found**: **SSH is disabled**. The daemon logs a diagnostic to `stderr` and exits cleanly with return code 0.

### 2.2 Directives & Validation Rules

FortressOS utilizes a **strict configuration parser** (`user/sshd/sshd_config.c`). Any unknown directive or out-of-range value immediately causes the daemon to refuse startup, printing a diagnostic in the exact format:
- `[sshd] sshd: unknown directive '<name>' at line <N>`
- `[sshd] sshd: out-of-range value '<val>' for <directive> at line <N>`

| Directive | Default | Allowed Values / Validation Rules | Purpose & Security Semantics |
| :--- | :--- | :--- | :--- |
| `Enabled` | `no` | `yes`, `no`, `true`, `false`, `1`, `0` | Master killswitch. When `no`, daemon exits immediately with code 0 without binding port 22. |
| `Port` | `22` | Integer `1` – `65535` | TCP listening port. |
| `ListenAddress` | `0.0.0.0` | Valid IPv4 dotted-quad string | IPv4 address to bind. Rejects invalid octets, formatting errors, and IPv6. |
| `PermitRootLogin` | `no` | **`no` only** (rejection of `yes`) | Remote root login is strictly forbidden by policy. Supplying `yes` is rejected as an invalid security setting at boot. |
| `PasswordAuthentication` | `yes` | `yes`, `no`, `true`, `false`, `1`, `0` | Controls whether shadow password authentication is permitted. |
| `PubkeyAuthentication` | `yes` | `yes`, `no`, `true`, `false`, `1`, `0` | Controls whether Ed25519 public key authentication (`~/.ssh/authorized_keys`) is permitted. |
| `MaxSessions` | `2` | Integer `1` – `2` | Global concurrent session cap. Rejects values `> 2` to prevent memory exhaustion on freestanding RAM. |
| `MaxAuthTries` | `5` | Integer `1` – `5` | Maximum failed authentication attempts permitted per connection before forced disconnect. |
| `LoginGraceTime` | `60` | Integer `1` – `600` (seconds) | Maximum elapsed time allowed for a client to complete authentication before timeout teardown. |

Comments begin with `#`. Empty lines and leading/trailing whitespace are ignored. Keys and values may be separated by whitespace or `=`.

---

## 3. Boot Readiness Gate Sequence

The privileged monitor executes an explicit, pinned 7-step readiness sequence before accepting connections. A failure at any gate prints a distinct diagnostic to `stderr` and fails closed:

1. **Verify Config Path & Status**:
   - Check precedence path; if disabled (`Enabled no`), print clean diagnostic and exit 0.
2. **Load Host Key via `SYS_HOSTKEY` (72)**:
   - Must load 64-byte Ed25519 host secret. Failure $\rightarrow$ `[sshd] host key unavailable/corrupt` (exit 1).
3. **Load Account Database (`userdb_t`)**:
   - Must load `/etc/passwd` and `/etc/shadow`. Failure $\rightarrow$ `[sshd] account database unavailable` (exit 1).
4. **Strong Randomness Gate via `SYS_GETRANDOM` (77)**:
   - Probe CSPRNG DRBG. Failure or partial read $\rightarrow$ `[sshd] strong randomness unavailable` (exit 1, fails closed, no weak fallback).
5. **Bind Port 22 via `SYS_BIND` (34)**:
   - Bind TCP socket to configured `ListenAddress` and `Port`. Failure $\rightarrow$ `[sshd] bind failed on <addr>:<port>` (exit 1).
6. **Listen on Port 22 via `SYS_LISTEN` (35)**:
   - Set listen backlog. Failure $\rightarrow$ `[sshd] Failed to listen on TCP socket.` (exit 1).
7. **Enter Accept Loop**:
   - Log `[sshd] Listening on port 22 (SSH-2.0-FortressSSH_0.1)`.

---

## 4. Admission Control & Capacity Limits

- **Global Concurrency Cap**: Hard ceiling of **2 concurrent sessions** across all IPs.
- **Enforcement Point**: Checked immediately upon `SYS_ACCEPT` **before** IPC pipe creation, buffer allocation, or `SYS_SPAWN_EXT`.
- **Wire Disconnect**: Connection 3 receives the version banner `SSH-2.0-FortressSSH_0.1\r\n` followed by an unencrypted RFC 4253 `SSH_MSG_DISCONNECT` (reason 11 `SSH_DISCONNECT_BY_APPLICATION`, description `"Too many concurrent sessions"`) and an immediate socket close.

---

## 5. Teardown Sequence (SIGTERM / Reboot)

On receiving `SIGTERM` or `SIGINT`, the monitor executes a bounded 7-step teardown:
1. Signal handler flags `g_terminate = 1`.
2. Immediate closure of the TCP listening socket (`sock_fd`), preventing new connections.
3. Send `SIGTERM` to each active worker PID.
4. Each worker receives `SIGTERM`, breaks out of its duplex poll loop, and invokes `pty_bridge_cleanup()`.
5. `pty_bridge_cleanup()` closes the PTY master descriptor (`master_fd`), which triggers the kernel PTY driver to deliver `SIGHUP` to the session leader's `controlling_sid`.
6. Monitor waits bounded (up to 5000ms / 5s) for workers to exit, polling `SYS_WAITPID(..., WNOHANG)` in 50ms intervals.
7. If any worker process remains alive after 5s, the monitor escalates to `SIGKILL` on the worker PID, reaps it, closes monitor IPC pipes, and exits 0 cleanly with `[sshd] Daemon shut down cleanly.`.

---

## 6. Verification & Test Targets

| Makefile Target | Test Scope | Verification Tooling |
| :--- | :--- | :--- |
| `make test-ssh` | Phase 8 SSH end-to-end multi-firmware acceptance suite under QEMU (`scripts/test_ssh.py`): automated validation across BIOS and UEFI firmware at SMP=1 with authentic Linux OpenSSH client (`ssh`, `ssh-keyscan`). Covers all 8 acceptance gates. | Multi-firmware QEMU + OpenSSH |
| `make test-ssh-transport-host` | RFC 4253 packet framing, ChaCha20-Poly1305 AEAD, Curve25519-SHA256 KEX, strict prefix truncation, live OpenSSH client oracle | ASan/UBSan |
| `make test-ssh-auth-host` | RFC 4252 user authentication, monitor IPC, shadow password constant-time dummy path, Ed25519 pubkey verification, `~/.ssh/authorized_keys` permission checks | ASan/UBSan |
| `make test-ssh-channel-host` | RFC 4254 channel state machine, 64 KiB window credits, `WINDOW_ADJUST`, EOF/close interleavings | ASan/UBSan |
| `make test-ssh-pty-host` | Kernel PTY driver bridge (`SYS_PTY_PAIR` 80), duplex short writes, window resize (`SIGWINCH`), staged session zero-race binding | ASan/UBSan |
| `make test-ssh-daemon-host` | Configuration schema & strict parser, precedence hierarchy, boot readiness gate sequence, global admission cap & disconnect packet, 5s bounded teardown | ASan/UBSan |

---

## 7. Multi-Firmware Acceptance Verification Evidence (Phase 8)

The end-to-end integration test (`make test-ssh` / `scripts/test_ssh.py`) was verified on 2026-10-17 with 100% PASS across BIOS and UEFI firmware on disposable storage with e1000 networking.

### 7.1 Acceptance Matrix & Gate Verification

| Gate | Acceptance Requirement | Test Oracle & Evidence | BIOS Result | UEFI Result |
| :--- | :--- | :--- | :--- | :--- |
| **Gate 1** | Boot Readiness Gate & Service Launch | Pinned 7-step sequence (`sshd_readiness.c`) loaded via live media config `/mnt/.fortress/sshd.conf`. Emits `Listening on port 22 (SSH-2.0-FortressSSH_0.1)`. | **PASS** | **PASS** |
| **Gate 2** | Host Key Fingerprint Match | `ssh-keyscan -t ed25519 -p <port> 127.0.0.1` scanned public key. `ssh-keygen -lf` SHA-256 fingerprint verified byte-for-byte against the provisioned NVMe host key. | **PASS** | **PASS** |
| **Gate 3** | Ed25519 Public Key Authentication | Authentic OpenSSH client key authentication against `~/.ssh/authorized_keys` (0700 dir, 0600 file). Non-interactive command execution `whoami; id; hostname` confirmed `operator`, `uid=1000`, `gid=1000`, groups=[1000, 10, 44, 104]. | **PASS** | **PASS** |
| **Gate 4** | Remote Root Login Rejection | Authentic OpenSSH client connection with `root@127.0.0.1` rejected with `Permission denied` via constant-time dummy password/pubkey verification path. | **PASS** | **PASS** |
| **Gate 5** | Password Authentication | Authentic OpenSSH client authentication via `SSH_ASKPASS` using `operator:ssh-acceptance-pass` verified through `userdb_t` `/etc/shadow` SHA-256 hash. | **PASS** | **PASS** |
| **Gate 6** | Interactive PTY Session & Terminal Control | Interactive pseudoterminal session (`ssh -tt`) with in-kernel PTY driver, line-discipline echo, `/dev/tty` handoff in shell pipeline, command typing, and exit. | **PASS** | **PASS** |
| **Gate 7** | Global 2-Session Concurrency Cap | Two active background sessions opened; 3rd concurrent connection accepted and immediately rejected pre-spawn with unencrypted wire RFC 4253 `SSH_MSG_DISCONNECT` (reason 11 `SSH_DISCONNECT_BY_APPLICATION`). Zero leaked workers or descriptors. | **PASS** | **PASS** |
| **Gate 8** | Clean 5s Bounded Daemon Teardown | `SIGTERM` sent to daemon. Monitor stopped accepting connections, propagated `SIGTERM` to workers, workers triggered `SIGHUP` to controlling sessions via PTY master close, and daemon exited cleanly within 5s with `Daemon shut down cleanly.`. | **PASS** | **PASS** |
