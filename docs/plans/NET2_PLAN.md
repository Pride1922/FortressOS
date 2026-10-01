# NET-2 — TCP streams and userspace DNS

2026-10-01. User authorized starting after engineering review and then step 2.
Steps 1–2 are implemented and host verified; live integration remains planned.
This replaces the pasted draft's contradictory lease semantics and pool split.
It does not claim TCP sockets, a live TCP transport, or completed NET-2.

Baseline: UDP retains 16 socket handles, four datagrams each and its existing
five-second receive contract. SYS_NETCTL=42 remains unchanged. Physical UDP
passed on 5590 per the user; its independent capture audit remains pending.
5530 8086:1A1E driver success is a separate user-reported observation.

## Seven execution steps

| Step | Scope | Gate |
| --- | --- | --- |
| 1 | Socket/lifetime design and pure TCP codec | Independent packet/checksum tests under ASan/UBSan; strict kernel build |
| 2 | Bounded transport engine and deterministic simulator | Ordered, exactly-once byte delivery under loss, duplicates, reordering, partial ACKs, wraparound and zero windows |
| 3 | Numeric-IP client, worker and socket integration | Frozen ABI, wait/lifecycle/lock proof; real Ring 3 fixture and client; unchanged UDP/ping gates |
| 4 | Listener, accept and finite server | Backlog/half-open bounds, ownership rollback, listener exit, child independence |
| 5 | Finite nc and complete QEMU matrix | Send stdin, half-close, receive until EOF; BIOS/UEFI × NIC/backend plus BSP SMP smoke |
| 6 | Physical TCP acceptance | Both application directions and saved independent capture, failures/recovery and usable shell |
| 7 | Userspace DNS and shared tool resolution | Bounded parser; UDP with TCP truncation fallback; deterministic responder and LAN evidence |

Step 2 implementation/budget: [transport design](TCP_TRANSPORT_DESIGN.md).
Executed gates and limits: [step 2 report](../roadmap/net2-step2.md).

## Storage and ownership decisions

Keep 16 common socket handles; add at most eight independently allocated static
TCP blocks (`tcp_cb_t`, not the scheduler's `tcb_t`). A stream socket consumes
both resources; UDP can still consume all 16 handles when TCP is unused.
Initial TCP byte buffers: 8192 TX + 8192 RX per block (131072 bytes total).
This limits outstanding bytes, not total connection transfer size. Tests must
transfer substantially more data than fits in either buffer.

Retransmission metadata references retained TX byte ranges, not duplicate packet
payloads. Partial ACKs trim ranges; no acknowledged bytes are retransmitted.
All transmitted-but-unacknowledged bytes remain retained across syscall return,
reservation expiry, STOP and final fd close until ACK or transport failure.
RX reservations do not consume bytes before successful user-copy commit.

State includes tuple/generation, byte-buffer indices, snd_una/nxt/wnd/wl1/wl2,
rcv_nxt, FIN positions, MSS, cwnd/ssthresh, duplicate ACK/recovery state,
SRTT/RTTVAR/RTO, retransmission descriptors and bounded out-of-order metadata.
Record separate retransmission, persist, handshake/user, FIN_WAIT_2 and TIME_WAIT
deadlines. No aggregate counter substitutes for these distinct lifetimes.
Before step 2, publish actual sizeof and a concrete descriptor/OOO/TIME_WAIT
pool budget. The 128 KiB buffer total is not a complete memory claim.

Final fd close detaches the application; it does not immediately recycle an
active transport block. FIN follows accepted TX bytes. Worker owns teardown;
all queued actions carry a connection generation. Listener destruction aborts
unaccepted children; accepted children have independent ownership. Graceful
close with unread data versus abort policy must be frozen before integration.

TIME_WAIT uses a separate bounded lightweight tuple/sequence/expiry pool, freeing
large buffers after transition. Its allocation must be reserved before active
close can require it; pool exhaustion cannot silently skip TIME_WAIT. Keep
tuple reuse excluded until expiry. Generation protects internal reuse; wire
sequence validation and initial-sequence policy protect delayed old segments.
Initial-sequence generation and reboot tuple safety need an explicit design,
not a guess based on the coarse timer or a claimed cryptographic entropy source.

## Transport profile

IPv4 LAN first, no fragments/reassembly or PMTU discovery. Unscaled receive
window reflects actual free storage. No window-scale offer, SACK, timestamps,
ECN negotiation, urgent-data application API or TCP offload in the first pass.
Parse safely and ignore well-formed unsupported options. MSS is negotiated:
peer MSS/default plus local MTU/header limits determine transmit payload.
The default Ethernet ceiling does not authorize always sending 1460 bytes.

Reno includes slow start, congestion avoidance, fast retransmit/recovery and
timeout response. Adaptive RTO uses SRTT/RTTVAR and Karn's rule, starting at
one second, backing off conservatively. Connection/user deadlines are separate
from RTO and reservation leases. Persist probes, lost window updates and
zero-window behavior are required even when buffers are small.
Control/data flags, acceptable sequence/ACK/window ranges, FIN ordering, RST
validation, simultaneous open/close and unsupported traffic policy belong in
the simulator gate, not only in a successful echo test.

## Worker and syscall boundary

Keep sole BSP protocol ownership, 64 RX packets per pass and existing tick wake.
Before integration specify TX/control/timer budgets and prove UDP/ICMP progress
and shell responsiveness under continuous TCP RX. Sweep eight blocks per pass;
deadline processing occurs even after a full RX batch. Idle still sleeps.
No scheduler/signal/lock-rank/wait signature, new timer hook, DMA or driver change.

Copy immutable action snapshots under the socket lock; drop it before protocol,
ARP or NIC work; validate generation before completion publication. Distinguish
accepted bytes, submitted sequence space and ACKed bytes. Local ARP delay/NIC
backpressure is not a successfully transmitted segment or network-loss sample.
Wait predicates use atomics only under scheduler lock; no rank-1 nesting.

See [TCP_SOCKET_ABI.md](TCP_SOCKET_ABI.md) for intended stream behavior. Its
indefinite blocking contract requires a proof before step 3: reservation expiry
wakes a continuation to re-register safely, without exposing a spurious timeout,
retaining user pointers, consuming RX or cancelling accepted TX. A resumed stale
continuation cannot commit into a replacement generation. If this cannot be
proved with existing primitives, stop and report an explicit timed-feature
fallback; do not silently substitute EAGAIN every five seconds.

## DNS and tools

First nc is finite request/response; interactive full-duplex needs a separate
readiness design. TCP sockets should support existing read/write with explicit
pointer/error/SIGPIPE integration plus send/recv flags; prove this in step 3.
DNS stays a userspace library used by tools, not shell command parsing. Numeric
addresses bypass DNS. Specify a userspace-readable resolver configuration (an
explicit tool/server argument first is sufficient); boot dns= alone is not an
implemented userspace interface. Gateway is not presumed to be a DNS server.
Bound names, compression traversal, CNAME chains and response size; match peer,
transaction ID and question. Truncation triggers TCP fallback with bounded
length-prefixed framing. No DNS integration is needed for step 6 numeric-IP tests.

## Evidence and stop conditions

Codecs are not transport acceptance. Simulator is not live Ring 3 acceptance.
QEMU is not physical acceptance. Do not create a make test-net-tcp-physical
target that could be mistaken for an automated physical pass.
Use disposable no-data-disk fixtures and existing strict preflights. Run related
host suites at each stage; live integration requires UDP/ping/ARP/ring/absent-NIC
regressions and signal/pointer fixtures. Only record commands actually executed.
Stop if protected contracts require changes, queues become unbounded, generation
or lock exclusion cannot be proved, or independent capture disagrees unexplained.

Sources: [TCP](https://www.rfc-editor.org/rfc/rfc9293.html),
[RTO](https://www.rfc-editor.org/rfc/rfc6298.html),
[congestion control](https://www.rfc-editor.org/rfc/rfc5681.html).
