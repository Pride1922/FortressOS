# NET Phase 4a — IPv4/ICMP and Ring 3 ping (2026-10-01)

Implemented against [Phase 4 plan](../plans/NET_PHASE4_PLAN.md). Physical Phase
4b acceptance remains pending; earlier Dell ARP evidence is not ICMP evidence.

## Implementation and concrete ABI

`icmp.c`/`icmp.h` provide bounded pure Echo encode/decode, type 8/0, code 0,
host-order identifier/sequence, and whole-message checksums including odd
lengths. `ipv4_encode` now rejects 16-bit length overflow. `net_ipv4.c` delivers
validated IHL=5, TTL>0, unfragmented local-unicast ICMP only. IPv4 options,
forwarding, broadcast/multicast echo, UDP and other protocols remain unsupported.
Echo replies preserve data/identifier/sequence with fresh IP/ICMP checksums.
Short Ethernet frames are zero-padded; padding is excluded from IP total length
and ICMP checksum. IPv4 header copies retain wire-order words.

IP source/destination policy rejects zero-net, loopback, multicast/reserved
high addresses and on-link network/broadcast addresses. Route selection ARPs
for the peer on-link or configured on-link gateway off-link; IP destination
remains the final peer. No ingress-source-MAC routing shortcut. One static
outbound ping slot and one pending inbound reply slot bound storage; saturation
drops new inbound requests. RX packets are never retained for ARP/TX: echo data
is copied before `net_input` recycles once. DMA ownership remains in the driver.

Pending ARP tries at most three requests one second apart, with three-second
expiry; same-hop retries are coalesced. No sleeps inside demux and no ARP flood
per tick. Worker still sleeps on the existing network channel when idle. Separate
static ARP/IP TX scratch prevents shared scratch from being clobbered.

The concrete 48-byte ABI was written before Checkpoint C in
[NETCTL_PING_ABI.md](../plans/NETCTL_PING_ABI.md). Shared `ping_abi.h` enforces
size and every offset. `SYS_NETCTL=42`, command `NETCTL_PING=1`; 38–41 remain
reserved/unimplemented. Dispatcher validates command/size, validates writable
user range before reading, enforces pinned BSP context, copies input, sleeps,
then revalidates writable range before copying the result. No user/TCB/pbuf
pointer enters mailbox state. Real Ring 3 fixture `/bin/net-ping-probe` checks
kernel/unmapped/overflow/read-only pointers and malformed ABI fields.

`net_ping.c` serializes a single finite mailbox with an ordinary rank-1 lock.
Lock is released before protocol/device/scheduler/user-copy operations. Worker
is the sole ARP/IP/ICMP owner. Wait predicate acquire-reads token/completion only;
completion is release-published before waking, with no mailbox lock held. Echo
match requires peer/local IP, identifier, sequence and exact 32-byte generated
data including the full generation token. RTT starts at echo TX submission,
excludes ARP wait, and returns actual BSP ticks/frequency.

## Selected owner-exit fallback

No scheduler, signal or process-exit changes. Lease is `timeout_seconds + 6`
seconds (at most 11), including up to one-second pacing, three-second ARP,
echo wait and two-second collection grace. An owner killed or stopped while
waiting cannot retain the slot indefinitely. Worker expires the lease, cancels
the matching protocol transaction, invalidates token and wakes waiters. A stale
continuation cannot collect/cancel a new caller's transaction.

Existing wait semantics provide caught-interruption EINTR and default signal
termination. STOP/CONT may resume with an expired lease/EINTR. No new guarantee
of immediate slot release on hard exit or Ctrl-C: lease bounds release. Further
signal UX and cross-core syscall delivery remain Phase 5. This is the narrower
fallback selected before Checkpoint C, preserving protected machinery.

`/bin/ping [-c 1..100] [-W 1..5] <numeric IPv4>` defaults to four probes.
Inter-probe pacing sleeps through the mailbox/worker, with one-second delay
after prior probe completion; it never polls user-space for readiness. Reports
reply/ARP timeout/echo timeout/TX failure, probe/reply/loss and integer millisecond
RTT min/avg/max. Any reply gives exit 0, all-loss/control failure exit 1, usage
exit 2. Largest `ping_main` compiler frame is 112 bytes. No DNS/general sockets.

Optional `net_test=icmp` executes a bounded worker-owned gateway echo probe;
it does not replace the real Ring 3 acceptance gate. `net_test=rings` still
excludes worker publication. Normal ISO entry now uses default static QEMU
config; raw-ring diagnostics have a separate opt-in menu entry. User's existing
raw-image LAN debug configuration was preserved.

## Verification and boundaries

Executed in WSL Ubuntu-24.04:

- `make test-net-icmp-host`: ASan/UBSan PASS, independent checksum vector,
  all 1473 supported data lengths, truncation/bounds/corruption and IP overflow.
- `make test-net-ipv4-host`: ASan/UBSan PASS, actual protocol code with mocked
  NIC/ARP/ticks; exact odd echo bytes and zero padding, route selection, invalid
  checksums/TTL/fragments/options/source/protocol, pending-slot saturation,
  three ARP retries, strict reply matching, pacing, echo expiry and TX failure.
- `make test-net-ping-host`: ASan/UBSan PASS, actual mailbox with pthread lock
  and protocol/scheduler adapters; busy, publication/collect, abandoned owner
  and uncollected result expiry, cancellation and stale generation protection.
- `python3 scripts/test_net_icmp.py` / `make test-net-icmp`: 8 QEMU cases,
  BIOS/UEFI × e1000/e1000e × user/socket backend, SMP=1. Real `/bin/ping` gets
  4/4 replies and returns to the shell. User backend is actual SLIRP gateway;
  socket backend independently emulates peer ARP/ICMP, sends wrong-sequence and
  corrupt replies, injects a cold-peer odd-sized request after idle ticks, checks
  byte-exact guest echo response, silent-peer timeout and next-probe recovery.
  Ring 3 ABI probe passes. Socket cases check concurrent busy, owner KILL and
  lease recovery; BIOS/e1000/socket also checks STOP/CONT after lease expiry.
  Python independently verifies outbound IP/ICMP checksums, addresses and lengths
  in pcap. Disposable ISO/OVMF vars; exact argv preflight rejects storage; no
  data disks. Final `make test-net-icmp` aggregate: **8/8 PASS**, including
  `net_test=icmp` boot-probe matching in every case and the lease checks above.
- Regressions PASS: `make test-net-host` (103), `make test-net-eth-host`,
  `make test-net-eth` (8), `make test-net-pci` (4 including absent NIC BIOS/UEFI),
  `make test-net-rings` (host sanitizer + 4 raw QEMU), `make test-pipe-host`.
- `make`: kernel/ISO/raw image built, GPT/FAT/ext2 image checks PASS.
  `git diff --check` and 512-byte own-frame warning checks PASS. No entry/return
  assembly changed, so NMI transition coverage was not rerun.

No physical ICMP, sustained-load, measured idle CPU percentage, AP packet
execution or full IPv4 standards conformance claim. No physical disk was written.
For Dell 4b, use configured LAN peer/gateway and a bidirectional ARP/ICMP capture
plus guest output and usable shell. A fresh generated raw image is a build
artifact; install it only through the user's normal hardware procedure.
