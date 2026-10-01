# Networking Phase 5 — UDP and bounded socket syscalls

Status: **5a IMPLEMENTED AND VERIFIED (2026-10-01); 5590 UDP user-reported PASS; capture audit pending**.
User authorized implementation after approving the pre-coding contract.
Companion to [NET_PLAN.md](NET_PLAN.md); implemented headers
take precedence over historical sketches. Phase 4a automated evidence and
Phase 4b manual acceptance remain separate in the roadmap.

Review update (2026-10-01): user approved BSP-only callers, the five-second
receive timeout, and 16 sockets / four RX slots / 1472-byte payload capacity.
The concrete pre-coding contract is [UDP_SOCKET_ABI.md](UDP_SOCKET_ABI.md),
including field offsets and deterministic error precedence.
SYS_NETCTL=42 is unchanged, including NETCTL_PING's 48-byte v1 layout,
command set, return/error semantics and existing behavior.

Implementation/evidence: [Phase 5a](../roadmap/net-phase5a.md). The sections
below preserve the reviewed planning baseline. Socket syscalls and tools have
landed; NET-1 closure still requires the separate physical UDP capture gate.

Hardware observation: [5590 UDP user-reported PASS](../roadmap/net-phase5b.md);
raw capture/application artifact audit remains pending.

## 1. Goal and scope

Deliver IPv4 UDP datagrams through real Ring 3 descriptors: `socket`, `bind`,
`sendto`, `recvfrom`, and existing `close`, with `/bin/udptest` demonstrating
two-way exchange. Complete host/QEMU gates first (5a), then a separate physical
Dell/Windows LAN gate (5b). Passing both closes the deliberately limited NET-1
milestone; it does not establish full IPv4 host or POSIX socket conformance.

Keep static `net=` configuration, one NIC, polling-only ingress, sole BSP
protocol worker, existing tick wake, driver workarounds and DMA ownership.
Socket syscall callers must execute on the BSP and be BSP-pinned, just like
NETCTL_PING. Reject unsupported CPU context explicitly, including nonblocking
calls. SMP-enabled boot is allowed; cross-core socket operation is deferred.

No TCP, DNS, DHCP, IPv6, broadcast/multicast UDP, IP fragmentation/reassembly,
UDP options, connected sockets, listen/accept, select/poll, SO_REUSEADDR,
arbitrary socket options, or NIC IRQ/MSI. Local self-delivery is unsupported.
ICMP port-unreachable generation and asynchronous ICMP-to-socket errors are
deferred; an unbound destination drops with a counter. `/bin/ifconfig` and
expanded NETCTL interface control are separate work, not closure requirements.

## 2. Baseline and corrections to the older master plan

Read PROTECTED.md and AGENTS.md §§4, 7.1, 7.5–7.6, 9, the net annex, then
`src/include/net.h`, `src/net/net.h`, `net_ipv4.h`, `net_ping.h`,
`src/fs/vfs.h`, `src/kernel/thread.h`, `syscall.h`, and `src/mm/vmm.h`.
Trace the pipe implementation and fd clone/dup/CLOEXEC/exit/reaper callers.

| Existing mechanism | Phase 5 consequence |
| --- | --- |
| `net_ipv4.c` has static headers, frames, routing and ARP state | Syscalls never call protocol TX directly. They submit copied, bounded work; worker alone routes, resolves and transmits. |
| `sched_wait_until` is BSP-only; predicates execute under scheduler lock | Predicates acquire-load atomically published readiness/generation/deadline values; no socket lock or mutable queue traversal. |
| `net_timer_tick` wakes the existing worker once per BSP tick | Worker publishes expired socket operations and wakes their channels; no extra APIC hook or signature change. |
| `vfs_close` invokes `node->close` on final `file_t` release | Reuse descriptor ownership for sockets, including dup, spawn sharing, CLOEXEC, normal exit and hard-kill reaping. |
| Syscall numbers 38–41 are reserved; NETCTL_PING=42 exists | Fill only the reserved socket numbers; preserve the ping layout and numbering. |

The older NET_PLAN §4 direct `sys_sendto -> driver` sketch and raw queue-pointer
wait predicate are not implementation instructions for this phase. This draft
replaces them with worker submission and atomic readiness. Its cross-core
socket ambition is deferred explicitly, rather than silently changing the
documented scheduler contract. Revise the master plan on implementation approval.

## 3. Proposed ABI — freeze before Checkpoint C coding

Proposed shared header: `src/include/socket_abi.h`. This is a FortressOS ABI,
not a promise of Linux errno numbers or full POSIX semantics.

| Syscall | Number | Arguments in existing ABI register order |
| --- | --- | --- |
| SOCKET | 38 | domain, type, protocol |
| BIND | 39 | fd, address pointer, address size |
| SENDTO | 40 | fd, data pointer, length, flags, destination pointer, destination size |
| RECVFROM | 41 | fd, data pointer, capacity, flags, source pointer, source-size pointer |
| CLOSE | existing 3 | fd |

`AF_INET=2`, `SOCK_DGRAM=2`, protocol 0 or 17. Optional
`SOCK_CLOEXEC=0x80000` creates a descriptor with the existing fd flag; reject
all other type bits. `MSG_DONTWAIT=0x40` is the only initial message flag and
is supported on recvfrom only; send flags must be zero.

Proposed `net_sockaddr_in_t`: size 16, alignment 4, exact-size input.

| Offset | Field | Type | Bytes | Representation |
| --- | --- | --- | --- | --- |
| 0 | family | uint16_t | 2 | host-order AF_INET |
| 2 | port | uint16_t | 2 | network-order |
| 4 | address | uint32_t | 4 | network-order IPv4 |
| 8 | reserved | uint8_t[8] | 8 | zero on input and output |

Add static assertions for size, alignment and every offset. Bind accepts
address 0 (wildcard) or the configured local IP, port 0 (allocate ephemeral)
or explicit nonzero port. Ports are exclusive per interface, including wildcard
versus specific-local conflicts; no reuse. Ephemeral scan is bounded to
49152–65535, deterministic cursor, skips occupied ports. Send auto-binds an
unbound socket; destination must be supported unicast with nonzero port.
No privilege restriction on low ports in this initial system.

Send returns payload length only after driver submission succeeds; it does not
mean remote receipt. No partial sends. Maximum data is min(1472, MTU−28);
oversize fails before queue publication. Zero-length datagrams are supported.
Send can block for bounded cold ARP (three attempts, one-second spacing,
three-second absolute budget); full command capacity returns EAGAIN promptly.

Receive consumes one whole datagram. Return min(capacity, data length), discard
any remainder, and return 0 for a zero-length datagram (not EOF). Capacity 0
still consumes a datagram. With MSG_DONTWAIT, empty queue returns EAGAIN.
**Approved bounded default:** flags=0 waits at most five seconds, measured
against an absolute BSP-tick deadline, then EAGAIN. No timer reset after wakes.
This intentionally differs from an indefinitely blocking POSIX receive and
was approved by the user. Socket options/timeouts can extend the ABI later.

Source and source-size pointers must both be null or both nonnull. If present,
validate the uint32_t size pointer as writable before reading capacity. Require
capacity at least 16, validate exactly the 16 output bytes, and write actual
size 16. Validate payload ranges before access, revalidate outputs after sleeps,
and do not consume a queued datagram if output validation fails. A null payload
pointer is permitted only for zero capacity/length. Copy source and payload
into kernel-owned operation storage before any sleep; never retain user/TCB
pointers in socket or protocol state. Define deterministic validation precedence
and overlapping output-buffer policy in the frozen ABI (proposed: reject overlap).

| Existing syscall error | Value | Proposed socket use |
| --- | --- | --- |
| EINVAL | −1 | bad lengths/flags/address fields, oversized payload, output overlap |
| EFAULT | −2 | invalid user memory |
| EBADF | −3 | missing descriptor or a descriptor that is not a socket |
| EMFILE | −6 | per-process fd table full |
| EIO | −9 | interface unavailable, TX failure, ARP exhaustion or observed link loss |
| ENOMEM | −10 | file/node allocation failure |
| ENOSPC | −13 | fixed socket pool or ephemeral port range exhausted |
| EOPNOTSUPP | −14 | unsupported family/type/protocol or CPU context |
| EEXIST | −15 | binding conflicts or attempt to rebind |
| EAGAIN | −21 | TX capacity/busy operation, empty nonblocking receive, receive timeout |
| EINTR | −22 | existing signal interruption or invalidated operation token |

Keep the project's existing values; do not substitute Linux errno numbers.
Write the final layout, return semantics and error precedence into a dedicated
UDP_SOCKET_ABI.md before C starts. No new errno is required by this proposal.

## 4. Checkpoint A — pure UDP codec

Add `src/net/udp.h/.c`: eight-byte header, host-order decoded ports and length,
wire-order output. Decode within the IPv4 payload bound; require length ≥8 and
length no greater than the bounded IP payload. Checksum covers only the UDP
length, excluding any trailing IP data or Ethernet padding.

Compute checksum over the IPv4 pseudo-header and UDP header/data. TX always
generates a checksum; calculated zero is transmitted as 0xffff. IPv4 RX zero
checksum means omitted and is accepted; otherwise verify before delivery.
Document these rules with [RFC 768](https://www.rfc-editor.org/rfc/rfc768)
and the UDP requirements in [RFC 1122](https://www.rfc-editor.org/rfc/rfc1122).
This phase retains its restricted IP subset and does not claim full conformance.

Gate: proposed `make test-net-udp-host`, actual codecs under ASan/UBSan.
Independent known vectors, zero/odd/even/max data, computed-zero checksum,
omitted versus corrupt checksum, wrong pseudo-header address/protocol/length,
truncations, length overflow, encoder bounds and trailing-padding cases.

## 5. Checkpoint B — bounded sockets and worker delivery

Proposed fixed capacities: 16 sockets globally, four RX datagrams per socket,
1472 data bytes per datagram, one outstanding send and one receive operation
per socket. Preallocate packet/operation backing in BSS; allocate only the
small anonymous VFS node/file objects at socket creation. Publish a memory
budget using actual sizeof values before integrating (RX payload alone 94,208
bytes, plus metadata/TX staging). No MTU arrays on kernel stacks; check frames
against a 512-byte individual-frame budget and inspect transitive call depth.

Each static slot has a monotonic generation, stable wait-channel address,
bind state, atomically published readiness and operation tokens. Final-close
invalidates the generation; old worker snapshots and waiters must never access
a reused slot. RX copies into owned storage, then the normal net_input path
recycles every NIC pbuf exactly once. Full RX queue drops newest and counts it;
unbound ports and unsupported/malformed packets drop without retained DMA.

Extend validated local IPv4 demux for protocol 17 while preserving ICMP.
Reuse the existing route policy, ARP retry/coalescing and zero Ethernet padding.
Generalize worker TX helpers with bounded per-socket operation frames; keep
ping and inbound Echo reply state isolated. Fair round-robin socket service,
at most one new send per socket per pass and bounded total work; retain the
64-packet ingress budget and tick sleep. Full batches may yield for fairness;
idle worker never loops on yield. Coalesce ARP retries for identical next hops
across ICMP and UDP with fixed capacity and absolute expiration.

An ordinary rank-1 socket lock protects slot/queue/operation publication.
Never nest it with device, ping, process, scheduler or ext2 locks. Reserve a
slot, drop the lock, validate/copy into exclusively owned operation storage,
then publish under lock; cancellation/reuse requires generation checks.
Worker snapshots copied work under lock, releases it before protocol/driver
calls, then publishes result under lock. Publish readiness before waking,
with all locks dropped. No user copies under socket lock.

Receive waits use an atomic predicate over readiness, token validity and
published expiration. Worker advances timeout state each existing tick and
wakes affected channels. No direct access to mutable queue pointers under the
scheduler lock. Multiple inherited readers compete safely for one datagram;
serialize the active receive operation and return EAGAIN to competing operations.
Each waiter rechecks under the socket lock before reservation/consumption.

Interface fatal state or an observed link-down completes pending sends and
wakes receives with EIO. Inspect the existing driver status API first; if a
new read-only status accessor is needed, specify it before coding. Do not
guess driver state or add reset/recovery. A quiet peer with link up is a
receive timeout, not proof of interface-down.

Gate: actual worker/socket code with fake NIC/ticks/scheduler and sanitizer
lock adapters. Test demux, independent bytes/checksums, cold/warm/off-link ARP,
retry coalescing, pool/queue saturation, fairness with concurrent ping,
expiry, failure, exact-once recycle and stale-generation races.

## 6. Checkpoint C — descriptors, syscalls and lifecycle

Use an anonymous VFS stream node whose close callback releases the socket on
final file reference. `SYS_READ`/`SYS_WRITE` on it initially return
EOPNOTSUPP; datagram traffic goes through SENDTO/RECVFROM. Type-check safely
against socket-owned node identity before interpreting fs_private.
Preserve generic fd layout and reference-count conventions. Existing dup,
dup2, spawn sharing and CLOEXEC apply; closing one reference keeps the shared
socket alive, last close cancels work and makes the port reusable safely.

Before coding, trace final-close contexts in normal exit, hard-kill reaper,
spawn rollback, CLOEXEC and dup2 replacement. The close callback must be
nonblocking, bounded, and safe from every existing caller. Socket pool backing
remains static; worker releases any in-flight reservation by token. No exit
hook or scheduler changes. Operation leases bound abandoned send/receive
reservations even when another inherited descriptor keeps the socket alive.
Recommended send reservation lease: five seconds (three ARP plus collection
margin); receive reservation lease: seven seconds (five receive plus margin).
Leases start at reservation and do not reset on wakes. They invalidate the
operation, not an otherwise live socket or its queued packets.

Use existing process_signal_interrupt/sched_wait_until behavior with all
socket locks released. Test caught interruption, default Ctrl-C, hard KILL,
STOP/CONT beyond lease and stale resumed continuations. Do not promise immediate
hard-kill port reuse before existing fd cleanup/reaping runs; prove bounded
observed recovery in QEMU and record its limits.

Fallback policy proposed for review: keep BSP-only sockets and finite leases;
do not expand scheduler, signals, lock ranks, or descriptor ownership to make
the feature fit. If existing final-close/sleep mechanisms cannot support the
defined lifecycle, stop and report the exact caller/race before changing scope.
A nonblocking-only subset would need a revised reviewed plan and would not
silently count as completion of this draft.

Gate: proposed `make test-net-socket-host` plus Ring 3 ABI fixture. Include
all pointer classes, page crossings, size overflow, read-only outputs,
six-register argument passing, fd exhaustion/allocation rollback, bind conflicts,
auto-bind, zero datagrams, truncation, shared fd references, final close,
CLOEXEC/exit/KILL, signal interruptions, leases and slot reuse.

## 7. Checkpoint D — tools and QEMU gate (5a)

Add `/bin/udptest <numeric-ip> <port> <message>`: create, send, receive matching
echo, compare peer/port and exact bytes, print bounded result, close, and return
nonzero on mismatch/timeout/error. A second mode
`udptest --listen <port> <count>` binds and echoes a finite number of datagrams;
five-second receive timeout bounds a silent run. Cap message at 1472 bytes,
port 1–65535 and count 1–100. Binary/zero/max payloads belong in a separate
Ring 3 fixture; argv text alone cannot verify those cases. No user busy loops.

Provide a small host Python UDP peer script, usable on Windows 11, for finite
echo-server and client runs. No external dependency or persistent service.
Record explicit bind address and bounded lifetime. Physical firewall setup
must be a narrowly scoped user action, with cleanup instructions.

Proposed `make test-net-udp`: BIOS/UEFI × e1000/e1000e × user/socket = eight
cases, SMP=1, disposable ISO/OVMF vars, no data disks, final argv preflight.
User backend: real UDP echo service on host loopback, guest reaches it through
SLIRP; separate hostfwd mapping for a guest-listener inbound test. Prove actual
reachability in the runner, never assume ICMP gateway behavior implies UDP.
Socket backend: independent raw Ethernet/IP/UDP injection and pcap audit;
distinguish this transport backend from the implemented UDP socket API.

Require real Ring 3 client and listener exchanges, byte-exact independent
wire decoding/checksums, cold ARP, reverse delivery after idle ticks, bounded
timeouts, recovery, malformed/checksum drops, saturation, competing operations,
KILL/STOP/CONT, shared fd/CLOEXEC cleanup, port reuse and simultaneous ping.
No kernel boot probe substitutes for user tools. Retain logs and pcap artifacts,
bound every runner wait and always terminate QEMU.

Relevant regressions: Phase 0/3/4 host, Phase 3/4 QEMU, raw rings, absent NIC,
pipe/fd host coverage and job/signal integration covering touched paths.
Add an SMP=4 BIOS/UEFI smoke pair with BSP-pinned tools and explicit AP syscall
rejection fixture; this establishes rejection, not cross-core socket support.
Measure idle worker CPU tick deltas over a recorded interval using existing
procinfo/top, report ticks/frequency/interval and comparison to Phase 4 baseline.
No numerical idle percentage claim from mocked wait calls alone.

## 8. Physical Dell/Windows gate (5b)

Reuse the accepted Dell I219-LM and static configuration
`net=192.168.0.168/24,192.168.0.1`; verify addresses before running. Windows
LAN peer previously used 192.168.0.222. No boot script is needed beyond the
existing configuration; UDP acceptance does require a peer application.

1. Run a finite Windows UDP echo peer on port 7777, permit inbound UDP only
   from the guest as needed, and start Wireshark on that Ethernet interface.
2. Guest runs `udptest 192.168.0.222 7777 fortress-phase5`. Require exact echoed
   message, correct peer/port, success exit and usable shell.
3. Guest runs `udptest --listen 7777 4`; Windows sends four distinct tagged
   messages and verifies four exact responses. Require clean listener exit.
4. Stop the peer; require bounded guest timeout and a usable shell, restart
   and verify recovery. Repeat ping both directions to check regression.
5. Save raw pcapng, guest output, peer output, boot configuration and commands.
   Filter: `arp or (udp and ip.addr == 192.168.0.168 and ip.addr == 192.168.0.222)`.
   Verify payload tags, addresses/ports, UDP/IP lengths/checksums and reply
   pairing from the capture. Diagnose host checksum-offload annotations before
   interpreting them as guest corruption. Remove the temporary firewall rule.

Gate cannot be satisfied by TX DD, guest banners, screenshots alone or QEMU.
If raw capture is unavailable, record partial manual observations and keep the
independent payload/checksum gate pending. Physical evidence stays manual;
no sustained-load, line-rate, internet or cross-core claim follows from it.

## 9. Order, deliverables and stop conditions

Order: A codec -> B worker/socket state -> **freeze reviewed ABI and lifecycle**
-> C descriptors/syscalls -> D tools/QEMU 5a -> physical 5b.
Each checkpoint has a reviewable diff and actual evidence; update roadmap,
annex and AGENTS status when its gate passes. Do not mark 5b complete from 5a.

Review decisions: BSP-only scope; 16 sockets/four RX slots; five-second receive
default; synchronous bounded sends; inherited descriptor semantics and finite
operation leases; deferred ICMP errors; raw-capture physical completion gate.

Stop and report if the design requires scheduler/signal changes, new wait
primitives/signatures/timer hooks, nested rank-1 locks, direct reentrant
protocol calls, unbounded packet retention, unresolved fd final-close contexts,
DMA ownership changes, driver workaround/reset changes, or unexplained capture
disagreement. Planning creates no permission to cross protected boundaries.
