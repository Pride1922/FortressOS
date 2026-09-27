# Shell S7 Phase 6: verification handoff

**NOT STARTED (2026-09-27).** Phase 5B's runner path is complete; S7 as a whole
is not. This checkpoint is verification-only, with no new implementation code.
Prior evidence: [Phase 5A](shell-s7-phase5a.md) and
[Phase 5B](shell-s7-phase5b.md). Acceptance references:
[S7 plan, Phase 6](../plans/S7_PLAN.md#phase-6-multi-core-smp--formal-acceptance)
and [G6/G7 matrix](../plans/S7_PLAN.md#5-acceptance-audit-checklist--verification-matrix).

## Verification boundary

Pipe peers remain pinned to the BSP. A VM with four or eight CPUs still executes
all pipeline stages on CPU 0. Phase 6 must establish that pipeline correctness
holds when other CPUs exist; it cannot establish that pipelines use multiple
cores. Moving wake channels off the BSP and executing cross-core pipe peers
belong to a future milestone.

The existing plan's G6 specifies a producer on CPU 1 and consumer on CPU 2.
Neither that arrangement nor CPU 1 producer / CPU 0 consumer is achievable
within this BSP-pinned, verification-only phase. The literal cross-core G6
criterion remains deferred and must not be marked passed by an SMP boot.
This records the user's current scope clarification; it does not rewrite the
plan or claim its original G6 has been satisfied.

## Pending acceptance gates

| Gate | Required evidence | Status |
| --- | --- | --- |
| QEMU with other CPUs present | S7 suite under `-smp 4` and `-smp 8`, each with BIOS and UEFI; record exact invocation, actual QEMU arguments, firmware and logs. The existing one-CPU `make test-shell-s7` pass is not this evidence. | NOT STARTED |
| Streaming and throughput | Byte-exact payloads exceeding 256 KiB through 2-, 3- and 8-stage pipelines using the new utilities, across the four firmware/CPU cells; record payload size, comparison method and measured throughput. | NOT STARTED |
| Lock discipline under SMP | Lock hierarchy assertions and contention metrics from the same SMP runs, while pipe peers remain on the BSP; record observations without inferring cross-core wakeup safety. | NOT STARTED |
| Cross-core portion of original G6 | Producer/consumer on different CPUs requires future wake-channel support and removal of the BSP restriction. | DEFERRED beyond Phase 6 |
| G7, Dell Latitude 5590 | Physical pipeline execution, stream utilities, clean serial/framebuffer output and clean shutdown; identify build, device and observed results. QEMU passes do not satisfy this gate. | NOT STARTED |

Keep QEMU storage disposable and distinguish host checks, QEMU runs and physical
observations in the eventual evidence. No Phase 6 test or hardware result is
claimed at this handoff. `AGENTS.md` status changes remain a separate user task.
