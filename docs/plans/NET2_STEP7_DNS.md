# NET-2 Step 7 — userspace DNS and nslookup

Status: COMPLETE (2026-10-02); Dell 5590 physical acceptance confirmed by the user.
The user's implementation request approves the reviewed DNS API and TCP deadline
design. Host/QEMU evidence and retained failures are recorded in
[Step 7 evidence](../roadmap/net2-step7.md). This document does not authorize
further socket ABI changes. Section E is accepted; NET-2 is 7/7 complete.

## Outcome and scope

Implement a shared freestanding userspace IPv4 stub resolver, `nslookup`, and
hostname resolution for the existing finite `nc` client. Use an explicit DNS
server; do not assume the gateway provides DNS.

```
nslookup -s <server-IPv4> <name>
nc -s <server-IPv4> <host> <port>
nc <IPv4> <port>
nc -l <port>
```

Numeric nc targets bypass DNS completely. Listener behavior, terminal-stdin
skip, single accept, and the serial nc limitation remain unchanged. Resolving
a name does not give nc's subsequent application exchange a timeout.

Support IN/A queries, compressed names, bounded CNAME following and UDP-to-TCP
fallback. Defer AAAA, reverse lookup, DNSSEC, caching, EDNS, search domains,
resolver files, DHCP configuration and iterative referral chasing. Request
recursion from the explicitly selected server. No shell-parser integration.

## A — approved deadline decision and DNS API review gate

The existing socket ABI supports a five-second blocking UDP receive, but TCP
receive has no application timeout. CONNECT has a 30-second handshake deadline
and a separate 120-second reboot quiet-time restriction. A server that accepts
TCP and never finishes its DNS response can currently block the resolver
indefinitely. Nonblocking retry loops would busy-poll; they are not acceptable.

Decision made: use the narrow, opt-in TCP I/O deadline extension for fallback.
Its concrete ABI, timeout error precedence, partial-I/O semantics,
generation/lifetime checks and worker wakeup proof are specified in the separate
addendum [TCP_IO_DEADLINE.md](TCP_IO_DEADLINE.md), now approved and implemented;
host/live evidence is recorded in the Step 7 roadmap entry.
Preserve existing untimed operations and SYS_NETCTL. Reuse existing
wait/wake primitives; no scheduler, signal, lock-rank, sched_wait_until-signature,
timer-hook, DMA or driver change. If those boundaries cannot be maintained,
stop and report.

Alternative: deliver UDP-only DNS as an explicitly partial milestone, with TC
reported as unsupported. This does not satisfy the current Step 7 TCP-fallback
gate and does not close NET-2. Ctrl-C support and host-runner termination are
not substitutes for a guest-side resolver deadline.

Before codec/transaction implementation, review [DNS_USER_API.md](DNS_USER_API.md): function
prototypes, context/result field types and sizes, ownership, limits, numeric
bypass, error mapping and publication rules. This is a userspace library API,
not a new DNS syscall. The user owns this explicit pre-coding approval gate;
additional reviewers may be invited by the user.

DNS_USER_API.md review criteria: function prototypes and argument types;
context/result struct layout with explicit ownership; library-status mapping
from existing project errno values; the 30-second budget and five-second UDP
overshoot; numeric bypass; publication rules for partial results; single-caller
context/thread-safety rules; and the exact nslookup output format. No DNS codec
or transaction code lands before this API is approved.

Approved policy: three UDP attempts, at most eight CNAME links,
eight returned IPv4 addresses, 64 resource records per response, a 4096-byte
TCP response cap, and a 30-second overall resolution budget. Each blocking
operation must honor the remaining budget; retries, discarded packets, CNAME
queries and fragmented TCP reads must not restart it. Existing UDP receive
timeouts may overshoot a remaining budget by up to five seconds unless an
approved deadline mechanism also covers UDP; document the chosen bound
explicitly rather than claiming an exact overall deadline.

## B — pure codec and hostile-input tests

Separate bounded wire encoding/decoding from sockets and tool output. Use
caller-owned storage, checked arithmetic and explicit network byte order.
Bound presentation input, labels, expanded wire names and compression traversal.
Reject cycles, invalid offsets, unsupported label encodings, truncated fields
and inconsistent record lengths. Validate unknown records structurally before
skipping them. No recursive parser and no response-driven allocation.

Validate response direction/opcode, transaction ID and exactly one matching
IN/A question, using DNS case-insensitive name comparison as specified in
[DNS_USER_API.md](DNS_USER_API.md#finite-policy-and-transaction-contract).
Bound all declared
section counts before traversal. Follow only a CNAME chain rooted at the
question, with loop detection and a resolution-wide hop budget; unrelated A
records must never become results. Distinguish NXDOMAIN, NOERROR/no A data,
server failure, malformed response and resource-limit failure. Publish results
only after validation; failure leaves the caller's result unchanged.

Gate: ASan/UBSan tests with independently written literal wire vectors and a
worksheet explaining offsets/checksums where applicable. Include compression
cycles, nested pointers, mixed case, trailing dot, maximum names, oversized
counts/RDATA, unrelated answers, CNAME loops/chains, negative replies and
deterministic malformed-input fuzz. Encoder/decoder round trips alone are not
the golden reference.

## C — bounded socket transaction

Implementation prerequisite: DNS TCP fallback requires SYS_SEND_UNTIL,
SYS_RECV_UNTIL and SYS_CONNECT_UNTIL to be implemented and verified per
[TCP_IO_DEADLINE.md](TCP_IO_DEADLINE.md) gates 3–5. Checkpoint B and the UDP
side of C can be implemented and tested independently after DNS_USER_API.md
approval. Implementation order: codec, UDP transaction, deadline extension
implementation/verification, TCP fallback, then full D/E acceptance. A UDP-only
nslookup can ship as a partial milestone; NET-2 remains open until fallback and
the remaining gates pass.

Use CLOEXEC descriptors and close every descriptor on success, error, timeout
and interruption. Match UDP source IPv4 and port as well as ID and question.
Give ignored packets a finite count budget, independent of time. Define ID
generation using available inputs without claiming cryptographic entropy or
off-path spoofing resistance.

Start with classic UDP DNS, no EDNS. Receive into the full 1472-byte datagram
capacity so the socket's discard-on-short-buffer behavior cannot disguise an
oversized response as a valid 512-byte message. Reject over-policy messages.
On a matching, valid truncated response, discard partial answers and retry the
query over TCP to the same server and port. A truncated response need not have
complete answer records: validate the header/question needed to authorize
fallback without requiring the truncated tail to parse as a complete message.

Implement the two-byte TCP length prefix with exact short-read/short-write
loops. Reject zero, undersized and over-cap lengths before reading the body.
EOF/reset mid-prefix or body is an error. Bound stalled and trickled responses
using checkpoint A's approved design. Do not wait for TCP EOF after receiving
the complete framed answer. No TCP transport or window workaround.

Gate: actual userspace resolver under syscall adapters, covering successful
UDP, wrong peer/ID/question, retry exhaustion, packet floods, CNAME follow-up,
TC fallback, fragmented prefix/body, partial writes, premature EOF, reset,
stalled/trickled TCP, caught interruption and complete descriptor cleanup.
Test numeric bypass with a socket adapter that fails if DNS is attempted.

## D — tools and live QEMU evidence

Package `/bin/nslookup`; link the shared resolver into nc. Print server,
canonical name and returned IPv4 addresses with stable diagnostics and nonzero
failure status. nc uses the first returned address in wire order; automatic
address failover is deferred. Reject malformed CLI input before socket creation.

Use a deterministic DNS fixture serving controlled `.test` names on UDP/TCP
port 53. The socket backend can synthesize port-53 frames without privileged
host binds. For the user backend, settle port-53 hosting/forwarding during
fixture design; do not silently add a production CLI port option or depend on
public DNS. Retain exact fixture configuration and QEMU argv.

Gate: BIOS/UEFI × e1000/e1000e × user/socket, SMP=1, plus existing BSP/AP
dispatch smoke where applicable. Exercise A, CNAME, NXDOMAIN, malformed input,
UDP timeout, TCP fallback, stalled fallback, Ctrl-C recovery and hostname nc
against a cooperating finite service. Demonstrate prompt recovery and follow-up
ping. Do not turn a runner timeout into a claimed guest timeout pass.

An independent audit must compare application results and both capture
directions, including fallback framing and response identity. It must not import
the fixture's protocol helpers. Missing/malformed required logs or capture
inputs fail the case. Flush pcaps, injection logs, UART logs, manifest, audit
diagnostics and exact argv to persistent case directories before teardown;
verify retention on assertion, timeout and QEMU crash.

Run relevant new host gates, then the Step 3 client, Step 4 server and Step 5
TCP matrix; UDP, ICMP, ARP/worker, rings and absent-NIC regressions remain
required. Record commands actually run and retain failures. Any approved socket
deadline extension also requires pointer/error, shared-fd, owner-exit,
generation-reuse and wait-lifecycle regression coverage.

## E — physical acceptance and closure

On Dell 5590, keep the accepted numeric network boot configuration and connect
Ethernet before boot. Use an explicit reachable DNS server, preferably a
controlled LAN UDP/TCP fixture with known `.test` records. Independently capture
on that peer with Wireshark.

Record UDP A success, CNAME success, NXDOMAIN, forced TC-to-TCP success,
unresponsive/stalled server returning within the documented guest bound, and
hostname nc to a real finite LAN service. For every case record exact commands,
expected/observed output, capture filename, prompt recovery and follow-up ping.
Keep physical evidence separate from QEMU results; no automated physical-pass
target. Close Step 7 only after host/live gates and user-reviewed physical
evidence agree. Update the roadmap, subsystem status, ABI/proof addenda and
NET-2 completion count in the final commit.

## References

DNS wire format: [RFC 1035](https://www.rfc-editor.org/rfc/rfc1035.html),
sections 2.3.4 and 4.1–4.2. DNS TCP requirements:
[RFC 7766](https://www.rfc-editor.org/rfc/rfc7766.html).
Implementation policy limits above are FortressOS policy limits, not claims
of complete DNS conformance. Existing contracts: [UDP socket ABI](UDP_SOCKET_ABI.md),
[TCP socket ABI](TCP_SOCKET_ABI.md) and [TCP wait proof](TCP_WAIT_LIFECYCLE_PROOF.md).
