# Engineering Plan: Telnet Client (Issue #41)

**Issue**: [#41 - telnet client](https://github.com/Pride1922/FortressOS/issues/41)  
**Goal**: Minimal line-mode telnet client for TCP verification with RFC 854 option negotiation, NVT ASCII formatting, escape character command handling, and non-blocking I/O multiplexing via `SYS_POLL`.

---

## 1. Architectural Overview & State Machine

The FortressOS Telnet client (`/bin/telnet`) provides a standards-compliant, lightweight Network Virtual Terminal (NVT) implementation designed for TCP connectivity, network diagnostics, and verification.

```
                      +-------------------+
                      |   Standard Input  |
                      +-------------------+
                                |
                                v
                   [Escape Character Check]
                     |                  |
           (Normal text)                (Escape `^]`)
                     |                  |
                     v                  v
             [Line Mode NVT]      [Telnet Prompt]
             (\n -> \r\n)         (close, quit, status)
                     |
                     v
             +---------------+
             |  TCP Socket   | <====> [Remote Telnet / TCP Server]
             +---------------+
                     |
                     v
             [Telnet IAC Engine]
             (RFC 854 Negotiator)
              - IAC WILL -> IAC DONT
              - IAC DO   -> IAC WONT
              - IAC SB..SE -> Swallow
              - \r\n / \r\0 -> \n
                     |
                     v
             +-------------------+
             |  Standard Output  |
             +-------------------+
```

### 1.1 Telnet Protocol (RFC 854) Option Negotiation Engine
All incoming bytes from the TCP socket pass through a deterministic state machine:
- `IAC` (`0xFF` / 255): Interpret As Command.
- Command codes:
  - `WILL` (`251` / `0xFB`): Server offers to enable option -> reply with `IAC DONT <opt>` (or `IAC DO 3` for Suppress Go Ahead `TELOPT_SGA`).
  - `WONT` (`252` / `0xFC`): Server refuses option -> acknowledge with `IAC DONT <opt>`.
  - `DO` (`253` / `0xFD`): Server requests client to enable option -> reply with `IAC WONT <opt>` (refuse all subnegotiations/options).
  - `DONT` (`254` / `0xFE`): Server requests client not to enable option -> acknowledge with `IAC WONT <opt>`.
  - `SB` (`250` / `0xFA`): Subnegotiation Begin -> transition to `STATE_SB`, swallow all bytes until `IAC SE` (`240` / `0xF0`).
  - `IAC IAC`: Escaped literal `0xFF` byte passed to terminal.
  - Simple 2-byte commands (`NOP`, `DM`, `BRK`, `IP`, `AO`, `AYT`, `EC`, `EL`, `GA`): Handled/ignored and return to `STATE_NORMAL`.

### 1.2 Network Virtual Terminal (NVT) ASCII Translation
- **Transmit Path (stdin -> socket)**:
  - Local newline `\n` (`0x0A`) is expanded to NVT line ending `\r\n` (`0x0D 0x0A`).
  - Literal `0xFF` byte from input is escaped to `0xFF 0xFF`.
- **Receive Path (socket -> stdout)**:
  - NVT line endings `\r\n` and `\r\0` (CR NUL) are collapsed to local `\n`.

### 1.3 Non-Blocking I/O Multiplexing Loop (`SYS_POLL`)
- Uses POSIX-compatible `poll()` (`SYS_POLL`, syscall 88) across `stdin` (fd 0) and the TCP socket.
- Prevents deadlock hazards when communicating with streaming or interactive peers.
- Detects remote disconnection (`read == 0` on socket), prints `\nConnection closed by foreign host.\n`, and exits cleanly with status 0.

### 1.4 Escape Character & Interactive Command Prompt
- Default escape character is `^]` (`0x1D` / ASCII 29).
- Typing `^]` pauses stream forwarding and presents the `telnet> ` prompt:
  - `close` or `quit`: disconnects TCP socket and terminates `telnet`.
  - `status`: displays connection state, peer IP, port, and active escape character.
  - `help` or `?`: lists available commands.
  - `<Enter>` (empty input): immediately resumes the active session.

---

## 2. Gate G1 Discipline & Invariants

1. **Stack Budget**:
   - Every function's stack frame must remain strictly $\le 1024$ bytes (`-Wframe-larger-than=1024 -fstack-usage`).
   - All I/O buffers (`4096` B) and DNS contexts live in static BSS memory.
2. **Freestanding Runtime**:
   - Zero dynamic heap allocations (`malloc`/`free` omitted).
   - Pure freestanding C11 linking against `user/tools/start.asm`.
3. **Security & Input Validation**:
   - Bounds-checked port range ($1 \le \text{port} \le 65535$).
   - Validated IPv4 address parsing with fallback to `/etc/network/resolv.conf` DNS resolution.

---

## 3. Verification Strategy

### 3.1 Host Test Suite (`tests/telnet_host.c`)
Under ASan/UBSan with mock syscalls:
- **Test 1**: Telnet option negotiation:
  - Server sends `IAC DO 1` (Echo) -> Client replies `IAC WONT 1`.
  - Server sends `IAC WILL 3` (SGA) -> Client replies `IAC DO 3`.
  - Server sends `IAC WILL 24` (Terminal Type) -> Client replies `IAC DONT 24`.
  - Server sends `IAC SB 24 1 IAC SE` -> Client swallows subnegotiation without emitting bytes.
  - Server sends `IAC IAC` -> Client outputs single literal byte `0xFF`.
- **Test 2**: NVT EOL conversions:
  - Stdin `Hello\n` -> Socket `Hello\r\n`.
  - Socket `Response\r\n` -> Stdout `Response\n`.
  - Socket `Response\r\0` -> Stdout `Response\n`.
- **Test 3**: Escape prompt handling:
  - Input `^]quit\n` -> triggers socket close and exits 0.
  - Input `^]status\n` -> prints status and continues session.
- **Test 4**: Argument parsing and port defaults:
  - `telnet 10.0.2.15` defaults to port 23.
  - `telnet 10.0.2.15 8080` uses port 8080.
  - `telnet -h` prints usage.

### 3.2 Multi-Firmware QEMU Acceptance Suite (`scripts/test_telnet.py`)
Under BIOS and UEFI firmware:
- Launch in-guest server (`/bin/httpd -p 8080 -d /etc &` or python host server forwarded through QEMU).
- Run `/bin/telnet 127.0.0.1 8080`:
  - Verify `Trying 127.0.0.1...` and `Connected to 127.0.0.1.`.
  - Send `GET /motd HTTP/1.0\r\n\r\n`, verify response contains `Welcome to FortressOS`.
- Test escape character `^]` and command `quit`.
- Clean shutdown.
