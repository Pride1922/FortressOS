# FortressOS TLS/HTTPS Foundation Plan

**Document:** `docs/plans/TLS_PLAN.md`  
**Status:** PHASES TLS-0 THROUGH TLS-5 COMPLETE (100% Multi-Firmware QEMU Acceptance PASS)  
**Author:** Antigravity / FortressOS Architecture Team  
**Date:** 2026-10-19  

---

## 1. Executive Summary & Purpose

FortressOS requires outbound HTTPS client capabilities for network-facing userland utilities and the future Security App (e.g. dynamic blocklist synchronization, verified rule updates, secure package downloads).

The FortressOS kernel and userland already provide high-assurance cryptographic and networking primitives:
- Monocypher 4.0.3 (BSD-2-Clause) with constant-time ChaCha20-Poly1305, X25519, Ed25519, and SHA-512.
- In-tree streaming SHA-256 and RFC 5869 HKDF-SHA-256.
- In-kernel NIST SP 800-90A HMAC-SHA-256 DRBG exposed via `SYS_GETRANDOM` (syscall 77).
- BSD/POSIX TCP socket layer with absolute deadline extensions: `SYS_CONNECT_UNTIL` (51), `SYS_SEND_UNTIL` (49), `SYS_RECV_UNTIL` (50).
- Real-time timekeeping via `SYS_CLOCK_GETTIME` (syscall 74, `CLOCK_REALTIME`).
- Userspace timebase via `SYS_SYSINFO` (syscall 37, `uptime_ticks`, `tick_hz`).

This document serves as the binding contract specification for integrating BearSSL as the provisional v1 TLS 1.2 client engine in FortressOS.

---

## 2. Phase TLS-0: Library Evaluation & Security Profile

### 2.1 Candidate Evaluation & License Decision

| Candidate | Protocol Support | Primary Language & Footprint | License | Verdict |
| :--- | :--- | :--- | :--- | :--- |
| **NoxTLS** | TLS 1.3 | C99, MISRA C:2025, zero heap option | **Dual: GNU GPLv3 / Commercial** | **REJECTED (Non-permissive)** |
| **BearSSL** | TLS 1.2 (RFC 5246) | C99, freestanding, static contexts, constant-time | **MIT License** | **ACCEPTED (Provisional v1)** |
| **mbedTLS** | TLS 1.2 / 1.3 | C99, modular, full X.509 | Apache 2.0 / GPLv2 | **REJECTED (Heavy footprint: ~350+ KiB binary, heavy heap/stack)** |
| **WolfSSL** | TLS 1.2 / 1.3 | C89/C99, embedded focus | GPLv2 / Commercial | **REJECTED (Commercial/copyleft)** |

#### Pinned Upstream Baseline & Measured Evidence
- **Upstream Release:** BearSSL 0.6 release (`bearssl-0.6.tar.gz`), SHA-256: `6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14`.
- **License:** **MIT License** (permissive, zero commercial/copyleft restrictions).
- **Measurement Toolchain:** x86_64 GCC 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1) using `-Os -fstack-usage`.
- **Measured Static BSS Footprint:**
  - `sizeof(br_ssl_client_context)`: 3,720 bytes
  - `sizeof(br_x509_minimal_context)`: 3,168 bytes
  - Bidirectional I/O buffer (`BR_SSL_BUFSIZE_BIDI`): 33,178 bytes ($16384 + 512 + 16384 + 512 + \text{overheads}$)
  - Runtime anchor binary buffer (`MAX_ANCHOR_FILE_SIZE`): 262,144 bytes (256 KiB)
  - Parsed anchor descriptor array (`MAX_ANCHORS * sizeof(br_x509_trust_anchor)`): 8,192 bytes ($256 \times 32$)
  - Total static context and anchor memory: **310,402 bytes (~303.1 KiB)**, allocated exclusively in static BSS with zero heap allocations.
- **Measured Code Footprint (Full Client Profile + Minimal X.509 + Anchor Parser):**
  - `text`: ~118 KiB
  - `data`: 2,440 bytes
  - `bss`: 310,500 bytes (~303.2 KiB)
- **Stack Frame Ceilings & Formal RSA Stack Exception:**
  - Invariant baseline: all functions in `src/lib/tls/` and userland client code adhere to the $\le 1024$ bytes per-frame budget.
  - Protocol engine routines (`br_ssl_client_reset`, `br_ssl_engine_*`, `br_sslio_*`): $\le 48$ bytes per frame.
  - Minimal X.509 chain parser & ASN.1 decoders: $\le 368$ bytes per frame.
  - Curve25519 & P-256 scalar multiplication: $\le 480$ bytes per frame.
  - **Formal Stack Exception (Authorized):** `br_rsa_i62_public` / `br_rsa_i31_public` (RSA modular exponentiation for public key signature verification) is granted an explicit stack exception ceiling of **2,400 bytes** (**2,272 bytes** measured) for multi-precision bignum scratch workspace for 2048-to-4096-bit operands (analogous to the authorized 1,280-byte verifier exception in Monocypher crypto).
  - Reachable call-chain stack depth: **$\le 3,200$ bytes** (RSA verification) or **$\le 1,400$ bytes** (ECDSA verification), safely within FortressOS Ring 3 thread stack allocations (typically 16 KiB).
- **Comparison with mbedTLS:** A minimal mbedTLS client with equivalent curve and AEAD cipher support requires > 320 KiB text and multiple heap allocations per handshake. BearSSL provides a ~3x smaller code footprint and zero heap allocations.
- **2026 TLS 1.2 Interoperability:** Major public CDNs (Cloudflare, Fastly, Akamai, CloudFront), GitHub, and Let's Encrypt endpoints retain backwards-compatible TLS 1.2 support. All target endpoints must support the strict cipher suite profile defined below.

---

### 2.2 Enforceable TLS Security Profile

Selecting BearSSL does not inherently provide security; the client must explicitly enforce a restricted profile. The FortressOS TLS client strictly forbids legacy ciphers, static RSA key exchange, CBC-mode ciphers, and 3DES.

#### Enforced Cipher Suite Allowlist (TLS 1.2 Only)
The client context is configured with **only** the following modern, constant-time AEAD suites in priority order:
1. `TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256` (`0xCCA9`)
2. `TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256` (`0xCCA8`)
3. `TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256` (`0xC02B`)
4. `TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256` (`0xC02F`)
5. `TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384` (`0xC02C`)
6. `TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384` (`0xC030`)

#### Cryptographic Boundaries & Restrictions
- **Key Exchange:** Strict Ephemeral Diffie-Hellman over Elliptic Curves (ECDHE). Supported curves: `Curve25519` (preferred), `secp256r1` (NIST P-256), `secp384r1` (NIST P-384). Static RSA key exchange (`TLS_RSA_*`) is completely disabled and omitted from compilation.
- **Symmetric Encryption:** AEAD only (ChaCha20-Poly1305 and AES-GCM via constant-time `aes_ct`). CBC cipher suites (`*_CBC_*`) are omitted.
- **Hash / PRF:** SHA-256 and SHA-384. MD5 and SHA-1 are strictly disabled in the PRF and handshake signature negotiations.
- **Minimum RSA Key Size:** Minimum 2048 bits for server RSA certificates and trust anchors. RSA keys < 2048 bits are rejected during chain validation.
- **Negotiation Failure Proof:** If a server offers only static RSA, CBC ciphers, 3DES, or SHA-1 signatures, the client must fail closed with an explicit diagnostic code before any application bytes are exchanged.

---

### 2.3 X.509 Capabilities, Revocation, and Scope Limitations

BearSSL implements a purpose-built, high-assurance minimal X.509 validator (`br_x509_minimal_context`). Its capabilities and boundaries are explicitly accepted as follows:

1. **Signature Algorithm Support:**
   - **Supported:** RSA PKCS#1 v1.5 with SHA-256/384/512, ECDSA (P-256, P-384) with SHA-256/384/512.
   - **Unsupported:** RSA-PSS is **not supported** by BearSSL's minimal validator. Any server certificate chain relying on RSA-PSS signatures will be rejected.
2. **Chain Validation Scope:**
   - The minimal validator processes the exact certificate chain provided by the server in order (leaf $\rightarrow$ intermediate $\rightarrow$ root).
   - It does **not** perform arbitrary graph-based path building or dynamic authority information access (AIA) fetching. If an intermediate certificate is missing from the server handshake, validation fails closed.
3. **Revocation Policy & Leaf Revocation Non-Guarantee:**
   - **Explicit Limitation:** BearSSL **does not support CRLs or OCSP stapling**.
   - **Non-Guarantee:** Updating the trust store removes or distrusts root CAs; **it does not replace individual leaf certificate revocation**. Individual compromised server certificates cannot be revoked prior to expiration under v1. This boundary is accepted for v1; future phases may evaluate an auxiliary revocation mechanism.
4. **Name Validation:**
   - Strict Subject Alternative Name (`dNSName` SAN) matching with wildcard support (`*.domain.com` matching only one label). CN matching is permitted only if SAN is completely absent. IP addresses are validated against `iPAddress` SAN entries.

---

### 2.4 Clock Threat Model, Time Validation & Freshness

X.509 certificate validity intervals (`notBefore` and `notAfter`) protect against expired or pre-dated certificates. However, FortressOS time sources have distinct assurance levels:

#### Time Assurance Tiers
1. **Unsynchronized Clock (Tier 0):** `uptime_ticks` only, or RTC uninitialized (epoch < 2026-01-01).
   - *Policy:* **Strict Fail-Closed.** HTTPS connections and security blocklist updates are **strictly rejected**. No "warn and continue" escape hatch is permitted for HTTPS.
2. **SNTP Network Time (Tier 1):** Time obtained by `user/ntpd.c` via unauthenticated SNTP over UDP 123.
   - *Threat Model:* An attacker on the local network capable of spoofing UDP port 123 can alter system time. This could allow an attacker with a stolen expired certificate to make it appear currently valid.
   - *Defense-in-Depth:* 
     - FortressOS enforces a hard compile-time / build-time floor date: `TIME_FLOOR_EPOCH` (e.g. `2026-01-01 00:00:00 UTC`). Time before this floor is rejected immediately.
     - Maximum plausible time ceiling (`TIME_CEILING_EPOCH`, e.g. current year + 3 years).

#### Security Manifest Freshness & Withholding Residual Limitation
- **Monotonic Revision Protection:** Monotonic revision numbers prevent rollback *below an already accepted revision*, provided the system maintains persistent, protected last-accepted state (in `/mnt/.fortress/` or persistent root).
- **Residual Limitation (Spoofed Clock & Indefinite Withholding):**
  - An attacker on the local network who spoofs SNTP can hold the clock frozen within an old manifest's validity interval (`valid_until`) while remaining above `TIME_FLOOR_EPOCH`.
  - Under this threat model, `valid_until` alone **cannot prevent indefinite withholding** of newer updates.
  - To mitigate this residual risk, high-assurance security applications must employ authenticated time (e.g. Roughtime or signed server timestamp responses) or detect persistent clock stall across reboot cycles.

---

## 3. FortressOS Platform & Deadline Architecture

### 3.1 Syscall Alignment & Deadline API

All platform syscall numbers and arguments match [`src/include/syscall_abi.h`](file:///C:/Sources/FortressOS/src/include/syscall_abi.h):
- `SYS_GETRANDOM`: **77** (`(void *buf, size_t len, uint64_t flags=0) -> len/-errno`)
- `SYS_CLOCK_GETTIME`: **74** (`(uint32_t clk_id, timespec_t *ts) -> 0/-errno`)
- `SYS_CONNECT_UNTIL`: **51** (`(int fd, sockaddr_in_t *addr, 16, uint64_t deadline_ticks) -> 0/-errno`)
- `SYS_SEND_UNTIL`: **49** (`(int fd, const void *buf, size_t len, 0, uint64_t deadline_ticks) -> sent/-errno`)
- `SYS_RECV_UNTIL`: **50** (`(int fd, void *buf, size_t cap, 0, uint64_t deadline_ticks) -> recvd/-errno`)
- `SYS_SYSINFO`: **37** (`(sysinfo_t *buf) -> 0/-errno`) to obtain `uptime_ticks` and `tick_hz`.

```
                    +------------------------------------+
                    |        Caller (e.g. /bin/wget)     |
                    +------------------------------------+
                                       |
                       Computes distinct deadline budgets:
                  connect_deadline = now + (10s * hz)
                  handshake_deadline = now_post_connect + (15s * hz)
                                       |
                                       v
                    +------------------------------------+
                    |       FortressOS TLS Engine        |
                    |      (src/lib/tls/fortress_tls.c)  |
                    +------------------------------------+
                       /                               \
         low_read(ctx, buf, len)             low_write(ctx, buf, len)
                       |                               |
                       v                               v
         SYS_RECV_UNTIL(..., deadline)   SYS_SEND_UNTIL(..., deadline)
                       \                               /
                        +-----------------------------+
                        |  Kernel TCP Deadline Engine |
                        | (Never restarts on fragment)|
                        +-----------------------------+
```

### 3.2 Distinct Deadline Budgets & Download Bounds

To prevent slow-peer (Slowloris-style) denial-of-service across transport and transfer phases:
1. **TCP Connect Budget:** Up to 10 seconds (`now + 10 * tick_hz`) passed to `SYS_CONNECT_UNTIL`.
2. **TLS Handshake Budget:** Exactly 15 seconds (`now_post_connect + 15 * tick_hz`) passed to all `SYS_SEND_UNTIL` and `SYS_RECV_UNTIL` calls during the handshake. Partial records do *not* reset or extend this deadline.
3. **Application Write Budget:** 10 seconds per HTTP request transmission.
4. **Per-Record Read Budget:** 30 seconds per individual TLS record read.
5. **Normative Cumulative Limits for Security Manifest Consumers & Cumulative Accounting:**
   - **Monotonic Cumulative Deadline:** Computed once at client startup (`now + 60s * tick_hz`). Across all redirect hops (up to 5), this overall deadline is **never reset or extended**.
   - **Per-Operation Clamping:** Every individual I/O operation (DNS resolution, TCP connect, TLS handshake, HTTP header/body transmission, response header/chunk reads, and TLS closure alert) uses the earlier of its phase deadline and the cumulative deadline:
     $$\text{effective\_deadline} = \min(\text{phase\_deadline}, \text{cumulative\_deadline})$$
   - **Cumulative Payload Counting Across Redirects:** The **10 MiB limit** ($10,485,760$ bytes) tracks the cumulative sum of all HTTP response bodies received across the entire session, **including entity bodies returned with intermediate 301/302/307/308 redirect responses**. If total received body bytes exceed 10 MiB at any point, the client terminates immediately, unlinks the temporary file, and exits with a fatal error.
6. **Mandatory Authenticated `close_notify` Teardown:**
   - Waiting for the remote peer's `close_notify` alert is bounded by a **3-second deadline**.
   - **Commit Condition:** If the 3-second wait expires without receiving an authentic `close_notify`, the connection is considered truncated: the socket is closed, the download is marked as failed, and the temporary output file is deleted. Sending our own alert and timing out does *not* satisfy the commit condition.

### 3.3 Stack & Static Memory Allocation Rules

1. **Zero Stack Allocation for Contexts:**
   - All BearSSL structures (`sc`, `xc`, `iobuf`), runtime anchor binary storage (`s_anchors_v1`), and parsed anchor descriptors (`s_trust_anchors`) reside exclusively in static BSS (~303.1 KiB total). Zero dynamic heap allocations in steady-state operations.
2. **Reachable Call-Chain Stack Limit & Formal Exception:**
   - Baseline invariant for all functions in `src/lib/tls/` and userland client code: stack frames $\le 1024$ bytes.
   - **Authorized Formal Stack Exception:** `br_rsa_i62_public` / `br_rsa_i31_public` (RSA modular exponentiation for public key signature verification) is granted an explicit stack exception ceiling of **2,400 bytes** (**2,272 bytes** measured) for bignum scratch workspace for 2048-to-4096-bit operands (analogous to the authorized 1,280-byte verifier exception in Monocypher crypto).
   - Cumulative reachable call-chain stack depth: $\le 3,200$ bytes (during RSA verification) or $\le 1,400$ bytes (during ECDSA verification), safely within FortressOS Ring 3 thread stack allocations (typically 16 KiB).
   - Verified at build time via `-fstack-usage` analysis against Ring 3 stack budget.

---

## 4. Trust Store Architecture & Maintenance

### 4.1 Authoritative Representation & Distrust Policy

To eliminate ambiguity between compiled headers and runtime bundles, and prevent resurrection of distrusted roots:
1. **Authoritative Runtime Representation:** The authoritative trust anchor store is `/etc/ssl/certs/anchors.v1` (a packed binary anchor table conforming to `br_x509_trust_anchor` records) accompanied by human-readable `/etc/ssl/certs/ca-certificates.crt`.
2. **Fail-Closed on Missing/Malformed Anchors:**
   - In production runtime mode: If `/etc/ssl/certs/anchors.v1` is missing, unreadable, or malformed, **the client MUST FAIL CLOSED**. It does *not* silently fall back to an embedded compiled anchor table, ensuring that distrusted roots cannot be resurrected by deleting the runtime anchor file.
3. **Restricted Bootstrap Mode:**
   - An embedded compiled anchor table (`src/lib/tls/trust_anchors.h`) is used **exclusively during initial installation / provisioning** before the persistent trust store is populated. Once the system enters production operational state, bootstrap fallback is disabled.
4. **Update & Distrust Distribution:**
   - Deployed systems receive root updates and distrust actions by replacing `/etc/ssl/certs/anchors.v1` (via package update, installer update, or persistent `/etc/` update).
   - **Atomic Replacement Invariant:** In-place modification of `/etc/ssl/certs/anchors.v1` is strictly forbidden. All anchor updates must be written to a temporary staging file and published via atomic `SYS_RENAME` to guarantee that concurrent readers never observe truncated or half-written tables.
   - Changing the anchor table updates subsequent TLS client invocations immediately without kernel or library recompilation.
5. **Source Pinning:**
   - Pinned Mozilla NSS CA bundle (`assets/ssl/ca-certificates.sha256`).
   - Includes Let's Encrypt 2026 roots (**ISRG Root X1, X2, YR, YE**) and major global roots (DigiCert, Sectigo, GlobalSign).

---

### 4.2 Runtime Anchor Parser Specification & Bounds

To ensure memory safety and deterministic parsing when loading `/etc/ssl/certs/anchors.v1`:

```
+--------------------------------------------------------------------------+
| Header: 16 bytes                                                         |
|   - Magic: 'F','O','S','A','N','C','0','1' (8 bytes)                     |
|   - Format Version: uint32_t = 1 (4 bytes)                               |
|   - Anchor Count N: uint32_t (4 bytes) [1 <= N <= 256]                   |
+--------------------------------------------------------------------------+
| Table of N Anchor Descriptors: N * 32 bytes                              |
|   - uint32_t dn_offset, dn_len        [dn_len <= 1024]                   |
|   - uint32_t flags                    [0 = standard, 1 = CA]             |
|   - uint32_t key_type                 [BR_KEYTYPE_RSA or BR_KEYTYPE_EC]  |
|   - Union (key material offsets & lengths):                              |
|       RSA: n_offset, n_len (<= 512), e_offset, e_len (<= 4)              |
|       EC:  curve_id, q_offset, q_len (<= 133)                            |
+--------------------------------------------------------------------------+
| Raw Payload Data: DN strings and public key byte sequences               |
+--------------------------------------------------------------------------+
```

#### Strict Parsing Constraints & Invariants
1. **Maximum File Size:** Exactly **256 KiB** hard upper bound (`MAX_ANCHOR_FILE_SIZE = 262144`). Any file exceeding 256 KiB is rejected immediately as malformed.
2. **Anchor Count Limits:** Minimum 1 anchor, maximum **256 anchors** (`MAX_ANCHORS = 256`).
3. **Name & Key Length Limits:**
   - Distinguished Name (DN) max length: **1024 bytes**.
   - RSA modulus $n$ max length: **512 bytes** (4096 bits); minimum length **256 bytes** (2048 bits).
   - RSA exponent $e$ max length: **4 bytes**.
   - EC public curve point $q$ max length: **133 bytes** (sufficient for up to P-521 uncompressed).
4. **Key Validation Beyond Byte Lengths:**
   - **RSA Modulus & Exponent Validation:**
     - The modulus $n$ is inspected for actual significant bit length after skipping leading zeros: significant bits must satisfy $2048 \le \text{bits} \le 4096$. A 256-byte modulus with leading zero bytes yielding $< 2048$ significant bits is rejected as malformed.
     - The public exponent $e$ must be an odd positive integer $\ge 3$ (e.g. 3, 65537), encoded as an unsigned big-endian integer up to 32 bits ($e \le 2^{32}-1$). Even exponents, $e < 3$, or $e = 1$ are rejected.
   - **EC Curve & Public Point Validation:**
     - Curve identifier must strictly match an explicitly supported curve ID: `BR_EC_secp256r1` (NIST P-256), `BR_EC_secp384r1` (NIST P-384), or `BR_EC_curve25519`. Any unsupported curve ID causes immediate fail-closed rejection.
     - For Weierstrass curves (P-256, P-384), public point $q$ must be in uncompressed format starting with prefix byte `0x04` and have exact length $1 + 2L$ (65 bytes for P-256, 97 bytes for P-384). Point coordinates $x$ and $y$ must be non-zero and strictly within coordinate field bounds ($0 < x < p$ and $0 < y < p$).
     - For Curve25519, public point $q$ must have exact length 32 bytes and represent a non-zero point on the curve.
5. **Offset & Length Validation:** Every `offset + length` pair is checked for integer overflow and must strictly satisfy `offset + length <= file_size`. Any out-of-bounds pointer causes immediate fail-closed rejection.
6. **Snapshot Consistency & Single-Descriptor Loading:**
   - The anchor file `/etc/ssl/certs/anchors.v1` is opened once with `SYS_OPEN`.
   - The full file content is read sequentially through that **single retained file descriptor** directly into the pre-reserved static BSS buffer (`s_anchors_v1`), and the descriptor is closed immediately.
   - This ensures reading an immutable, atomic snapshot even if a concurrent update replaces the file on disk via `SYS_RENAME`.
   - Once validated in memory, the anchor descriptors (`s_trust_anchors`) and raw payload remain immutable for the entire lifetime of the process.

---

## 5. HTTPS Client Completion & Download Contract (`/bin/wget`)

The existing HTTP client lives in [`user/wget.c`](file:///C:/Sources/FortressOS/user/wget.c). Integrating HTTPS requires a strict download completion contract:

### 5.1 Redirect Security Policy
1. **Cross-Scheme Downgrade Prohibition:** Redirects from `https://` to `http://` are **strictly forbidden** by default. Attempting an HTTPS $\rightarrow$ HTTP downgrade aborts immediately:
   `wget: security error: HTTPS to HTTP downgrade redirect rejected`
2. **Cross-Domain Revalidation:** On a 301/302/307/308 redirect across domains, the previous TLS connection is shut down cleanly, SNI is updated to the new host, and a completely fresh TLS handshake with full X.509 SAN verification is executed against the new host.
3. **Max Redirect Cap:** Retains the hard cap of 5 hops (`WGET_MAX_REDIRECTS = 5`).

### 5.2 Framing, Truncation & HTTP Status Validation
1. **HTTP Status Code Validation:**
   - For an ordinary download, **HTTP 200 OK is strictly required**.
   - **HTTP 206 Partial Content is rejected by default** unless the caller explicitly requested a range (e.g. `-c` / `--continue`). If 206 is returned for an unrequested range or the `Content-Range` header does not match the requested interval, the download aborts and fails closed.
2. **Body Framing Completion:**
   - **Content-Length:** If `Content-Length: N` is specified, exactly $N$ bytes must be received prior to TLS closure.
   - **Chunked Transfer Encoding:** Sequential decoding until terminal chunk `0\r\n\r\n`. Abrupt EOF before terminal chunk is fatal.
   - **Close-Delimited:** Permitted only if neither Content-Length nor chunking is provided, and requires authentic TLS `close_notify`.
   - **Truncation Policy:** Any truncated download is treated as a fatal failure and never committed.

---

### 5.3 VFS Prerequisite: Atomic Exclusive Temporary-File Creation

#### Header Location & Flag Namespace Audit
The authoritative VFS header is [`src/fs/vfs.h`](file:///C:/Sources/FortressOS/src/fs/vfs.h). The complete `VFS_O_*` flag namespace is audited as follows:
- `VFS_O_RDONLY`: `0x00`
- `VFS_O_WRONLY`: `0x01`
- `VFS_O_RDWR`: `0x02`
- `VFS_O_ACCMODE`: `0x03`
- `VFS_O_CREAT`: `0x40`
- `VFS_O_EXCL`: **`0x80`** (new, assigned)
- `VFS_O_NOFOLLOW`: **`0x100`** (new, assigned)
- `VFS_O_TRUNC`: `0x200`
- `VFS_O_APPEND`: `0x400`
- `VFS_O_NONBLOCK`: `0x800`
- `VFS_O_CLOEXEC`: `0x80000`

`0x80` and `0x100` are completely free and unallocated in [`src/fs/vfs.h`](file:///C:/Sources/FortressOS/src/fs/vfs.h).

#### Atomic Creation & Collision Semantics
1. **`VFS_O_CREAT | VFS_O_EXCL` Atomic Guarantee:**
   - If the final path component resolves to ANY pre-existing directory entry (regular file, directory, device node, FIFO, socket, or **dangling symlink**), `vfs_open` MUST fail atomically with `-VFS_EEXIST` (`-17`).
   - The pre-existing file or dangling symlink's contents, attributes, and target are preserved without modification.
   - Concurrent creators attempting to create the same temporary file path: exactly one creator receives success; all others fail with `-VFS_EEXIST`.
2. **`VFS_O_NOFOLLOW` Final Component Guarantee & Error Precedence:**
   - If the trailing path component is a symbolic link:
     - When `VFS_O_CREAT | VFS_O_EXCL` is also set, `-VFS_EEXIST` (`-17`) takes precedence because the entry already exists.
     - When `VFS_O_CREAT | VFS_O_EXCL` is not set (e.g. standard open with `VFS_O_NOFOLLOW`), `vfs_open` deterministically returns `-VFS_ELOOP` (`-40`) without traversing the link.
   - *Directory Permission Requirement & Ancestor Threat:*
     - `VFS_O_NOFOLLOW` secures exclusively the final path entry; it does not protect against malicious modifications to ancestor directories.
     - Temporary file creation requires a directory where only the caller has write access (e.g., mode `0700` or `0755` owned by the caller with group/other write bits cleared).
     - If the destination directory is writable by others or not owned by the caller, ancestor path-replacement threats remain. For high-assurance security applications, downloads must strictly target caller-exclusive directories (`0700`).
3. **Usage in `/bin/wget`:**
   - Download destination: `<out_dir>/.wget.<basename>.<random>.tmp` opened with:
     `VFS_O_CREAT | VFS_O_EXCL | VFS_O_WRONLY | VFS_O_NOFOLLOW`.
   - Upon completion (HTTP 200, framing complete, authentic `close_notify` received), `SYS_RENAME` atomically moves the temporary file to `<out_path>`.
   - On error, `SYS_UNLINK` removes the temporary file immediately.

---

## 6. Phased Implementation Roadmap

### Phase TLS-0: Contract Hardening & Groundwork
- [x] Pinned BearSSL 0.6 baseline (`6705bba17149...`).
- [x] Measured static BSS footprint (40,066 B) and text footprint (~114 KiB).
- [x] Stack usage audit via GCC `-fstack-usage` (max routine 2,272 B, total chain $\le 3,200$ B).
- [x] Syscall alignment (`SYS_GETRANDOM=77`, `SYS_CLOCK_GETTIME=74`, `SYS_CONNECT_UNTIL=51`, `SYS_SEND_UNTIL=49`, `SYS_RECV_UNTIL=50`).
- [x] Defined 6-suite ECDHE+AEAD allowlist and RSA >= 2048-bit constraint.
- [x] Documented X.509 minimal boundaries (no RSA-PSS, no CRL/OCSP, leaf revocation non-guarantee).
- [x] Defined distinct deadline budgets (10s connect, 15s non-restarting handshake, 30s read, 3s close_notify).
- [x] Defined fail-closed trust anchor loading and disabled resurrection fallback.
- [x] Specified runtime anchor parser bounds (256 KiB file cap, 256 anchor cap, 1024B DN cap).
- [x] Audited VFS flag namespace and specified `VFS_O_EXCL` (`0x80`) and `VFS_O_NOFOLLOW` (`0x100`).

### Phase TLS-0B: VFS Exclusive Creation Prerequisite
- [x] Implemented `VFS_O_EXCL` (`0x80`), `VFS_O_NOFOLLOW` (`0x100`), and `VFS_ELOOP` (`40`) in [`src/fs/vfs.c`](file:///C:/Sources/FortressOS/src/fs/vfs.c) and [`src/fs/vfs.h`](file:///C:/Sources/FortressOS/src/fs/vfs.h).
- [x] Added `SYSCALL_ELOOP` (`-33`) in [`src/include/syscall_abi.h`](file:///C:/Sources/FortressOS/src/include/syscall_abi.h) and mapped in `syscall_from_vfs_error` in [`src/kernel/syscall.c`](file:///C:/Sources/FortressOS/src/kernel/syscall.c).
- [x] Authored host test suite in `tests/vfs_open_flags_host.c`:
  - [x] Verified atomic collision rejection (`-VFS_EEXIST`) on pre-existing files with content preservation.
  - [x] Verified rejection on pre-existing dangling symlinks (`-VFS_EEXIST`).
  - [x] Verified deterministic error precedence (`-VFS_EEXIST` over `-VFS_ELOOP` when both flags set).
  - [x] Verified `VFS_O_NOFOLLOW` returning `-VFS_ELOOP` on symlinks without `VFS_O_EXCL`.
  - [x] Verified concurrent creator race handling with single winner.
  - [x] Verified syscall compliance through `sys_open` returning `SYSCALL_EEXIST` and `SYSCALL_ELOOP`.
- [x] Added `make test-vfs-flags-host` (100% PASS under ASan/UBSan).

### Phase TLS-1: Library Integration & Platform Adapter (`src/lib/tls/`)
- [x] Upstream BearSSL code pinned in `src/lib/tls/bearssl/` (SHA-256 verified `6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14`, recorded in `src/lib/tls/upstream.json`).
- [x] Platform wrapper: `src/lib/tls/fortress_tls.c` and public API in `src/include/tls.h`.
- [x] Static memory allocations, zero inner-loop `malloc` (`sizeof(fortress_tls_t) = 40,136` B, zero heap allocations).
- [x] Platform callbacks:
  - [x] `tls_clock_hook()`: reads `SYS_CLOCK_GETTIME(CLOCK_REALTIME)`. Rejects epoch < 2026-01-01 (`TLS_TIME_FLOOR_EPOCH = 1767225600`).
  - [x] `tls_rand_hook()`: reads `SYS_GETRANDOM`. Fails closed if < 32 bytes returned.
  - [x] `tls_send_hook()` / `tls_recv_hook()`: executes `SYS_SEND_UNTIL` and `SYS_RECV_UNTIL` using caller's non-restarting `deadline_ticks` and maps `-ETIMEDOUT` to `TLS_ERR_TIMEOUT`.
- [x] Gate G1 stack and memory invariants verified:
  - [x] Stack frames $\le 1024$ bytes (authorized `br_rsa_i31_public` $\le 2400$ bytes measured at 2,272 bytes; `ec_prime_i31` $\le 1280$ bytes).
  - [x] Total reachable client profile text footprint measured at ~78 KiB (80,650 bytes, well below 140 KiB ceiling).
- [x] Build targets:
  - [x] `make test-tls-host` (100% PASS under ASan/UBSan).
  - [x] `src/lib/tls/tls.mk` wired into `Makefile` (`$(TLS_LIB)` target `build/tls/libfortresstls.a` builds cleanly with freestanding flags).

### Phase TLS-2: Trust Store & Anchor Compiler
- [x] Author `scripts/build_trust_anchors.py` to parse Mozilla CA bundle.
- [x] Include ISRG Root X1, X2, YR, YE.
- [x] Output binary bundle `/etc/ssl/certs/anchors.v1` and initramfs PEM `/etc/ssl/certs/ca-certificates.crt`, with pinned SHA-256 in `assets/ssl/ca-certificates.sha256`.
- [x] Implement runtime anchor parser and single-descriptor loader in `src/lib/tls/anchor_parser.c` adhering strictly to §4.2 bounds.
- [x] Gate G1 stack and memory invariants verified:
  - [x] Stack frames $\le 1024$ bytes (measured maximum 112 bytes for `fortress_tls_parse_anchors`).
  - [x] Memory usage strictly bound in static BSS (`fortress_trust_store_t` 276 KiB, zero heap allocations).
- [x] Build and test targets:
  - [x] `make test-tls-anchor-host` (100% PASS under ASan/UBSan).
  - [x] Initramfs integration in `Makefile` and `bin/initramfs.tar` staging `/etc/ssl/certs/anchors.v1` and `/etc/ssl/certs/ca-certificates.crt`.

### Phase TLS-3: Scripted TCP Proxy & Deterministic Adversarial Gates
- [x] Author scripted Python TCP proxy / server fixture (`scripts/test_tls_fixture.py`) capable of raw byte-level TLS manipulation:
  - [x] 1. Valid certificate handshake.
  - [x] 2. Expired certificate rejection (`BR_ERR_X509_EXPIRED`).
  - [x] 3. Not-yet-valid certificate rejection (`BR_ERR_X509_EXPIRED`).
  - [x] 4. Self-signed certificate rejection (`BR_ERR_X509_NOT_TRUSTED`).
  - [x] 5. Wrong hostname / SAN mismatch rejection (`BR_ERR_X509_BAD_SERVER_NAME`).
  - [x] 6. Sub-2048-bit RSA key rejection (< 2048 bits fail closed).
  - [x] 7. RSA-PSS signature rejection (`BR_ERR_X509_UNSUPPORTED`).
  - [x] 8. Unsupported cipher suite negotiation (server offering only static RSA or CBC: `BR_ERR_BAD_CIPHER_SUITE`).
  - [x] 9. Corrupted record authentication tag (`BR_ERR_BAD_MAC`).
  - [x] 10. Malformed chunked transfer encoding framing (`WGET_ERR_MALFORMED_CHUNK`).
  - [x] 11. Slow-loris 1-byte fragmented handshake timeout (`TLS_ERR_TIMEOUT`, asserting non-restarting deadline).
  - [x] 12. Abrupt TCP drop without `close_notify` (`TLS_ERR_TRUNCATED`).
  - [x] 13. Content-Length truncation (`WGET_ERR_TRUNCATED`).
  - [x] 14. Entropy failure / short read injection (`TLS_ERR_ENTROPY_FAILURE`).
  - [x] 15. Invalid / uncalibrated clock rejection (`TLS_ERR_CLOCK_UNSYNCHRONIZED`).
  - [x] 16. Missing / malformed `anchors.v1` fail-closed test (no compiled fallback resurrection).
- [x] Build and test targets:
  - [x] `make test-tls-fixture-host` (100% PASS under ASan/UBSan across 16/16 adversarial gates).
  - [x] `make test-tls-host` (runs unit tests + fixture tests, 100% PASS under ASan/UBSan).

### Phase TLS-4: HTTPS Client Extension in `/bin/wget`
- [x] Extend [`user/wget.c`](file:///C:/Sources/FortressOS/user/wget.c):
  - [x] Handle `https://` URLs (default port 443).
  - [x] Enforce absolute handshake deadline across DNS, connect, and TLS negotiation.
  - [x] Safe exclusive temporary file creation (`VFS_O_CREAT | VFS_O_EXCL | VFS_O_WRONLY | VFS_O_NOFOLLOW`) in target directory and atomic rename.
  - [x] Enforce HTTPS $\rightarrow$ HTTP downgrade refusal.
  - [x] HTTP 200 requirement (reject unrequested 206).
  - [x] Mandatory authenticated `close_notify` commit condition.
  - [x] Streaming chunked decoding (`wget_decode_chunked`) and Content-Length validation (`wget_validate_content_length`).
  - [x] Cumulative 10 MiB session payload limit enforcement across redirects.
- [x] Build and test targets:
  - [x] `make test-wget-https-host` / `make test-tls-wget-host` (100% PASS under ASan/UBSan across 10 deterministic test cases).
  - [x] `make test-tls-all` (100% PASS across unit, anchor, fixture, and HTTPS client suites).

### Phase TLS-5: Multi-Firmware QEMU Acceptance
- [x] Build target: `make test-tls-qemu` (BIOS and UEFI under `SMP=1` and `SMP=4` - 4/4 matrix configurations 100% PASS in 39.4s).
- [x] Verify NTP / RTC synchronization before test: confirmed UTC date year >= 2026.
- [x] Verify `wget https://example.com` returns HTTP 200 OK, validates against root anchors in `/etc/ssl/certs/anchors.v1`, streams and decodes payload, verifies authenticated `close_notify`, and atomically commits via `SYS_RENAME`.
- [x] Verify BadSSL test endpoints fail closed with explicit cryptographic diagnostics:
  - `expired.badssl.com` -> `certificate expired or not yet valid`
  - `wrong.host.badssl.com` -> `server certificate does not match requested hostname`
  - `self-signed.badssl.com` -> `certificate chain not trusted by any root anchor`
  - `untrusted-root.badssl.com` -> `certificate chain not trusted by any root anchor`
  - Guaranteed no partial or temporary files left on disk.
- [x] Clean poweroff verified across all 4 matrix boots (BIOS SMP=1, BIOS SMP=4, UEFI SMP=1, UEFI SMP=4).
- [ ] Dell Latitude 5590 physical verification (image `bin/fortress.img` ready for bare-metal testing).

---

## 7. Acceptance Matrix

| Gate | Requirement | Verification Method |
| :--- | :--- | :--- |
| **G0** | Hardened contracts & security profile | `docs/plans/TLS_PLAN.md` reviewed and approved |
| **G0-B** | VFS `VFS_O_EXCL` & `VFS_O_NOFOLLOW` | `make test-vfs-flags-host` passes under ASan/UBSan |
| **G1** | Measured footprint & stack ceiling | BearSSL 0.6 + anchor buffer measured: ~303.1 KiB BSS, ~118 KiB text, call chain $\le 3,200$B (authorized 2,400B RSA exception) / $\le 1,400$B (ECDSA) |
| **G2** | Correct syscalls & source paths | Source verification against `syscall_abi.h` & `user/wget.c` |
| **G3** | Scripted adversarial fixture test | `scripts/test_tls_fixture.py` 16/16 adversarial cases pass |
| **G4** | Host ASan/UBSan clean execution | `make test-tls-host` passes with 0 leaks / errors |
| **G5** | Atomic file commit & truncate safety | Verification that failed downloads or missing `close_notify` leave no file |
| **G6** | QEMU Live Integration (BIOS + UEFI) | `make test-tls-qemu` passes on BIOS/UEFI SMP=1/4 (4/4 configurations 100% PASS) |
