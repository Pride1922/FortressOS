# FortressOS TCP performance fix — four steps

2026-10-04. Plan approved with a Step 3 clock prerequisite. Steps 1–3 have an
implementation candidate; scoped automated verification passes and Dell
performance acceptance remains pending. See the execution record below.

## Problem and evidence

Dell Latitude 5590, I219-LM, same LAN server and 1 MiB payload: FortressOS
takes approximately seven seconds for both file output and stdout piped to
wc; Linux Mint takes 0.182 seconds. Storage does not explain the network-only
delay. Application poll hints and the eight-empty-turn receive grace both
failed to improve the observed Dell time.

The supplied tcpproblem.pcapng contains a 5.804-second TCP exchange. Repeated
server bursts are followed by approximately 20 ms until FortressOS ACKs with
a nearly full receive buffer, then another 20 ms until it advertises the
reopened window. Server response to window reopening has median latency
26 microseconds. These client delays account for approximately 5.7 seconds.
No gaps or overlapping server payload sequence ranges occur in this capture.
Windows capture offload aggregates 7300-byte records; these are not individual
wire segments. See [capture analysis and failed candidates](../roadmap/net-tcp-poll-hints.md).

The intervals match the two-tick scheduler quantum at 100 Hz. This strongly
supports scheduling pacing, but the capture cannot name the runnable thread
occupying each interval. Confirm that execution path before claiming a root
cause or making a wider scheduler change.

## Constraints

Keep the global quantum and scheduler implementation unchanged. Preserve sole
BSP protocol ownership, polling-only NIC operation, lock ranks, IRQ-excluded
sleep/wake transitions, socket identity/lifetime, absolute I/O deadlines,
120-second reboot quiet time, bounded queues and DMA quarantine. No filesystem,
USB durability, I219 errata or link-recovery changes. No per-packet console
logging. Increasing TCP buffers is outside this fix.

Use existing scheduling APIs. Wakeup enqueues a thread; yield gives a runnable
peer an opportunity, not a guaranteed transfer to a particular thread. Any
handoff must occur with no locks held and no prepared TCP action in flight.
Revalidate identity, signal/deadline state and user pointers where required
after a scheduling boundary. If these constraints cannot solve the measured
delay, record the evidence and discuss the necessary scheduler contract change
before implementing it.

## Step 1 — Hand received data to the reader promptly

Trace RX dispatch, endpoint readiness publication, reader wakeup, worker yield
and reader resume. Use bounded test instrumentation to identify remaining
quantum waits, including competing runnable tasks; retain evidence outside the
console. Audit the existing burst yield rather than adding a duplicate yield.

Arrange a cooperative handoff after a bounded receive batch and readiness
publication, at a safe boundary after TCP actions have committed and all locks
are released. Allow wget to consume available bytes promptly while maintaining
the 64-frame maximum batch and fair turns for other tasks.

Gate: actual-worker host coverage plus a real QEMU blocked-reader case show
reader progress without relying on expiry of a full quantum. Verify competing
tasks, STOP/CONT, cancellation and exact RX recycling. Record whether the
original yield already meets this gate; implement only the missing behavior.

## Step 2 — Service receive-window updates promptly

Trace the successful receive copy/consume continuation. After consumption
reopens space, publish the existing poll hint and provide a safe cooperative
handoff to the worker. Keep ACK transmission in the sole BSP worker; do not
perform NIC I/O under a socket lock or inside a syscall copy transaction.

Avoid yielding for zero-byte reads, errors or operations that produced no
useful work. Preserve byte counts, EOF/reset behavior, handle generations,
shared descriptors and timed/untimed receive semantics. Account for the
possibility that another runnable task is selected before the worker.

Gate: actual TCP manager/syscall tests prove a consumed window is advertised
without a quantum-dependent wait, including partial reads, concurrent handles,
close, signals and deadlines. Packet audit distinguishes an ordinary data ACK
from a later window update and measures the interval between them.

## Step 3 — Replace iteration grace with a bounded active RX wait

After useful TCP traffic or an ACK/window update that can trigger a response,
keep the worker available for a short measured interval. Start with an
experimental 200-microsecond active budget; validate it against the observed
peer response and CPU cost before freezing it. Eight empty iterations are
not a time budget and must be replaced, not layered underneath the new wait.

Use a validated monotonic high-resolution clock available in unlocked worker
context. Audit its API and accuracy before choosing it; the USB PIT-derived
TSC estimate is diagnostic evidence, not automatically an authoritative clock.
If a safe clock is unavailable, fail back to the existing timer sleep and
record the limitation. Do not borrow PIT channel 2 across device users.

Poll bounded batches with IRQs enabled outside transactions, retain cooperative
handoffs and a finite iteration backstop for a failed/stalled clock. Renew the
budget only on actual progress; empty polls cannot renew it. Carrier-down,
failure and inactivity return to normal sleeping. Run timer/deadline service
during sustained traffic and preserve fairness for other connections/tasks.

Gate: fake-clock host cases prove expiration, stalled/backward clock handling,
no empty-poll renewal and cold-link fallback. Real QEMU tests verify sustained
TCP progress, reader fairness, deadline service, shell responsiveness and idle
sleeping after a transfer. Tune the budget from measurements, not target speed.

## Step 4 — Verify, measure and accept on Dell

Run the strict build and relevant host gates: test-net-eth-host,
test-net-tcp-socket-host and test-net-socket-host. Run BIOS/UEFI TCP client and
wget checks with independent byte/packet verification; label focused cases
accurately. Include idle CPU observation, competing runnable work, UDP/ICMP,
signals, timeout and link-loss recovery as appropriate to the changed paths.

Build one identified image, recording hash and timestamp. On Dell, use the same
server/payload after the existing 120-second quiet period. Time the network-only
1 MiB command and retain a fresh capture first. Then check a 16 MiB transfer
and byte count/hash, and one file-output smoke check. Existing filesystem
acceptance does not need to be repeated for a network-only change.

Acceptance: repeated 20 ms ACK/window stalls cease to dominate the capture;
network-only 1 MiB is below two seconds as the initial performance gate, with
sub-second throughput the follow-up goal. Report actual elapsed and capture
times rather than claiming Mint parity. Hashes and existing correctness gates
must pass; no idle busy loop, console flood or loss of interactive responsiveness.
If the speed gate fails, quantify the remaining intervals before another change.

Record each step's diff, exact tests/results, retained evidence, limitations and
physical measurements in the investigation. Remove temporary hot-path probes
or keep them explicitly opt-in. Commit/push only when requested.

## Execution record — approved clock prerequisite

Clock audit: no HPET initialization or public high-resolution HPET clock exists
in the current tree. APIC ticks are 100 Hz and unsuitable for 200 us. TSC can
be read without claiming hardware timer ownership. Its availability is now
gated by CPUID TSC support and CPUID.80000007H:EDX[8] invariant TSC support.
The BSP measures its frequency during the existing early-boot APIC calibration
against the 11932-count PIT interval (1193182 Hz reference); no additional PIT
programming or ownership interval is introduced. Calibration requires a
successful reference interval, increasing samples, overflow-safe arithmetic
and a frequency within 100 MHz–10 GHz. Port-I/O/polling overhead remains in the
estimate; this is a short active-wait budget, not a change to syscall deadlines.

Runtime clock APIs are BSP-only and read TSC, with no PIT/USB/APIC timer writes.
There is no cross-core TSC synchronization claim. A zero frequency selects
timer sleep. QEMU TCG boot observations select this fallback. Physical Dell
feature/frequency admission must be confirmed from the one-time NET CLOCK line.
The earlier USB diagnostic TSC estimate is not used as the clock source.

Step 1 audit: the worker already yields after any nonempty RX batch after
protocol actions commit and readiness wakeups publish. Retained that handoff;
no duplicate yield or scheduler implementation change.

Step 2: successful stream receive yields after user-copy and consume commit.
Consume has already published the worker hint. No scratch buffer, user pointer
or endpoint reference is accessed after that yield; only the committed scalar
byte count returns. Zero length, would-block, pointer errors and EOF do not
yield. Partial positive results stay positive if a signal/deadline arrives
after bytes have been committed. The thread resumes the existing IF-clear
syscall continuation, and user-return signal handling remains in place.

Step 3: replace the eight-turn grace with a 200 us cycle budget after RX or
published application work; cooperative yields and full protocol/deadline
sweeps remain between batches. Empty polls do not extend the budget. A
backwards clock or 4096-turn backstop disables the clock for that worker's
lifetime, reverting to timer sleep. Cold links clear active polling. No busy
loop when idle, NIC interrupts, dynamic buffers or altered DMA timeout budgets.

Host ASan/UBSan: actual worker tests cover a peer response after the first
scheduling turn, exact recycling, measured expiration and return to timer
sleep; direct budget tests cover exact boundary, backwards, stalled and
unavailable clocks. Existing cold/idle/hint cases PASS. Actual TCP manager/
syscall tests assert unlocked handoff on positive consumption and absence on
zero-length/EAGAIN/EFAULT/EOF; existing timed/shared/listener/signal tests PASS.
Strict kernel build and opt-in EXT4 image construction PASS. Step 4 live
verification results and final image identity are recorded in the investigation.
No physical throughput improvement has been claimed.

## Approved scope extension — shell supervisor

Subsequent Dell capture after supervisor and TX fixes shows 1.45 seconds with
approximately 10 ms data-to-ACK cadence. User approved extending Step 3 RX grace
from 200 us to 1 ms, retaining cooperative yields and clock failure fallback.
See [implementation and retest](../roadmap/net-rx-poll-window.md).

The user approved replacing the runnable HLT supervisor after the clock/yield
candidate again took seven seconds. Network-local changes alone did not satisfy
the performance gate. kmain now blocks using the existing scheduler wait API
and a terminal-state exit sequence, preserving the global quantum and boot
initialization order. This extends the earlier scheduler-implementation scope
constraint by agreement; no runqueue or context-switch implementation change.
See [implementation, wait ordering and regression evidence](../roadmap/kernel-supervisor-wait.md).
