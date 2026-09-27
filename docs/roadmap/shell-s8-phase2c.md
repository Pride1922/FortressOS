# Shell S8 Phase 2C — stop/continue and durable child notification

Implementation handoff, 2026-09-27. Runtime acceptance is pending: the user
requested implementation only and will run the tests. No new QEMU, sanitizer,
SMP, resource-audit or physical-hardware pass is claimed.

## Implementation

`THREAD_STOPPED` retains the TCB, stack, address space, scheduler references and
descriptors. It shares the owner CPU's inactive list with `THREAD_BLOCKED`, but
both channel wakeups and the ordinary signal wake scan explicitly require
`THREAD_BLOCKED`. Stopped tasks never reside on a runqueue.

Default STOP/TSTP/TTIN/TTOU consumption commits the stopped metadata and child
report under `g_process_lock`, with local IRQs disabled at the call site. The
task then parks its kernel continuation using the existing switch/CR3/TSS
handoff discipline, releasing the scheduler lock before `switch_context`.
Caught stop signals use the existing handler path; ignored stop bits remain
pending and ineligible until a disposition change or CONT cancellation. STOP
and KILL remain uncatchable, unignorable and unblockable.

CONT clears pending stop bits even when blocked/ignored. A stop publication
clears pending CONT and any unclaimed continue request. CONT's resume effect is
separate from its blockable/ignorable handler notification. Owner CPUs service
the atomic request/KILL state using the existing bounded timer scan (100 Hz),
not remote TCB destruction or an NMI handler. They snapshot stopped PIDs under
the scheduler lock, release it, claim each resume and publish its report under
the process lock, then unlink and enqueue READY under the scheduler lock.
Local IRQs remain disabled through this handoff; these rank-1 locks never nest.
The scheduler alone owns queue membership, and a second scan cannot enqueue an
already-removed stopped task.

KILL takes priority over CONT/default stops. A KILL resume does not publish
CONTINUED. Its saved continuation observes pending KILL before user return;
blocking syscalls unwind bookkeeping (including timed-reader accounting) and
the existing signal boundary invokes normal exit/descriptor cleanup/reaping.
No user instruction runs first. CONT resumes a stopped blocking operation at
its predicate loop; input retains its original deadline, and reads/writes do
not replay completed transfers. These checks are outside subsystem locks.

Child records now have a slot `identity_generation`, `event_seq`, `reported_seq`
and explicit latest event kind/payload. All accesses are process-lock protected.
Stop, committed CONT resume, and terminal exit/death advance the event counter;
publication alone, repeated stops, and CONT while running do not. Wait option
filtering does not consume excluded reports. Consumption uses equality and
advances `reported_seq`; stop/continue retain the reservation. Terminal state
replaces transient state and remains collectable through either wait API once.
Wrap triggers an invariant trap, including in non-debug builds. PIDs remain
monotonic and unrecycled; slot identity is separate from event delivery.

Durable state and the parent wake sequence precede SIGCHLD publication. Default
CHLD drops only the notification; caught CHLD is coalesced and delivered through
the existing handler/restorer. Explicit SIG_IGN for CHLD and all unsupported
action flags (including NOCLDWAIT) are rejected. Parent exit detaches signal
state and discards its reservations under the same lock. No orphan-group,
setsid, PTY or shell job-table policy is added.

## Return-path compatibility

The return disposition is implemented as a per-entry vector marker in the
kernel-stack syscall frame, rather than a separate scratch slot. The user
confirmed that the four sigreturn NMI boundaries verify this path under BIOS
and UEFI, with 24 distinct-boundary sigreturn NMIs per firmware across syscall
and timer origins. This is user-reported Phase 2B evidence, not a new Phase 2C
run. No syscall-entry, sigreturn, restorer or interrupt assembly was changed.
S8_PLAN.md's disposition subsection now describes this accepted implementation.

## Test handoff

- `make test-s8-stops-host`: actual process-table code with pthread lock adapter;
  exact sequence/consumption rules, latest-state coalescing, option filtering,
  duplicate reports, blocked/ignored CONT, stop cancellation, simultaneous
  pending KILL/CONT with one resume claim, CHLD, parent-exit races, slot reuse,
  terminal supersession and raw legacy exit codes. This is metadata coverage,
  not a scheduler/runqueue or IRQ simulation.
- `make test-s8-stops SMP=1` (also `SMP=4` and `SMP=8`): disposable test ISO,
  no data disks, paired OVMF firmware, BIOS/UEFI. Ring 3 fixture runs 81 children
  across self-stop, syscall-free loop, blocked pipe reader/writer and terminal
  reader cases; verifies stop/continue reports, ignored/blocked CONT, caught
  CHLD with a scalar-only handler, repeated stops, channel wakes leaving stopped
  readers untouched, KILL without CONT, no post-kill user marker, EOF cleanup
  and repeated descriptor/child reuse. User pipe peers remain BSP-pinned; SMP
  boot configurations alone are not cross-core stopped-task acceptance.
- Regressions to run: `make test-s8-process-host test-s8-process`,
  `make test-s8-signals-host test-s8-signals`, `make test-pipe-host test-pipe`,
  `make test-nmi`, and `make test-shell-s6-resources`. Existing Phase 2A tests
  now reject reserved SIGQUIT instead of newly supported STOP/CONT. The pipe
  host adapter has a signal-check shim; it does not claim STOPPED coverage.

Built the changed kernel objects and both signal/stop Ring 3 ELFs with strict
warnings. Host fixture C syntax, runner Python syntax and whitespace checks
were performed; no test executable or QEMU runner was executed. Independent
stack/page/descriptor baseline audits and cross-core scheduling stress remain
runtime acceptance work, not conclusions from compilation or repeated spawns.
