# Pending reschedule at safe user return — 2026-10-09

The Dell pipe wait investigation validates 24 logs and finds critical-reader
ready-to-selection means of 2.09 ms (A) and 3.49 ms (B), with one delayed
wakeup responsible for roughly 97% of runnable wait. The original pipe
regression did not reproduce. Source audit finds reschedule IPIs previously
only incremented a counter: a busy target waits for blocking/yield/timer
quantum expiry. This experiment tests prompt, bounded request service, not
a proven explanation of every measured delay.

The user explicitly authorized the proposed safe post-EOI scheduling change.
Ordinary hardware handlers still never context switch. No interrupt frame,
GS assembly offsets, stack alignment, syscall transition windows, normal
quantum, queue order, pipe semantics or address-space ownership changes.

## Implementation and boundary audit

- Reschedule IPI release-stores a coalesced per-CPU pending bit and increments
  diagnostic requests. No scheduling, locks, allocation or output in handler.
- After dispatcher EOI and IRQ-depth unwind, the existing user-return branch
  of `interrupts.asm` calls `sched_resched_user_return` on the intact saved
  frame before signal handling and GS restoration. The extra C call uses
  the same aligned stack; register/frame order is untouched. Exceptions/NMI
  and kernel/nested returns skip this branch.
- Normal syscall dispatch calls the same hook after committing result to
  the frame, before user signal handling. The syscall stub/entry/exit assembly
  is unchanged. Sigreturn's special early disposition bypasses this service;
  the pending request remains for another eligible return.
- Eligibility requires a real interrupt/syscall vector 32..255, Ring 3 saved
  CS, saved IF set, current IF clear, IRQ depth zero, no held locks, local
  preemption enabled and a running non-idle user TCB. Unsafe returns leave
  pending untouched. Isolated IF=0 test fixtures remain excluded.
- Eligible service atomically clears one pending request and invokes existing
  `thread_yield`; it preserves scheduler lock release, IRQ exclusion through
  TSS/CR3/stack exchange and existing deferred reaping/lifetime accounting.
  Atomic consume occurs before switching; a later delivered IPI can publish
  another request. No loop or repeated draining at a single boundary.
- Normal selection of a queued peer also clears the local pending bit while
  IF is clear, avoiding a redundant service after an already-satisfying
  timer/block/yield switch. Idle retains its existing yield/HLT loop.
- Requests are published by the target's IPI handler, not by direct remote
  stores. Same-CPU wakes retain existing behavior. Kernel work is never
  preempted by this new hook; it is serviced on an eligible user return.
- Appended per-CPU request/service fields preserve all existing GS offsets.
  Services count eligible yield attempts, not necessarily successful switches.

This intentionally extends the user-return boundary to scheduling; the
ordinary handler-only and single-owner EOI contracts remain intact. A busy
kernel continuation can still delay service. More frequent user switches
can cost throughput, so pipe waits and plain spawn throughput must both be
measured before calling the candidate a general improvement.

## Outgoing context publication race

Initial SMP=8 candidate runs failed under BIOS and UEFI while running
spawn_wait after pipes. Retained evidence is in
`build/resched-return-{bios,uefi}-20261009`. A CPU stopped on #DB; peers
subsequently reached the fail-closed TLB ACK timeout. In UEFI the fault RIP
was the fresh user trampoline after sched_post_switch, with saved flags
0xfd2 (including TF), instead of the initialized 0x202. The initial A control
completed the same workload. This is evidence of corrupted/resumed context;
it does not independently prove every causal step.

Source audit identifies a concrete race: thread_yield queues the outgoing
task and releases its scheduler lock before switch_context saves its RSP.
A remote thief could select it using its previous saved context while the
original CPU still executes on that stack. Blocking/stopping have the same
exposure if a remote wake makes the outgoing task runnable before stack save.

The correction marks those outgoing contexts busy under their owning
scheduler lock before publishing READY/BLOCKED/STOPPED. Thieves acquire-load
and exclude busy contexts. The incoming task release-clears the outgoing
flag only after stack save and post-switch CR3/zombie bookkeeping complete.
CPU-private post-switch handoff now preserves incoming RFLAGS while disabling
IRQs throughout that bookkeeping, so nested timer scheduling cannot overwrite
its transient state. No lock crosses switch_context. Both matched variants
include this correction; their only difference is pending-request service.
No claim is made that this race caused prior Dell timing variance.

## Verification status

After the user's approved WSL restart, final eligibility/wait host suites and
strict kernel build pass. After the outgoing-context correction:

- BIOS/UEFI lifecycle at 1/4/8 CPUs: 6/6 PASS, including 100 spawn/exit cycles,
  allocation/table accounting and deferred reclamation.
- BIOS/UEFI SMP=8: two runs each, three timed repetitions each of profiled
  pipes/spawn_wait/signals, all complete without a panic. Evidence:
  `build/resched-handoff-{bios,uefi}-20261009`. These precede the final
  IRQ-excluded post-switch bookkeeping edit.
- Final frozen A BIOS SMP=8 phase mode and B UEFI SMP=8 wait-only mode complete
  pipes/spawn_wait/signals without a panic, with valid benchmark barriers.
  Evidence: `build/resched-final-{A,B}-20261009`.
  Read-only stopped-state inspection finds requests on all eight CPUs;
  A services are zero, B services are positive, services never exceed requests,
  and recorded fault vectors are zero. Audit:
  `build/resched-counter-validation.json`.
- Final per-CPU GS/GDT/TSS/IST, AP #DF and real hardware NMI delivery:
  BIOS/UEFI x 1/4/8 CPUs 6/6 PASS. The runner initially timed out because it
  recognized only the obsolete fortress> prompt; its acceptance now requires
  the Ring 3 shell banner plus either recognized prompt. The guest had booted.
- Final exact NMI syscall/sigreturn boundary suite: BIOS/UEFI PASS, 28 exact syscall-boundary NMIs and 24 exact sigreturn-boundary NMIs per firmware, plus kernel GS/IRET recovery. Alias roundtrips are not counted as exact-boundary injections.
- Both final raw images pass disposable UEFI USB boot, RW persistent mount,
  signals/pipes, capture larger than 4096 bytes independently read with
  debugfs, clean shutdown and offline ext2 audit; frozen originals unchanged.
  A phase mode, B wait-only mode. Evidence: `build/resched-usb-{A,B}-20261009`.

QEMU establishes these correctness observations, not Dell performance benefit.
The initial failed bundle `build/resched-ab-20261009` is retained for evidence
and must not be used for Dell acceptance.

## Prepared comparison workflow

The corrected bundle is `build/resched-handoff-ab-20261009`.
`make prepare-resched-comparison` freezes common objects/initramfs/benchmark
and isolated thread objects. A defines FORTRESS_RESCHED_RETURN_DISABLED:
service is a no-op. B enables service. Both retain outgoing-context theft
exclusion, pending publication, normal selection cleanup, identical assembly
and syscall hooks, wait diagnostics, fast ELF copy and batch kernel stacks.
Only thread.o's service implementation differs. Embedded raw/ISO kernel and
initramfs checks and source/object hashes are retained in the manifest.
This is a fresh controlled counterfactual, not the prior diagnostic image.

Dell order A1 -> B1 -> B2 -> A2. Per boot run commands.txt: pipes in modes
plain/wait-only/phase/phase/wait-only/plain (eight workers, seven timed reps),
then plain spawn_wait (eight workers, five timed reps). Persistent Storage RW,
same AC power/firmware/background/thermal conditions. Export each shutdown
to build/resched-dell-results/{A1,B1,B2,A2} before overwriting captures.
## Dell ABBA result — 2026-10-09

User supplied all A1/B1/B2/A2 exports. Twenty-eight complete workload captures
validate: eight workers, seven timed repetitions for each pipe capture and
five for each spawn capture, all-worker checksums/barriers/profile lifecycle,
summary min/median/max, eight CPUs observed, and identical expected benchmark
SHA-256. There are 168 timed pipe cohorts and 20 spawn cohorts, with warmups
excluded. Variant identity relies on the user's flash/run labels; sysinfo's
shared build ID and benchmark hash alone do not attest the kernel variant.

Some B phase summaries use rounded human-readable milliseconds because their
command omitted -c; exact worker microseconds remain available. The analyzer
now verifies their nearest-0.1-ms rounding against reconstructed extrema and
median before deriving exact summary fields. Raw files remain unchanged.
Lock snapshot filenames differ slightly across boots and are identified by
before/after content naming rather than renamed.

| Measure | A control | B candidate | Interpretation |
| --- | ---: | ---: | --- |
| Plain pipes pooled median, 28 cohorts each | 13.1485 ms | 14.8735 ms | B elapsed +13.1% |
| Wait-only pipes pooled median | 13.198 ms | 14.923 ms | B elapsed +13.1% |
| Phase pipes pooled median | 13.408 ms | 14.9805 ms | B elapsed +11.7% |
| Plain spawn_wait pooled median, 10 cohorts each | 14.5745 ms | 14.5855 ms | +0.08%, essentially unchanged |
| Wait-only critical reader mean ready-to-selection | 4.6949 ms | 0.01494 ms | -99.68% |
| Wait-only critical reader mean blocked time | 2.2108 ms | 3.3006 ms | More time before runnable |

Plain per-file medians A1: 13.258/13.146 ms; B1: 14.828/14.893 ms;
B2: 14.923/14.839 ms; A2: 13.132/13.338 ms. Both B boots are consistently
slower; the returning A boot returns to the lower range. This is a meaningful
negative throughput result within this experiment, not merely a noisy median.

Phase attribution uses critical-worker elapsed cycles, not pure CPU time:
reader spawn call 0.583 -> 3.489 ms, header 4.465 -> 0.890 ms, and read loop
7.045 -> 9.257 ms. The kernel spawn instrumentation itself is 0.570 ->
0.540 ms. The larger user-observed spawn call therefore suggests delayed
return/resumption rather than slower profiled allocation/ELF work. Writer
queued-to-run mean falls 1.962 -> 0.468 ms, but writer write elapsed grows
9.898 -> 11.221 ms. These are attribution clues, not a count or CPU-cost
measurement of additional switches.

The implementation achieves prompt runnable-reader service but does not
improve this pipe workload. The outgoing-context guard remains a correctness
fix and is shared by both variants; this A/B does not measure its cost.
Recommend retaining that guard and testing a narrower request policy next:
distinguish blocked-reader wakeups from fresh process/work-publication hints,
so a fresh-work hint does not force an immediate yield at every eligible user
return. Source currently uses the same smp_send_resched path for publication,
waiter wakes and signal nudges. Exact switch/reason counts would make this
next experiment easier to interpret. No narrower policy is implemented yet.

Reproducible artifacts: build/resched-dell-analysis.json,
build/resched-dell-summary.json, build/resched-dell-attribution.json,
build/summarize_resched_dell.py and build/attribute_resched_dell.py.