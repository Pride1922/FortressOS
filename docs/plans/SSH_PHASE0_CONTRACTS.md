# SSH Phase 0 — Contracts and Dependency Audit

**Phase 1 implementation override (2026-10-17):** the explicitly authorized
implementation chooses syscall 77, HMAC-SHA-256, zero-only 64-bit flags,
nonblocking bounded instruction retries, and mandatory reseed before every
request. Any source failure erases state and latches `EIO` until reboot.
These decisions supersede the provisional §1.4 SHA-512, continued output after
reseed failure and `GRND_NONBLOCK` proposals below. No shutdown-path change
is made; RAM erasure on power loss or physical source quality is not claimed.
See [Phase 1 contract and evidence](../roadmap/ssh-phase1-random.md).

**Status:** PLANNING ONLY — no implementation, no code changes, no commits.  
**Date:** 2026-10-16  
**Source plan:** `docs/plans/CRYPTO_SSH_PLAN.md` (see §11 for filename discrepancy)  
**Covers:** Randomness, key publication, privilege separation, session ownership,
PTY isolation, crypto library, SSH protocol core, resource ledger, error precedence,
threat model, and dependency order.

---

## Table of Contents

1. [Randomness Contract](#1-randomness-contract)
2. [Key-Publication Contract](#2-key-publication-contract)
3. [Privilege Separation Design](#3-privilege-separation-design)
4. [Remote-Session Ownership and Duplex I/O](#4-remote-session-ownership-and-duplex-io)
5. [PTY Isolation Finding](#5-pty-isolation-finding)
6. [Crypto Library Decision](#6-crypto-library-decision)
7. [SSH Protocol Core Decision](#7-ssh-protocol-core-decision)
8. [Proposed Limits and Resource Ledger](#8-proposed-limits-and-resource-ledger)
9. [Error Precedence](#9-error-precedence)
10. [Threat Model](#10-threat-model)
11. [Dependency Order and Discrepancies](#11-dependency-order-and-discrepancies)

---

## 1. Randomness Contract

### 1.1 What SSH Needs From a Random Source

The SSH daemon and host-key provisioner require strong, unpredictable, non-repeating
random bytes from a **dedicated, bounded, kernel-mediated service** for:

| Use | Entropy required | Notes |
|-----|-----------------|-------|
| Ed25519 private-key generation | 32 bytes | One-time, at provisioning |
| X25519 ephemeral private key per KEX | 32 bytes | Per connection, per rekey |
| ChaCha20-Poly1305 session-IV seed | 32 bytes | Per direction, per NEWKEYS |
| Protocol nonce material | 16–32 bytes | Per connection |
| Random padding length selection | 1 byte | Per packet |

All require genuine CSPRNG-quality output. Hardware absence or failure **must fail
closed** for every one of them. There is no acceptable degraded mode.

### 1.2 Current State

#### TCP boot secret (src/net/net_tcp.c)

```c
static uint8_t boot_secret[32];
static bool isn_crypto_guaranteed = false;
```

`boot_secret` is derived once at `net_tcp_init()` from:
- Four 64-bit RDSEED/RDRAND words (64-retry loops each; RDSEED preferred)
- TSC sample, APIC ticks, local IP, MAC address, CMOS RTC registers (6 bytes)

SHA-256 is applied to the pool to produce `boot_secret[32]`.  
`isn_crypto_guaranteed` is `true` only if all four hardware-random words succeeded.
AGENTS.md explicitly notes: *"hardware entropy assurance is conditional."*

**The boot secret is NOT a CSPRNG.** It is a one-time initialization secret used
only for RFC 6528 ISN keying. It is not reseeded. Its MAC/IP/TSC inputs are
observable or predictable under adversarial conditions if RDRAND/RDSEED fail.

#### SYS_INSTALLSEED (syscall 73, src/kernel/accounts.c)

```c
int64_t accounts_seed(uintptr_t address, size_t length, const creds_t *actor) {
    if (!length || length > 256) return SYSCALL_EINVAL;
    if (!actor || actor->euid || !(actor->cap_effective & CAP_SYS_ADMIN))
        return SYSCALL_EPERM;
    // RDSEED (preferred) > RDRAND fallback, 64 retries each, 8 bytes per slot
    // Returns SYSCALL_EIO if no hw entropy for any slot; wipes scratch on exit
}
```

`SYS_INSTALLSEED` is a **privileged, bounded, one-call** hardware-entropy delivery
mechanism for the installer path. It requires `euid == 0` and `CAP_SYS_ADMIN`.
Maximum output is 256 bytes per call. No DRBG conditioning. Not a general service.

**No general user-space CSPRNG API exists in the current checkout.**

### 1.3 What Must Not Happen

The following fallbacks are **explicitly prohibited** for SSH randomness, with no
exception path:

- Reusing `boot_secret` or any TCP ISN material
- Falling back to timestamp (TSC, APIC ticks, CMOS RTC) alone
- Falling back to MAC address or IP address
- Falling back to password hashes or account material
- Silently degrading to a weaker source when RDRAND/RDSEED are absent
- Broadening `SYS_INSTALLSEED` into a general-purpose interface
- Altering TCP ISN policy or rekey logic in any way

### 1.4 Proposed Randomness Service Design

**Name (proposed):** `SYS_GETRANDOM` (syscall number TBD — first available after 76
appears to be 77; allocation requires authorization before Phase 1 implementation).

**Architecture:** A new bounded kernel-side CSPRNG with an explicit readiness gate.

**1. Hardware entropy collection** — identical discipline to `accounts_seed()`:
RDSEED preferred, RDRAND fallback, 64 retries per word, `EIO` if any word
fails. Minimum viable seed: 256 bits of hardware entropy (32 bytes / 4 words).

**2. DRBG** — HASH-DRBG (SHA-512) recommended over CTR-DRBG (AES-256), because
SHA-512 is already required by Monocypher for Ed25519 and avoids adding a kernel-side
AES dependency. Design parameters:
- Seeded at kernel init from hardware entropy, before any user process starts.
- Reseed every 2^16 output blocks (NIST SP 800-90A).
- Reseed source: additional RDSEED/RDRAND if available; on failure, output continues
  within the NIST-allowed window, but the event is logged to dmesg.
- Internal state lives in kernel BSS with no user-addressable alias; inaccessible
  to callers.

**3. Readiness flag** — a kernel-internal predicate `random_ready()` (no syscall
overhead). Until true:
- SSH key generation is refused.
- SSH handshake ephemeral key generation is refused.
- Both terminate with a distinct error code, not a hang.

**4. Output API (proposed):**

```
SYS_GETRANDOM(void *buf, size_t len, uint32_t flags) → ssize_t
```

- `len` capped at 256 bytes per call; callers loop if needed.
- `flags`: `0` = block until ready (bounded wait, explicit timeout error on
  failure); `GRND_NONBLOCK` = return `EAGAIN` if not yet ready.
- Returns bytes written, or `EAGAIN`, `EIO` (hardware failed and DRBG
  uninitialized), or `EINVAL` (bad buf/len).
- User range validated before touching DRBG state.

**5. Privilege model:** `SYS_GETRANDOM` is available to **all processes** — no
capability required. The DRBG state is kernel-resident; no per-process state exposed.

**6. Cross-CPU correctness:** DRBG state is protected by a dedicated Rank-1
spinlock, separate from all network and process locks. All CPUs may call
`SYS_GETRANDOM`; the kernel serializes access. No per-CPU DRBG fork.

**7. Process spawn and restart:** Spawned processes inherit no DRBG state — they
call `SYS_GETRANDOM` into the shared kernel service. Monitor restart re-enters the
same service; no seed material is carried through user space.

**8. Exhaustion:** HASH-DRBG is not exhaustible in the output sense. The only failure
modes are: hardware failure before first seed (fatal, returns `EIO`); or reseed
failure (logged, output continues within NIST window, then `EIO` past reseed limit).

**9. Temporary seeds (test-only):** Test images may inject a deterministic seed via
a privileged build-time path. This path is `#ifdef TEST_RANDOM` and absent from
release media. No test-seed path exists in the DRBG hot path.

**10. Wipe discipline:** Temporary entropy pool buffers are zeroed immediately after
the SHA-512 step that derives the DRBG seed. The DRBG internal state is zeroed on
kernel shutdown (power-down path).

> [!IMPORTANT]
> `SYS_GETRANDOM` must be designed and gated (Phase 1) **before** any SSH key
> generation or KEX code is written. Phase 2 (crypto library) may proceed in
> parallel with Phase 1, but Phase 3 (host provisioning) and Phase 4 (transport)
> have a hard dependency on Phase 1 completion.

---

## 2. Key-Publication Contract

### 2.1 Host Key Location and Permissions

```
/etc/ssh/ssh_host_ed25519_key         root:root mode 0600
/etc/ssh/ssh_host_ed25519_key.pub     root:root mode 0644
```

The directory `/etc/ssh/` must be:
- Owned by root (uid 0, gid 0), mode 0755 or 0711
- Not writable by any other user
- Validated before any key file is created or read

These constraints must be enforced by examining the **opened object**, not by a
separate path-based stat/chmod sequence. See §2.4 for the validation protocol.

### 2.2 Key File Format

OpenSSH unencrypted Ed25519 private key format (`-----BEGIN OPENSSH PRIVATE KEY-----`
envelope), with:
- Magic: `openssh-key-v1\0`
- cipher: `none`, kdf: `none`
- 1 key pair
- Public key: 4-byte length + `ssh-ed25519` name + 4-byte length + 32-byte public key
- Private section: 32-byte check bytes (two identical copies), 64-byte private scalar
  || 32-byte public key, optional comment

The public key file uses the standard one-line format:
```
ssh-ed25519 <base64> fortress-host-key
```

**Unsupported variants** (bcrypt KDF, multi-key, passphrase, other key types) must
be detected and rejected by the reader — not silently parsed in a degraded mode.

### 2.3 Safe No-Replace Secret-File Creation

The `ssh-keygen` provisioner (Phase 3) must use a **write-then-rename** protocol,
never overwriting an existing key in place:

1. Generate the complete key pair entirely in memory.
2. Open a temporary file in `/etc/ssh/` with `O_CREAT | O_EXCL | O_WRONLY`,
   mode 0600, using a randomized temporary name (e.g., `.ssh_host_key_tmp_XXXXXX`).
3. Write the complete formatted private key in a bounded loop that tracks written
   bytes and fails explicitly on any short write. See §2.5.
4. Call `SYS_SYNC` (syscall 14) — global flush — to push data to the block device.
5. Rename the temporary file to the final name via `SYS_RENAME` (syscall 13).
6. Call `SYS_SYNC` again to make the directory entry durable.
7. Wipe the in-memory key material immediately.
8. On any failure: unlink the temporary file and report failure. Do not mark SSH
   as provisioned.

**Do not overwrite an existing valid key.** If the final name already exists:
- Read and validate it (§2.6).
- If valid: leave in place; idempotent success.
- If corrupt/invalid: disable SSH with a diagnosis; do NOT silently replace.
  Operator intervention is required. See §2.7.

### 2.4 Directory and File Validation

Before creating or reading any key file, validate by examining the **opened object**,
not by a separate path-based re-lookup:

```
open("/etc/ssh/", O_RDONLY | O_DIRECTORY) → dir_fd
fstat(dir_fd) → check uid=0, gid=0, mode & 0022 == 0

open("/etc/ssh/ssh_host_ed25519_key", O_RDONLY) → key_fd
fstat(key_fd) → check:
    st_uid == 0, st_gid == 0
    st_mode & 0177 == 0600  (no group/world bits)
    st_type == regular file (not symlink, not device, not directory)
    st_size in [expected_min, expected_max]
```

**Discrepancy D4:** The current VFS/EXT4 ABI has only `SYS_STAT` (by path, syscall 5)
and `SYS_STAT_EXT` (syscall 57). Neither operates on a retained file descriptor.
A path-based re-lookup is vulnerable to race/symlink/rename attacks.

**Resolution required before Phase 3:** Add `SYS_FSTAT` (stat by fd). This is a
small, bounded syscall with no new capability or lock requirements. Authorization
must precede Phase 3 implementation. Path re-lookup is not an acceptable substitute.

### 2.5 Full-Write and Short-Write Handling

`SYS_WRITE` may return a short count. The provisioner must:
- Loop until all bytes are written or an error occurs.
- On any error mid-write: set an error flag, unlink the temp file, do not mark SSH
  as provisioned.
- `ENOSPC` and `EIO` are permanent failures; do not retry beyond one re-attempt.
- Never mark SSH provisioned if the write loop did not confirm all bytes written.

### 2.6 Durability Barriers

`SYS_SYNC` (syscall 14) is a global sync — it flushes all dirty EXT4 journal and
data buffers. There is no `SYS_FSYNC` (per-fd) in the current ABI.

**Discrepancy D2:** The plan states "supported durability barriers" without naming
which syscall. `SYS_SYNC` is confirmed available. `SYS_FSYNC` does not exist.
The global sync is over-broad (flushes all dirty state) but not incorrect for this
use case. Two `SYS_SYNC` calls are required:

```
write(tmp_fd, key_data, len)   ← all bytes confirmed
SYS_SYNC                       ← data on disk before rename
SYS_RENAME(tmp, final)         ← atomic name swap
SYS_SYNC                       ← directory entry on disk
```

### 2.7 Failure Modes

| Failure | Action |
|---------|--------|
| `/etc/ssh/` missing or wrong permissions | Create directory; fail if cannot set permissions |
| Temporary file creation fails (ENOSPC, EMFILE) | Report; leave SSH disabled |
| Short write or write error | Unlink temp file; leave SSH disabled |
| `SYS_SYNC` fails | Unlink temp file; leave SSH disabled |
| Rename fails | Unlink temp file; leave SSH disabled |
| Existing key is valid | Leave in place; idempotent success |
| Existing key is corrupt/malformed | **Log diagnosis to dmesg; disable SSH; do NOT rotate** |
| Existing key has wrong permissions | Log; disable SSH (operator must fix) |
| Partial key (priv exists, pub missing or mismatched) | **Disable SSH; log both findings** |

No partial key is trusted by sshd. The daemon validates the key pair at startup
before the TCP listener binds. See §2.8.

### 2.8 Key Pair Validation Protocol

Before sshd marks SSH as provisioned and opens the listener:

1. Open `/etc/ssh/ssh_host_ed25519_key` → `priv_fd`; validate by fd (§2.4).
2. Read entire private key file into a bounded buffer (max 1 KiB).
3. Parse the OpenSSH format; extract the 64-byte Ed25519 private scalar and
   embedded 32-byte public key. Fail on any parse error.
4. Open `/etc/ssh/ssh_host_ed25519_key.pub` → `pub_fd`; validate by fd.
5. Parse the public key file; extract the 32-byte public key.
6. Compare the embedded public key (step 3) to the standalone public key (step 5).
   If mismatch: disable SSH; log the mismatch; close both fds.
7. Perform one Ed25519 sign-and-verify self-test with the loaded key pair.
8. Wipe the private key from the parse buffer immediately after loading it into
   the in-memory monitor state. The parse buffer (stack-allocated) must be zeroed.

---

## 3. Privilege Separation Design

### 3.1 Monitor vs. Worker — Role Assignment

```
┌─────────────────────────────────────────────────────┐
│  SSH Monitor (privileged, long-lived)               │
│  - Holds host private key in memory (after §2.8)   │
│  - Reads shadow data for password auth              │
│  - Performs Ed25519 host signatures                 │
│  - Checks password hash via db_verify()             │
│  - Spawns worker processes per incoming connection  │
│  - Owns the TCP listener socket fd                  │
│  - Owns the accepted connection fd (temporarily)    │
│  - Terminates or resets misbehaving workers         │
└─────────────────────────────────────────────────────┘
              │ pipe (bounded protocol)
              ↓
┌─────────────────────────────────────────────────────┐
│  SSH Worker (restricted, per-connection)            │
│  - Handles unauthenticated SSH parsing/transport    │
│  - Owns the TCP socket fd (transferred from monitor)│
│  - Sends typed, length-prefixed requests to monitor │
│  - Receives typed, bounded responses from monitor   │
│  - Terminated by monitor on deadline expiry         │
└─────────────────────────────────────────────────────┘
```

### 3.2 No Fork — Spawn Protocol

There is no `fork()` in FortressOS. Privilege separation uses `SYS_SPAWN_EXT`
(syscall 20) with `spawn_opts_t` fd_actions to pass retained descriptors.

**Monitor spawns each worker as follows:**

```c
spawn_fd_action_t actions[] = {
    /* fd 0  = accepted TCP socket (stdin) */
    { .type = SPAWN_FD_DUP2, .from_fd = conn_socket_fd, .to_fd = 0 },
    /* fd 3  = worker reads monitor responses */
    { .type = SPAWN_FD_DUP2, .from_fd = monitor_read_pipe, .to_fd = 3 },
    /* fd 4  = worker writes requests to monitor */
    { .type = SPAWN_FD_DUP2, .from_fd = worker_write_pipe, .to_fd = 4 },
    /* All other fds: CLOEXEC-marked at open time, not transferred */
};
spawn_opts_t opts = {
    .version = 1,
    .flags = SPAWN_SETPGROUP,
    .fd_actions = actions,
    .action_count = N,
};
SYS_SPAWN_EXT("/sbin/sshd-worker", &opts, sizeof(opts));
```

The monitor closes its copy of the connection socket immediately after spawn.
The worker's host-key and shadow-data descriptors are CLOEXEC-marked before spawn.

**Pipe creation:**
```c
SYS_PIPE(pipefd, 0);   /* monitor_to_worker: worker reads fd[0], monitor writes fd[1] */
SYS_PIPE(pipefd, 0);   /* worker_to_monitor: monitor reads fd[0], worker writes fd[1] */
```

Pipe ends not transferred to the worker are CLOEXEC-marked via `SYS_FCNTL`.

### 3.3 Bounded Monitor/Worker Protocol

The pipe protocol is **typed, length-prefixed, and bounded**. All messages:

```c
uint8_t  msg_type;      /* one of the enum values below */
uint8_t  session_id;    /* monitor-assigned; bounds-checked on every message */
uint16_t payload_len;   /* bounded: 0..MAX_MSG_PAYLOAD (256 bytes) */
uint8_t  payload[payload_len];
```

Maximum message size: 4 + 256 = 260 bytes.

**Worker-to-monitor request types:**

| Type | Payload | Purpose |
|------|---------|---------|
| `REQ_HOST_SIGN` | 32-byte session ID | Request Ed25519 signature over SSH host key proof |
| `REQ_AUTH_PASSWORD` | bounded username + bounded password | Verify password against shadow |
| `REQ_AUTH_PUBKEY` | username + key blob | Check if key is authorized |
| `REQ_SESSION_ACCEPT` | uid, gid, username | Commit authenticated session |
| `REQ_WORKER_DONE` | exit code | Worker signals clean teardown |

**Monitor-to-worker response types:**

| Type | Payload | Purpose |
|------|---------|---------|
| `RESP_HOST_SIG` | 64-byte Ed25519 signature | Result of host signing |
| `RESP_AUTH_OK` | uid, gid, gids[], ngroups | Auth success with identity |
| `RESP_AUTH_FAIL` | attempts remaining | Auth failure |
| `RESP_AUTH_DENY` | — | Terminal denial (5 attempts exceeded) |
| `RESP_ERROR` | error code | Monitor-side failure |

**Monitor validation rules (enforced on every message):**
- `session_id` must match the monitor's record for this pipe pair.
- `payload_len` must not exceed `MAX_MSG_PAYLOAD`.
- Operation ordering enforced: `REQ_HOST_SIGN` only before NEWKEYS;
  `REQ_AUTH_*` only after NEWKEYS; `REQ_SESSION_ACCEPT` only after auth success.
- Any out-of-order or malformed message: close pipe, send SIGTERM to worker, wait
  for worker exit (bounded deadline), log incident.

**Monitor host-signing oracle restrictions:**
- Signs only the specific SSH host key proof structure (RFC 4253 §8).
- Does not expose private key bytes.
- Does not sign arbitrary data.
- Returns only the 64-byte signature.

### 3.4 Credential Management for Workers

Workers run under a dedicated service account:

```
username: sshd-worker
uid:      assigned during SSH provisioning (>= 1000)
gid:      dedicated group
shell:    none (non-login)
caps:     all dropped before worker executes
```

This account is created during host provisioning (`/sbin/ssh-keygen`). The worker
has no capabilities: no `CAP_SYS_ADMIN`, no `CAP_NET_BIND`, no `CAP_SYS_RAWIO`,
no `CAP_FOWNER`. euid, uid, suid all set to the service account UID.

After authentication succeeds, the monitor spawns a **shell process** with the
authenticated user's credentials following the full `CREDENTIAL_SNAPSHOT_PROTOCOL.md`
protocol. The worker itself never executes as the authenticated user.

### 3.5 Sandbox Audit

**Finding:** There is no chroot, namespace, seccomp, or capability-based sandbox API
beyond the existing capability-drop mechanism (`SYS_CAPSET`, `creds_capset()`).
`creds_capset()` is drop-only — capabilities cannot be re-acquired after dropping.
Workers start with zero capabilities via spawn credential derivation
(`creds_inherit()` never replenishes capabilities, even for UID 0).

**Assessment (Discrepancy D7):** Service-account identity separation plus all-cap-drop
is the only isolation mechanism. This is weaker than a Linux seccomp sandbox but is
the current system's maximum available isolation. Documented and accepted for Phase 5;
the threat model (§10) reflects it.

### 3.6 Descriptor Discipline

All monitor file descriptors are opened with `O_CLOEXEC` where applicable. Before
spawning a worker, the monitor explicitly enumerates transferred fds in `fd_actions`;
all others are CLOEXEC-marked or explicitly closed.

`MAX_PROCESS_FDS` = 32. The monitor uses at most:
- 1 listener fd
- 4 pipe fds at steady state (2 connections × 2 pipes each, for the monitor-side ends)
- 2 transient accepted-socket fds (closed immediately after spawn)
- 1 host key fd (closed after §2.8 validation)
- Shadow file fd (opened/closed per auth request)
- Total active: ≤ 10 fds at peak. Well within the 32-fd limit.

---

## 4. Remote-Session Ownership and Duplex I/O

### 4.1 Session Ownership Model

Each SSH connection is owned by a **monitor/worker pair**:
- The monitor owns the listener socket (persistent across all connections).
- The monitor holds the accepted socket temporarily during spawn; transfers it to
  the worker via fd_actions and closes its copy immediately.
- The worker owns the accepted TCP socket for the connection's lifetime.
- After authentication, the worker owns the PTY master fd (Phase 6 prerequisite;
  see §5).
- The authenticated shell process owns its stdin/stdout/stderr (transferred via
  spawn fd_actions from the worker).

No two processes share a socket or PTY in the same role.

### 4.2 Duplex I/O Problem

SSH requires simultaneous:
- **Inbound (network → shell):** Read TCP → decrypt → parse SSH packet → write to
  shell stdin (PTY master or pipe).
- **Outbound (shell → network):** Read shell stdout (PTY master or pipe) → SSH
  packet → encrypt → write TCP.
- **Control (network → session):** Channel window updates, rekey requests,
  disconnect, keepalive — must not be starved by data I/O in either direction.

The current network ABI has:
- `SYS_RECV_UNTIL` / `SYS_SEND_UNTIL` — absolute-deadline blocking I/O.
- `SYS_NANOSLEEP` — absolute-deadline sleep.
- **No `poll()`, no `select()`, no non-blocking socket flag** (Discrepancy D6).
- Pipes: blocking reads/writes; no non-blocking pipe mode confirmed.

A single-threaded worker cannot simultaneously block on both the TCP socket and the
PTY/pipe without a multiplexing primitive.

### 4.3 Duplex I/O Resolution — Selected Option B

**Selected: Option B — Explicit I/O Processes with Retained fd Ownership.**

The worker spawns two child processes after the authenticated session begins:

```
SSH Worker (protocol engine)
├── Network-RX process  (reads TCP socket → writes to inbound_pipe[1])
└── Network-TX process  (reads outbound_pipe[0] → writes TCP socket)
```

The protocol engine:
- Reads decrypted data from `inbound_pipe[0]` with deadline.
- Processes SSH packets (channel data, window adjust, rekey, keepalive).
- Writes formatted/encrypted data to `outbound_pipe[1]`.
- Manages SSH window credits: does not pass data to PTY if remote window exhausted.

The **PTY bridge** (Phase 6) adds two more child processes:

```
PTY-RX process: reads PTY master → writes outbound_pipe[1]
PTY-TX process: reads inbound_pipe[0] → writes PTY master
```

Each segment can block independently without starving any other segment.

**Backpressure proof:**
- Blocking TCP read (net-rx) cannot prevent draining shell output (PTY-RX reads
  PTY master independently).
- Blocking PTY write (PTY-TX) blocks only its own inbound_pipe drain.
- SSH window credit management provides natural backpressure to the shell:
  when the remote window is zero, PTY-TX stops consuming inbound_pipe.

**Resource cost per session:** 4 processes (worker + net-rx + net-tx + shell);
at 2 simultaneous connections: 8 additional process slots. See §8.5.

**Pipe-only command bridge (Phase 4 intermediate):** Before PTY is implemented,
Phase 4 transport tests use a pipe-only command bridge (no PTY, no interactive
terminal) to validate the protocol engine. This is an explicit intermediate test
fixture, not the production design.

### 4.4 Absolute-Deadline I/O

All SSH process I/O uses absolute-deadline syscalls:
- `SYS_RECV_UNTIL(fd, buf, len, deadline_ticks)` — TCP reads
- `SYS_SEND_UNTIL(fd, buf, len, deadline_ticks)` — TCP writes
- `SYS_NANOSLEEP` — sleep without busy-waiting

All deadlines use monotonic BSP APIC ticks. A helper converts wall-clock durations
(e.g., "10 seconds from now") to an absolute tick value once per operation.

**Per-phase deadlines:**
- KEX deadline: 10 s from TCP accept to NEWKEYS
- Auth deadline: 60 s from NEWKEYS to `SSH_MSG_USERAUTH_SUCCESS`
- Rekey triggered by byte/packet/time thresholds (§8.6)

Any expired deadline causes the worker to send `SSH_MSG_DISCONNECT
(SSH_DISCONNECT_BY_APPLICATION)` and exit. The monitor reaps the worker within
the bounded teardown deadline (§8.6).

---

## 5. PTY Isolation Finding

### 5.1 Current Terminal State

**The current checkout has no PTY implementation.**

The terminal subsystem is:
- A single global `/dev/tty` VFS node (type `VFS_STREAM`), registered at boot via
  `devfs_init(root, tty, null_node)`.
- `SYS_TERMCTL` (syscall 16): per-process output endpoint; modes MIRROR/LOCAL/SERIAL/PLAIN.
- `SYS_TERMATTR` (syscall 34): shared input settings (ISIG flag, vintr, vsusp).
- `SYS_INPUT_READ` (syscall 17): blocking raw non-echoed byte read.
  - **Documented in `src/drivers/input.h`:** "BSP-only single interactive consumer;
    all other CPUs rejected."

Search results:
- `rg --files src/ | grep -i pty` → **zero results**
- `rg --files src/ | grep -i tty` → **zero results** (excluding the `/dev/tty` node)

No PTY master/slave abstraction exists. No per-session terminal attribute state
exists. The foreground process group via `SYS_TCSETPGRP`/`SYS_TCGETPGRP` (syscalls
30/31) operates only on the single global terminal.

### 5.2 What Is Needed for SSH Interactive Sessions

Phase 6 (PTYs, job control, session channels) requires, per remote session:
- An independently owned PTY pair — separate from framebuffer/UART terminal and
  all other SSH sessions.
- Per-session echo/canonical/raw mode (independent terminal attribute state).
- Per-session foreground process group (independent SIGINT/SIGTSTP delivery).
- Per-session terminal size (for SIGWINCH propagation).
- Per-session signal delivery from remote client keys (Ctrl+C → SIGINT to the
  session's foreground group only, not the local console or other sessions).

None of these properties are provided by the current terminal layer.

### 5.3 Required New Abstractions

A PTY subsystem must be designed and implemented as a **Phase 6 prerequisite**.
Minimum interface elements:

```c
/* Create a new PTY pair; returns master fd and slave fd */
int64_t pty_open(int *master_fd, int *slave_fd);

/* Set terminal attributes on a PTY slave (echo/canonical/raw) */
int64_t pty_setattr(int fd, const terminal_attrs_t *attrs);

/* Set window size on PTY slave (triggers SIGWINCH on foreground group) */
int64_t pty_resize(int fd, uint16_t rows, uint16_t cols);

/* PTY read/write (deadline-driven, returns EIO when slave/master closed) */
```

The slave fd is passed to the shell via spawn fd_actions (stdin/stdout/stderr all
set to the slave fd). The master fd is held by the PTY-RX/PTY-TX bridge processes.

**Interaction with existing terminal:** The existing `/dev/tty` and `SYS_INPUT_READ`
remain the local-console path. SSH sessions use PTY masters/slaves exclusively;
they never write to or read from `/dev/tty`. `SYS_TCSETPGRP`/`SYS_TCGETPGRP`
must be extended to support PTY slave fds.

**Multi-CPU correctness (new constraint):** PTY I/O must work from any CPU.
Unlike `SYS_INPUT_READ` (BSP-only), PTY reads/writes must not carry the BSP-only
restriction. This is a new kernel contract not present in the current input subsystem.

### 5.4 Impact Assessment

PTY isolation is a **MAJOR GAP** requiring new kernel implementation before SSH
interactive sessions can be accepted. Affected phases:
- Phase 6 (PTYs, job control) — directly dependent
- Phase 7 (daemon lifecycle) — depends on Phase 6
- Phase 8 (physical acceptance) — depends on Phase 6

PTY design should not begin until the monitor/worker protocol (Phase 5) is stable,
because the PTY pair ownership flows through the worker spawn sequence.

---

## 6. Crypto Library Decision

Phase 2 source-audit correction (2026-10-17): the candidate table below is
historical planning, not implemented API evidence. Monocypher 4.0.3 raw X25519
does not reject zero DH output; its cofactored verifier permits noncanonical
point encodings. FortressOS adds those rejection policies in wrappers. The
existing digest API is `digest_init/update/final`, not `sha256_hash`. Current
stack/erasure findings and the exact pinned identity are recorded in
[the crypto annex](../subsystems/crypto.md); no independent upstream audit by
FortressOS is claimed.

### 6.1 Required Primitives

The SSH protocol profile (`CRYPTO_SSH_PLAN.md §3`) requires:

| Primitive | Algorithm | RFC |
|-----------|-----------|-----|
| KEX | X25519 ECDH | RFC 7748 |
| Key exchange hash | SHA-256 | RFC 6668 |
| Host signature | Ed25519 sign + verify | RFC 8032 |
| Ed25519 internal hash | SHA-512 | RFC 8032 |
| Session cipher + MAC | ChaCha20-Poly1305 AEAD | RFC 8439 |
| Length encryption | ChaCha20 (separate key) | OpenSSH chacha20 spec |
| HMAC for misc | HMAC-SHA-256 | RFC 4231 |

SHA-512 is required internally for Ed25519. Any library omitting SHA-512 is
insufficient.

### 6.2 Candidate Evaluation

#### Candidate A: TweetNaCl

| Criterion | Assessment |
|-----------|-----------|
| Size | ~660 lines (`tweetnacl.c` + header) |
| License | Public domain |
| Primitives | Ed25519, X25519, ChaCha20-Poly1305, SHA-512, SHA-256 |
| HMAC | Not included — must add |
| Freestanding C | Yes — no stdlib dependencies |
| Maintenance | Minimal; last significant update ~2014 |
| Review history | Academic origin (Bernstein et al.); no ongoing security audits |
| Stack usage | Moderate; no heap |
| Negative-vector behavior | No explicit low-order point rejection in base code |
| Constant-time | Designed constant-time; no formal proof |

**Concerns:** Maintenance stagnant since ~2014. HMAC-SHA-256 absent. Low-order
point rejection absent (callers must check). Very small size comes with shallow
review relative to the cryptographic stakes.

#### Candidate B: Monocypher

| Criterion | Assessment |
|-----------|-----------|
| Size | ~4000 lines (all primitives) |
| License | BSD 2-Clause + public domain notice |
| Primitives | Ed25519, X25519, ChaCha20-Poly1305, BLAKE2b, SHA-512 |
| HMAC | `crypto_hmac_sha512` native; SHA-256 HMAC via small glue |
| Freestanding C | Yes — no stdlib dependencies |
| Maintenance | Active; version 4.x (2023–2024) |
| Review history | INRIA involvement; multiple independent audits; ANSSI/Mozilla sponsorship; formal-methods review of Poly1305 |
| Stack usage | Bounded; explicit `crypto_wipe()` discipline |
| Negative-vector behavior | Explicit low-order point check (X25519); canonical-form checks (Ed25519) |
| Constant-time | All secret-dependent operations constant-time; Valgrind-tested |

**Concerns:** Larger than TweetNaCl. HMAC-SHA-256 requires a small glue wrapper
(~50 lines using existing `digest.h` SHA-256 — must not modify password/checksum
formats). SHA-512 is native.

#### Candidate C: BearSSL

**Excluded** — ~50k LOC, TLS-focused architecture, reduced maintenance since ~2021.
Overkill for the required primitive set.

#### Candidate D: micro-ecc

**Excluded** — NIST curves only; no Ed25519, no ChaCha20.

#### Candidate E: From Scratch

**Excluded** — `CRYPTO_SSH_PLAN.md §3` explicitly states: *"Prefer a small,
reviewed portable upstream implementation over newly invented curve arithmetic."*
From-scratch has zero independent review; curve arithmetic bugs are silent and
catastrophic.

### 6.3 Recommendation: Monocypher

**Recommendation: Port Monocypher (version 4.x, exact commit pinned at Phase 2 start).**

**Rationale:**
1. **Complete primitive set** — Ed25519 (with SHA-512), X25519, ChaCha20-Poly1305,
   BLAKE2b; all SSH-required primitives covered.
2. **Freestanding C** — no stdlib, no OS calls, no dynamic allocation.
3. **Active maintenance and review** — INRIA, ANSSI/Mozilla, multiple audits.
4. **Explicit hardening** — low-order point rejection, canonical-form checks, wipe
   discipline built in.
5. **Acceptable size** — 4000 lines is small relative to the SSH transport code
   that surrounds it.

**What must be added alongside Monocypher:**
- **HMAC-SHA-256:** Small glue layer using `digest.h`'s existing `sha256_hash()`
  plus the HMAC construction (2 × SHA-256 passes; ~50 lines). Must not modify
  `digest.h` or `root_digest.c` in ways that affect existing password/checksum formats.

**License and provenance obligations:**
- BSD 2-Clause license requires preserving the copyright notice.
- Exact upstream commit hash recorded in `src/lib/crypto/README.md`.
- FortressOS does not independently audit Monocypher. This must be stated explicitly
  in any documentation. "Well-reviewed upstream" ≠ "independently audited by FortressOS."

**Placement:** `src/lib/crypto/` with the public freestanding header at
`src/include/crypto.h` (per `CRYPTO_SSH_PLAN.md §5`).

---

## 7. SSH Protocol Core Decision

### 7.1 Candidates

#### Candidate A: Dropbear (~100k LOC)

POSIX dependencies: `fork()`, `select()`/`poll()`, `openpty()`/`login_tty()`,
`getpwnam()`, `pam_*`, `syslog()`, `setenv()`, `getenv()`, dynamic linking,
pervasive `malloc`/`free`. Adaptation would replace nearly every I/O and process
management call. The result would bear little resemblance to the reviewed codebase.

**Excluded** — POSIX dependencies too deep for a freestanding port to be meaningful.

#### Candidate B: libssh (~80k LOC)

Client+server design; even heavier POSIX dependencies than Dropbear; TLS backends.

**Excluded** — too large, too POSIX-dependent.

#### Candidate C: wolfSSH (~30k LOC)

GPL v2 license (commercial license required for non-GPL use). wolfSSL dependency
adds a second ported library.

**Excluded** — license incompatible with FortressOS source model without a commercial
license.

#### Candidate D: From Scratch — Minimal SSH-2 Subset

The required subset is narrow and well-specified:
- RFC 4253 transport: identification, KEXINIT, KEX, NEWKEYS, encrypted packet records
- RFC 8731 curve25519-sha256 KEX only
- RFC 4252 userauth: `publickey` and `password` methods only
- RFC 4254 connection: one `session` channel per connection, `pty-req`, `shell`,
  `exec` (bounded), `window-adjust`, `data`, `eof`, `close`
- Strict KEX extension (OpenSSH)

Estimated implementation size: ~3000–5000 lines. No POSIX surface; designed for
FortressOS ABIs from the first line.

### 7.2 Recommendation: From Scratch

**Recommendation: Implement the SSH-2 protocol core from scratch.**

**Rationale:**
1. **No viable upstream port.** All three maintained implementations have deep POSIX
   dependencies. Adapting any of them requires replacing nearly all I/O and process
   management — producing an unrecognizable codebase that inherits neither the review
   nor the architecture of the upstream.
2. **Bounded scope.** Eight RFCs, one KEX method, two auth methods, one channel type.
   ~3000–5000 lines is feasible for a careful implementation.
3. **Protocol control.** FortressOS-specific limits (two connections, one channel,
   36 KiB record buffers, five auth attempts, strict KEX required) are naturally
   expressed in a purpose-built implementation.
4. **No POSIX surface to strip.** From-scratch has no `fork`, `select`, `openpty`,
   `getpwnam` to remove.

**Risk mitigation:**
- Protocol correctness verified by interoperability with standard OpenSSH client
  (Phase 8 gate).
- Cryptographic correctness delegated entirely to Monocypher (Phase 2) — the
  protocol core contains no curve arithmetic.
- Differential testing against known-good SSH implementations is a Phase 4 gate
  requirement.
- Fuzz coverage of the packet parser is a Phase 4 gate requirement.
- The plan explicitly prohibits deploying a knowingly vulnerable prototype as a
  production service.

**Proposed implementation layout (per `CRYPTO_SSH_PLAN.md §5`):**

```
user/sshd/
├── ssh_packet.c       — packet framing, length/padding, sequence numbers
├── ssh_kex.c          — KEXINIT, curve25519 KEX, NEWKEYS, transcript hash
├── ssh_auth.c         — userauth state machine, publickey, password
├── ssh_channel.c      — channel lifecycle, window credits, data pump
├── ssh_transport.c    — connection state machine, rekey, keepalive, disconnect
├── ssh_monitor.c      — monitor main loop, worker spawn, pipe protocol
├── ssh_worker.c       — worker main loop, pipe requests, TCP I/O
└── ssh_session.c      — post-auth shell spawn, PTY bridge (Phase 6)

user/ssh-keygen/
└── ssh_keygen.c       — host key generation, safe file creation, fingerprint
```

---

## 8. Proposed Limits and Resource Ledger

### 8.1 Connection Limits

| Resource | Limit | Rationale |
|----------|-------|-----------|
| Total SSH connections (incl. preauth) | 2 | Plan requirement; leaves 5 TCP blocks for other use |
| Channels per connection | 1 | Plan requirement; session channel only |
| Authentication attempts per connection | 5 | Plan requirement |
| Pending (half-open) connections | 1 | SSH claims 1 slot of NET_TCP_BACKLOG_MAX=4 |

### 8.2 TCP Socket Budget

```
Total TCP connection blocks:   TCP_CB_MAX = 8
Total socket slots:            NET_SOCKET_MAX = 16

SSH claims:
  1 listener  (LISTEN state, 1 TCB)
  2 accepted  (1 TCB per SSH session)
  Subtotal SSH TCP: 3 TCBs, 3 socket slots

Remaining for non-SSH use:
  5 TCP connection blocks
  13 socket slot entries
```

This satisfies the plan's requirement: *"one listener plus two accepted connections
must not consume all eight TCP connections."*

> [!IMPORTANT]
> The SSH listener must not open an additional TCP connection block when both session
> slots are occupied. Backlog = 1 means at most one unaccepted connection waits.
> Additional SYN segments are silently dropped (consistent with TCP_BACKLOG_MAX
> overflow behavior in net_tcp.c).

### 8.3 Buffer Sizes

The plan specifies "36 KiB RX and TX record buffers." These are **SSH record-layer
buffers** (application-level reassembly and send staging), not TCP stack buffers.
(Discrepancy D3: the TCP stack has `TCP_TXBUF_MAX` = 8 KiB and `TCP_RXBUF_MAX` =
64 KiB per connection — these are the transport and do not change.)

| Buffer | Size | Per-connection |
|--------|------|---------------|
| SSH RX record buffer | 36 KiB | 36 KiB |
| SSH TX record buffer | 36 KiB | 36 KiB |
| Monitor↔worker pipe (each direction) | 1 KiB | 2 KiB |
| Inbound channel data pipe | 4 KiB | 4 KiB |
| Outbound channel data pipe | 4 KiB | 4 KiB |
| Per-connection total | — | ~82 KiB |

### 8.4 SSH Memory Budget

```
SSH memory ceiling: 512 KiB (separate from 576 KiB network-state budget)
Exclusions: process stacks, page tables

Estimated usage (2 connections at peak):
  RX/TX record buffers:    2 × 72 KiB  = 144 KiB
  Pipe buffers:            2 × 6 KiB   =  12 KiB
  SSH protocol state:      2 × 8 KiB   =  16 KiB
  Monitor state:           1 × 4 KiB   =   4 KiB
  Crypto scratch:          1 × 16 KiB  =  16 KiB
  Total estimated:                       192 KiB
  Remaining headroom:                    320 KiB

Network-state budget (576 KiB): unchanged.
Shared credits (8): unchanged.
```

### 8.5 Process Slot Budget

```
PROCESS_CAPACITY = 64

SSH processes (2 sessions, Option B duplex model):
  1 monitor
  2 workers (one per connection)
  2 net-rx processes
  2 net-tx processes
  (2 authenticated shell processes — user overhead, not SSH infrastructure)
  SSH infrastructure subtotal: 7 slots

Remaining process slots: 57 (includes init, local shell, dhcpc, etc.)
```

The monitor must terminate workers and their bridge children promptly on connection
close. Zombie accumulation is prohibited.

### 8.6 Admission Budgets and Deadlines

| Resource | Limit |
|----------|-------|
| KEX deadline | 10 s from TCP accept to NEWKEYS |
| Auth deadline | 60 s from NEWKEYS to `SSH_MSG_USERAUTH_SUCCESS` |
| Per-source connection rate | 1 new connection per 5 s (enforced by monitor) |
| Monitor pipe read deadline | 5 s per message from pending request |
| Worker teardown deadline | 10 s before SIGKILL |
| Rekey byte threshold | 1 GiB per direction |
| Rekey packet threshold | 2^31 packets |
| Rekey time threshold | 1 hour |
| Password auth minimum delay | 50 ms (constant-time comparison + mandatory floor) |

All deadlines use monotonic absolute BSP tick values. No wall-clock dependency.

### 8.7 Resource Ledger Summary

| Category | Allocated | Ceiling | Margin |
|----------|-----------|---------|--------|
| TCP connection blocks | 3 | 8 | 5 free |
| Socket slots | 3 | 16 | 13 free |
| Process slots (SSH infra) | 7 | 64 | 57 free |
| SSH memory (excl. stacks) | ~192 KiB est. | 512 KiB | ~320 KiB |
| Network-state budget | unchanged | 576 KiB | unchanged |
| Shared credits | unchanged | 8 | unchanged |
| Open fds in monitor | ≤ 10 peak | 32 | 22 free |

---

## 9. Error Precedence

Error handling follows the same discipline as the existing network syscalls: errors
propagate from innermost to outermost; the first definitive error wins.

### 9.1 Precedence Order (innermost wins)

```
1. Memory / descriptor exhaustion    (ENOMEM, EMFILE)
2. Credential / permission failures  (EPERM, EACCES)
3. Admission rejection               (rate limit, connection cap)
4. Protocol validation failure       (malformed packet, wrong message order)
5. Crypto failure                    (AEAD tag mismatch, invalid signature, bad KEX)
6. Authentication failure            (bad key, bad password, max attempts)
7. I/O / transport errors            (EIO, ECONNRESET, ETIMEDOUT, deadline expiry)
8. Application-level                 (channel EOF, session close)
```

### 9.2 Admission Rejection

Occurs in the **monitor** before the worker is spawned:
- Connection cap (2 active): TCP accept succeeds; monitor sends `SSH_MSG_DISCONNECT`
  and closes the fd immediately. No process slot consumed.
- Rate limit: same path — accept, disconnect, close.

### 9.3 Protocol Validation Failure

Occurs in the **worker** during unauthenticated parsing:
- Unknown or unsupported message type: `SSH_MSG_UNIMPLEMENTED` (post-NEWKEYS) or
  `SSH_MSG_DISCONNECT(SSH_DISCONNECT_PROTOCOL_ERROR)` (pre-NEWKEYS).
- Packet length outside bounds: immediate disconnect, no response.
- Negotiation failure (no common algorithm): `SSH_MSG_DISCONNECT
  (SSH_DISCONNECT_KEY_EXCHANGE_FAILED)`.

The worker sends `REQ_WORKER_DONE` to the monitor with an error code before exiting.

### 9.4 Crypto Failure

Occurs in the worker:
- AEAD tag mismatch: **immediate disconnect, no error response** — do not reveal
  which byte caused failure; do not log packet content.
- Invalid Ed25519 signature: disconnect (`SSH_DISCONNECT_KEY_EXCHANGE_FAILED`).
- Low-order X25519 point: disconnect with KEX failure.
- CSPRNG not ready: refuse to proceed with KEX; disconnect.

Crypto failure is never reported to the remote peer with detail. Local dmesg logs
event type and source IP only.

### 9.5 Authentication Failure

Handled in the **monitor** via pipe protocol:
- Monitor performs the comparison; returns `RESP_AUTH_FAIL` or `RESP_AUTH_DENY`.
- Worker sends `SSH_MSG_USERAUTH_FAILURE` to peer.
- At 5 failures: monitor returns `RESP_AUTH_DENY`; worker disconnects.
- Bounded dummy verification (50 ms minimum) applied even on account-not-found
  paths to prevent username enumeration via timing.
- Password material wiped from all buffers before returning from the auth handler.

### 9.6 Response to Hostile Input

| Input type | Response |
|-----------|---------|
| Pre-KEX oversized packet | Disconnect; no error response |
| Post-NEWKEYS AEAD failure | Disconnect; no error response |
| Invalid/unexpected message type | `SSH_MSG_UNIMPLEMENTED` or disconnect |
| Repeated auth failures | 50 ms base delay × attempt; disconnect at 5 |
| Connection rate exceeded | Accept + immediate disconnect (no SSH response) |
| Wrong packet sequence number | Disconnect (strict KEX violation) |
| Channel data exceeding window | Disconnect (protocol error) |
| Any path/string exceeding bounds | Disconnect (protocol error) |

---

## 10. Threat Model

### 10.1 Trust Boundaries

```
TRUSTED:          kernel, SSH monitor, installed-root filesystem
SEMI-TRUSTED:     authenticated SSH worker (post-auth, carries session)
USER-TRUSTED:     authenticated shell process
UNTRUSTED:        pre-auth SSH worker process, remote peer, all network data
```

### 10.2 Hostile Inputs

Treated as hostile at all times:
- **Unauthenticated network data** — every byte before `SSH_MSG_USERAUTH_SUCCESS`;
  all lengths, string lengths, algorithm names, and KEX data are bounds-checked
  before any use.
- **Usernames** — treated as untrusted byte strings; never passed to shell or path
  expansion before userdb lookup.
- **Public keys presented for auth** — treated as attacker-controlled until after
  Ed25519 signature verification. Mere key presence is insufficient.
- **Terminal requests (pty-req)** — window size, term env, mode bytes — all bounded
  and validated before any kernel terminal API call.
- **File paths (exec command)** — validated against `VFS_MAX_PATH` (256) and
  character set before any VFS operation.
- **Channel data** — bounded by declared channel window; excess causes disconnect.

### 10.3 In-Scope Attacks

| Attack | Mitigation |
|--------|-----------|
| Unauthenticated connection exhaustion | Admission cap (2 connections), rate limit (1 per 5 s per source IP) |
| Malformed SSH packet parsing | Bounded parsers, length-check before allocation, disconnect on error |
| Auth bypass via protocol confusion | Strict KEX required; message ordering enforced by monitor |
| Username enumeration via timing | 50 ms minimum dummy hash on account-not-found |
| Username enumeration via error message | Same error for bad user and bad password |
| MITM / host key spoofing | Host key fingerprint must be verified by client |
| Privilege escalation via worker | Worker has no capabilities; monitor is the only signing oracle |
| Monitor oracle abuse | Operation ordering and session identity validated on every message |
| Private key theft via worker | Worker never receives private key bytes; only signatures |
| Process group escape | Each session has an independent process group; SIGINT via PTY |
| Pipe protocol injection | Type, session ID, payload length validated on every message |
| Descriptor leak into worker | All non-transferred fds are CLOEXEC-marked before spawn |
| Key file race (TOCTOU) | Validate by retained fd; no path re-lookup after open |
| Partial key file trust | Priv/pub pair validated together; mismatch disables SSH |

### 10.4 Out-of-Scope Attacks

| Attack | Reason |
|--------|--------|
| Timing side channels on crypto | Monocypher is constant-time by design; no formal proof claimed by FortressOS |
| Cache-timing attacks on AES | Not applicable (ChaCha20 is cache-safe) |
| Rowhammer / speculative execution | Hardware-level; outside software threat model |
| Physical access (cold-boot, DMA) | Out of scope |
| Supply-chain attacks on Monocypher | Provenance recorded but not independently audited by FortressOS |
| Traffic analysis / metadata | Out of scope for this phase |
| Complete DoS hardening | Admission limits are present; exhaustive DoS hardening is not a goal |

### 10.5 Explicit Non-Features (Request Failures, Not Silent Accepts)

| Feature | SSH response |
|---------|-------------|
| SFTP/SCP (subsystem `sftp`) | `SSH_MSG_CHANNEL_FAILURE` |
| TCP forwarding (direct-tcpip, tcpip-forward) | `SSH_MSG_REQUEST_FAILURE` |
| Agent forwarding | `SSH_MSG_REQUEST_FAILURE` |
| X11 forwarding | `SSH_MSG_REQUEST_FAILURE` |
| Compression (zlib, zlib@openssh.com) | Rejected at KEXINIT negotiation |
| OpenSSH certificates / sk-* / cert-* key types | `SSH_MSG_USERAUTH_FAILURE` |
| Root remote login | `SSH_MSG_USERAUTH_FAILURE` (enforced by monitor) |
| Multiple channels per connection | `SSH_MSG_CHANNEL_OPEN_FAILURE` (resource shortage) |
| Any channel type other than `session` | `SSH_MSG_CHANNEL_OPEN_FAILURE` (unknown type) |
| Any KEX other than curve25519-sha256 | Disconnect at KEXINIT |
| Any host key type other than ssh-ed25519 | Disconnect at KEXINIT |
| Any cipher other than chacha20-poly1305@openssh.com | Disconnect at KEXINIT |

These are permanent exclusions, not gaps to be filled later.

---

## 11. Dependency Order and Discrepancies

### 11.1 Dependency Graph

```
Phase 0  (this document — contracts, audit, threat model)
  │
  ├──→ Phase 1: SYS_GETRANDOM + HASH-DRBG (secure randomness)
  │         prerequisites: Phase 0 contracts, syscall number allocation
  │         parallel: Phase 2 crypto library may begin in parallel
  │         │
  │         └──→ Phase 2: Monocypher port + HMAC-SHA-256 glue (#50)
  │                   │
  │                   ├──→ Phase 3: Persistent host identity (#52)
  │                   │         prerequisites: SYS_GETRANDOM, SYS_FSTAT (new)
  │                   │         GATE: #52 closes before #51 can start
  │                   │
  │                   └──→ Phase 4: SSH transport (#51 prerequisite)
  │                             prerequisites: crypto lib, host identity
  │                             │
  │                             └──→ Phase 5: Auth + privilege separation
  │                                       prerequisites: transport, accounts ABI
  │                                       │
  │                                       └──→ Phase 6: PTY + session channels
  │                                                 prerequisites: PTY subsystem (new)
  │                                                 │
  │                                                 └──→ Phase 7: Daemon lifecycle
  │                                                           │
  │                                                           └──→ Phase 8: Acceptance
  │
  └──→ Terminal/session prerequisites (can proceed in parallel with Phases 1–5):
          - PTY subsystem design and implementation
          - SYS_FSTAT syscall design and authorization
```

**Issue #52 (host provisioning) closes before issue #51 (SSH daemon) can start.  
Issue #50 (crypto library) closes before issue #52 or #51 can start.**

### 11.2 Discrepancies Between Plan and Current Checkout

All discrepancies are documented. None are silently reconciled.

| # | Plan statement | Actual state | Impact |
|---|---------------|--------------|--------|
| D1 | Task prompt references `SSH_CRYPTO_PLAN.md` | Actual filename: `CRYPTO_SSH_PLAN.md` | Documentation only |
| D2 | "Supported durability barriers" | Only `SYS_SYNC` (global, syscall 14) exists; no `SYS_FSYNC` (per-fd) | Phase 3 must use global sync twice; or add SYS_FSYNC |
| D3 | "36 KiB RX/TX record buffers" | TCP RX=64 KiB (`TCP_RXBUF_MAX`), TX=8 KiB (`TCP_TXBUF_MAX`); 36 KiB refers to SSH record-layer buffers, which are separate | No conflict; needs explicit clarification in Phase 2 spec |
| D4 | "Validate ownership/type/mode by retained opened object, not by path re-lookup" | No `SYS_FSTAT` (stat-by-fd) exists in current ABI | New syscall needed before Phase 3; authorization required |
| D5 | "Per-SSH-session PTY isolation has not been established" | No PTY subsystem exists at all — not isolated but literally absent | Major gap; new kernel subsystem required for Phase 6 |
| D6 | "General poll/select/nonblocking interface was not established" | Confirmed absent; no `SO_NONBLOCK`, no `O_NONBLOCK` on sockets or pipes | Option B (explicit I/O processes) selected as resolution |
| D7 | "Do not assume existing sandbox/chroot API" | Confirmed: no chroot, no namespace, no seccomp | Only capability-drop + service-account isolation available |
| D8 | Plan startup banner note about overstating hardware assurance | `isn_crypto_guaranteed` flag exists; banner text not audited | Separate documentation issue; does not affect SSH planning |
| D9 | `SYS_INSTALLSEED` "not a general randomness contract for unprivileged SSH workers" | Confirmed: requires euid=0 + CAP_SYS_ADMIN; max 256 bytes; no DRBG conditioning | New `SYS_GETRANDOM` required; authorized separately |
| D10 | `SYS_SPAWN_EXT` with "retained descriptors and pipes" | `SYS_SPAWN_EXT` (syscall 20) with fd_actions confirmed; `SYS_PIPE` (syscall 24) confirmed | Plan and checkout agree; no discrepancy |
| D11 | Plan does not detail `SYS_INPUT_READ` constraint | `input.h` documents: "BSP-only single interactive consumer; all other CPUs rejected" | SSH must never use `SYS_INPUT_READ`; must use per-session PTY reads only |

### 11.3 Open Prerequisites Requiring Authorization Before Implementation

1. **`SYS_GETRANDOM` syscall number** — next available after 76 is 77. Requires
   authorization before any Phase 1 implementation.
2. **DRBG algorithm** — HASH-DRBG (SHA-512) recommended; avoids kernel-side AES
   dependency. Requires authorization.
3. **`SYS_FSTAT` addition** — stat-by-fd; required for Phase 3 key publication
   contract. Requires authorization before Phase 3 begins.
4. **PTY subsystem design** — required for Phase 6. Design may proceed in parallel
   with Phases 1–5; implementation must not begin until Phase 5 monitor/worker
   protocol is stable.

---

*This document is Phase 0 planning output only. No code was written or modified.
No commits were made. No production defaults were changed. The network SMP code,
DHCP, time subsystem, permissions, and recent tools (sed, awk, dmesg, tail, time,
date, free, checksum, traceroute) are unaffected.*
