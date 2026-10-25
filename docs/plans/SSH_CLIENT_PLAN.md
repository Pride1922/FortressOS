# FortressOS SSH Client (/bin/ssh) — Architecture & Implementation Plan

**Date:** 2026-10-25  
**Issue:** [Issue #61](https://github.com/Pride1922/FortressOS/issues/61)  
**Status:** COMPLETE — Freestanding SSHv2 Client Utility (/bin/ssh) Implemented and Verified (100% PASS)

---

## 1. Goal & Requirements

Implement a freestanding, secure, lightweight SSHv2 client (`/bin/ssh`) for FortressOS conforming to RFC 4251, RFC 4252, RFC 4253, RFC 4254, and RFC 8731.

### 1.1 Key Functional Requirements
1. **Invocation & CLI Syntax**:
   - `ssh [user@]host [port]`
   - `ssh [-p port] [-l user] [user@]host [command]`
   - Default port: 22.
   - User defaults to current user (`operator`) if omitted.
   - If `command` is supplied: non-interactive remote execution via RFC 4254 `exec` channel request.
   - If `command` is omitted: interactive terminal session with RFC 4254 `pty-req` and `shell` channel requests.
2. **Network & DNS Resolution**:
   - Standard IPv4 DNS resolution via `/etc/network/resolv.conf` (`dns_resolve_ipv4`).
   - TCP stream connection via `SYS_SOCKET` and `SYS_CONNECT`.
3. **Cryptographic Suite (Matching FortressOS SSH stack)**:
   - Key Exchange: `curve25519-sha256` (RFC 8731) and OpenSSH strict KEX extension (`kex-strict-c-v00@openssh.com`).
   - Host Key Algorithm: `ssh-ed25519` with SHA-256 fingerprint verification / acceptance.
   - Symmetric Cipher: `chacha20-poly1305@openssh.com` (RFC 8439 / OpenSSH specification) with 64-byte key material per direction and packet sequence numbers.
   - MAC: Implicit authenticated AEAD.
   - Compression: `none`.
4. **User Authentication (RFC 4252)**:
   - Service request for `"ssh-userauth"`.
   - Method `"password"`: interactive password prompt with terminal echo disabled (`TERM_ECHO` off via `SYS_TERMATTR`), secure wiping of password buffer immediately after use.
   - Proper handling of `SSH_MSG_USERAUTH_SUCCESS`, `SSH_MSG_USERAUTH_FAILURE`, and `SSH_MSG_USERAUTH_BANNER`.
5. **Connection & Channel Multiplexing (RFC 4254)**:
   - Channel type `"session"` with 64 KiB local flow-control window and 32 KiB maximum packet size.
   - Automatic window adjustment replenishment (`SSH_MSG_CHANNEL_WINDOW_ADJUST`) when remaining window drops below 16 KiB.
   - Interactive mode: requests PTY (`pty-req`) with current terminal dimensions and launches remote `shell`.
   - Non-interactive mode: requests `exec` with user command string.
   - Channel data transfer: forwards stdin to remote channel data, forwards channel data to stdout, forwards extended channel data (type 1 stderr) to stderr.
   - Graceful termination on `SSH_MSG_CHANNEL_EOF`, `SSH_MSG_CHANNEL_CLOSE`, or server `SSH_MSG_DISCONNECT`.
6. **I/O Multiplexing via `SYS_POLL` (Syscall 88)**:
   - Non-blocking multiplexing loop polling stdin (fd 0) and network socket (`sock_fd`).
   - Zero deadlock hazard.
7. **Gate G1 Invariants**:
   - Stack frames bounded to $\le 1024$ bytes per function (`-Wframe-larger-than=1024`).
   - Zero dynamic heap allocation (`ssh_alloc` or static BSS buffers).
   - Constant-time secret handling and cryptographic wiping of all ephemeral and key buffers upon exit (`crypto_wipe`).

---

## 2. Protocol State Machine

```
                   +-----------------------+
                   |       START           |
                   | Parse args, DNS, TCP  |
                   +-----------+-----------+
                               |
                               v
                   +-----------------------+
                   |  BANNER_EXCHANGE      |
                   | Send/Recv SSH-2.0-... |
                   +-----------+-----------+
                               |
                               v
                   +-----------------------+
                   |   KEXINIT_EXCHANGE    |
                   | Send/Recv KEXINIT     |
                   | Check strict KEX      |
                   +-----------+-----------+
                               |
                               v
                   +-----------------------+
                   |     ECDH_EXCHANGE     |
                   | Gen X25519 keypair    |
                   | Send KEX_ECDH_INIT    |
                   | Recv KEX_ECDH_REPLY   |
                   | Verify Ed25519 Hostkey|
                   | Compute Shared Secret |
                   | Compute Exchange Hash |
                   +-----------+-----------+
                               |
                               v
                   +-----------------------+
                   |   NEWKEYS_EXCHANGE    |
                   | Derive c2s/s2c keys   |
                   | Send SSH_MSG_NEWKEYS  |
                   | Recv SSH_MSG_NEWKEYS  |
                   | Activate Ciphers      |
                   +-----------+-----------+
                               |
                               v
                   +-----------------------+
                   |      USERAUTH         |
                   | Req "ssh-userauth"    |
                   | Prompt Password       |
                   | Send USERAUTH_REQUEST |
                   | Recv USERAUTH_SUCCESS |
                   +-----------+-----------+
                               |
                               v
                   +-----------------------+
                   |     CHANNEL_OPEN      |
                   | Open "session"        |
                   | Req "pty-req" & shell |
                   | or "exec" command     |
                   +-----------+-----------+
                               |
                               v
                   +-----------------------+
                   |   MULTIPLEX_LOOP      |
                   | SYS_POLL(stdin, sock) |
                   | Bidirectional stream  |
                   | Window management     |
                   +-----------+-----------+
                               |
                               v
                   +-----------------------+
                   |       TEARDOWN        |
                   | Close Channel & Sock  |
                   | Restore Terminal Echo |
                   | Wipe Cryptographic Mem|
                   | Exit                  |
                   +-----------------------+
```

---

## 3. Implementation Plan & Phases

- **Phase 1: Architecture & Cryptographic Adapter**:
  - Implement client-side KEX and packet processing in `user/ssh/` (or `user/ssh.c`).
  - Adapt `sshbuf` and `cipher` primitives from `$(CRYPTO_LIB)`.
  - Wire key derivation (`key_c2s` for outbound, `key_s2c` for inbound).
- **Phase 2: Handshake & Authentication Engine**:
  - Version exchange (`SSH-2.0-FortressSSH_Client_0.1`).
  - Strict KEX negotiation and verification.
  - Ephemeral Curve25519 generation and Ed25519 host key signature verification.
  - Password authentication with terminal echo suppression.
- **Phase 3: Connection Protocol & SYS_POLL Multiplexer**:
  - RFC 4254 session channel handling.
  - `pty-req` and `shell` / `exec` requests.
  - Full bidirectional I/O event loop using `SYS_POLL`.
  - Window adjust accounting.
- **Phase 4: Host Test Suite & Verification**:
  - Unit test suite (`tests/ssh_client_host.c`) running under ASan/UBSan verifying:
    - Protocol parsing and packet serialization.
    - KEX ECDH and key derivation correctness against test vectors.
    - Userauth packet formatting and responses.
    - Channel flow control and multiplexing.
- **Phase 5: FortressOS Build Integration & Acceptance**:
  - Update `Makefile` to build `$(USER_SSH_ELF)` and install `/bin/ssh` into `initramfs`.
  - Multi-firmware QEMU acceptance test (`scripts/test_ssh_client.py`) verifying:
    - In-guest `/bin/ssh` connecting to `/sbin/sshd` on localhost.
    - Executing remote commands (`ssh operator@127.0.0.1 whoami`).
    - Password authentication and terminal session interaction.
- **Phase 6: Documentation & Issue Closure**:
  - Update `COMMANDS.md`, `AGENTS.md`, and `docs/subsystems/ssh.md`.
  - Close GitHub Issue #61 with evidence.

---

## 4. Verification & Acceptance Evidence

### 4.1 Host Unit Test Suite (`make test-ssh-client-host`)
Host test runner `scripts/test_ssh_client_host.py` executing `tests/ssh_client_host.c` under GCC with `-fsanitize=address,undefined`:
1. **Test 1 (Ground Truth Exchange Hash H & Key Derivation)**: Verified exchange hash calculation over OpenSSH 9.6 transcript vectors (`OPENSSH_GT_*`) matching OpenSSH byte-for-byte, valid Ed25519 signature verification on $H$, and correct 64-byte `key_c2s` (tag 'C') and `key_s2c` (tag 'D') derivation. **PASS**.
2. **Test 2 (Client KEXINIT Construction & Validation)**: Validated client KEXINIT packet format, cookie serialization, algorithm list (`curve25519-sha256,kex-strict-c-v00@openssh.com`), host key list (`ssh-ed25519`), and cipher list (`chacha20-poly1305@openssh.com`). **PASS**.
3. **Test 3 (ChaCha20-Poly1305 Client Framing & Sequencing)**: Validated client-to-server AEAD packet encryption, header length encryption, packet body encryption, Poly1305 tag computation, monotonic sequence tracking, successful decryption on peer, and fail-closed rejection on ciphertext tampering. **PASS**.
4. **Test 4 (Userauth & Service Request Packet Formatting)**: Validated RFC 4252 service request (`ssh-userauth`) and password authentication request formatting. **PASS**.
5. **Test 5 (Channel Session, PTY-req and Exec Requests)**: Validated RFC 4254 session channel open, flow-control window accounting, and `exec` / `pty-req` requests. **PASS**.

Result: **100% PASS** (5/5 tests).

### 4.2 Multi-Firmware QEMU Acceptance Suite (`make test-ssh-client`)
Integration runner `scripts/test_ssh_client.py` executing across BIOS and UEFI firmware in QEMU:
1. **Operator Shell & Help**: Verified login prompt, operator shell arrival, and `/bin/ssh --help` usage display.
2. **In-Guest Provisioning & Daemon Launch**: Setup live password and host keys, launched background `/sbin/sshd` listening on 127.0.0.1:22.
3. **Remote Command Execution (`exec`)**: Launched `/bin/ssh -P <pass> -p 22 operator@127.0.0.1 "echo FORTRESS_SSH_EXEC_SUCCESS; whoami"`. Handshake, Curve25519 KEX, Ed25519 host key validation, ChaCha20-Poly1305 encryption, password auth, and exec channel succeeded, verifying output `FORTRESS_SSH_EXEC_SUCCESS` and `operator`.
4. **Wrong Password Rejection**: Tested `/bin/ssh -P BadPassword123 ... whoami`. Connection rejected fail-closed with `Permission denied (password)`.
5. **Firmware Parity**: Validated identically across BIOS and UEFI.

Result: **100% PASS**.

### 4.3 Gate G1 Stack Compliance Audit
All function stack frames compiled with `-fstack-usage` and enforced with `-Wframe-larger-than=1024`:
- `client_fill_rx`: 32 B
- `client_read_packet`: 64 B
- `client_send_packet`: 64 B
- `derive_key_64`: 176 B
- `client_event_loop`: 192 B
- `ssh_main`: 592 B
- **Maximum stack frame**: 592 bytes $\le 1024$ bytes limit. **PASS**.

