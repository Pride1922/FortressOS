# NETCTL_PING v1 — concrete Phase 4 contract

2026-10-01; fixed layout agreed for implementation in Checkpoint C.
`SYS_NETCTL=42`; `NETCTL_PING=1`; arguments RDI=command, RSI=in/out address,
RDX=48 exactly. Number 38–41 remains unimplemented/reserved for Phase 5.
All fields host-order except destination IPv4 (network order).

| Offset | Field | Type | Bytes | Direction |
| --- | --- | --- | --- | --- |
| 0 | version | uint32_t | 4 | in, exactly 1 |
| 4 | destination | uint32_t | 4 | in, network-order unicast IPv4 |
| 8 | timeout_seconds | uint32_t | 4 | in, 1–5 |
| 12 | sequence | uint32_t | 4 | in, 0–65535 |
| 16 | outcome | uint32_t | 4 | out; zero on input |
| 20 | echoed_bytes | uint32_t | 4 | out; zero on input |
| 24 | rtt_ticks | uint64_t | 8 | out; zero on input |
| 32 | tick_hz | uint64_t | 8 | out; zero on input |
| 40 | start_delay_ms | uint32_t | 4 | in, 0 or 1000 for non-spinning CLI pacing |
| 44 | reserved | uint32_t | 4 | in, zero |

Size 48, alignment 8; compile-time assertions enforce size/offsets.
Echo data is a fixed 32-byte generated pattern including generation token.
Reply outcome=0, ARP timeout=1, echo timeout=2, TX failure=3.
Successful syscall return is 0 even for transport outcomes; inspect outcome.

| Return | Project macro | Meaning |
| --- | --- | --- |
| -1 | SYSCALL_EINVAL | unknown command, wrong size/version/fields/address |
| -2 | SYSCALL_EFAULT | inaccessible/read-only input-output user range |
| -9 | SYSCALL_EIO | no active interface/worker or unusable tick frequency |
| -14 | SYSCALL_EOPNOTSUPP | caller not pinned to BSP |
| -21 | SYSCALL_EAGAIN | mailbox busy |
| -22 | SYSCALL_EINTR | caught signal cancellation or expired/stale lease |

Fallback selected for Phase 4: **no scheduler, signal or process-exit changes**.
One worker-serviced finite lease covers pacing, three-second ARP budget,
echo timeout and two-second result collection grace. Owner hard-exit or stop
cannot retain the mailbox forever; expiry invalidates generation and wakes
old waiters. Generation checks prevent stale collection/cancel affecting a
new caller. Catchable interruptions use existing wait semantics if supported;
prompt cancellation extensions otherwise remain Phase 5. Default Ctrl-C can
terminate the caller under existing signal handling, with lease reclamation.
Do not add owner-exit hooks. Stopped callers may resume with EINTR after expiry.

Validate writable range before copy-in and again before copy-out. Never retain
user pointers/TCBs; only copied scalar request/result values enter the mailbox.
