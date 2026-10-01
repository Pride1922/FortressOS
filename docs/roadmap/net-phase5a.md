# NET Phase 5a — UDP sockets and Ring 3 tools

2026-10-01. Implementation complete; host gates and all ten automated UDP matrix cases
passed. Physical UDP passed on 5590 per the user; capture audit remains pending.
See [Phase 5b observation](net-phase5b.md); there is no automated hardware target.
Design: [Phase 5 plan](../plans/NET_PHASE5_PLAN.md).
Contract written before implementation: [UDP socket ABI](../plans/UDP_SOCKET_ABI.md).

## Implementation and boundaries

`src/net/udp.c` implements eight-byte UDP framing and IPv4 pseudo-header
checksums. TX always emits a checksum (computed zero becomes 0xffff); RX
accepts omitted IPv4 UDP checksums and rejects corrupt nonzero checksums.
Lengths are bounded to the IPv4 payload; Ethernet padding is excluded.
Protocol 17 shares the existing validated local-unicast IPv4 delivery subset.
No IP options, fragments/reassembly, broadcast/multicast, UDP options or
ICMP port-unreachable/asynchronous socket errors are implemented.

`net_socket.c` provides 16 static sockets with four RX datagrams each, up to
1472 data bytes (further bounded by interface MTU). One send and one receive
reservation per socket use monotonic operation tokens, stable static channels
and atomic readiness. Full RX drops newest and counts it; unbound ports and
malformed UDP are counted internally. There is no new statistics ABI.
NIC pbufs are copied into owned storage and remain recycled exactly once by
the existing ingress path. No socket retains or frees NIC DMA backing.

All ARP/routing/protocol TX remains on the sole BSP worker, using separate
per-socket pending frames and bounded shared next-hop ARP retry coalescing.
Socket service rotates its starting index and has a fixed pass limit. An
ordinary rank-1 socket lock never nests with device, scheduler, process, ping
or ext2 locks. Publish readiness before waking, after unlocking. Wait predicates
only acquire-load atomic tokens/completion; no queue traversal or socket lock
under the scheduler lock. No idle yield loop or new timer hook.

`net_socket_syscall.c` implements reserved syscalls 38–41 with the concrete
16-byte address ABI and existing project errno values. All calls require
BSP execution and affinity=0; AP calls reject before user-memory access.
Bind is exclusive, ephemeral ports use a bounded 49152–65535 scan, and send
or receive auto-binds an unbound socket. Send returns only after driver
submission; remote delivery is not guaranteed. Empty blocking receive has a
five-second absolute deadline; nonblocking receive returns EAGAIN immediately.
Zero datagrams and truncating receives are supported, with one datagram
consumed per successful receive. Invalid outputs do not consume a datagram.

Validate full register-width scalar arguments and all user ranges before
access; reject overlapping receive outputs. After sleep revalidate output
ranges before staging/copy/commit. Kernel continuations resume with IF clear;
exclusive operation storage and no user copies under socket locks preserve
the copy/consume transaction. Socket state holds no user or TCB pointers.

Anonymous VFS stream nodes supply final-reference close. Generic read/write
callbacks reject with EOPNOTSUPP; existing close/dup/dup2/spawn/CLOEXEC/exit
and hard-kill reaper ownership is reused. Final close invalidates operations;
the BSP worker wakes their static channels on its next pass (including close
from an AP reaper). Five-/seven-second send/receive leases reclaim abandoned
reservations without expiring a shared live socket or its RX queue. STOP/CONT
beyond lease returns EINTR; stale cancellation cannot affect a new operation.
No process-exit hook, scheduler, signal or lock-rank change.

`e1000_network_online()` is a read-only, unlocked-thread-context accessor:
under the existing device lock it reads fatal/NET_UP state and STATUS.LU, then
drops the lock. There are no new register writes/reset/recovery or changes to
I219 evidence-backed workarounds. Worker-observed offline completes pending
I/O with EIO; a silent peer with link up produces receive timeout instead.

SYS_NETCTL=42 and NETCTL_PING's 48-byte v1 layout, command set, errors and
behavior are unchanged; syscall transition assembly and sched_wait_until's
signature are unchanged. TCP, DNS, DHCP, IPv6 and cross-core sockets remain
deferred. Phase 5b hardware acceptance is not implied by this implementation.

## Storage and stack budget

Actual x86_64 object sizes: socket slots 145,664 bytes (9,104 per socket),
worker payload snapshot 1,472 bytes, pending IPv4 frames 28,224 bytes for 18
slots, UDP completion metadata 512 bytes, shared ARP retry observations 432
bytes, plus small existing headers/config/lock state. Of the pending frames,
two already existed for ICMP; UDP adds 25,088 bytes. RX data alone is 94,208
bytes; the extra receive staging and TX backing are deliberate fixed storage,
not additional queue capacity. Small VFS node/file objects allocate at socket
creation and unwind on failure/final release; packet backing is BSS.

New core modules compile with `-Os -Wframe-larger-than=512 -fstack-usage`.
Observed socket worker frame 144 bytes (dynamic, bounded), socket syscall
112 bytes, and Ring 3 binary fixture 280 bytes. This is individual-frame
evidence, not a complete transitive-stack bound including drivers, IRQs and
preemption. The existing 16 KiB usable kernel stacks and guard qualification
remain unchanged.

## Verification record

Host gates executed and passed under ASan/UBSan:

- `make test-net-udp-host`: actual codec, independent checksum construction,
  zero/odd/even/max data, omitted/corrupt and computed-zero checksum,
  truncation, wrong pseudo-header and encoding bounds.
- `make test-net-socket-host`: actual socket/syscall/IPv4/UDP implementation
  with memory/fd/allocator/tick/scheduler/NIC adapters. Pointer/argument bounds,
  allocation rollback, binds, auto-bind, zero/max/truncated data, queues/pool,
  five-second timeout, interruption, stale leases, shared references and
  failure/recovery. Predicates execute under a mock rank-1 scheduler lock;
  copies/allocations/device calls assert no socket lock. Sixteen simultaneous
  cold sends coalesce to one ARP attempt and all transmit after resolution.
- Phase 0 (103/103), Phase 3 host, IPv4/ICMP/ping host, pipe/SIGPIPE host,
  actual e1000 rings host and mocked I219 host regression targets passed.
  Driver tests additionally cover read-only online/link-down/fatal snapshots.

The full UDP matrix passed 10/10: BIOS/UEFI × e1000/e1000e × user/socket
backends at SMP=1, plus BIOS/UEFI e1000 user cases at SMP=4. AP rejection
uses direct handler calls from an AP kernel thread, not Ring 3 entry evidence.
Two additional focused BIOS/e1000/socket and UEFI/e1000/user runs passed with
caught SIGINT and an unaligned address spanning two mapped pages. Coverage includes real
Ring 3 tools, binary/ABI/fd fixtures, independent pcap decoding, inbound traffic,
timeouts, KILL/Ctrl-C/STOP/CONT, port reuse and ping recovery.

`make test-net-icmp test-net-eth test-net-rings test-net-pci` passed:
ICMP 8/8, Ethernet/ARP 8/8, raw rings 4/4 and PCI discovery 4/4.
`make bin/fortress.iso` and `make bin/fortress.img` passed, including generated
GPT/FAT32/ext2 image checks; no physical USB was written. Python syntax checks
and `git diff --check` passed.

Idle accounting is opt-in through `net_test=udp`, using the worker's own
charged CPU ticks over at least 500 BSP ticks at 100 Hz with no RX or active
IPv4 transmit/echo transaction during the interval. SYS_PROCINFO enumerates
user metadata and cannot observe this kernel worker, so no new introspection
syscall was added. An isolated archive of Phase 4 commit 3381d34 with equivalent
read-only observation recorded 0/500 ticks under BIOS and UEFI. `python3 scripts/test_net_idle_baseline.py --compare-only` passed both
comparisons: Phase 5 also recorded 0/500 ticks at 100 Hz. Quantized accounting can show
zero while the worker still executes short passes; no zero-cost or physical
idle-percentage claim follows.

Initial QEMU attempts timed out during startup/readiness and were not counted
as passes. Live socket-UART logs now use the Linux temporary filesystem, then
copy to build after QEMU shutdown; matrices run serially. Failed readiness logs
are retained separately. No protected kernel behavior was changed to address
test startup timing.

## Physical Phase 5b procedure (artifact collection)

Build with `make`. Choose the existing LAN boot entry with
`net=192.168.0.168/24,192.168.0.1`. No additional guest boot script is required.
The previously tested Windows peer was 192.168.0.222; verify current addresses.
Use [scripts/udp_peer.py](../../scripts/udp_peer.py) with Python 3 on Windows.

In an administrator PowerShell, if needed:

```powershell
New-NetFirewallRule -Name "FortressOS-UDP-Test" -DisplayName "FortressOS UDP Test" -Direction Inbound -Action Allow -Protocol UDP -LocalPort 7777 -RemoteAddress 192.168.0.168 -Profile Any
```

Start Wireshark on that Ethernet interface and a Windows echo peer:

```powershell
py scripts/udp_peer.py echo --bind 192.168.0.222 --port 7777 --count 4
```

Guest: run `udptest 192.168.0.222 7777 fortress-phase5` four times. Each must
report `UDP echo PASS`, with matching peer bytes and a usable shell.
Then guest: `udptest --listen 7777 4`; Windows:

```powershell
py scripts/udp_peer.py client 192.168.0.168 --port 7777 --count 4
```

Require four exact tagged responses and guest `UDP listener PASS`. Stop the
peer, require a bounded timeout/error 21, restart it and verify recovery.
Repeat ICMP ping both directions. Capture filter for display:

```text
arp or (udp and ip.addr == 192.168.0.168 and ip.addr == 192.168.0.222)
```

Save pcapng, guest and peer outputs and exact boot/commands. Inspect tags,
addresses/ports, UDP/IP lengths and checksums; account for Windows capture
offload annotations. Remove the temporary rule afterward:

```powershell
Remove-NetFirewallRule -Name "FortressOS-UDP-Test"
```

Physical completion requires that independent raw capture and both application
directions. The user now reports physical UDP PASS on 5590; see
[Phase 5b](net-phase5b.md). Raw capture and command/output artifacts were not
supplied, so formal NET-1 closure remains pending that audit. Existing ICMP
screenshots/outputs remain Phase 4b evidence only. No sustained-load/line-rate/internet/cross-core claim.
