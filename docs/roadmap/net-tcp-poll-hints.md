# TCP application work hints — 2026-10-04

## Evidence and scope

Dell 5590 testing found both 1 MiB wget-to-EXT4 and wget-to-stdout/wc take
approximately seven seconds. Profiling image reports PIT-derived TSC rate
1971896600 cycles/s. Using the latest explicit pre-download snapshot (read
28/47104/13483292, write 0/0/0, flush 4/0/752511, other 4/72/1444758) and
post-download rows (read 623/2008064/337960989, write 520/2126848/176879871,
flush 202/0/243324116, other unchanged), USB command time is approximately
0.38 s. The estimate includes PIT programming overhead and is not a precise
clock calibration, but the measured network-only baseline also demonstrates
that storage is not the explanation for the current seven-second baseline.

The code audit found TCP application operations queue actions but do not wake
the network polling worker. Its sleep predicate accepts only the next BSP
clock tick, so even an early scheduler wake cannot satisfy it. Receive consumption
queues an ACK/window update; delaying service can add avoidable pacing. This
is a concrete scheduling gap, not yet proof of all physical throughput cost.

## Change

- Add one static atomic pending-work flag and an unlocked thread-context
  net_request_poll hint API.
- A request publishes the flag then wakes the existing worker channel.
  Repeated hints coalesce in the flag; they do not queue protocol operations.
- The worker consumes the flag at the beginning of its bounded batch.
  Its wait predicate checks both the flag and existing tick deadline, so
  work posted before sleeping is retained and no device/endpoint lock is
  acquired under the scheduler lock.
- Successful connect, accepted application send, nonempty receive consumption
  and successful shutdown request service after releasing endpoint locks.
- Existing 64-RX batch bound, yield on full batches, BSP ownership, tick
  fallback, cold-link polling, transport buffers, deadlines/quiet period,
  NIC completion polling and DMA quarantine remain unchanged. No NIC IRQs,
  scheduler implementation changes or filesystem barrier changes.

## Verification

Strict kernel build and make image-ext4 PASS. Host ASan/UBSan:
make test-net-eth-host verifies idle/cold/online timer paths plus work hints
that make the predicate ready with no tick advance, duplicate hints and
consumption before the next wait. make test-net-tcp-socket-host verifies a
real application receive requests poll service, all existing stream/listener/
deadline/lifecycle cases and the prepare/commit deadline guard. Host lock
adapters assert hints execute with no lock held. make test-net-socket-host
UDP regression PASS.

Delivered USB smoke BIOS/UEFI PASS (build/ext4-usb/run-y7e4m31m): profile
report, real SYS_SYNC/post-sync mutation, clean shutdown and Linux bytes/fsck.
Focused BIOS/UEFI e1000e wire/client tests PASS (2/2): independent SLIRP TCP
and Linux host application, real Ring 3, 64 KiB both ways, ABI, reset,
caught signal, unread close and pcap audit. Both idle observations report
zero worker CPU ticks over 500 elapsed ticks at 100 Hz. Evidence is retained
in build/net2-step3-bios-e1000e-user-smp1-serial.log and the corresponding
UEFI serial log, stderr logs and pcaps. This is a focused pair, not a full
client matrix run. make test-wget PASS: host sanitizers and all eight cases
under each of BIOS and UEFI, including download bytes, stdout pipeline,
redirects and error handling, followed by clean poweroff.
The client runner initially waited for the obsolete fortress>
prompt; updated it to recognize the cwd/status prompt and restarted tests.
Those preliminary prompt timeouts are not transport failure evidence.
None constitutes a Dell throughput pass.

Delivered optional non-journaled profiling image: 2026-10-04 06:30:55,
SHA-256 713d17b309fa9505a136ef953e0c0655778075658a0d27b6c2dbb771386cc55d,
data PARTUUID F4690BA2-4D70-460F-A6EB-F47DABB68DF2. Save needed evidence
before reflashing the designated disposable USB; default fortress.img remains
ext2. Retest network-only 1 MiB first, then file output and hashes. Keep the
120-second TCP reboot quiet time, same HTTP host and idle competing traffic.
Physical improvement remains pending, with no promised under-2-second target.

## Dell retest and bounded receive bursts

The user retested the 06:30:55 image: both file output and the network-only
`wget -q -O - ... | wc -c` still take approximately seven seconds for 1 MiB.
The application wake hint did not improve observed throughput. The current
bottleneck must be investigated in the network path, not filesystem writes.

The worker previously slept until the next BSP tick after any batch smaller
than 64 frames, even if its ACK could trigger an immediate peer response.
It now yields after any nonempty batch and allows eight empty scheduling
turns before returning to the normal tick wait. Incoming frames replenish
this bounded grace; carrier-down clears it. Each turn still processes at
most 64 frames and yields outside locks, allowing awakened applications to
consume their window. An entirely idle worker takes the original sleep path.
No NIC interrupt, scheduler implementation, TCP buffer or timer budget changes.
The I219 TX PIT completion path is unchanged in this candidate.

Strict kernel build and actual-worker ASan/UBSan PASS. The host fixture injects
a second peer frame only after the first yield, observes both frames without
tick progress, exact recycling, ten yields (two receive batches plus eight
empty turns), then three normal timer waits. Existing cold/idle/work-hint
cases still pass. Focused BIOS/UEFI e1000e wire reruns PASS (2/2): real
Ring 3 64 KiB both ways, independent Linux peer/pcap audit, reset, caught
signal, unread close and ABI checks. Both idle measurements remain zero
worker ticks over 500 elapsed ticks. make test-wget rerun PASS: host
sanitizers and all eight BIOS plus eight UEFI cases, clean shutdown.

User supplied capture row 430 at 5.704674600 seconds: FortressOS ACK
49152 -> 8000, Seq=121 Ack=1036807 Win=892 Len=0. With the current
unscaled 8192-byte receive window, 892 free bytes corresponds to 7300
buffered bytes (five 1460-byte segments). This is evidence of window
pressure, not proof of a particular stall duration or packet loss from one
row alone. The bounded scheduling candidate gives readers a turn before
sleeping; its physical throughput effect still requires measurement.

Candidate image: 2026-10-04 06:48:42, SHA-256
3ce2516209c130733bb4d92701479a9bff67f5ed180e33cd2c08cf682f17bf39,
data PARTUUID 5D10960A-58CF-48C8-BCCB-BE454206B9FD. Dell throughput remains
unverified; the eight-turn grace is a bounded candidate, not a measured
microsecond polling interval or a promised throughput result.

## Full Dell capture: tcpproblem.pcapng

User reports the 06:48:42 candidate still takes seven seconds. Capture from
192.168.0.168:49152 to 192.168.0.153:8000 contains 438 frames, one TCP
connection, 1048782 server payload bytes (206 HTTP header bytes plus 1 MiB
body), and 5.804332 seconds from SYN to final ACK. Windows host offload
aggregates 143 server payload records of 7300 bytes and one of 4676 bytes;
these are capture records, not oversized wire segments. Those aggregates
have zero IPv4 total length; captured frame length supplies their payload
length. No TCP window scaling is negotiated. Server payload sequence ranges
are contiguous with no overlaps or gaps in this capture.

145 server payload records have median data-to-client-ACK delay 20.134 ms,
summing to 2.937716 seconds. For 142 ACKs advertising 892 bytes free,
the next client ACK with the same acknowledgment and window 8192 follows
after median 19.865 ms, summing to 2.797811 seconds. Server response after
those open-window ACKs has median 26.107 microseconds (total 3.613 ms).
Thus two repeated client-side waits account for about 5.7 seconds.

Example frames 9–12, relative to first SYN:
server 7300-byte capture aggregate at 0.080356 s; client ACK Win=892 at
0.100309 s; client window update Win=8192 at 0.120188 s; next server
aggregate at 0.120202 s. Similar cycles dominate the entire stream.

This identifies client receive/ACK/window-service pacing; the two 20 ms
delays match DEFAULT_QUANTUM_TICKS=2 at 100 Hz. Scheduler wake only enqueues
a ready thread. The capture supports a scheduling-handoff explanation but
does not expose which runnable thread occupies each interval. Increasing
receive capacity alone would amortize, rather than remove, the delay.
The next targeted change should ensure queued window updates and readers
receive prompt scheduling turns and use a measured bounded active RX wait,
rather than assuming eight empty turns cover the peer response latency.
No scheduler implementation or protected contracts were changed for this
capture analysis. Parsing output is in build/tcp-capture-rows.json; original
user evidence remains C:/Users/fabio/Desktop/tcpproblem.pcapng.

## Approved four-step implementation candidate

[Approved plan and clock determination](../plans/TCP_PERFORMANCE_FIX_PLAN.md).
The existing receive-batch yield was retained. The missing positive-receive
syscall handoff now yields after copying/consuming bytes and publishing work;
no shared scratch or user pointer is accessed after that boundary. The earlier
iteration grace is replaced by a measured 200 us invariant-TSC budget, with
unavailable/failed-clock timer fallback. TSC is feature-gated and measured
during the existing APIC early-boot PIT calibration, with no runtime PIT use.
Global quantum, scheduler implementation, boot initialization ordering and
USB/I219 settings are unchanged. Host sanitizer and strict build gates PASS.

Current candidate image: 2026-10-04 07:14:57, SHA-256
3b29aa58dd7943bf99f508573c70a326dd1cbfe518b4716ba12d18bd44419f6f,
data PARTUUID F57EAA04-68BB-4A5F-ADD1-A18FFD7D298D.
Live TCP/wget verification PASS against the initial 07:13:18 candidate:
focused BIOS/UEFI e1000e client 2/2, independent peer/pcap, 64 KiB both ways,
ABI/reset/signal/unread-close; zero idle worker ticks over 500 elapsed ticks.
Wget host sanitizer and all eight BIOS plus eight UEFI cases PASS.
the subsequent change added permanent clock-fault fallback and extra host
assertions. TCG observes no admitted invariant TSC, so its live protocol tests
exercise fallback and the new syscall handoff; active TSC polling remains
host fake-clock coverage plus pending Dell verification. Final-image USB boot/
sync smoke PASS BIOS/UEFI, actual SYS_SYNC/post-sync mutation and Linux
bytes/fsck audits; retained build/ext4-usb/run-cyl50m3j. Do not label automated tests as physical
performance acceptance or claim all Step 4 gates complete.

## Dell retest: boot-calibrated TSC candidate still unchanged

The user supplied a boot photo with NET CLOCK invariant TSC boot-calibrated
Hz=0x7117D8F4 (1897388276 Hz), followed by another seven-second stdout test.
Updated tcpproblem.pcapng, captured 2026-10-04 07:26:21, uses client port
49154. It again has 438 TCP frames and 5.804193 seconds SYN-to-final-ACK.
Median data-to-ACK is 20.037 ms (145 records, summed 2.902140 seconds);
892-to-8192 window reopening is 19.961 ms (142 pairs, summed 2.835620 seconds).
Server response after reopening is 22.888 microseconds median (total 3.422 ms).
The clock admission and syscall yield did not remove the recurring waits.
Boot clock admission does not alone prove every later burst used the clock.

Further source audit finds the shell supervisor in kmain remains an ordinary
runnable thread while process_is_alive(pid) is true. Its loop reaps dead tasks
then executes sti; hlt. HLT pauses the CPU but does not mark the thread BLOCKED;
unlike the scheduler's designated idle thread, this supervisor keeps normal
two-tick quantum accounting. Cooperative network/reader yields can therefore
select it and wait for its quantum. This is a concrete additional runnable
waiter, consistent with the capture; identifying each individual interval still
requires a scheduler trace or targeted regression.

Proposed scope extension, pending user agreement: replace the supervisor's
HLT wait with a kernel shell-exit wait using existing sched_wait_until and
child-exit notification. Snapshot the event sequence before checking liveness
outside the scheduler lock; use a lock-free sequence predicate under the lock
to avoid lost wakes and rank nesting. Recheck liveness on wake, reap outside
locks, and retain existing exit-status collection and shell restart behavior.
The current process_wait_child API is user-only and must not be blindly used
by kmain. No timer quantum, switch assembly or initialization ordering change.
Regression must prove the supervisor is BLOCKED during networking, preserves
exit/restart, and cannot lose an exit occurring between check and sleep.
The approved plan explicitly requires discussion before expanding beyond its
network-local solution; no kernel supervisor change has been made yet.

The user subsequently approved the extension. The supervisor now blocks using
a persistent terminal-state sequence and the existing wait/wake path.
[Implementation and verification](kernel-supervisor-wait.md): supervisor
BLOCKED/zero idle tick growth/three restarts 4/4 BIOS/UEFI SMP=1/4 PASS;
focused TCP BIOS/UEFI e1000e 2/2 PASS; wget host and BIOS/UEFI PASS; final-image
EXT4 smoke PASS. Dell speed remains pending for the 07:39:14 image.
