# NET-2 Step 7 — DNS implementation and acceptance record

2026-10-02. The reviewed API and TCP deadline design were authorized by the
user's implementation request. DNS codec/resolver, nslookup, hostname nc and
opt-in TCP deadlines are implemented. **Physical DNS acceptance was confirmed
by the user on Dell Latitude 5590; NET-2 is complete, 7/7.** Physical results
below are manual, user-supplied observations, separate from QEMU evidence.

## Behavior and boundaries

`nslookup -s <server-IPv4> <name>` performs bounded IN/A resolution. Numeric
addresses bypass sockets and the clock. `nc -s <server-IPv4> <host> <port>` uses
the first returned address. Existing numeric nc, finite serial I/O and single
accept listener semantics remain unchanged. The server is explicit; the gateway
is never assumed to provide DNS. There is no kernel DNS parser or shell parsing
change, cache, EDNS, IPv6, search domain or automatic reboot-quiet-time wait.

The 16-KiB caller-owned resolver context holds all large scratch buffers and a
staged result; the workspace envelope is 8,800 bytes. The result is published
once on success and remains unchanged on failure. Context init clears diagnostics;
each admitted call clears them again. Every admitted entry validates reserved.
A busy context is untouched; initializing a busy context violates the documented
caller precondition. Syscall diagnostics do not replace DNS status.

The parser bounds names, compression steps, records, CNAME links, addresses and
noise. It compares wire names case-insensitively, accepts only the chain rooted
at the question, and ignores unrelated/additional addresses for publication.
Matched TC envelopes authorize length-prefixed TCP fallback. One absolute BSP
deadline covers connect, short writes, prefix/body reads and CNAME follow-ups;
no trickled byte extends it. UDP retains its five-second receive overshoot policy.
Scheduling and existing bounded UDP SENDTO/ARP waits are separately qualified;
this is not a strict 35-second wall-clock guarantee for every resolution.

New calls 49–51 implement the approved SEND_UNTIL/RECV_UNTIL/CONNECT_UNTIL
extension. Existing untimed ABI and SYS_NETCTL are unchanged. One non-owning
earliest wake hint per static endpoint uses the existing atomic predicate,
channel, scheduler wait/wake, manager lock and network worker. No scheduler,
signal, lock-rank, wait-signature, timer-hook, DMA or driver change was made.
Runtime action_inflight assertions prevent expiry between TX prepare/commit.
A host subprocess exercises the actual trap (SIGILL required).

Measured x86-64 layouts: endpoint_t=136 bytes (previously 128; +128 bytes across
16 records), net_tcp_wait_t=48 bytes (previously 24). Clock observation adds a
uint64_t and bool plus alignment. GCC freestanding stack-usage reports bound the
resolver at 240 bytes and decoder at 192; buffers live in context/BSS. These are
per-function measurements, not a fabricated whole-call-chain stack bound.

## Bugs found by the new gates

The initial DNS matrix retained three e1000e fallback failures under
`build/net2-step7/99e0383b`. Wire bytes arrived before the deadline, but an RX
waiter remained asleep. The manager cached a two-byte readiness count; userspace
drained it, and another two-byte batch arrived before the worker sampled zero.
The equal cached count suppressed the next event. `net_tcp_consume` now
invalidates endpoint.observed before publication; `net_tcp_send` applies the
same rule for TX-space transitions. No transport sequence, ACK, retransmission,
window or FIN behavior changes. `repeated_receive_event_test` deterministically
failed at net_tcp_ready before this fix and passes afterwards.

A subsequent defensive clock check initially treated the existing shell-start
BSP uptime reset as a failure. Failed runs remain in `0ac15bcb`, `99bca211` and
`464d8bbe`. The worker now latches backwards-clock failure only when a timed hint
depends on the previous epoch; entry clock floors also protect continuations.
The no-hint boot reset and active-hint clock failure both have host regressions.
Boot sequencing was preserved. Earlier fixture/audit failures remain retained
instead of being relabeled as passes.

## Executable evidence

Commands are run from WSL Ubuntu-24.04 in `/mnt/c/Sources/FortressOS`.

- `make -j2`: freestanding kernel/tools, ISO and raw image build; GPT/FAT/ext2
  verification passes. No disk was flashed.
- `make test-net-dns-host`: ASan/UBSan literal RFC vectors, name/compression/
  section bounds, 20,000 malformed inputs, actual resolver syscall adapters,
  100/1000-Hz clocks, noise, TCP short framing, quiet/interrupt/timeout/EOF,
  numeric bypass, cleanup and failure publication.
- `make test-net-tcp-socket-host`: existing client/server/ownership fences plus
  timed pointer/flags/horizon/zero-length precedence, no consumption on expiry,
  shared earliest-hint rearming, stale reuse, cancel, backwards clock, and
  equal-sized RX readiness. Event exhaustion invalidates the identity and rejects
  stale hints across slot reuse. Runtime prepare/commit assertion fault gate passes.
- `make test-net-nc-host test-net-tcp-fixture`: finite nc and independent TCP
  vectors/audit regressions pass.
- DNS captures under `build/net2-step7/7d917f19`: eight BIOS/UEFI × e1000/e1000e
  × user/socket cases passed after the observation fix. Subsequent clock-guard
  changes require the final live run recorded below.
- Final DNS matrix `build/net2-step7/e5a707f1`: **10/10 PASS**, eight backend/NIC/
  firmware combinations plus BIOS/UEFI e1000/user SMP=4 BSP-tool/AP direct
  dispatch rejection. Each case exercises UDP A/CNAME/NXDOMAIN/malformed/timeout,
  numeric bypass, TC TCP fallback, stalled/trickled/early-EOF responses, hostname
  nc, shell recovery and follow-up ping. The clock hint locking refinement is
  additionally checked by the focused final-image run below.
- `python3 scripts/test_net_tcp_client.py`: Step 3 **5/5 PASS**, including
  captured 64-KiB directions, ABI/signal/reset/EOF and SMP=4 BSP/AP smoke. Idle
  network worker reports 0 CPU ticks across 500 elapsed BSP ticks in those cases;
  this is a QEMU observation, not a hardware power/throughput claim.
- `python3 scripts/test_net_tcp_server.py`: Step 4 **5/5 PASS** on the final
  build. An earlier concurrently loaded SMP=4 attempt timed out in the host
  app and was treated as a failure; focused rerun and full final suite passed.
- Step 5 TCP matrix `build/net2-step5/33076f23`: 10/10 pass, including SMP=4 BSP
  tools/AP direct-dispatch smoke. This is not Ring 3 AP socket support.
- `make test-net-udp test-net-icmp test-net-eth test-net-rings test-net-pci`:
  respectively 10/10, 8/8, 8/8, 4/4 and 4/4 pass. Corresponding codec/socket/
  IPv4/ping/worker/ring sanitizer regressions pass. Absent-NIC cases remain covered.
- `python3 scripts/test_net_tcp_matrix.py --retention`: assertion, timeout and
  QEMU-crash inputs/artifacts retained; directories `565067cb`, `0348b820`,
  `5a02347b` under `build/net2-step5` remain deliberately failed manifests.
- `python3 tests/test_dns_fixture.py build/net2-step7/e5a707f1/bios-e1000-socket-s1`:
  independent audit passes for the intact case and rejects missing/malformed
  injection logs and counterfeit application output.
- Final-image focused DNS run `build/net2-step7/18c26ac0`: BIOS/e1000e/socket
  **1/1 PASS** after moving backwards-clock hint inspection under its existing
  manager lock. Independent DNS/TCP/ICMP captures and shell recovery pass.
- `python3 scripts/test_net_tcp_deadlines.py`: BIOS/UEFI **2/2 PASS** in
  `build/net2-step7/98498f8f`. Actual Ring 3 CONNECT_UNTIL, shared dup/close fd,
  accepted 8-KiB SEND followed by zero-window blocked SEND_UNTIL timeout,
  unanswered SYN timeout, prompt recovery and ping. The independent audit
  verifies zero advertised window, one-byte persist probes and unanswered SYNs;
  accepted TX deliberately remains undelivered, so no full-transfer claim.
- `python3 scripts/test_net_dns_lifecycle.py`: BIOS/UEFI **2/2 PASS** in
  `build/net2-step7/e6f6a3df`. Caught SIGINT during timed TCP fallback,
  STOP beyond the original deadline then CONT→DNS_TIMEOUT, KILL/close recovery,
  actual shell and ping. Runner termination is never accepted as a guest timeout.

The host suite additionally forces SYN/ACK completion at the exact application
CONNECT deadline, before the syscall retry: ETIMEDOUT still cancels the endpoint
and existing raced-established teardown sends FIN. It checks the no-start expiry
separately from this completion race. The buffered-data/FIN tests verify an
expired timed receive consumes nothing, then untimed reads deliver data and EOF.

The full user-backend DNS command needs loopback UDP/TCP 53 bind permission;
it never changes system port policy or silently escalates. Actual full-run command:

```powershell
wsl -d Ubuntu-24.04 -u root -- env GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=safe.directory GIT_CONFIG_VALUE_0=/mnt/c/Sources/FortressOS python3 scripts/test_net_dns.py --all --jobs 4
```

The temporary per-command Git setting permits recording the existing checkout
revision as root; no global Git configuration is changed. Socket-only cases,
host tests, lifecycle and deadline runners require no privileged DNS bind.

The independent DNS audit imports no peer protocol helper. It reads both pcap
directions, verifies IP/UDP/TCP bytes and checks exact DNS TCP framing against
actual retained UART results. Required capture, injection log and application
logs must exist and parse; missing/malformed injection logs or counterfeit
output fail. Vectors are hand-computed RFC 1035 bytes with a worksheet, not a
claimed Linux pcap. Fixtures may reuse transport-peer helpers; the audit does
not share their encoders/response constructors.

Case directories retain exact argv, disposable ISO/OVMF, source copies, versions,
UART, pcap, injection/events, stream expectations, manifest and audit results.
The existing persistent Case teardown drains/flushed evidence before reaping
QEMU; assertion, timeout and QEMU-crash retention are independently exercised.
No data disk enters these tests. Host adapters alone make no scheduler/IRQ claim.

## Physical acceptance — user-confirmed PASS (2026-10-02)

The user confirms all six cases on Dell Latitude 5590, I219 8086:15D7,
MAC c8:f7:50:0e:35:80, guest 192.168.0.168 and gateway 192.168.0.1.
The Windows peer was 192.168.0.153 on Wi-Fi. The authoritative session note is
[physical notes](evidence/net2-step7-physical.md).

| Case | User-confirmed result |
| --- | --- |
| service.test | UDP A returned Address: 192.168.0.153 |
| alias.test | CNAME chain returned Name: service.test and Address: 192.168.0.153 |
| missing.test | nslookup: DNS_NXDOMAIN; exit 1 |
| tcp.test | UDP TC triggered TCP fallback; Address: 192.168.0.153 |
| stall.test | nslookup: DNS_TIMEOUT; exit 1; bounded recovery |
| hostname nc | `echo fortress-dns \| nc -s 192.168.0.153 service.test 7777` echoed successfully; prompt returned |

Every case was followed by a passing `ping -c 1 192.168.0.1` and a usable
shell. The session used the controlled DNS fixture on UDP/TCP 53 and the peer's
echo server on TCP 7777, after the 120-second reboot quiet period. No driver,
kernel, scheduler, signal, lock-rank, timer-hook, DMA or transport behavior was
changed during this physical session.

The user identifies `dns-physical-2026-10-02.pcapng`, Dell screen photographs,
the DNS fixture log and `nc-hostname.peer.stdout` as evidence. This closure
records the user's acceptance; the peer pcap/photos/fixture log were not supplied
for an independent repository audit. No new checksum or exact wall-clock timing
claim is inferred. Section E is accepted and NET-2 is **7/7 complete**.

## Physical procedure retained for reproduction

Keep `net=192.168.0.168/24,192.168.0.1 verbose`; connect Ethernet before boot.
On the Windows peer (update .153 if its address changed), start the controlled
fixture from this checkout:

```powershell
python scripts/dns_lan_server.py --bind 192.168.0.153 --answer 192.168.0.153
```

Permit incoming UDP and TCP port 53 on the peer's active LAN firewall profile.
Capture that adapter in Wireshark, with this display filter:

```text
ip.addr == 192.168.0.168 && (udp.port == 53 || tcp.port == 53)
```

Run on FortressOS, retaining output and separate capture files:

```text
nslookup -s 192.168.0.153 service.test
nslookup -s 192.168.0.153 alias.test
nslookup -s 192.168.0.153 missing.test
nslookup -s 192.168.0.153 tcp.test
nslookup -s 192.168.0.153 stall.test
```

Expect A=.153; alias canonical service.test; DNS_NXDOMAIN; UDP TC followed by
TCP A=.153; and guest DNS_TIMEOUT for the stalled TCP response. The TCP cases
require the existing 120-second reboot quiet period to have ended; DNS_TCP_QUIET
returns immediately before it ends. For hostname nc, run a finite echo service
on the peer and `echo fortress-dns | nc -s 192.168.0.153 service.test <port>`.
The peer must consume the request before returning a large response, per nc's
documented serial limitation. Retain the TCP stream alongside DNS.

After every case, record prompt recovery and `ping -c 1 192.168.0.1`, exact
commands, elapsed stalled-case time and capture filename. Save original pcapng
files, not only screenshots. The user-confirmed session above closes section E.
