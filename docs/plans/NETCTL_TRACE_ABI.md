# NETCTL_TRACE_PROBE v1 — proposal for review

Status: APPROVED FOR IMPLEMENTATION (2026-10-03), with the user's timeout, distinct-label and no-contention-retry clarifications below. Review owner: the user. Existing NETCTL_PING, IFGET and IFSET remain unchanged; executable verification is still required.

## Call and layout

Propose `SYS_NETCTL=42`, new command `NETCTL_TRACE_PROBE=4` (currently unused). RDI=command, RSI=writable in/out address, RDX=64 exactly. Alignment 8; fields are host order except IPv4 addresses, which use the existing project network-order representation. Implementation must assert all offsets and size.

| Offset | Field | Type | Bytes | Contract |
| --- | --- | --- | --- | --- |
| 0 | version | uint32_t | 4 | input, exactly 1 |
| 4 | destination | uint32_t | 4 | input, unicast IPv4 |
| 8 | ttl | uint32_t | 4 | input, 1–30 |
| 12 | timeout_seconds | uint32_t | 4 | input, 1–5 |
| 16 | sequence | uint32_t | 4 | input, 0–65535; CLI label, not wire identity |
| 20 | outcome | uint32_t | 4 | output, zero on input |
| 24 | responder | uint32_t | 4 | output, router/destination IPv4; zero on input |
| 28 | icmp_type | uint8_t | 1 | output, zero on input |
| 29 | icmp_code | uint8_t | 1 | output, zero on input |
| 30 | reserved0 | uint16_t | 2 | input, zero |
| 32 | rtt_ticks | uint64_t | 8 | output, zero on input |
| 40 | tick_hz | uint64_t | 8 | output, zero on input |
| 48 | deadline_ticks | uint64_t | 8 | input, absolute BSP deadline shared by the whole trace |
| 56 | reserved1 | uint64_t | 8 | input, zero |

CLI: `traceroute [-m MAX_TTL] [-q PROBES] [-W SECONDS] IPV4`. Defaults 30/3/1; accepted ranges 1–30/1–3/1–5. Obtain BSP ticks/frequency through SYS_SYSINFO, calculate one checked 120-second command deadline and pass it unchanged. No hostname resolution in v1. No sleeps or busy polling.

Each probe uses a distinct sequence label, starting at 1 and increasing across hops (at most 90 labels); probes at one hop do not share a label. The label is not shown in output and is independent of the kernel wire identity.

Output starts `traceroute to ADDRESS, MAX_TTL hops max`; each probe prints one result line beginning with its hop number and followed by `ADDRESS RTT ms` or `*`. An unreachable probe appends `!N`, `!H`, `!P`, `!PORT`, `!FRAG` or `!ROUTE` for codes 0–5 respectively. Stop on destination reply, supported unreachable or total budget exhaustion. Exit 0 only on destination reply; exit 1 on timeout/unreachable/syscall error/interruption, 2 on usage error. Tick resolution, not wire resolution, bounds reported RTT precision. A timeout does not prove the router is unreachable.

On SYSCALL_EAGAIN (ping/trace contention), traceroute reports contention and exits 1 without retrying. Retry is the user's responsibility.

An EINTR returned to the tool exits 1; default signal termination keeps the existing shell semantics (Ctrl-C/SIGINT produces shell status 130), without installing a new signal handler.

## Outcomes and error precedence

Successful syscall return 0 publishes the full result even when the network outcome is unsuccessful:

| Value | Outcome | Fields |
| --- | --- | --- |
| 0 | DESTINATION_REPLY | responder=destination; type/code=0/0; RTT valid |
| 1 | HOP_EXPIRED | responder=router; type/code=11/0; RTT valid |
| 2 | UNREACHABLE | responder=router/peer; type=3; code=0–5; RTT valid |
| 3 | PROBE_TIMEOUT | responder/type/code/RTT=0; frequency valid |
| 4 | ARP_TIMEOUT | responder/type/code/RTT=0; frequency valid |
| 5 | TX_FAILED | responder/type/code/RTT=0; frequency valid |

Ignore unsupported ICMP error codes; they do not terminate the probe. In v1 this includes administrative-unreachable codes beyond the RFC 792 code set. Existing ping reply handling must still run.

Entry precedence follows existing NETCTL dispatch: BSP affinity check (`SYSCALL_EOPNOTSUPP=-14`), exact size (`EINVAL=-1`), writable range (`EFAULT=-2`), copied scalar/version/reserved/output/address/deadline validation (`EINVAL`), unavailable interface/clock (`EIO=-9`), expired deadline (`ETIMEDOUT=-30`), occupied ping/trace facility (`EAGAIN=-21`). The plan's generic "EBUSY" wording maps to existing EAGAIN; do not invent a new errno. All reserved/output fields are checked on every entry.

A future deadline must be at most 120 seconds away using checked tick arithmetic; an already-expired deadline returns ETIMEDOUT without publication. Caught signal cancellation or expired/stale result lease returns EINTR=-22. Revalidate the writable user range before final copy-out; a failure there returns EFAULT. Negative syscall returns leave the user result unpublished. Worker link loss terminates the pending operation with EIO, without changing driver behavior.

Probe timeout includes routing/ARP and starts when the worker accepts the request. Bound any ARP attempt by the remaining probe and whole-command budgets; do not add a fresh ARP budget after the deadline. RTT starts at successful NIC submission. Result grace is two seconds after publication; it is resource cleanup time, not added probe time.

The effective probe deadline is the earlier of its per-probe deadline and the unchanged whole-command deadline. If the whole-command deadline is reached mid-probe, cancel that probe, publish PROBE_TIMEOUT, and terminate the command; do not start another probe or retry.

## Correlation and finite ownership proof obligations

Share the finite ping/trace admission resource: exactly one operation owns it, with EAGAIN for other callers. Preserve ping's existing ABI and outcomes. Mailbox contents are copied scalars/results; no retained user pointers, task pointers or fd reservations. Use generation-checked collect/cancel and the existing scheduler wait/wake APIs.

Allocate a monotonically increasing 32-bit wire identity for trace probes, split across echo identifier/sequence; the caller's sequence is only a display label. Do not reuse a pair during the boot: refuse further allocation with EAGAIN at exhaustion. Generation checks independently protect local publication. This excludes same-boot delayed-quote reuse, but is not authentication or a cross-reboot stale-packet guarantee; document that boundary. Echo payload contains the normal fixed 32-byte pattern plus generation correlation.

Time Exceeded/Unreachable quotes match the original local/destination IPv4 addresses, ICMP protocol, unfragmented Echo Request type/code and allocated identifier/sequence. Require at least the quoted IPv4 header plus eight ICMP bytes; do not require the full original IP total length or token payload. Validate the complete outer ICMP checksum; do not try to validate a full inner ICMP checksum on a partial quote. Validate quoted IPv4 header bounds/checksum and reject fragment offsets/MF. Router source may differ from destination. Destination Echo Reply additionally requires the full expected echo payload and exact destination source.

The sole BSP worker performs protocol mutation. Mailbox locks never nest with stack/device/scheduler locks; release before protocol calls and wakes. Wait predicate uses atomic active/done generations only under the scheduler lock. Cancellation/expiry invalidates the generation, cancels protocol state and wakes waiters. Hard exit/STOP cannot hold admission indefinitely: the finite worker lease reclaims it; resumed stopped callers receive EINTR after lease expiry. No new process-exit hook, scheduler primitive, signal behavior or timer hook.

Trace packets use IPv4 ID 1, while existing ping packets keep ID 0. Error quotes also match that IP ID, preventing an old ping quote from aliasing a trace's shorter wire identity; this does not change the approved syscall layout or ping's wire bytes.

The approved design has been checked against the actual worker order: the existing `net_ping_worker_tick` calls before and after `net_ipv4_tick` service both kinds, with no added timer/wait primitive. The implementation's finite ownership argument is in [TRACE_WAIT_LIFECYCLE_PROOF.md](TRACE_WAIT_LIFECYCLE_PROOF.md). Before landing, host tests must prove stale generations, signal/STOP/exit leases, range revalidation, busy contention and link loss; BIOS/UEFI synthetic router tests must prove TTL progression and independent quoted-wire matching. Existing ping/TCP/UDP/DNS/link/configuration fences must pass. Physical multiple-hop acceptance is a separate topology-dependent gate.
