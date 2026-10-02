# TCP I/O deadlines — Step 7 prerequisite

Status: APPROVED AND IMPLEMENTED (2026-10-02), including the user's two requested
additions. Review owner: the user. Implementation and executable evidence are
recorded in [Step 7 evidence](../roadmap/net2-step7.md); physical DNS acceptance is user-confirmed separately. The user's implementation request also approved DNS_USER_API.md.

## Approved decision

Add three opt-in, per-call TCP operations with absolute BSP-tick deadlines:
CONNECT_UNTIL, SEND_UNTIL and RECV_UNTIL. An absolute deadline is reused across
DNS query writes, the two-byte response prefix and all response-body reads.
Successful short I/O does not renew the deadline. CONNECT is included because
its existing 30-second timeout could otherwise exceed the resolver's remaining
budget before response I/O even begins.

Use existing sched_wait_until and static TCP endpoint channels. Add one
non-owning earliest-deadline wake hint per endpoint; worker expiry publishes
an event and wakes that channel outside the manager lock. No per-thread timer
registry, new scheduler primitive, syscall-entry change, signal change,
lock-rank change, timer IRQ hook, transport timer change, DMA or driver change.

This deliberately extends the public syscall ABI with new numbers. Existing
SEND/RECV/CONNECT and stream READ/WRITE keep their accepted behavior. No
socket-wide option is introduced: dup/inherited descriptors must not silently
change another continuation's timeout. SYS_NETCTL and UDP remain unchanged.

## Existing source anchors

| Mechanism | Implemented source |
| --- | --- |
| TCP validation, I/O and wait loop | `src/net/net_tcp_syscall.c:net_tcp_syscall` |
| Generation/event snapshot, atomic predicate, static channel | `src/net/net_tcp.c:net_tcp_snapshot`, `net_tcp_ready`, `net_tcp_channel` |
| Worker release publication followed by unlocked wake | `src/net/net_tcp.c:publish`, endpoint sweep in `net_tcp_tick` |
| Prepared action ordering | IF-clear prepare/submit/commit block in `net_tcp_tick`; pool maintenance follows all commits |
| Sleep insertion and unlocked context switch | `src/kernel/thread.c:sched_wait_until` |
| Channel-wide wake | `src/kernel/thread.c:sched_wake_all` |
| Userspace timebase | SYS_SYSINFO in `src/kernel/syscall.c`: `uptime_ticks`, `tick_hz` |
| Current errors, buffered ordering and CONNECT cancellation | [TCP_SOCKET_ABI.md](TCP_SOCKET_ABI.md) |

The existing atomic predicate detects identity/event changes, not elapsed time.
The existing worker wakes only when it publishes a change. Merely adding a
deadline check to the syscall loop would therefore leave a quiet receiver asleep.
Both the expiry publication and the timed predicate extension are required.

## Concrete ABI

Numbers 49–51 are currently unused in `src/include/syscall_abi.h`; recheck at
implementation time before reserving them. These are project numbers, not Linux
syscall numbers. Return through the existing dispatcher and frame->rax.

| Proposed operation | RAX | RDI | RSI | RDX | R10 | R8 | R9 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| SYS_SEND_UNTIL | 49 | fd | data pointer | length | flags=0 | deadline_ticks | unused |
| SYS_RECV_UNTIL | 50 | fd | writable data pointer | capacity | flags=0 | deadline_ticks | unused |
| SYS_CONNECT_UNTIL | 51 | fd | sockaddr pointer | sockaddr size=16 | deadline_ticks | unused | unused |

Each deadline is a by-value uint64_t absolute BSP tick count. There is no new
user struct, embedded pointer, output field or ABI layout to pack. Buffers use
the existing lengths and validation rules; sockaddr remains the existing
16-byte layout. Unused registers are ignored, consistent with the original
calls. Flags are zero only; MSG_DONTWAIT is rejected with EINVAL because these
operations already define bounded blocking semantics.

No zero/infinity sentinel: deadline=0 is already expired. Any deadline <= current
ticks is expired, not invalid. A future deadline is limited to 60 seconds from
entry, using checked `60 * tick_hz` arithmetic; a greater interval returns
EINVAL. This policy cap covers DNS's 30-second budget while keeping these
new calls explicitly finite. The 60-second horizon is a project policy constant;
it may be raised in a future revision without changing the syscall shape or the ABI.
Valid zero-length SEND/RECV returns 0 after ordinary
fd/scalar/range validation without connection/deadline testing, as today.

Use SYS_SYSINFO to obtain the clock. Convert a duration to ticks with checked
arithmetic, rounding upward; check addition before forming the absolute deadline.
There is no hard-coded 100 Hz or conversion through quantized milliseconds.
Timer frequency is assumed fixed after initialization; a future runtime
frequency-change or post-initialization BSP-clock reset feature must revisit
this clock contract. A future boot at
1000 Hz works through tick_hz. Unavailable frequency returns EIO. Clock wrap is
not modular: fail closed rather than reinterpret a wrapped deadline as future.

## Timeout and precedence

The deadline limits waiting and new I/O acceptance. Expiry is checked on syscall
entry and after every potentially blocking interruption check, before starting
a nonzero transfer. Once a BSP IF-clear copy/consume or queue acceptance begins,
it completes and returns its positive count even if a later tick/signal would
expire/interrupt the call. A completed transfer is never rolled back.

Order for nonzero timed SEND/RECV:

1. BSP eligibility, fd/stream kind, length/flags, initial range validation,
   deadline horizon/timebase validation, initial endpoint snapshot.
2. On every iteration: existing signal/STOP handling; revalidate saved generation,
   user range and device availability after it, since STOP can switch context.
3. If current BSP ticks >= deadline, return ETIMEDOUT without accepting TX or
   consuming RX. Otherwise attempt the existing operation; positive count,
   EOF or transport error returns immediately. Only EAGAIN enters timed wait.

Thus caught interruption precedes expiry, invalid memory precedes expiry, and
expiry precedes a newly observed EOF/reset/data when the call has already run
out of time. This is an explicit opt-in distinction from untimed buffered-data
precedence. Bytes remain queued for a later call with a live deadline or for
ordinary READ/RECV. Within an unexpired timed call, data still precedes EOF/reset.
Concurrent readiness does not authorize a transfer after the expiry check has
failed. No strict wall-clock completion guarantee is made for copying, scheduling
latency, driver polling or a stopped process.

| Result | Mapping / effect |
| --- | --- |
| Positive short SEND/RECV | Actual accepted/copied bytes; no retry of accepted bytes |
| RECV EOF before expiry | 0 after preceding buffered data drains |
| Application deadline | ETIMEDOUT=-30; SEND/RECV leaves connection usable |
| Caught signal before transfer | EINTR=-22; existing signal behavior |
| Invalid user range | EFAULT=-2; no RX consumption or TX acceptance |
| Invalid flags/length/horizon/address | EINVAL=-1 |
| Invalid fd / non-stream | EBADF=-3, as existing TCP dispatch |
| AP/ineligible caller | EOPNOTSUPP=-14 |
| Missing device/timebase | EIO=-9 |
| Saved endpoint invalidation | ENOTCONN=-28, as current client loop |
| Reset / shutdown / refusal | Existing ECONNRESET=-27, EPIPE=-20 with SIGPIPE, ECONNREFUSED=-26 before expiry |

Transport retry exhaustion can also return ETIMEDOUT; the syscall does not add
a distinct errno for distinguishing it from application expiry.

CONNECT_UNTIL preserves address/route/online/quiet-time/state validation. During
reboot quiet time it returns EAGAIN and starts no handshake. After ordinary
validation, an expired deadline returns ETIMEDOUT without starting an attempt.
A live deadline bounds waiting in addition to the existing transport handshake
deadline; whichever expires first ends the call. On application expiry after
starting, invoke generation-checked cancellation, including if the handshake
raced to completion. Close/recreate that endpoint before retry. Cancellation's
existing best-effort RST versus raced-established graceful FIN policy is retained;
this proposal does not add an unconditional RST. CONNECT interruption retains
the same cancel/detach behavior and EINTR precedence.

## Wait lifecycle and wake-hint argument

Storage: add `uint64_t wake_deadline_ticks` and `bool wake_deadline_set`
to each of the 16 static endpoint records. These are manager-owned hint fields,
not an operation reservation. Clear both on endpoint creation/reset/adoption and
identity invalidation; never inherit a hint across common-slot reuse. Account
for actual alignment/padding in the implementation's measured layout.

Extend the continuation-local wait snapshot with a timed marker and deadline.
Legacy snapshot callers must initialize timed=false explicitly; ACCEPT and
untimed operations must never inspect an uninitialized extension. The predicate
continues to acquire-load generation/event; timed waits additionally read the
existing atomic BSP tick counter and compare against their own deadline. No
manager/process lock, allocation or user-pointer access inside the predicate.

Before sleeping on EAGAIN, with incoming BSP IF clear:

1. Capture generation/event before testing operation readiness, as today.
2. Register the deadline hint under the manager lock only if identity still
   matches; retain the minimum of the stored hint and this caller's deadline.
   Release the lock before any interruption check or scheduler call.
3. Call `sched_wait_until(channel, timed_ready, &local_wait)` using the existing
   signature. Its predicate check and BLOCKED insertion remain IRQ-excluded.
4. After return, retry full validation. If still EAGAIN and unexpired, resnapshot
   and register again before sleeping. Keep the original absolute deadline.

The worker checks the hint against freshly read BSP ticks in each endpoint
sweep. On expiry, clear the hint, then use existing publish(e) under the manager
lock; release that lock and restore incoming IRQ state before sched_wake_all.
Do not use a stale pass-start timestamp for expiry. Do not invoke TCP input/tick
or expiry publication inside a prepared action's prepare/commit interval.
Application wake expiry neither calls changed() nor alters a transport timer.

| Event / concern | Required invariant |
| --- | --- |
| Deadline before sleep insertion | Timed predicate sees expiry and refuses sleep |
| Deadline after insertion | Worker publishes event, then wakes the static channel |
| Two timed callers, different deadlines | Earliest hint expires once and wakes all; still-waiting later callers rearm their own deadline |
| Worker clears hint before a later waiter resumes | Published event differs from its saved snapshot, so sched_wait_until returns to the syscall loop instead of silently sleeping again without a hint |
| Early caller finishes/interrupted/exits | Stale hint owns nothing; at most one unnecessary expiry publication/wake, then it clears |
| STOP past expiry | No bytes/reservation owned; after CONT revalidation observes the original deadline and returns ETIMEDOUT unless a higher-precedence condition applies |
| KILL | Existing unwind/descriptor teardown; no promised syscall return and no timer registration resource to reclaim |
| Shared descriptor close | Existing file reference prevents premature final close; closing another reference does not cancel this caller |
| Final close / reuse | Invalidate generation and clear hint; stale registration cannot touch replacement endpoint |
| Untimed waiter shares channel | May receive one incidental wake; no timer-only event churn when no hint is set |
| Event counter exhaustion | Existing publication invalidates identity; no wrap into a matching stale event |

This is a bounded hint rather than a waiter list: no user buffer, syscall frame,
file pointer or TCB pointer enters manager storage. No thread-exit cleanup hook
is needed. Do not clear a hint on individual successful calls: it may also be
the earliest hint for another shared-fd caller. Leaving a stale minimum trades
one incidental wake for a much smaller ownership proof.

Idle worker sleep remains BSP-tick driven through the existing network channel.
Expiry becomes eligible on the first worker endpoint sweep observing its tick;
this is not a hard real-time dispatch bound. Continuous RX still has the existing
64-packet cap per worker pass, not per timer tick. Expiry adds at most 16 hint
checks and 16 endpoint publication/wake requests per pass. sched_wake_all's cost
also includes traversal of the existing bounded blocked-thread population;
do not describe it as constant time. No hint means no new idle TCP wake churn.

## DNS budget integration

Use one resolution deadline across TCP connect/write/prefix/body calls. A
positive short result does not create a new deadline. When TCP fallback starts,
check remaining budget first; never grant a fresh 30 seconds. If the response
finishes, close without waiting for EOF. On deadline/error, close and publish
no partial resolver result.

UDP retains its existing five-second receive bound. The reviewed policy allows
a nominal 30-second resolution budget plus at most five seconds of UDP receive
overshoot. Check time before/after each receive and before every retry/CNAME
query/fallback; packets and retries never reset the resolution deadline. This
allowance covers UDP receive only, not an extra CONNECT or SEND timeout. A
complete DNS budget proof must separately account for existing bounded UDP
SENDTO/ARP waits and syscall dispatch latency; this TCP proposal alone does not
prove a strict 35-second wall-clock bound for the whole resolver.

## Implementation and review gates

1. User reviews syscall numbers/registers, tick units, 60-second horizon,
   expiry precedence and CONNECT cancellation. Decide whether the three explicit
   calls are preferable to a single operation-selector syscall; this draft
   recommends explicit calls, sharing one internal implementation.
2. Extend [TCP_WAIT_LIFECYCLE_PROOF.md](TCP_WAIT_LIFECYCLE_PROOF.md) and
   [TCP_SOCKET_ABI.md](TCP_SOCKET_ABI.md) with the approved proposal before coding.
   The existing apic_timer_get_bsp_ticks uses an acquire atomic load; verify its
   use inside the actual scheduler predicate remains lockless and allocation-free,
   and measure storage/stack changes. If a proof obligation requires a protected
   change, stop and report; approval of this draft cannot authorize an unspecified
   scheduler/signal/locking change.
3. Host tests: entry-expired and zero length; invalid pointer/flag/horizon
   precedence; no consumption after expiry; short counts before expiry;
   deadline versus data/FIN/reset/signal; stalled and trickled reads; blocked
   send; CONNECT expiry/SYN-ACK race; earliest-hint sharing; stale hint after
   early success/interruption; event-before-sleep; STOP/CONT and generation reuse;
   event exhaustion; untimed API regression. Verify 100 Hz and 1000 Hz fake clocks.
   Add a runtime assertion in the worker sweep that expiry publication cannot
   run between prepare and commit, mirroring the existing worker-ordering invariant.
   Exercise the assertion with a host fault-injection test; a comment alone
   does not satisfy this gate.
4. Ring 3 BIOS/UEFI tests prove actual quiet-peer timeout and idle sleep,
   blocked-send timeout, caught signal, STOP beyond deadline then CONT,
   KILL/cleanup, shared/inherited descriptor use, prompt recovery and ping.
   A host runner killing a hung guest is a failure, never a timeout success.
5. Run Step 3 client, Step 4 server, Step 5 matrix and related UDP/ICMP/worker
   regressions. Retain exact commands, failure artifacts and capture audits.
   NMI/entry tests become necessary only if entry/exit code changes; such a change
   is outside this proposal and requires discussion first.

## Implemented correspondence

The dispatcher and shared TCP syscall implementation provide numbers 49–51.
The wait predicate acquire-loads generation/event and the existing atomic BSP
clock; it takes no manager lock, allocates nothing and dereferences no user data.
net_tcp_tick checks hints before preparing TX, commits each prepared action
before further maintenance, and traps if deadline_expire runs while an action
is in flight. The host fault-injection subprocess must terminate with SIGILL.

A continuation also retains its entry clock floor. A backwards BSP clock fails
timed calls with EIO; when a hint is outstanding, the worker latches that failure
and publishes hinted endpoints so sleeping callers revalidate. The existing
boot-time uptime reset with no timed hint establishes a new observation epoch
without failing future calls. Reinitialization clears a latched failure.
No transport clock or untimed call is changed. This
implements the existing missing/invalid-timebase error, without another syscall.

Static hints own no references. Accepted/consumed byte publication invalidates
the manager's cached readiness observation, so equal-sized consecutive RX/TX
batches cannot hide a readiness transition after a userspace drain. See the
deterministic regression and physical-evidence boundary in the Step 7 record.
The user-confirmed Dell 5590 DNS session closes NET-2 at 7/7.
