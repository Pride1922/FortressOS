# DNS userspace API — Step 7 review gate

Status: APPROVED AND IMPLEMENTED (2026-10-02). The user's implementation request
approves this API/transaction contract, including the five review clarifications.
Host and real Ring 3 verification are recorded in
[Step 7 evidence](../roadmap/net2-step7.md). The approved
[TCP deadline extension](TCP_IO_DEADLINE.md) is implemented. Physical DNS
acceptance is user-confirmed on Dell 5590; NET-2 is 7/7 complete.

## Library and ownership

Files: `user/dns.h`, `user/dns.c`, and a private pure codec module
`user/dns_codec.c`/`user/dns_codec.h`. Link into nslookup and nc; the shell
does not resolve names. All code uses freestanding project/compiler headers.

One synchronous resolution at a time per caller-owned context. No heap
allocation, global shared mutable resolver state, retained user pointer in the
kernel, recursion or background task. The context contains all large scratch
buffers and staged results; put it in tool BSS, not on the 16-KiB user/kernel
stack. Distinct contexts may operate independently under existing BSP-only
socket eligibility. Reentry using the same context returns DNS_BUSY without
changing the active call's buffers or diagnostics. Initialization of an active
context has undefined behavior (a caller-precondition violation), not a
cancellation mechanism; dns_context_init does not inspect uninitialized storage
for a guard word or promise to refuse busy initialization.

The call borrows context, options, name and output only until it returns. They
must be valid caller-owned memory and must not overlap. These are C library
preconditions, not kernel user-pointer validation or a new syscall ABI.

## Public declarations and exact layout

```c
#define DNS_CONTEXT_WORDS 2048u
#define DNS_NAME_CAP 256u
#define DNS_ADDRESS_MAX 8u

typedef struct {
    uint64_t private_words[DNS_CONTEXT_WORDS];
} dns_context_t;

typedef struct {
    uint32_t server_ipv4;
    uint32_t reserved;
    uint64_t deadline_ticks;
} dns_options_t;

typedef struct {
    char canonical_name[DNS_NAME_CAP];
    uint32_t addresses[DNS_ADDRESS_MAX];
    uint32_t address_count;
    uint32_t flags;
} dns_result_t;

void dns_context_init(dns_context_t *ctx);
int dns_resolve_ipv4(dns_context_t *ctx, const dns_options_t *options,
                     const char *name, size_t name_length,
                     dns_result_t *result);
int64_t dns_last_syscall_error(const dns_context_t *ctx);
const char *dns_status_name(int status);
```

dns_status_name returns a read-only static status-name string for tool diagnostics;
an out-of-range status maps to "DNS_INVALID". It allocates nothing and accesses
no resolver context. Numeric status values in the table below remain fixed.

| Type / field | Offset | Bytes | Meaning |
| --- | --- | --- | --- |
| dns_context_t.private_words | 0 | 16384 | Opaque workspace, zeroed by initialization; callers do not inspect |
| dns_options_t.server_ipv4 | 0 | 4 | Explicit unicast IPv4 server in sockaddr-compatible network byte representation |
| dns_options_t.reserved | 4 | 4 | Must be zero |
| dns_options_t.deadline_ticks | 8 | 8 | Optional absolute BSP tick deadline; zero selects the entry-time 30-second budget |
| dns_result_t.canonical_name | 0 | 256 | Normalized ASCII name, NUL-terminated; unused bytes zero |
| dns_result_t.addresses | 256 | 32 | Up to eight IPv4 values in sockaddr-compatible network byte representation |
| dns_result_t.address_count | 288 | 4 | Number of valid distinct addresses, 1–8 on success |
| dns_result_t.flags | 292 | 4 | DNS_RESULT_NUMERIC=1, DNS_RESULT_TCP=2; otherwise zero |

Sizes/alignments: context 16384/8, options 16/8, result 296/4. Add static size,
alignment and offset assertions when implementing. The opaque context is a
fixed storage envelope for this revision; measure the actual private layout and
assert it fits. Access private storage with alignment and C aliasing rules
respected; do not type-pun an unrelated struct over a uint64_t array. A larger
envelope is a library-contract revision requiring review, not a syscall change.

The public result is published once on success. Failure leaves every output
byte unchanged. Zero all unused result fields before publication. Diagnostics
are context-local: initialization and each admitted resolution clear the saved
syscall error to zero; a syscall failure updates it with the negative project
errno, and the final value remains readable until the next admitted resolution
or initialization. dns_last_syscall_error is read-only and diagnostic only; it
neither controls resolution status nor translates to Linux errno. DNS_BUSY
does not clear the active call's diagnostic. Cleanup errors do not overwrite
an already saved primary error.

## Inputs, names and numeric bypass

reserved is checked on every admitted dns_resolve_ipv4 entry, for both hostname
and numeric inputs; a nonzero value returns DNS_INVALID before resolution.
Busy-context rejection retains its precedence and leaves that context untouched.

name_length excludes the terminating NUL, which the resolver does not need to
read. Bound it to 1–254 before parsing. Accept ASCII host labels containing
letters, digits and interior hyphens, with labels 1–63 bytes; reject whitespace,
empty interior labels, leading/trailing hyphens and non-ASCII input. A trailing
dot is accepted and removed in display form. No search suffix is appended.
Normalize letters to lowercase. Expanded DNS wire names, including label length
octets and root terminator, must fit 255 bytes. Label/name limits are checked
independently; a presentation-length check alone is insufficient.

Recognize strict four-octet dotted decimal IPv4 before hostname encoding.
Keep existing nc's accepted decimal-octet behavior for numeric arguments; do
not interpret octal/hex or shortened forms. Inputs consisting only of digits
and dots that fail IPv4 parsing return DNS_INVALID, rather than generating a
surprising DNS query. Numeric success sets one address, a normalized dotted
decimal canonical_name and DNS_RESULT_NUMERIC. It makes no socket or time syscall
and ignores deadline/server validity, while still requiring reserved=0 and an
initialized, idle context. Numeric resolution is address parsing; nc retains
its existing destination validity checks when connecting.

Hostname calls require an explicit nonzero unicast server and a functioning
timebase. Port is fixed at 53. Gateway, boot dns=, environment and resolver files
are not defaults. Server=0 is permitted only for numeric bypass. No general
service-name or IPv6 parser is included.

## Finite policy and transaction contract

For standard tool invocation, options.deadline_ticks is zero. At hostname
resolution entry, the resolver calls SYS_SYSINFO once to obtain uptime_ticks
and tick_hz, checks multiplication/addition, and computes
`deadline = uptime_ticks + 30 * tick_hz`. It passes that same absolute deadline
unchanged to every CONNECT_UNTIL, SEND_UNTIL and RECV_UNTIL. It re-reads the clock
only to check remaining budget around operations, before UDP retries, CNAME
follow-ups and TCP fallback; it never starts fallback with an expired deadline.
Numeric bypass performs no clock call. For callers supplying a nonzero absolute
deadline, the entry clock read validates it against the 30-second horizon and
the resolver reuses it unchanged instead of granting a new budget. Zero-as-default
is only this library option; zero passed to a timed TCP syscall remains expired.

| Policy | Value / scope |
| --- | --- |
| Default tool budget | 30 seconds, converted to an absolute deadline once |
| Future caller deadline | At most 30 seconds at hostname call entry; longer returns DNS_INVALID |
| UDP sends | At most 3 per question, including initial send |
| CNAME links | At most 8 across all replies/questions; keep a visited-name set |
| Questions | At most 9: original plus one for each followed alias |
| Ignored UDP datagrams | At most 32 across the entire resolution; exhaustion is DNS_LIMIT |
| Resource records | At most 64 total across all sections of one complete response |
| Compression traversal | Iterative, at most 128 label/pointer steps per decoded name; cycle detection |
| Distinct returned addresses | At most 8; a ninth distinct answer returns DNS_LIMIT rather than silent truncation |
| Classic UDP response | At most 512 bytes; receive buffer is the full 1472-byte socket payload capacity |
| TCP DNS response | At most 4096 bytes, excluding the two-byte framing prefix |

Query RD=1, one IN/A question, no EDNS. Match UDP source IPv4/port, ID and
normalized question. Wire-name comparisons are ASCII case-insensitive for
question matching, owner/CNAME matching and visited-name detection; ID, type,
class, address and port comparisons remain exact. This implements the
[Step 7 checkpoint B comparison requirement](NET2_STEP7_DNS.md#b--pure-codec-and-hostile-input-tests).
Generate IDs from per-context sequence plus available
clock inputs; do not claim cryptographic randomness or spoofing protection.
Fresh sockets and existing ephemeral allocation help distinguish transactions
but are not authentication. Retry the same question; do not reset its or the
resolution's time budget. Retry on UDP receive timeout; syscall/send failures
and validated server error replies fail rather than retrying indefinitely.

Before and after every blocking operation, check the original deadline. Existing
UDP receive can exceed it by up to five seconds; this is accepted policy. No
extra receive/send/CNAME/TCP call starts after expiry. Check the bound of existing
UDP SENDTO/ARP waits separately during implementation and document dispatch
latency. A stopped process cannot have a hard wall-clock return guarantee.
Timestamp checks do not justify busy-polling: each empty receive blocks through
existing UDP machinery, and ignored packets have the finite count limit above.

Only a matching header/question with TC=1 authorizes TCP fallback; do not
require its incomplete answer tail to parse successfully. Discard partial
answers. Use CONNECT_UNTIL/SEND_UNTIL/RECV_UNTIL with the same resolution deadline.
Read exactly the length prefix and declared body with bounded short-I/O loops.
Reject malformed/over-cap framing before receiving a body. A zero/early EOF,
reset, second truncated reply over TCP or stalled/trickled response is failure.
Do not wait for connection EOF after the complete answer. Allow at most one
TCP fallback transaction per question.

On complete responses, structurally validate every section and bounded record
length before publishing. Follow only IN/CNAME records rooted at the current
question; terminal IN/A records must belong to that chain and occur in the
answer section. Additional-section addresses and unrelated answers are not
results. Duplicate identical A records are deduplicated in first-wire order.
Conflicting CNAME targets, CNAME plus A at the same owner, cycles and malformed
IN/A RDATA are DNS_MALFORMED. Unsupported records are structurally skipped.
If a chain ends without A, query its terminal alias within the global hop,
question and time limits; a response with no progress and no A is DNS_NO_DATA.
No referral chasing; authority data cannot redirect the server destination.

For output, normalize the terminal canonical name to the supported host syntax;
a valid DNS name outside this limited host-name syntax returns DNS_UNSUPPORTED.
This is a bounded IPv4 host resolver, not a decoder for every DNS record type.

## Library statuses and precedence

Use named integer constants; these are library statuses, not negative syscall
errno. The return type is int to avoid freezing compiler-dependent enum layout.

| Name / value | Meaning |
| --- | --- |
| DNS_OK=0 | Complete numeric or DNS result published |
| DNS_INVALID=1 | Bad options/name/length/deadline horizon |
| DNS_BUSY=2 | Same context already active |
| DNS_TIMEOUT=3 | Overall budget expired, or UDP attempts exhausted waiting |
| DNS_INTERRUPTED=4 | Underlying operation returned EINTR |
| DNS_NXDOMAIN=5 | Matching validated response says name does not exist |
| DNS_NO_DATA=6 | NOERROR response has no usable A and no alias progress |
| DNS_SERVER_FAILURE=7 | SERVFAIL, REFUSED, FORMERR or other recognized nonzero server RCODE |
| DNS_MALFORMED=8 | Matched reply has invalid complete encoding/framing or contradictory chain |
| DNS_LIMIT=9 | Response/work/chain/address policy bound exceeded |
| DNS_IO=10 | Socket/timebase/resource failure, reset or premature EOF |
| DNS_UNSUPPORTED=11 | Unsupported opcode/extended semantics or canonical host-name syntax |
| DNS_TCP_QUIET=12 | CONNECT_UNTIL returns reboot quiet-time EAGAIN; return immediately, with no auto-wait or automatic retry through quiet time |

Precedence: idle-context/structural argument checks, name validation, numeric
bypass, hostname server/timebase/horizon validation, then expiry. An active
context returns DNS_BUSY without modifying it. Expired hostname entry performs
no socket operation. Capture negative syscall diagnostics before cleanup; close
errors do not replace the primary resolution status. EINTR maps to
DNS_INTERRUPTED. TCP ETIMEDOUT maps to DNS_TIMEOUT whether application or
transport expiry caused it. Do not auto-wait through CONNECT reboot quiet time.

Wrong peer/ID/question packets are ignored subject to the budget; matched
malformed complete replies fail rather than being treated as arbitrary noise.
Validate envelope/question before acting on RCODE. At the post-operation time
check, expired budget wins over an otherwise successful reply; no late result
is published. An already observed syscall EINTR remains DNS_INTERRUPTED even
if time has elapsed. Cleanup always closes resolver-owned CLOEXEC descriptors
on success/failure/interruption. Caller descriptors and nc's application socket
are outside resolver ownership.

## Tool contract and approval evidence

nslookup uses the explicit server form in the Step 7 plan and prints server,
canonical name and all addresses. Failure prints a stable named diagnostic and
exits nonzero. nc uses the first returned address, preserving serial behavior;
numeric mode creates no resolver socket. No address failover is added.

Exact nslookup output is ASCII with LF line endings, no blank lines:

```
Server: 192.168.0.153
Name: fortress.test
Address: 192.168.0.168
```

Server is the explicitly supplied server normalized to dotted decimal; Name is
the published canonical_name. Print one Address line per distinct address in
result order. Success exits 0. Failure emits no success block and writes
`nslookup: DNS_STATUS_NAME\n` to stderr, using the exact named status above,
and exits 1. CLI errors write `usage: nslookup -s IPv4 name\n` to stderr and
exit 1. Numeric success uses the same block and requires syntactically valid
server CLI input, though the library numeric bypass does not use the server.
Check stdout writes; an output failure exits 1. syscall numeric diagnostics
remain available to tests through the context getter, not mixed into this format.

The user owns the explicit API approval gate. Review prototypes/argument types,
layout/ownership, library-status mapping from project errno, the 30-second budget
and five-second UDP overshoot, numeric bypass, publication, single-caller context
safety, exact output, supported name syntax and the 32-packet noise policy.
No DNS codec/transaction code lands before approval. Before enabling tools,
pass pure-codec ASan/UBSan tests with independent golden vectors, actual resolver
syscall-adapter tests for all status/lifetime paths, then Ring 3 QEMU/capture gates
in [NET2_STEP7_DNS.md](NET2_STEP7_DNS.md). Assert the workspace fits and measure
actual stack usage. Test data-before-EOF, expired-result rejection, reused
absolute deadlines under trickling input, descriptor cleanup and numeric bypass.

Implementation order after API approval: Step 7 B codec, C UDP transaction,
reviewed TCP deadline ABI/proof update and implementation verified per deadline
gates 3–5, then C TCP fallback, D full tools/QEMU and E physical acceptance.
The codec/UDP path can land independently and UDP-only nslookup remains a partial
milestone. Draft approval is not test evidence; NET-2 is complete, 7/7.
