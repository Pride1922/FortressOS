# Networking Phase 4 — IPv4 and ICMP Echo

Status: Phase 4a implemented (2026-10-01); physical 4b LAN-peer gate accepted (2026-10-01, manual/user-supplied evidence).
Approved implementation plan; verification lives in the roadmap. Companion to
[NET_PLAN.md](NET_PLAN.md), with implemented headers taking precedence.

Implementation update: Phase 4a implementation and verification are recorded in
[net-phase4a.md](../roadmap/net-phase4a.md). The concrete ABI and narrower
owner-exit fallback were specified before Checkpoint C in
[NETCTL_PING_ABI.md](NETCTL_PING_ABI.md). Physical 4b is accepted (2026-10-01, manual/user-supplied evidence — see [net-phase4b.md](../roadmap/net-phase4b.md)). The
checkpoint descriptions below preserve the approved planning baseline.

## 1. Goal and phase boundaries

Phase 4a delivers IPv4 unicast delivery, ICMP Echo Request/Reply, and a real
Ring 3 `/bin/ping`. Gates: a controlled host peer receives a correct echo
reply from FortressOS, and `/bin/ping 10.0.2.2` succeeds with QEMU user networking.
Phase 4b separately verifies a physical Dell LAN exchange with capture evidence.
Complete 4a before hardware experiments; do not infer 4b from QEMU.

Keep polling-only ingress, the BSP worker, existing timer wakeup and driver
callbacks. No driver initialization/PHY workarounds, DMA ownership, scheduler,
lock ranks, interrupt registration, syscall entry assembly, or address-space
ownership changes. UDP/socket APIs remain Phase 5. No forwarding, fragmentation,
reassembly, DHCP, DNS, TCP, IPv6 or general raw sockets. This is a deliberately
limited IPv4/ICMP subset, not full IPv4 host conformance.

## 2. Established baseline and gaps

Read PROTECTED.md, AGENTS.md §§4/7.1/7.5/7.6/9, and the networking annex first.
Then inspect these sources and callers before implementation:

| Source | Implemented contract / implication |
| --- | --- |
| `src/net/net.c`, `net.h` | Single BSP owner; static ARP/cache/TX scratch; IPv4 currently dropped. `arp_resolve` returns hit=0, resolving=1, failure=-1; it has no retry timer. |
| `src/include/net.h` | Callback ownership and 2048-byte pbuf backing; unlocked device calls. |
| `src/net/ipv4.c`, `ipv4.h` | Decoder checks header lengths/checksum/fragments and returns bounded payload excluding Ethernet padding. Copied IPv4 words remain wire-order. Encoder emits a 20-byte header, DF and TTL 64 by default. |
| `src/net/checksum.c`, `checksum.h` | Checksum result must be serialized in network order. Odd-length coverage exists. |
| `src/drivers/e1000.c` | TX copies caller bytes; RX is recycled exactly once by stack. Driver already enforces MTU/frame bounds and contains fatal failures. |
| `src/arch/x86_64/apic.c` | BSP timer calls `net_timer_tick` after EOI; network channel wakes each tick. |
| `src/kernel/thread.h` | Wait predicates run under scheduler lock and cannot acquire another lock; current documented blocking API is BSP-only. |
| `src/include/syscall_abi.h` | Implemented numbering ends at 37; planned 38–41 sockets and 42 NETCTL are not present. |
| `src/kernel/syscall.c`, `src/mm/vmm.h` | User range validation and existing signal-interrupted blocking conventions. |
| `user/`, `Makefile`, `scripts/test_net_eth.py` | Freestanding Ring 3 packaging, disposable QEMU fixture, exact argv and pcap checks. |

Phase 3 evidence supplied in this chat: working physical shell and a boot log
reporting gateway ARP resolution; second-host Wireshark frame 508 is a 60-byte
broadcast ARP request from C8:F7:50:0E:35:80, asking for 192.168.0.1 from
192.168.0.168. The source-MAC filter excludes the incoming gateway reply.
This establishes the reported boot/resolution and outbound capture, not ICMP,
sustained traffic or idle CPU measurements. Preserve the source photos/manual
attribution when recording this evidence; do not invent a saved pcap file.

Two gaps require explicit treatment:

1. `/bin/ping` cannot use a networking ABI that does not exist. Review the
   narrow NETCTL proposal below before coding that checkpoint. A boot-only
   probe does not satisfy the `/bin/ping` gate.
2. The worker is the only stack owner. Ring 3 dispatch must submit copied
   requests to it; it must never call `arp_resolve`/IPv4 TX directly, even on
   BSP, because preemption would make shared scratch reentrant.

## 3. Checkpoint A — pure ICMP codecs and IPv4 validation

Add `src/net/icmp.h`/`icmp.c`, independent of scheduler, heap and NIC. Implement
an 8-byte Echo header: type, code, checksum, identifier and sequence. Support
type 8 request and type 0 reply with code 0. Define host-order decoded
identifier/sequence explicitly; encode them to network order. Verify the
checksum over exactly the ICMP message, including odd-length payloads. A reply
preserves identifier, sequence and every payload byte while changing type and
recalculating checksum. Wire requirements: [RFC 792](https://www.rfc-editor.org/rfc/rfc792).

Use explicit bounds before arithmetic and copies. Review `ipv4_encode`'s
20+payload length cast: reject overflow and packets beyond interface MTU before
encoding, and strengthen the pure encoder's overflow rejection if needed.
Never verify options against a copied 20-byte struct: the incoming decoder
checks the original bounded bytes, whereas an output header copy lacks options.
For this phase explicitly reject IHL != 5 before stack delivery; document the
subset rather than silently claiming IP option processing.

Gate: proposed `make test-net-icmp-host` with actual codecs under ASan/UBSan.
Test known independent bytes, odd/even/zero echo data, every truncated header
length, checksum corruption, unsupported type/code, encoder bounds and IPv4
length overflow. No kernel or driver mocks needed for pure codecs.

## 4. Checkpoint B — worker-owned IPv4/ICMP stack

Extend Ethernet demux to `ipv4_input`. After `ipv4_decode`, accept only local
unicast destination, TTL > 0, supported header shape and unfragmented ICMP.
Reject invalid/unusable source addresses and broadcast/multicast echo targets;
never answer broadcast echo. Explicitly define source policy for unspecified,
multicast, limited broadcast, subnet broadcast, and loopback. Unsupported IP
protocols and ICMP error messages drop; generating ICMP errors is deferred.
Relevant host requirements: [RFC 1122](https://www.rfc-editor.org/rfc/rfc1122).

TX route selection uses the configured prefix: compare host-order IPs under
an overflow-safe mask, ARP for destination on-link and gateway off-link. The
IP destination stays the final peer; only the Ethernet destination changes
to next-hop MAC. Reject routes without a usable configured next hop.
Do not blindly use ingress Ethernet source as the routed reply destination.

Use static MTU-sized TX scratch (up to 1514 Ethernet bytes), separate from
ARP scratch. Pad short Ethernet frames to 60 with zeroes; IPv4 total length
and ICMP checksum exclude Ethernet padding. Encode fresh IP headers with
local source, reply peer destination, TTL 64 and DF. No buffer allocation or
RX DMA reuse for TX. `net_input` remains the sole RX recycler on every path.

Cold ARP needs bounded state, not a wait inside demux. Proposed fixed storage:
one outbound ping transaction plus one pending inbound echo-reply frame.
Copy the reply data before recycling RX; a full pending reply slot drops the
new request with a counter. Pending state expires after three ARP attempts,
spaced one second apart; absolute ARP budget three seconds. Coalesce attempts
for a shared next hop. Worker services pending state each tick; do not call
`arp_resolve` on every tick and flood ARP. No general packet queue.

Gate: host stack tests with actual demux and mocked callbacks. Verify exact
reply bytes, on-link/off-link route selection, zero padding, cold/warm ARP,
retry timing, full-slot drops, expiry, invalid input and send failure. Assert
one RX recycle per input and no retained DMA packet pointers. Add an opt-in
`net_test=icmp` worker probe for early QEMU integration; preserve rings/ARP
selftests and normal shell startup.

## 5. Checkpoint C — narrow Ring 3 ping control ABI (review before coding)

Recommended proposal: implement planned `SYS_NETCTL=42` with a single
`NETCTL_PING` command initially. Keep 38–41 reserved for Phase 5; do not
implement sockets or assign an unrelated syscall number for temporary tests.
This is a new dispatcher case and shared ABI, not syscall-transition machinery.

Proposed call: `(command, ping_request_v1 *, exact_size)` with a fixed-size
versioned in/out structure containing destination IP (network order), sequence,
bounded echo timeout, outcome, echoed byte count, RTT ticks and tick frequency.
Reserved fields must be zero. Fixed 32-byte kernel-generated echo data includes
a transaction token; cap echo timeout to 1–5 seconds. Return transport outcomes
through defined status fields; reuse project syscall errors for invalid ABI,
EFAULT, busy, interruption and unsupported execution context. Review concrete
struct layout, error mapping and ABI tests before committing it.

Validate the entire structure for writing before reading it, copy in once,
validate fields, and keep only values. Revalidate output before copying back.
No saved user pointer, process/TCB pointer, or RX pbuf in network transaction
state. Malformed requests must not transmit anything.

Use one BSS mailbox with generation token, copied request and copied result.
A rank-1 ordinary mailbox lock protects reservation/state; release it before
device/stack calls, console, user copies and scheduler operations. Worker copies
the submitted command, drops the lock, then owns ARP/TX/ICMP state. Match echo
replies by peer/local IP, identifier, sequence and exact generated data/token;
ignore unsolicited, duplicate, stale and mismatched replies. RTT starts when
echo TX is submitted, excludes ARP delay, and uses BSP tick frequency rather
than hardcoded conversion. Zero-tick RTT is allowed at timer resolution.

Initial ABI is BSP-only: verify caller execution/affinity before blocking and
return an explicit unsupported-context error for AP callers. Do not extend
the scheduler's blocking contract as incidental networking work. Shell ping
acceptance may run with SMP enabled but is not AP protocol acceptance.

Caller sleeps through existing `sched_wait_until` on a separate ping-completion
channel. The predicate acquire-reads an atomically published completion/token
only; it never takes the mailbox lock. Worker publishes completion before
`sched_wake_all`, after dropping mailbox/device locks. Worker remains serviced
by the existing network tick channel; no extra timer hook or NIC IRQ is needed.

Specify SUBMITTED -> ARP_WAIT -> ECHO_WAIT -> DONE/FAILED/CANCELLED transitions.
One active transaction means competing callers return busy. Timeout, TX failure,
STOP/CONT and caught signals must unwind through existing blocking conventions.
Cancellation uses the generation token so an old continuation cannot cancel a
new request. A hard-killed owner must not retain the slot indefinitely: give
each reservation a finite lease covering ARP + echo + result collection, with
worker-only lease reclamation and atomic invalidation. A resumed stale waiter
returns expired/interrupted, never another caller's result. Review and test the
lease/exit interactions before implementation; if they require process-exit,
scheduler or signal machinery changes, stop and discuss rather than add hooks.

## 6. Checkpoint D — `/bin/ping` and QEMU acceptance

Add `user/ping.c`, existing-style entry/linker integration and `/bin/ping`
initramfs staging. Initial CLI: `ping [-c count] [-W seconds] <numeric IPv4>`;
default four requests, one second between probe starts, bounded count and
timeout. No DNS or arbitrary payload size. Print per-probe reply/timeout and
sent/received/loss plus RTT summary; distinguish ARP failure from echo timeout.
Return success only when the chosen documented success criterion is met
(recommend at least one reply); bad arguments and all-loss runs return failure.

Do not busy-poll between probes. Implement pacing in the mailbox/worker request
schedule, allowing the syscall caller to sleep until the next permitted start.
The single-transaction lease must account for at most one second pacing plus
ARP/echo budgets. User data and format buffers remain small/BSS-static; check
512-byte individual stack frame budget and report transitive kernel stacks
separately. Ctrl-C should return promptly and leave the shell usable; job
STOP/CONT must not hold stack locks or leak a reservation.

Proposed targets: `test-net-ipv4-host` (stack), `test-net-ping-host` (mailbox/ABI
adapters), and `test-net-icmp` (QEMU integration). BIOS/UEFI × e1000/e1000e,
SMP=1, disposable ISO/OVMF vars, exact argv preflight, no data disks. Use:

- User backend: real `/bin/ping -c 4 10.0.2.2`, exact outbound IP/ICMP capture
  audit plus matched received replies. Readiness is the actual Ring 3 prompt.
- Loopback UDP socket backend: independently synthesize ARP and ICMP frames;
  verify guest echo reply payload/IDs/checksums and outbound probe matching.
  This is explicit RX injection, not host-to-guest SLIRP reachability or
  filter-dump injection. Send after idle ticks to exercise timer resumption.
- Negative cases: silent ARP peer, silent ICMP peer, bad checksums, fragments,
  wrong peer/ID/token, duplicates, odd-sized data, pending-slot saturation,
  competing ping callers, cancellation, owner exit and finite reclamation.

Keep all waits bounded, always terminate QEMU, and audit pcap independently
in Python rather than relying only on kernel success banners. SLIRP host
reachability is not assumed; socket peer provides the inbound gate.
Required regressions: Phase 0/3 host, Phase 3 QEMU, raw-ring tests, absent NIC
boot, and relevant syscall pointer/signal tests. Run NMI transition coverage
only if entry/return contracts unexpectedly change; such changes require review.

## 7. Phase 4b — physical Dell acceptance

**Result (2026-10-01): the physical LAN-peer gate is accepted.** On the Dell
Latitude 5590 with `net=192.168.0.168/24,192.168.0.1`, the guest `ping` to the
gateway returned 4/4 replies at 0% loss (reported 20 ms RTT) with a usable shell
afterward; a guest ping to a Windows 11 peer at `192.168.0.222` succeeded after
inbound ICMP was allowed; a second-host Wireshark screenshot shows four matched
request/reply pairs (sequences 1–4, all 74 bytes: frames 923/924, 931/932,
942/943, 947/948); and the reverse `ping` from Windows 11 to the guest returned
4/4 replies at 0% loss (TTL 64; RTT min 3 / max 17 / avg 9 ms). This is
**manual, user-supplied evidence, not an automated pass**: the screenshot
confirms outbound matching request/reply pairs and the reverse-direction result
is evidenced by Windows terminal output, with **no raw pcap** supplied, so no
independent payload/checksum verification or saved capture artifact is claimed.
Guest RTT includes polling/scheduling effects; no sustained-load, measured
idle-CPU, or cross-core acceptance is claimed. Full record:
[net-phase4b.md](../roadmap/net-phase4b.md).

Use the already working I219-LM path and the observed LAN configuration
`net=192.168.0.168/24,192.168.0.1` only after confirming the address remains
free and the gateway/network are unchanged. Do not alter driver registers to
compensate for protocol failure. Capture from a second host on the wired LAN.

Run `/bin/ping -c 4 192.168.0.1`, then ping the guest from an on-link second
host. Collect guest output, matched request/reply identifiers and payloads,
checksums, addresses, lengths, and shell prompt recovery. For replies destined
to another machine, use capture on that endpoint or a mirror port; a third
switched-port capture may not see unicast traffic. Use a two-direction filter
such as `arp or (icmp and ip.addr == 192.168.0.168)`, not only guest source MAC.

If the gateway suppresses ICMP, retain the failure evidence and use a known
responsive LAN peer; do not relabel that result as gateway ping acceptance.
Physical gate: independently captured matching ICMP exchange plus guest result
and usable shell. Record exact boot config, machine, firmware, command and
capture method. No sustained-load or measured idle-CPU claim without separate
measurements. Physical observations are not automated test passes.

## 8. Completion and stop conditions

Implement in order A -> B -> reviewed C -> D -> physical 4b. Each checkpoint
gets its own actual test evidence and reviewable diff. Update roadmap, annex,
AGENTS status and plan index only when its gate passes. Phase 4a cannot be
called complete with a kernel boot probe replacing `/bin/ping`; Phase 4b
cannot be called complete from DD, an outbound-only capture or QEMU.

Stop and report if delivery requires driver workarounds, another timer hook,
scheduler/lock/entry changes, unbounded retention/waits, unresolved mailbox
owner cleanup, or direct reentrant use of Phase 3 static state. This planning
request does not authorize those changes or the proposed ABI implementation.
