# NET-2 Step 5 — finite nc and independent TCP matrix

2026-10-02. Builds on Step 4 boundary f925aa1. Physical TCP acceptance and DNS
remain Steps 6 and 7; this checkpoint does not close NET-2.

Status: CLOSED by user acceptance. NET-2 is 5/7 done. Serial nc remains bounded
only by peer cooperation, errors or external cancellation; it has no automatic
application timeout. Physical TCP acceptance remains unverified.

## User interface

`nc <numeric-IPv4> <port>` connects. `nc -l <port>` accepts one child and closes
the listener. Both copy stdin with short-write handling, call SHUT_WR on stdin
EOF, then copy receive bytes to stdout until peer EOF. Binary data and empty
stdin work. Buffered data is printed before a following reset/error; I/O failure
returns nonzero. Descriptors use CLOEXEC and ordinary cleanup. Existing socket
read/write pointer, signal and SIGPIPE behavior is preserved.

Subsequent Step 6 Case C update: listener mode now skips terminal stdin using
the existing TERM_ISATTY operation, then half-closes and receives. Plain
`nc -l port` therefore works as a receive-only terminal command. Pipes/files
retain the serial flow above; client mode is unchanged. See the
[Case C investigation and update](net2-step6-caseC.md).

Examples after the existing 120-second CONNECT reboot quiet period:

```
echo hello | nc 192.168.0.222 7777
nc 192.168.0.222 7777 < /dev/null
echo listener-input | nc -l 7777
```

This is a serial request/response tool. Its peer must consume the request before
sending a large response. It does not forward stdin and socket simultaneously;
interactive/full-duplex readiness, repeated accepts, UDP, hostnames, scanning
and command execution are outside this step. No automatic HTTP request.

A peer that sends a large response before consuming the whole request can
deadlock the serial nc; use small finite requests or a cooperating peer.
The same warning appears in nc's usage text. Neither nc nor blocking TCP I/O
has an application timeout. A peer-controlled deadline/reset can bound this
case; a test-runner timeout alone does not prove nc terminates.

## Independent fixture

The [prerequisite plan](../plans/NET2_STEP5_SOCKET_FIXTURE.md) was written before
the peer/matrix and nc implementation. Immutable hand-computed wire vectors and
the [worksheet](../../tests/fixtures/NET_TCP_WIRE_WORKSHEET.md) cover SYN with and
without MSS, SYN/ACK, ACK, odd-length data, FIN/ACK, RST/ACK and sequence wrap.

`net_tcp_socket_peer.py` independently encodes/decodes Ethernet/IP/TCP and
implements only two finite test roles. Eight connection records, 8192 outstanding
payload/OOO bytes, 32 retained segments, eight retries, 65536 application bytes
each way, 20000 packet/event limits and a 120-second traffic deadline are
explicit failure boundaries. The real 120-second guest quiet period gets its
own 180-second warm-up bound. SYN/FIN consume sequence space, duplicate bytes
cannot append twice, gaps cannot be cumulatively acknowledged and advertised
windows bound injection, including FIN. Four half-open listener entries are
held explicitly; overflow, duplicate SYN, non-head completion and deadline
reset/recreation are separate observed gates.

The zero-window FIN bug was fixed in the peer, not the guest.

`net_tcp_wire_audit.py` imports no peer protocol helper. Its own checksum/parser
and bounded stream reconstruction check tuples, header/options/padding,
handshake, exact byte offsets/hashes, retransmitted overlaps, MSS, cumulative
ACK bounds/gaps and FIN accounting. It ignores peer PASS fields. Both programs
are checked against literal vectors; malformed checksums/headers and a forged
PASS must still fail the audit. The audit uses both filter-dump directions in
capture order and matches injected raw bytes with the injection log. Host wall
time is not merged with QEMU capture time: WSL exhibited a one-hour offset.
ARP replies are synthesized through loopback UDP; filter-dump only captures.

The socket runner always supplies the injection-log path. A missing file,
malformed JSON, missing hex field, invalid hex, or an unlogged inbound frame
raises an error and fails the case; there is no wall-clock or pcap-only fallback.
Passing `None` explicitly selects the user/SLIRP audit, where no synthetic
injection log exists. A host regression exercises the missing/malformed/empty
socket-log failures.

The three live nc flows are a six-byte `hello\n` client request, an empty client
request using `/dev/null`, and a one-child listener sending the fifteen-byte
`listener-input\n` request. Each peer waits for request EOF before responding.
These original matrix flows cover empty and short requests. The supplemental
`make test-net-nc-boundaries` uses BIOS/e1000/SLIRP, a disposable ISO and no data
disks. Its cooperating peer receives `/bin/nc` as an exact 17992-byte binary
request: 13 captured data segments, independent complete stream audit, response
and shell recovery PASS (2c9fe4fb/nc-large). The host adapter separately forces
over 1000 positive short sends across 8193 bytes through the actual nc loop;
that sanitizer test does not model TCP backpressure. The original live 64KiB
transfers use tcptest/tcpserve, not nc.

The supplemental BIOS/e1000/socket early-response case runs
`nc -l 9000 < /bin/nc > /dev/null`. The peer advertises zero request space,
consumes zero request bytes, and attempts a 32KiB response. After filling the
guest's 8192-byte receive buffer it sends a sequence-valid RST at the acknowledged
receive position on its three-second deadline. PASS: nc reports an I/O error
and returns the shell prompt in 3.06 seconds; a following shell command works
(`early-socket-gate`). The boundary audit checks capture/injection byte agreement,
checksums, at least 8192 response bytes, RST and no guest request FIN. It does not
apply complete-transfer assertions to this intentionally incomplete stream.
This is bounded peer-reset recovery, not a built-in nc timeout or a full-duplex
guarantee. Initial SLIRP attempts are retained (2c9fe4fb/nc-early-reset,
early-window-gate, early-large-gate): host/SLIRP buffering did not establish the
intended boundary, including a serial evidence-budget failure. No kernel change
was made to obtain either supplemental pass.

Supplemental commands: `make test-net-nc-boundaries` built the disposable ISO
and ran the exact large request gate; the refined early gate was executed with
`python3 -c "from pathlib import Path; import sys; sys.path.insert(0,'scripts');
from test_net_nc_boundaries import run;
run(Path('build/net2-step5/early-socket-gate'),True)"`.
`make test-net-nc-host test-net-tcp-fixture`: sanitizer nc PASS and 9/9 fixture
host tests PASS. These supplemental cases are not a rerun of the ten-case matrix.

The prerequisite live BIOS/e1000/socket gate used existing tcptest/tcpserve
before nc coding: 64KiB each direction, MSS 536/1460/omitted, wrap, ECN declined,
SYN/ACK loss, data loss/duplicate/gap, zero-window probe/reopen, FIN/ACK loss and
invalid checksum/tuple/future ACK. The separate listener backlog gate completes
a non-head child, observes half-open expiry RST and a new ISN on the reused tuple.

## Artifacts and runner boundaries

Each case creates `build/net2-step5/<run-id>/<case>/` before starting QEMU.
Serial output is drained by one dedicated host reader over an ephemeral /tmp
Unix socket, into a bounded 1MiB evidence buffer. The event loop writes chunks
directly into persistent serial.log with at most four flushes per second. The
reader never waits on filesystem writes or peer processing. A host regression
sends 500KB (more than socket buffering) with no logger progress, then verifies
the exact persisted bytes and reader shutdown. UART connection gates VM startup so
early serial evidence is retained. QEMU writes wire.pcap and qemu-stderr.log
directly there. Injection/events, exact argv, ports/limits/scenario, tool versions,
golden hash, streams and final result/error/traceback/audit are retained, as are
the test ISO/config and disposable OVMF vars. No fixture data disk is attached.

Finally stops injection, terminates/waits up to three seconds then kills/waits,
audits after QEMU exit and flushes/fsyncs host writers/manifests. Missing artifacts
are recorded. --retention forces assertion failure, timeout and QEMU crash,
checks retained files and proves the PID was reaped. A truncated pcap is preserved
and fails audit. Unflushed SIGKILL tails, disk-full, host crash and power loss
remain outside retention guarantees. Capture/app disagreement is a failing gate.

## Executed verification

Commands run from the repository under WSL Ubuntu-24.04:

- `make`: strict build/static nc ELF, ISO and 130MiB raw disk image; GPT/FAT32/ext2
  image verification and e2fsck zero errors. nc has 4096-byte BSS and separate
  RX/R/RW load pages, no undefined hosted symbols.
- `make test-net-tcp-host test-net-tcp-tcb-host test-net-tcp-socket-host
  test-net-nc-host test-net-tcp-fixture`: sanitizer client fence, transport/codec,
  Step 4 adoption/kind-capture regressions, actual nc adapters and eight fixture
  host tests PASS. Fixture Python tests are not ASan/kernel evidence.
- `python3 scripts/test_net_tcp_matrix.py --core`: BIOS/e1000/socket both roles,
  eight finite connections and named profiles PASS. Supplementary ECN-offer/
  dropped-final-handshake-ACK gate is included. Saved pass: 5f47ce99.
- `python3 scripts/test_net_tcp_matrix.py --backlog`: PASS (e404b1ba). Independent
  re-audit observes expiry resets at 30.051–30.052 capture seconds after SYN and
  validates changed ISN/recovery plus non-head exact 64KiB echo.
- `python3 scripts/test_net_tcp_matrix.py --all --jobs 4`: **10/10 PASS in one
  complete run**, b8495dac/matrix.json, after independent UART draining. Every
  case proves large client/server directions plus three nc flows. No retries
  or failed attempts are counted as passes in this run.
- `make test-net-tcp-retention`: forced assertion, timeout and QEMU crash PASS;
  serial/pcap/argv/ISO/trace/manifests survive and the tested PID is gone.
- `make test-net-tcp-server`: real Step 4 server/lifetime/signals 5/5 PASS.
  `python3 scripts/test_net_tcp_client.py --case bios-e1000`: reset/EOF/caught
  signal/unread-close/client ABI/concurrent UDP/ping/capture regression PASS.
- `make test-net-udp test-net-icmp test-net-rings test-net-pci`: UDP 10/10,
  ICMP 8/8, rings 4/4 and PCI/discovery/absent-NIC 4/4 PASS, with corresponding
  host sanitizer gates.

Host adapters prove logic; QEMU proves these virtual device/Ring 3 paths.
Neither is physical DMA/wire acceptance or general TCP conformance. SMP smoke
remains BSP tools plus kernel AP direct dispatch rejection, not Ring 3 AP
networking. The default matrix target uses two parallel cases; --jobs 1–4 is
explicitly bounded. The real guest quiet period is retained in every client boot.

No scheduler, signal, lock rank, wait signature, timer hook, driver/DMA,
SYS_NETCTL or kernel TCP/socket behavior was changed in this step.

Before dedicated UART draining, three full matrix attempts failed serial
observation. They are not counted as complete passes and remain under
build/net2-step5/: a31088d7 was 9/10 (BIOS/e1000e/socket), 49d646f5 was 9/10
(UEFI/e1000/socket), and 9a9d8d7a was 6/10 (four UEFI cases). Last printed lines
were boot self-tests; the first two affected cases passed isolated reruns
88240f08 and 0533fcf2. Read-only QMP register/CPU/LAPIC state in vm-state.json
placed every third-attempt failed guest at kmain's shell-wait loop
(main.c:5670): boot had continued despite the stopped serial log.

The UART driver's bounded serial_wait_transmit can latch output unavailable
when its host backend cannot drain. These observations are consistent with
host backpressure; putting serial draining and persistent writes/peer work in
one host loop was unsuitable. The dedicated bounded reader removes that
dependency. Its host regression and the final single-run 10/10 result establish
the corrected fixture gate; no protected boot/scheduler/driver change or hidden
retry was used. The final core-profile repeat also passes (8b484f33).
