# NET-2 Step 4: listener, ACCEPT and finite server

2026-10-02. Implemented after Step 3 commit ce37bd3 and recorded in the discrete
Step 4 commit boundary containing this report;
no physical TCP or complete NET-2 acceptance claimed.

## Behavior and ownership

SYS_LISTEN=44 requires a bound unused TCP socket and backlog 1..4; invalid,
unbound or repeated LISTEN returns EINVAL. SYS_ACCEPT=45 blocks indefinitely
using existing generation/event snapshots, sched_wait_until and sched_wake_all.
Optional peer sockaddr/capacity outputs are paired, validated after every wait
and after the final interruption check, including STOP/CONT. Flags are 0 or
NET_SOCK_CLOEXEC. Existing UDP syscalls and SYS_NETCTL are unchanged.

Four static pending entries per endpoint share the listener backlog between
SYN_RCVD and completed children. A listener consumes one of eight global blocks;
each child consumes another and reserves TIME_WAIT capacity before SYN/ACK.
New SYN overflow/global shortage silently drops; duplicate full tuples reuse
their connection. Half-open handshake deadline is absolute 30 seconds. The
worker removes expired/reset entries and reaps their bounded orphan teardown.
SYN ECN offers are accepted with plain SYN/ACK declining ECN negotiation; no
ECN transport support is introduced. Each new accepted SYN zero-initializes one
18200-byte connection and scans finite endpoint/backlog/pool arrays under its
per-segment IRQ exclusion, never allocating heap memory or sleeping.
Completed children behind half-opens are eligible; no queue fairness guarantee.

ACCEPT stages only a common handle and heap file/node, never another transport
block. Child remains queued through validation/allocation/fd failure. fd_alloc
inserts the initialized file, then manager-lock adoption/removal transfers the
existing block; no fallible work or scheduling remains. Staged rollback uses
net_tcp_unstage and cannot close the queued child. Successful adoption completes
flags and unlocked user copies with BSP IF clear. Mismatch traps rather than
silently dropping a child. No child/resources are retained over sleep or signal
checks. Competing acceptors wake together; one takes a child and others recheck.

Per-continuation fd tables share file references through spawn/dup, not table
mutation. Another process closing its listener fd preserves the acceptor's
reference. Defensive generation invalidation gives EBADF; caught interruption
gives EINTR. KILL unwinds and uses existing termination. Final listener close
detaches only unaccepted children; accepted endpoints are independent.
Passive children/TIME_WAIT reserve full tuples rather than exclusive listener
ports, permitting a listener rebind while an accepted child remains live.
Live listeners/client binds still conflict; exact retained tuples stay excluded.

No scheduler, signal, wait signature, timer hook, lock-rank, boot-order, driver,
DMA or pure transport algorithm change. Preserve uninterrupted per-block
prepare/submission/commit and its rejected-commit trap before pool maintenance.
64 RX packets remain per worker pass, not tick; segment IRQ state is restored.
Final-close captures stream/staged kind under the common socket lock before
releasing its slot, preserving AP-close/BSP-reuse exclusion. A deterministic
host unlock hook attempts staged reuse in that interval and proves the original
adopted child is detached using its captured kind without a trap or leak.

## Bounds

Endpoint records are now 128 bytes each (2048 total), including four 16-byte
child block/generation entries. The engine pool remains 146392 bytes with
8 x 8192-byte RX/TX rings, 32 retransmission entries per block and 16 TIME_WAIT
records. A 16-byte passive-TIME_WAIT classification array allows port reuse.
Measured manager BSS=151712 plus initialized lock=40; syscall staging BSS=8192.
Total mutable TCP static storage=159944 bytes (1312 above Step 3), including
object alignment. Existing common socket storage layout still fits the added
staged flag without growing its header. Heap file/node allocations use existing
common socket ownership. Code/rodata, user fixtures and transitive stack are extra.
Compiler stack reports: net_tcp_input=240, net_tcp_tick=128,
net_tcp_accept_commit=80, net_tcp_syscall=176 bytes; strict <=512 frame gate passes.
These are individual frames, not worst-case IRQ latency or total stack proof.

## Verification

The three client regressions were already in ce37bd3, not deferred. Before any
listener code, `make test-net-tcp-socket-host test-net-tcp-tcb-host` passed again.

Executed after implementation:

- `make test-net-tcp-socket-host test-net-socket-host test-net-tcp-host
  test-net-tcp-tcb-host test-net-ipv4-host test-net-eth-host`: ASan/UBSan PASS.
  Listener host additions exercise backlog/duplicate SYN, completed child behind
  a half-open, 30-second expiry, global pool shortage, output/signal/heap/fd/common
  handle rollback, fd insertion before removal, adoption with all eight blocks
  occupied, single delivery, child survival/rebinding and defensive invalidation.
  Host signals/scheduler/NIC are adapters; the host peer engine is not independent.
- `python3 scripts/test_net_tcp_server.py`: 5/5 PASS, BIOS/UEFI x e1000/e1000e
  SMP=1 plus BIOS/e1000 SMP=4 BSP-tool/AP direct-dispatch rejection smoke.
  Every case exercises real Ring 3 finite 64KiB echo, unaligned cross-page ACCEPT
  output and CLOEXEC, child traffic after listener close, caught SIGINT,
  inherited listener reference after parent close, two competing acceptors,
  STOP >9 seconds then CONT, KILL/rebind recovery, ping and independent outbound
  capture checksum/padding/exact stream reconstruction/FIN audits.
- `python3 scripts/test_net_tcp_client.py --case bios-e1000`: PASS for the
  existing client gates including 64KiB both ways, reset, caught CONNECT/receive,
  read/write/dup/half-close, UDP/ICMP concurrency, STOP/CONT, KILL and pcap audit.
- Strict `make bin/fortress.iso` / `make bin/fortress.elf` and full `make`: PASS;
  boot image regenerated with GPT/FAT/ext2/backup-GPT checks and e2fsck 0 errors.
- `make test-net-udp test-net-icmp test-net-rings test-net-pci`: PASS, UDP 10/10,
  ICMP 8/8, rings 4/4 and PCI 4/4 including absent-device fallback.
- Late ECN-offer ingress compatibility adjustment: host listener/client fence
  and strict rebuild PASS; focused final BIOS/e1000 server rerun PASS.
  Subsequent common-slot kind capture fix: TCP/UDP socket and Ethernet host
  sanitizer suites PASS, including deterministic close/reuse interleaving;
  strict ISO rebuild PASS. Earlier matrix results are not represented as fresh
  physical/AP-close measurements of that adapter-tested race.

The first development runner timed out after STOP/CONT despite guest PASS:
matching the last command text accidentally selected the later job Done line.
Fixed by retaining a command-start offset; focused rerun and full matrix passed.
No guest/scheduler fix was needed for that runner error.

QEMU user host forwarding terminates TCP in independent SLIRP and relays bytes
to a Linux socket app; it is not a direct Linux-kernel cable peer. Pcap audits
cover guest outbound wire frames, not a physical cable capture. Fixtures use
disposable ISO/OVMF copies, exact-argv unsafe-storage rejection, no data disks.
Artifacts: build/net2-step4-*-serial.log / *-wire.pcap / *-stderr.log (ignored).
Socket-backend TCP matrix, general nc and physical hardware gates remain later
steps. No Ring 3 AP socket support, FIFO fairness or measured physical IRQ claim.

## Use

`tcpserve <port>` binds locally, accepts one connection, closes the listener,
streams at most 65536 echoed bytes through 1024-byte chunks, then SHUT_WR/closes.
The peer must drain echoes while sending; this is a finite verification server,
not general nc. `--caught`, `--shared` and `--compete` are lifecycle fixtures.
`make test-net-tcp-server` builds and runs host gates plus the five-case matrix.
Next is Step 5 finite nc and the complete TCP backend matrix.
