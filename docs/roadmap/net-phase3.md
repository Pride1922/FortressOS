# NET Phase 3 — Ethernet/ARP and BSP ingress (2026-10-01)

Implemented in `src/net/net.c`/`net.h`: single-interface Ethernet filtering,
ARP request replies, reply-only cache learning, and nonblocking `arp_resolve`
(hit=0, request sent=1, invalid/send failure=-1). IPv4 and other EtherTypes
drop. Every non-null RX packet from a valid device callback is recycled exactly
once, including malformed and send-failure paths. No DMA allocations are freed.

The codecs decode EtherType and ARP opcode to **host order**; compare raw
constants. ARP IP fields stay in network order. Public codec comments clarify
this distinction. The driver publishes the same callbacks on successful QEMU
and supported I219 initialization; no driver/workaround changes were needed.

Configuration parses bounded `net=<IPv4>/<prefix>,<gateway>` tokens, with
prefix 1–30, defaults `10.0.2.15/24,10.0.2.2`, and warning/default fallback for
malformed or duplicate configuration. No panic. The state/cache/wire scratch
are static. Stack APIs are single-BSP-owner and non-reentrant; cross-core socket
callers and their synchronization remain future work.

## Timer gap and explicitly authorized hook

The plan's previously assumed network-channel tick wakeup did not exist.
`sched_wait_until` rechecks on resume; it has no timeout or automatic periodic
predicate scan. `apic_timer_handler` already acknowledges EOI, calls
`input_timer_tick(g_target_hz)`, then `sched_on_timer_tick`. The input tick
on BSP wakes `&g_terminal` for pending events and `&g_input` for pending data
or timed readers.

The user explicitly authorized the minimal extension on 2026-10-01:
`apic_timer_handler` now calls `net_timer_tick()` on BSP alongside the input
tick. Once the worker is published, the hook calls
`sched_wake_all(&g_net_poll_channel)`. The channel is a stable global address;
the hook allocates/logs nothing and holds no device lock. No scheduler,
spinlock, rank, signature, EOI ownership, or generic sleep primitive changed.

The BSP-pinned worker drains at most 64 packets per iteration. For idle or
partial batches it sets a static deadline to BSP ticks + 1, then calls
`sched_wait_until(&g_net_poll_channel, deadline_reached, &s_deadline)`.
The predicate only reads ticks and takes no locks under the scheduler lock.
Full batches yield for fairness. There is no yield loop on an idle interface.
Tick exclusion in the existing scheduler and repeated BSP tick wakeups prevent
a permanently lost wakeup. The worker starts after raw selftest execution;
`net_test=rings` skips worker creation to preserve exclusive raw RX ownership.
Absent devices and failed worker creation also skip publication.

## Verification

Run in WSL Ubuntu-24.04 at `/mnt/c/Sources/FortressOS`:

- `make test-net-eth-host`: ASan/UBSan PASS; actual stack/codec code with mocked
  NIC, scheduler and ticks. 52 RX inputs recycled exactly once. Reply bytes,
  cache hit/miss, malformed/truncated frames, send failures, config bounds,
  absent/failed creation and raw-test exclusion checked. Three mock idle waits
  verify deadline predicates and timer-channel wakes without idle yields.
- `make test-net-host`: all 103 Phase 0 sanitizer tests PASS.
- `make test-net-eth`: BIOS/UEFI × e1000/e1000e × user/socket backend, SMP=1.
  Disposable ISO/OVMF vars and exact argv preflight, no data disks. User backend
  verifies real SLIRP gateway ARP resolution; socket backend explicitly emulates
  gateway reply and injects a peer ARP request after idle ticks. Guest request
  and reply are independently checked as exact 60-byte outbound pcap records;
  RX injection comes from loopback UDP, never from filter-dump.
- `make test-net-rings`: raw ring host sanitizer and four QEMU regressions;
  PASS BIOS/UEFI × e1000/e1000e, SMP=1, with exact 60-byte TX/RX checks.
- Stack-frame warning check (`-Wframe-larger-than=512 -fstack-usage`) passed
  for `net.c`: largest own frame 144 bytes; worker frame 48 bytes. This is an
  individual-frame check, not a bound on the transitive driver call stack.

QEMU confirms protocol delivery and the real Ring 3 shell prompt while the worker runs;
host tick tests mock scheduler behavior. No SMP traffic, sustained load,
or measured idle CPU percentage is claimed. Later physical evidence is below.
No new hardware work or interrupt/MSI path.

## Later physical gateway ARP evidence (2026-10-01)

User-supplied Dell boot photo shows I219-LM 8086:15D7, MAC
C8:F7:50:0E:35:80, tick-bounded BSP worker startup, gateway ARP request submitted
and resolved, and the real Ring 3 shell prompt. The user confirmed the shell
is usable. A second-host Wireshark screenshot independently shows frame 508,
60-byte broadcast ARP from that MAC: who has 192.168.0.1, tell 192.168.0.168.
Its source-MAC filter excludes the gateway's incoming reply, so reply receipt
is evidenced by the guest resolution log. Manual observations plus outgoing
wire capture establish one physical gateway resolution, not a new automated
pass, physical ICMP, sustained traffic or measured idle CPU use. The screenshot
is not a supplied raw pcap. Preserve these limits in Phase 4 documentation.
