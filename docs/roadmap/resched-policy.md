# Reschedule reason policy experiment — 2026-10-09

The preceding Dell ABBA experiment reduced critical reader runnable wait by
99.68% but increased plain pipe median elapsed by 13.1%; spawn_wait was
unchanged. The user authorized distinguishing waiter wakes from fresh-process
publication hints. See resched-user-return.md for raw evidence and the shared
outgoing-context correctness guard, which this experiment retains.

## Implementation

- smp_send_resched retains vector 0xfd for waiter wakeups and signal nudges.
- smp_send_work_hint uses free vector 0xfb for kernel thread and user process
  publication, including existing unbound-work broadcasts. Registration/call
  audit finds no collision with timer, keyboard, UART, TLB, panic or spurious.
- Both handlers only OR a reason into the CPU-local pending mask and count
  received events. Dispatcher retains single-owner EOI. Distinct vectors avoid
  a remote shared reason mailbox and preserve reason identity across LAPIC
  coalescing. No lock, allocation, printing or scheduling in either handler.
- OR publication preserves an urgent request when a fresh-work hint follows it.
  Safe return eligibility is unchanged: Ring 3 saved IF, current IF clear,
  vector 32..255, IRQ depth zero, no locks, preemption enabled, running user TCB.
  Unsafe returns retain reasons. An eligible B return consumes coalesced bits,
  yielding for urgent reasons only. Work-only hints are counted and consumed;
  they do not force a busy user's return to yield. Timer/block/yield and idle
  scheduling still observe the published queues, and work IPIs wake idle CPUs.
- Normal selection clears pending as before. Same-CPU publication/wake semantics,
  quantum, runqueue order, stealing guard, CR3/stack ownership, syscall and
  interrupt return assembly, pipe semantics and benchmark ABI are unchanged.
- Appended CPU-local counters distinguish urgent/work requests, urgent/work
  service attempts and deferred work-only boundaries. Existing assembly offsets
  are unchanged. Mixed urgent/work service is attributed to urgent once. These
  count delivered/coalesced requests and yield attempts, not individual signals
  sent, successful switches, or CPU time. Read-only stopped-state GDB inspection
  is used; no new physical counter syscall is introduced.

## Matched variants

build/resched-policy-ab-20261009 has frozen common objects, source copies,
benchmark/initramfs, embedded kernel/rootfs audits and hashes.
A defines FORTRESS_RESCHED_ALL_WORK: immediate service for both reasons.
B uses the candidate urgent-only policy. Only thread.o differs. Both include
new reason vectors and counters and the outgoing-context guard; A is a fresh
matched control rather than the earlier Dell image. No improvement is claimed
before physical measurement.

A SHA256: 986a6dd2b0568aa059a299e6d17097329632e2d42f0ea0884a7f3908cec7f4c5
B SHA256: f268c17a5fb30e54ce0557cadb8d5888555209a097306dc322731518274f6986

## Verification

Strict make and ASan/UBSan return-gate/reason-policy tests PASS, including
work-only/urgent/mixed/unknown-bit decisions and wake-then-work OR coalescing.
BIOS/UEFI lifecycle at 1/4/8 CPUs 6/6 PASS. BIOS/UEFI per-CPU GS/GDT/TSS/IST,
real AP #DF and hardware NMI at 1/4/8 CPUs 6/6 PASS.
B BIOS SMP=8 wait-only and A BIOS SMP=8 phase mode each complete pipes,
spawn_wait and signals, warmup plus three timed reps, without panic, with
barrier/checksum/profile validation. All eight B CPUs receive both reasons,
service urgent requests, service zero work-only requests and defer hints.
All eight A CPUs service work-only and urgent requests, with zero deferred
hints. B UEFI SMP=8 phase mode also completes all three workloads without
panic, with the same per-CPU reason assertions. Counter audit:
build/resched-policy-counter-validation.json (and its retained Python script).
Exact BIOS/UEFI NMI return tests PASS: 28 syscall-boundary and 24 sigreturn
boundary injections per firmware, plus kernel GS/IRET recovery; alias
roundtrips are excluded from the exact-boundary counts.
Both frozen raw images pass disposable UEFI USB boot/RW mount, signals/pipes,
greater-than-4096-byte /mnt capture independently read with debugfs, clean
shutdown and offline ext2 audit; originals unchanged. A uses phase mode and B
wait-only mode. Evidence: build/resched-policy-usb-{A,B}-20261009.
The corrected comparison is ready for physical testing; no Dell performance
benefit has been measured yet.
Evidence: build/resched-policy-bios-20261009,
build/resched-policy-control-20261009 and standard lifecycle/percpu logs.

## Dell handoff

Use the bundle commands.txt with Persistent Storage RW and consistent AC,
firmware/background/thermal conditions. Order A1 -> B1 -> B2 -> A2. Per boot:
pipes plain/wait-only/phase/phase/wait-only/plain, n=8/r=7, followed by plain
spawn_wait n=8/r=5. All captures use /mnt/policy-*.txt and sync/shutdown.
Export before each next run/reflash to build/resched-policy-dell-results/
A1, B1, B2 or A2. B2 reboots the same B stick; A2 reflashes A.
Plain elapsed measures effect; wait-only attributes reader wake delay. Phase
mode is additional evidence and may perturb scheduling. Raw output and
benchmark hashes must be retained; kernel identity relies on flash/run labels.
## Dell ABBA result — 2026-10-09

All 28 workload captures validate: four labeled boots, eight workers, seven
pipe timed repetitions and five spawn_wait repetitions per file, expected
benchmark hash, eight CPUs observed, complete checksums/barriers/profile
lifecycles, and reconstructed summary min/median/max. Warmups are excluded:
168 timed pipe cohorts plus 20 timed spawn cohorts. Kernel variant identity
still relies on the user's flash/run labels. Raw exports remain unchanged;
lock snapshot filename differences are accommodated by before/after naming.

| Measure | A all-request service | B urgent-only service |
| --- | ---: | ---: |
| Plain pipe pooled median (28 cohorts each) | 13.0935 ms | 13.607 ms |
| Plain pipe pooled mean | 13.5638 ms | 13.5356 ms |
| Plain pipe observed maximum | 17.601 ms | 14.168 ms |
| Wait-only pipe pooled median | 13.0355 ms | 13.540 ms |
| Phase pipe pooled median | 13.210 ms | 13.4945 ms |
| Plain spawn_wait pooled median (10 cohorts each) | 14.3585 ms | 14.736 ms |
| Wait-only critical reader blocks, mean | 1.679 | 40.821 |
| Wait-only critical reader blocks, median | 1.5 | 42.5 |

B plain pipe median is 3.92% higher and spawn_wait median 2.63% higher.
Plain pipe means are essentially unchanged (-0.21%), because A has three
of 28 cohorts above 15 ms while B has none. The observed maximum falls
19.5%; these are sample tail observations, not a guaranteed worst-case bound.
Per-file plain medians A1 13.137/13.117 ms; B1 13.608/13.760 ms;
B2 13.574/13.502 ms; A2 13.070/13.061 ms. Both B boots have higher typical
elapsed than both A boots. B narrows the observed distribution rather than
winning on median throughput. Absolute results must not be compared directly
against the prior experiment as a controlled effect: new common reason vectors,
counters and fresh boot/environment conditions change that comparison.

Wait-only B critical reader blocked total averages 5.827 ms versus A 1.846 ms.
Its aggregate ready time averages 303 us versus A 215 us, but that total spans
many more blocks. Median aggregate ready is 328 us B versus 6.56 us A; A's
mean is dominated by a single roughly 5.5 ms ready interval. Largest ready
interval observed is 105 us in B versus 5.52 ms in A. Thus the data do not show
an individual multi-ms runnable wait as the main B cost.

Phase-mode attribution: reader spawn call falls 3.493 -> 0.675 ms, while read
loop grows 7.069 -> 9.925 ms. B's profiled kernel spawn is 0.661 ms, A 0.568 ms;
that modest kernel-work difference cannot alone explain A's much larger
user-observed spawn call. Writer write elapsed grows 9.501 -> 10.227 ms.
The benchmark writer emits 1024-byte chunks. B's 41-reader-block pattern is
consistent with a reader waking and draining small arrivals, then blocking
again: producer/consumer handoffs become a plausible dominant cost. Existing
wait counts include all worker waits and are not exact context-switch counts;
this remains an inference rather than a proven scheduling timeline.

Decision: urgent-only service is not a demonstrated median-throughput win.
Keep the shared context-publication correctness guard. Next useful experiment
is direct pipe handoff attribution: bytes/read and bytes/write, empty/full
transitions, reader/writer block counts, switch reasons and CPU placement.
That evidence can distinguish wake-driven ping-pong from pipe copying,
validation, or migration costs before choosing a bounded wake/handoff policy.
Simply withholding readable-data notifications or waiting for a full buffer
could break short writes/interactive latency; no such behavior is implemented.

Reproducible evidence: build/resched-policy-dell-analysis.json,
build/resched-policy-dell-summary.json, build/resched-policy-dell-attribution.json,
build/summarize_resched_policy_dell.py, build/attribute_resched_policy_dell.py
and build/check_resched_policy_tails.py. Raw file hashes are in the summary.