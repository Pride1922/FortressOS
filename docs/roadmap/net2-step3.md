# NET-2 step 3 — numeric IPv4 TCP client integration

2026-10-02. Client implementation and verification record. Listener/ACCEPT,
finite nc, complete socket-backend matrix, physical TCP and DNS remain steps
4–7. This is not NET-2 closure or physical acceptance.

## Implementation and boundary

SOCK_STREAM=1/protocol 0 or 6 uses the existing 16 common socket handles and a
separate bounded pool of eight TCP connections plus 16 TIME_WAIT records. UDP
still supports all 16 handles when TCP is unused. A new stream reserves a block
and lightweight record with a placeholder tuple that is never sent/demuxed;
CONNECT replaces it with the validated real tuple. Final-reference close can
come from an AP reaper, but only publishes detach/identity invalidation. The
BSP worker alone performs protocol detach/reap, RX input, timers and NIC work.
TCP/UDP bind namespaces are separate; TIME_WAIT/orphan TCP ports stay excluded.
No user, scheduler TCB, file or DMA pointer is stored in connection protocol state.

The pure internal type/API is now tcp_conn_t/tcp_conn_*; source filenames and
historical test target retain tcp_tcb. Layout remains 18200 bytes per connection,
146392 bytes for pool. No transport algorithm was changed by the rename.

Client calls 43 CONNECT, 46 SEND, 47 RECV, 48 SHUTDOWN are implemented; 44/45
LISTEN/ACCEPT are reserved/unsupported. Stream SYS_READ/SYS_WRITE dispatch to
the same validated implementation. Full-width stream lengths cap at 16384;
positive short transfers are normal. SOCK_CLOEXEC, existing dup/inherited fd
references and final-close callbacks are reused. SYS_READ also rejects a
full-width out-of-range fd before narrowing it. SYS_NETCTL=42 remains unchanged.
See [client ABI](../plans/TCP_SOCKET_ABI.md) for errors and precedence.

## Wait/lifecycle design

The review's lease-renewal sketch was replaced before integration by
reservation-free sleeping continuations. A call snapshots static endpoint
generation/event, checks readiness, sleeps through existing sched_wait_until,
then retries/revalidates. Predicates acquire-load atomics only under scheduler
lock; publication precedes an unlocked worker wake. No RX/TX reservation exists
across STOP or sleep, no expired-token history is required, and there is no
periodic EAGAIN or receive timeout. An idle receive has no timer-only event churn.
User-copy and RX consume or TX acceptance occur in one BSP IF-clear continuation
without holding subsystem locks across copy. Accepted TX stays in the connection.
No scheduler, signal, lock-rank, wait-signature or timer-hook change.

Prepare -> unlocked NIC submission -> commit is one bounded BSP IF-clear worker
transaction. Syscall preemption cannot mutate the prepared connection; AP close
only requests deferred detach. No rank-1 socket/device/scheduler locks nest.
NIC backpressure/unresolved ARP never advances submitted sequence or RTO state.
The existing synchronous driver has DD/PIT polling and diagnostics; this
transaction inherits that latency. No physical IRQ-latency/throughput claim.
The [lifetime argument](../plans/TCP_WAIT_LIFECYCLE_PROOF.md) records call sites,
linearization, assumptions and evidence limits. ACCEPT requires a separate gate.

CONNECT uses the engine's independent 30-second handshake deadline. Interruption
requests detach even if establishment raced to completion, leaving a terminal
endpoint that must be closed/recreated. The audit fixed closing after an already
processed cancellation: a detach flag must clear even when its block is gone.
Final close drains/discards orphan ordered RX to avoid a permanently closed
window; it preserves sequence gaps and FIN order, keeps accepted TX until ACK/
failure and retains bounded orphan timers. Attached reset reads buffered bytes
first, then ECONNRESET. SHUT_WR preserves receive; later writes publish writer-
only SIGPIPE/EPIPE using existing signal behavior.

## ISN and reboot policy

ISN uses modulo-32-bit M at 250000 units/second from BSP ticks, choosing previous
M+1 on equal/backward modular samples. Mix the host-order IP words and packed
ports with a salt from BSP ticks sampled at net_tcp_init and the local IP. This
is deterministic/predictable and can repeat across boots: no entropy or RFC
secret-key PRF claim. TIME_WAIT state does not persist across reboot.

TCP CONNECT therefore returns EAGAIN until 120 seconds of BSP uptime (one chosen
MSL), without starting a handshake. Boot explicitly reports this. UDP/ICMP/shell
continue normally; no timer or RTC primitive was added. Quiet time addresses
the assumed finite old-segment lifetime; it is not protection against malicious
sequence guessing or a proof that every LAN segment expires in 120 seconds.
The host test proves early refusal/no TCP frame; final QEMU retry uses actual
guest uptime, no test bypass. A production cryptographic/persistent ISN design
and stronger reboot tests remain future work, not inferred from simulator ISNs.

## Budget and work bounds

Measured final object sections: net_tcp.o BSS 150400 + initialized lock data 40;
net_tcp_syscall.o BSS 8192. Total additional static mutable TCP storage 158632
bytes, including alignment. Main objects: pool 146392, endpoint records 768,
TX payload scratch 1460, frame scratch 1514, action 48, queued TIME_WAIT header
24/tuple 12/flag, per-block ARP pacing 64, RX syscall staging 8192 and small
bookkeeping. Existing UDP data remains separate; code/rodata and user program
memory are additional. No packet-size kernel stack array.

New files compile with -Os, -Wframe-larger-than=512 and -fstack-usage. Largest
individual new frame is net_tcp_input 208 bytes; CONNECT 176, worker tick 112,
stream syscall 128. This is not a complete transitive/IRQ stack proof.

Per pass: existing 64 RX cap, all eight connection timer/teardown sweeps, up to
eight TCP actions plus one queued TIME_WAIT ACK. ARP at most once per block per
second. Sweep runs after a full RX batch. Existing full-batch yield and tick-
deadline idle sleep remain; idle protocol is not a yield loop. Full RX-budget
saturation is bounded by inspection; sustained adversarial physical load is not
claimed. Test-only net_test=tcp enables existing idle accounting and AP direct
dispatch rejection for socket/client calls; it does not bypass quiet time.

## Commands actually executed

- `make`: strict freestanding kernel/user build, ISO and raw image PASS;
  generated image GPT/FAT/ext2 verification PASS, no physical disk writes.
- `make test-net-tcp-host test-net-tcp-tcb-host`: ASan/UBSan PASS; renamed pure
  engine retains all step 2 directed/fault/fuzz/2-MiB gates.
- `make test-net-tcp-socket-host`: actual manager/socket/syscall/IP/TCP code,
  mocked memory/fd/NIC/clock/scheduler and engine peer, ASan/UBSan PASS. 64 KiB
  each way, short acceptance, shutdown/EPIPE, EOF, shared refs, stale identity,
  AP rejection, early quiet/no frame, caught interruption, cancellation/close
  reuse, refusal, absolute handshake timeout and unchanged idle event over 16
  fake seconds. Existing full UDP host baseline also runs inside this fixture.
- `python3 scripts/test_net_tcp_client.py`: initial 5/5 BIOS/UEFI × NIC client
  cases plus BIOS/e1000 SMP=4 BSP smoke PASS. Final focused quiet-time run also
  passed before adding cancelled-CONNECT recovery and concurrent-traffic gates.
- Final focused cases with quiet time, caught CONNECT/reuse, cross-page output
  and TCP/UDP/ICMP concurrency: BIOS/UEFI × e1000/e1000e SMP=1 **4/4 PASS**,
  each invoked as `python3 scripts/test_net_tcp_client.py --case <mode>-<NIC>`.
  All four idle observations were 0 worker CPU ticks / 500 ticks / 100 Hz.
  `--case bios-e1000 --cpus 4` initially failed to parse a byte-interleaved AP/BSP
  diagnostic despite the AP's PASS result. The AP fixture now publishes its
  result atomically for the BSP worker to print; strict build/host worker PASS.
  The final SMP=4 rerun **PASS**, with 0 worker CPU ticks / 500 ticks / 100 Hz.
  This is AP kernel direct dispatch, not Ring 3 AP entry.
  A final audit then limited ARP resolution to actual prepared TCP actions,
  preventing an attached failed/no-action endpoint from sending ARP forever.
  Host timeout/no-further-ARP regression and strict build PASS; the final
  `--case bios-e1000` live rerun **PASS**, including concurrent TCP/UDP/ICMP,
  caught CONNECT, STOP/CONT, reset, half-close and capture audit.
- `make test-net-udp test-net-icmp test-net-rings test-net-pci`: UDP 10/10,
  ICMP 8/8, rings 4/4 and PCI/absent-NIC 4/4 PASS. Automated driver ownership/
  failure sanitizer tests run within rings. UDP idle observations: 0 worker
  CPU ticks / 500 BSP ticks / 100 Hz across its ten cases.
- `make test-net-host test-net-ipv4-host test-net-eth-host test-s8-sigpipe-host`:
  PASS (Phase 0 103/103); SIGPIPE uses host adapters, not a handler-frame/IRQ claim.
- `git diff --check`: PASS.

The live fixture uses QEMU user networking: the independent guest TCP peer is
SLIRP, which relays to Linux host socket applications. It is not a direct Linux
kernel TCP wire peer or physical LAN capture. See
[QEMU network documentation](https://www.qemu.org/docs/master/system/devices/net.html)
and [SLIRP architecture](https://kvm-forum.qemu.org/2022/Slirp%20is%20dead%2C%20long%20live%20Slirp%21%20KVM%20Forum%202022.pdf).
Real Ring 3 verifies 64 KiB request/response, read/write, pointer/dup behavior,
half-close/EOF, buffered-before-reset, caught receive, unread close and refusal;
SMP=1 extended cases cover STOP >9 seconds then CONT, 16-second receive wait and
KILL/recovery. Independent outbound pcap audit verifies IP/TCP checksum, exact
ordered request bytes/retransmitted consistency, SYN/FIN and Ethernet padding.
Fixtures use disposable ISO/OVMF vars, exact argv preflight, no data disks.

## Use and next step

Reviewer follow-up (2026-10-02): `make test-net-tcp-socket-host
test-net-tcp-tcb-host` passed under ASan/UBSan after adding deterministic
cancelled-CONNECT/SYN-ACK delivery at the same fake timestamp before worker
detach (FIN observed, no RST), exact space-limited SEND counts, pending-signal
zero acceptance, and post-acceptance signal publication preserving the short
count with exact peer byte totals. The latter uses a host-only linker wrapper
around actual net_tcp_send; signal publication/scheduler remain adapters, not
real SIGINT handler delivery. Repeated same-timestamp ticks before and after
retransmission commit preserve deadlines, retry count and RTO backoff while
still advancing action revision. Source comments now explicitly require the
IF-clear prepare/commit transaction to finish before subsequent block/pool
ticks. Plan/ABI clarify per-pass RX scope and state-dependent cancellation
wire behavior. `git diff --check` passed; no new QEMU or physical run claimed.
Additional invariant guard: the worker now traps on a rejected prepared-action
commit instead of ignoring it; ordinary failed submission still uses a valid
submitted=false commit. The NET2 plan explicitly requires uninterrupted
prepare/commit ordering before pool maintenance, including future listener work.
Both TCP host sanitizer suites and `make bin/fortress.elf` passed again with
this guard enabled.

After the quiet period, `tcptest <IPv4> <port>` sends a deterministic 65536-byte
binary request, SHUT_WR, then verifies the same bytes returned and peer EOF.
It requires a cooperating finite binary responder; it is a diagnostic client,
not netcat or an arbitrary text echo command. Local capture artifacts are
build/net2-step3-*.log / *.pcap (ignored build outputs).

Next is step 4 listener/accept/backlog ownership. No physical TCP, general nc,
DNS, simultaneous interactive forwarding, cross-core socket API or adversarial
load acceptance is claimed here. Changes are uncommitted at this checkpoint.
