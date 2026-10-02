# NET-2 — TCP streams and userspace DNS

2026-10-01. User authorized starting after engineering review and then step 2.
Steps 1–7 are complete; NET-2 is 7/7 done (2026-10-02). Step 7 DNS + nslookup
has host/QEMU evidence and user-confirmed Dell 5590 physical acceptance.
Step 5 adds finite nc and the independent TCP socket fixture/matrix; executed
acceptance evidence is recorded in [Step 5](../roadmap/net2-step5.md).
This replaces the pasted draft's contradictory lease semantics and pool split.
Step 6 physical TCP acceptance on Dell 5590 is user-confirmed in the
[physical checkpoint](../roadmap/net2-step6.md). This does not close NET-2.

2026-10-02 review revision: concrete decisions and prerequisites below replace
the remaining open-ended close/ISN/tool choices. They are planned behavior,
not new implementation or a claim that the step 3 proof has passed.

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
TCP connections (`tcp_conn_t`, not the scheduler's `tcb_t`). A stream socket consumes
both resources; UDP can still consume all 16 handles when TCP is unused.
Step 3 renamed the pure type/API to `tcp_conn_t` / `tcp_conn_*`, including tests
and layout docs; no compatibility alias or layout change. Source filenames and
historical step 2 test target remain tcp_tcb. Socket creation reserves a block
and TIME_WAIT record with a never-transmitted placeholder tuple; CONNECT
initializes the real tuple. This preserves all 16 UDP handles when TCP is idle.
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
close is the chosen policy even with unread data: discard local unread bytes,
retain accepted TX, and send FIN after those bytes. The orphan worker must drain
and discard subsequently received ordered bytes so an abandoned RX window
cannot stall teardown. Preserve sequence validation, OOO bounds and FIN order;
do not discard gaps by advancing rcv_nxt. Existing orphan deadlines may still
abort a stalled connection. This is not a promise of peer delivery. Explicit
SHUT_WR keeps receive available; applications use it when they need the response.
Step 3 implements/tests this in the worker after pure detach; it consumes only
contiguous ordered RX, leaving gaps bounded until data arrives or teardown ends.

TIME_WAIT uses a separate bounded lightweight tuple/sequence/expiry pool, freeing
large buffers after transition. Its allocation must be reserved before active
close can require it; pool exhaustion cannot silently skip TIME_WAIT. Keep
tuple reuse excluded until expiry. Generation protects internal reuse; wire
sequence validation and initial-sequence policy protect delayed old segments.
Initial-sequence generation and reboot tuple safety need an explicit design,
not a guess based on the coarse timer or a claimed cryptographic entropy source.

Step 3 ISN policy: `ISN = M + mix(tuple, boot_salt)` modulo 2^32, where
M advances at 250000 sequence units per second using the existing BSP timebase.
For coarse ticks, allocate successive opens distinct M values with a bounded
counter. The uint32 clock wraps modulo 2^32; modular comparison selects at least
the previous clock plus one for equal/backward samples. Mix host-order address
words and packed host-order local/remote ports, independent of struct padding.
The salt mixes `apic_timer_get_bsp_ticks()` at net_tcp_init with the local IP;
it is predictable and may repeat across boots. Never
call that timestamp random entropy or assume microsecond resolution. No new timer,
RTC driver or scheduler primitive is authorized by this proposal.

This non-cryptographic mixer is a limited LAN profile, not RFC 9293's secret-key
PRF or a guarantee against sequence prediction or cross-boot collisions.
TIME_WAIT records do not survive reboot. Implemented conservative reboot policy:
hold TCP output/opens until 120 seconds of BSP uptime (one chosen MSL), keeping
the shell, UDP and ICMP usable; report the condition explicitly rather than
silently hanging CONNECT: EAGAIN without starting a handshake, accompanied by a
boot diagnostic. Removing quiet time requires an explicitly reviewed risk decision or
a stronger persistent/entropy-backed design. Source/formula and coverage limits
are in the step 3 report; injected simulator ISNs do
not establish production ISN correctness.

## Transport profile

IPv4 LAN first, no fragments/reassembly or PMTU discovery. Unscaled receive
window reflects actual free storage. No window-scale offer, SACK, timestamps,
ECN negotiation, urgent-data application API or TCP offload in the first pass.
Parse safely and ignore well-formed unsupported options. MSS is negotiated:
peer MSS/default plus local MTU/header limits determine transmit payload.
An omitted IPv4 MSS option means 536 bytes; send payload is limited by
min(peer MSS or 536, local MTU minus actual IP/TCP headers, implementation cap).
The 536 default and fixed-header MTU clamp already exist in the step 2 engine.
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
The 64-packet RX cap is per worker pass, not per timer tick.
Each net_tcp_input restores incoming IRQ state, so its IRQ-off scope is per
segment, not continuous across the RX batch; this is not a measured latency bound.
Before integration specify TX/control/timer budgets and prove UDP/ICMP progress
and shell responsiveness under continuous TCP RX. Sweep eight blocks per pass;
deadline processing occurs even after a full RX batch. Idle still sleeps.
No scheduler/signal/lock-rank/wait signature, new timer hook, DMA or driver change.

Step 3 uses one BSP IF-clear prepare/submission/commit transaction, with no
socket lock across ARP/NIC work. AP final-close only publishes detach requests;
it cannot mutate the protocol connection/action.
Every tcp_conn_tick invokes changed(), advancing the revision and invalidating
prepared actions. No tick, RX input, consume, sleep or reentrant protocol work
may occur for that connection between prepare and commit, including a failed
local submission committed with submitted=false. Each block transaction must
finish before the later tcp_pool_tick maintenance sweep; the worker traps if
commit rejects its prepared action. Step 4 listener/accept refactors must
preserve this ordering and BSP IRQ exclusion.
Syscalls queue TX/consume RX in
their own IF-clear continuation; worker owns RX/timers/wire activity. Distinguish
accepted bytes, submitted sequence space and ACKed bytes. Local ARP delay/NIC
backpressure is not a successfully transmitted segment or network-loss sample.
Wait predicates use atomics only under scheduler lock; no rank-1 nesting.

See [TCP_SOCKET_ABI.md](TCP_SOCKET_ABI.md) for the frozen client ABI. The review's
renewing-reservation sketch was replaced before integration by reservation-free
sleeping continuations: snapshot endpoint identity/event, check availability,
sleep with atomics, then retry/revalidate. No RX/TX ownership is held over sleep
and no user pointer enters protocol storage. Accepted TX belongs to the connection.
A stale continuation cannot commit into a replacement generation. No periodic
user EAGAIN or timed receive fallback was introduced.
The required named artifact is [TCP_WAIT_LIFECYCLE_PROOF.md](TCP_WAIT_LIFECYCLE_PROOF.md).
It records the reviewed client lifetime/lock argument and runtime evidence;
ACCEPT has the reviewed proof extension and matching implementation with host
rollback and real Ring 3 lifetime evidence in [step 4](../roadmap/net2-step4.md).
Step 4 starts with [NET2_STEP4_ADDENDUM.md](NET2_STEP4_ADDENDUM.md): rerun the
already implemented client fence before listener code, then freeze ACCEPT
ownership/ABI and bounded backlog policy before integration. Do not treat
the UDP one-shot continuation as evidence for indefinite stream waits. A failed
proof triggers discussion of an explicit receive timeout returning ETIMEDOUT;
that narrower feature and its timeout interface are not pre-approved here.

## DNS and tools

Step 5 begins with [the TCP socket-backend fixture plan](NET2_STEP5_SOCKET_FIXTURE.md)
before nc or matrix implementation. Step 5 implements the synthetic bidirectional
TCP peer and socket matrix alongside independent SLIRP user networking.
The prerequisite defines wire roles, sequence/ACK/FIN accounting, bounded
loss profiles and independent capture gates against Step 4 boundary f925aa1.

First nc has this concrete serial request/response interface:

- `nc <IPv4> <port>` connects; copies stdin to the socket with short-write
  handling; stdin EOF triggers SHUT_WR; then copies receive bytes to stdout
  until peer EOF, drains buffered bytes before reset/error and exits nonzero
  on I/O failure. Empty stdin is a valid zero-byte request.
- `nc -l <port>` accepts one connection, closes the listener, then uses the same
  stdin -> SHUT_WR -> receive-until-EOF sequence for nonterminal stdin. Terminal
  stdin is skipped using existing TERM_ISATTY, so plain nc -l is receive-only.
  It does not automatically echo
  inbound data or accept another client.
- No simultaneous stdin/socket forwarding, early exit merely on stdin EOF,
  `-k`, UDP, scanning, `-e` or `-c`. Numeric IPv4 only until step 7; no automatic
  HTTP request. `echo hello | nc <IPv4> <port>` sends those bytes and waits for
  the response after half-close.

Both peers must consume the request while it is sent; a peer streaming a large
response before consuming the whole request can deadlock this serial tool.
Document this limitation and use small finite requests or a cooperating peer
in its gates. Interactive/full-duplex and arbitrary bidirectional bulk transfer
need a separate readiness design; the transport can still be tested with a
dedicated fixture. TCP sockets should support existing read/write with explicit
pointer/error/SIGPIPE integration plus send/recv flags; prove this in step 3.
DNS stays a userspace library used by tools, not shell command parsing. Numeric
addresses bypass DNS. Specify a userspace-readable resolver configuration (an
explicit tool/server argument first is sufficient); boot dns= alone is not an
implemented userspace interface. Gateway is not presumed to be a DNS server.
Step 7 starts with `nslookup -s <server-IPv4> <name>`; no implicit server default.
The [Step 7 plan](NET2_STEP7_DNS.md) and approved [DNS API](DNS_USER_API.md)
specify the implementation. The approved [TCP deadlines](TCP_IO_DEADLINE.md)
are implemented; [Step 7 evidence](../roadmap/net2-step7.md) separates host/live
results from the user-confirmed physical DNS gate.
Shared tool resolution takes an explicit server argument; hostname-enabled nc
uses `nc -s <server-IPv4> <host> <port>`. Numeric targets bypass the resolver.
File-based resolver configuration and environment defaults are deferred, with
no shell or filesystem prerequisite for deterministic DNS tests.
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
ISN/reboot discussion: RFC 9293 sections 3.4.1–3.4.3; MSS default: section 3.7.1.
