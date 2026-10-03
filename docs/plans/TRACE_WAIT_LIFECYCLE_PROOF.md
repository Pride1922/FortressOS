# Trace probe finite-wait lifecycle

2026-10-03. Implements the user-approved [64-byte ABI](NETCTL_TRACE_ABI.md) and its three clarifications. This argument depends on the existing BSP worker continuing to run; it does not claim independent scheduling progress when that worker or clock is stalled.

## 1. Admission and copied ownership

`net_trace_submit` in `src/net/net_ping.c` validates copied scalar fields, remaining horizon and usable clock/interface before acquiring the existing rank-1 ping mailbox lock. Ping and trace share `s_used`, one active generation and `g_net_ping_channel`. A competing request returns EAGAIN without replacing the owner. The user tool does not retry. No user pointer, TCB pointer, fd or allocated buffer enters the mailbox.

The syscall's automatic `token` belongs to the waiting caller's stack; only its value is copied into protocol state. This uses the existing scheduler wait lifecycle, without retaining that stack address in the mailbox or NIC state.

## 2. No nested network/scheduler locks

Mailbox state copies occur under `s_lock`; the worker releases it before starting/taking/cancelling the IPv4 transaction and before `sched_wake_all`. The trace predicate is the existing `net_ping_ready`: acquire-load atomic active/done generations only. It takes no lock and performs no NIC, ARP or protocol work under the scheduler lock. Host adapters assert that NIC/ARP/wake calls run with no mailbox lock held.

`SYS_NETCTL` fast entry keeps its existing IRQ state and calls `sched_wait_until(&g_net_ping_channel, net_ping_ready, &token)`. No signal, scheduler, rank, wait-signature or timer-hook change is involved.

## 3. Sole protocol owner and deadlines

The BSP network worker alone builds/sends probes, handles matching RX and completes/clears IPv4 trace state. `net_worker_main` retains its existing RX cap and before/after IPv4 mailbox sweeps; trace does not add another worker or interrupt path. Each input restores its caller's IRQ state as before. TCP prepare/commit source order is untouched.

On acceptance by the worker, the effective deadline is `min(now + timeout_seconds * hz, request.deadline_ticks)`, using checked/saturating arithmetic. It includes routing/ARP; successful send records the RTT origin but does not restart the timeout. Both input and tick paths check expiry before accepting a response or sending further trace traffic. Reaching the whole-command deadline cancels pending TX/wait state and publishes PROBE_TIMEOUT. The CLI rechecks the unchanged command deadline before its next call, so no new probe starts after exhaustion.

No quoted ICMP error is accepted before actual probe submission. Quotes correlate local/destination IPv4, protocol/type/code, IP ID 1 and a never-reused same-boot trace identifier/sequence pair. Full Echo Replies additionally correlate the token-bearing payload. Cross-reboot stale traffic and hostile forgery are outside this correlation guarantee.

## 4. Publication and collection

IPv4 completion clears pending/active/waiting trace state before the manager takes the result. Under the mailbox lock, publication checks the same active generation, trace kind and absence of cancellation; it stores the result/error and release-publishes `s_done`. Wake runs after unlock. The waiter rechecks atomic generations and `net_trace_collect` revalidates ownership under the mailbox lock.

Successful collection clears admission and active generation. A wrong-kind/stale collect cannot consume another caller's result. Link loss publishes EIO internally; collection releases admission but does not publish a user result. The syscall revalidates the writable range before its sole full-struct copy-out. A copy-out failure releases ownership already collected; it leaves no reservation.

## 5. Interruption, STOP and hard exit

On a caught pending signal after wait, the syscall publishes cancellation for its generation and returns EINTR without user copy-out. The next existing worker sweep invalidates ownership, cancels the matching IPv4 state and wakes waiters. Default signal exit requires no new owner-exit hook: the finite protocol deadline produces a result, then the result lease expires. STOP uses the same finite reclamation; resumption after expiry yields EINTR from stale collection.

Result grace is two seconds from publication. A not-yet-serviced request also has an absolute lease bounded by the command deadline plus two seconds. Stale cancellations cannot clear a new generation, and protocol cancellation checks its token again. The copied mode/token before unlock prevents a later admission from changing which old protocol operation is cancelled.

## 6. Fixed bounds and fail-closed parsing

One shared ping/trace TX slot, one 32-byte payload, one 64-byte copied request/result and a monotonic 32-bit wire counter bound state. Counter exhaustion returns EAGAIN instead of wrapping. No hot-path allocation occurs. Quote processing requires only bounded outer/quoted headers and eight echo bytes; it never treats quoted total length as available storage. Malformed or unsupported quotes leave the caller-visible result unchanged.

## 7. Executable evidence and limits

`test-net-trace-host` uses the actual mailbox/IPv4/ICMP and CLI with fake BSP ticks, NIC/ARP/syscall and lock adapters. It checks 100/1000 Hz, wrong/stale/duplicate identities, unrelated ping quotes, bounded hostile parsing, shared admission, per-probe/whole-command/ARP deadlines, cancellation, EIO, abandoned/stopped-owner/result expiry and unchanged ping TTL/IP ID. The CLI adapter asserts distinct sequence labels and one syscall on EAGAIN.

`scripts/test_net_trace.py` supplies a synthetic two-router topology under BIOS/UEFI × e1000/e1000e. Real Ring 3 checks cover ranges/reserved fields and a 100 ms whole-command deadline inside a five-second probe, no-retry contention with real ping, default Ctrl-C exit and finite recovery. Default Ctrl-C is not evidence of a caught-handler trace-specific run; caught/STOP lifetime behavior uses host adapters plus the existing unchanged wait mechanism. Capture auditing imports no peer protocol helpers, checks exact TTL progression and unique identities, and rejects missing/malformed injection inputs. See the [implementation evidence](../roadmap/net-traceroute.md) for executed results and regression boundaries. Physical multi-hop acceptance remains pending.
