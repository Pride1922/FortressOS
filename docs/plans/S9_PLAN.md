# Shell S9 — System introspection

Status: planning only, 2026-09-29. No implementation, test execution or acceptance
is claimed. This document authorizes no kernel changes. Implement each phase
only after its prerequisites and concrete kernel scope have been accepted.

## Context and reading-pass findings

S9 exposes S8 process identity and lifecycle through three standalone Ring 3
programs: `/bin/ps`, `/bin/sysinfo` and `/bin/top`. Preserve signals, job control,
terminal ownership, scheduling policy and BSP pipe/input affinity. Follow the
phase gates and evidence separation in `docs/plans/S8_PLAN.md:491` and `:697`.
The binding implementation rules are `AGENTS.md:204` (invariants), `:367`
(syscall recipe), `:412` (test recipe), `:423` (user-program recipe) and `:498`
(protected contracts); re-read their headings if these baseline line numbers move.
`PROTECTED.md:5` requires discussion of protected changes unless explicitly
authorized. This plan is that discussion surface, not a report of completed work.

Verified baseline:

| Surface | Finding and source |
| --- | --- |
| Process metadata | `process_record_t` has `uint64_t pid, parent, pgid, sid`; `bool used, published, exited, stopped`; `signal_state_t *signals`; and `process_group_t *group`. No name or ticks. `src/kernel/process_table.c:15`. |
| Child status | Separate `child_record_t` retains PID/parent/PGID, code, generation, event sequences, used/done, signal and event kind. It lacks name, SID and CPU ticks. `src/kernel/process_table.c:22`. |
| Capacity and ownership | 64 process slots; private rank-1 process lock; metadata and scheduler locks must not nest; no TCB pointer may escape as an identity snapshot. `src/kernel/process_table.h:5`, `:16`; `src/include/spinlock.h:34`. |
| Name | `tcb_t.name[32]` is initialized in `process_spawn_internal()` before publication. `src/kernel/thread.h:50`; `src/kernel/thread.c:1210`, `:1347`, `:1354`. |
| Ticks | `tcb_t.total_ticks` exists. Timer increment is outside the scheduler lock. `src/kernel/thread.h:60`; `src/kernel/thread.c:960`, `:974`. |
| Lookup | No general public PID-to-TCB snapshot API in `src/kernel/thread.h:101`. Internal liveness/wait scans exist; `process_wait_extended()` consumes exit records and may yield, so cannot implement introspection. `src/kernel/thread.c:1732`, `:1759`. |
| Exit retention | Exit marks metadata exited and publishes child status. Reaper calls `process_record_forget()` independently of wait collection. Wait collection clears the child slot. `src/kernel/process_table.c:230`, `:255`, `:330`; `src/kernel/thread.c:270`. |
| User copy | `SYS_WAITPID` validates writable output, uses kernel-local storage, then copies after the metadata operation. `src/kernel/syscall.c:1269`. |
| Numbers | Existing numbers end at `SYS_GROUP_RELEASE=35`; 36 is unused in `src/include/syscall_abi.h:6` through `:43`. Recheck on implementation. |
| Build | C stream tools use a parameterized entry stub, static ELF and the shared page-separated linker script; staging rebuilds USTAR. `Makefile:361`, `:376`, `:433`; `user/tools/start.asm:6`; `user/shell.ld:1`. |

## Phasing

| Phase | Deliverable | Required predecessor |
| --- | --- | --- |
| 0 | Complete, bounded enumerable process metadata: name, ticks, durable zombies. No syscall or user program. | Explicit kernel prerequisite approval. |
| 1 | `SYS_PROCINFO` and `/bin/ps`. | Accepted Phase 0 lifecycle/accounting gate. |
| 2 | `SYS_SYSINFO` and `/bin/sysinfo`. | Accepted Phase 1 and approved time/memory metric semantics. |
| 3 | `/bin/top`, using both syscalls without another syscall. | Accepted Phase 2 and verified terminal/sampling behavior. |

Each phase starts with a reading pass and reports discrepancies before editing.
Stop and report on a kernel gap beyond the accepted scope; do not insert an
unreviewed lookup, pointer lifetime mechanism or scheduler change. Documentation
and tests described below are future deliverables, not work performed by this plan.

## Phase 0 — Process-metadata prerequisites

### Name ownership

Recommendation: add zero-initialized `char name[16]` to the process record.
In `process_spawn_internal()`, after initializing `p->name` and before
`process_record_commit()`, invoke a metadata initialization helper that copies
at most 15 bytes from that TCB name, writes the terminating NUL, and zeroes all
remaining bytes. The call must hold no scheduler lock. Both direct and VFS spawn
paths reach this constructor (`src/kernel/thread.c:1133`, `:1210`, `:1386`,
`:1427`). Staged children receive their name during construction, not on release.
Abort clears the reservation; no failed construction becomes enumerable.

This copies values while the constructor owns the TCB. It introduces neither a
retained TCB pointer nor a lookup after teardown. Preserve the existing source
name policy; introspection truncates it but does not rename processes.

### CPU accounting and snapshot discipline

Recommendation: keep authoritative cumulative ticks in the TCB, and add a
`uint64_t cpu_ticks` cache in each process record. Move the TCB tick increment
inside a short owner scheduler-lock section, preserving the existing
preemption-enabled/current-thread conditions and scheduling decisions
(`src/kernel/thread.c:968`). A remote snapshot cannot be justified by a reader
lock while the writer remains unlocked.

Use a bounded, value-only refresh in thread context:

1. Under one CPU's scheduler lock at a time, gather PID/tick pairs from owned
   current, ready, blocked/stopped and scheduler-owned exit-transition locations
   as applicable. Copy values only; never retain a TCB pointer after unlocking.
   Exclude the separate BSP-owned staged list: its tasks have not run, retain
   their initialized zero ticks, and are not enumerable. That list is populated
   outside scheduler locking (`src/kernel/thread.c:1348`) under BSP publication
   exclusion (`src/kernel/thread.c:1414`). Never scan detached reaper-owned TCBs;
   final metadata must already contain their accounting.
2. Release every scheduler lock before acquiring the process lock. Merge by
   monotonic PID into still-existing, non-finalized records; accept only
   nondecreasing values. A stale sample may neither create a record nor update
   a reused slot by array index.
3. Under the process lock, project metadata into kernel storage; release it
   before user copy. The projection helper itself acquires no scheduler lock.

Refresh is bounded by supported CPUs and task capacity. Temporary arrays must
have explicit capacity and ownership; do not use an unprotected global scratch
buffer shared by simultaneous syscalls. Audit kernel stack use if local arrays
are chosen. No scan, process-lock acquisition or logging is added to the timer
handler. This avoids a process-lock operation in IRQ context and keeps the two
rank-1 locks separate (`src/include/spinlock.h:34`; `AGENTS.md:217`).

At exit, freeze the final tick value while the exiting continuation still owns
its TCB, then pass the value into exit metadata publication before relinquishing
the TCB. Preserve local IRQ exclusion through capture/publication so another
tick cannot escape final accounting. The existing order publishes process exit
before taking the scheduler lock (`src/kernel/thread.c:1705`); implement the
capture as a separate non-nested operation. Late refreshes cannot overwrite a
final value. Abort, staged cancellation, reaping and migration must be covered
by the ownership audit before coding. Existing scheduler scans are examples of
locations, not a complete ready-made snapshot API (`src/kernel/thread.c:1783`).

This is a documented best-effort cache, not a simultaneous all-CPU sample.
Missing a migrating task in one pass leaves its prior value; a later pass can
advance it. `top` must tolerate uneven sampling. Exact cross-CPU CPU percentages
are not claimed merely because metadata can be read from all CPUs.

### Durable zombie visibility

Recommendation: add `uncollected` and `teardown_complete` lifecycle flags to
process metadata. Under the existing process lock, set `uncollected` exactly
when a matching used child record becomes done. Retain the copied identity,
name and final ticks after TCB destruction while this flag remains true.
No retained zombie keeps a TCB, signal pointer or group membership alive.

`process_record_forget()` becomes teardown notification: clear resources as
today, mark teardown complete, and release the metadata slot only if no child
status remains. `process_record_wait()` consuming a terminal child event clears
the matching flag in the same lock transaction; the record immediately stops
being enumerable and is freed if teardown already completed. If collection
comes first, retain the hidden slot until teardown. Thus both orderings work.
Apply the same flag cleanup when parent exit discards child reservations
(`src/kernel/process_table.c:249`); do not strand zombies whose status can no
longer be collected. Abort/cancel remains a rollback, never a visible zombie.

ZOMBIE means exited with status still retrievable by waitpid, irrespective of
whether the TCB has been reaped. Collected processes are omitted. DONE is
reserved. Exit continues releasing group membership immediately
(`src/kernel/process_table.c:233`); introspection must not extend group lifetime.

Retaining zombies consumes the existing 64 process slots longer. This is an
intentional bounded-resource consequence requiring acceptance and exhaustion
tests; do not silently enlarge capacity or overwrite uncollected records.

### Gate

Proposed `test-s9-metadata-host`: actual metadata code with named pthread/IRQ
adapters; name lengths 0/15/16/32, zero fill, begin/abort/commit, staged exclusion,
stop/continue, tick monotonicity, stale refresh, final freeze, both wait/reaper
orders, parent-exit discard, nonwaitable exits, capacity and rollback. Follow the
existing metadata harness surface (`tests/process_group_host.c:1`,
`Makefile:120`). Mocks do not prove IRQ exclusion or remote TCB lifetime.

Proposed `test-s9-metadata`: bounded kernel integration plus existing Ring 3
job-control probes, BIOS/UEFI, no data disks. Exercise real owner-CPU accounting,
exit/reaper races and cross-CPU readers at SMP=1/4/8 with AP-count evidence.
Phase 0 adds no introspection syscall. Review lock ownership and unchanged S8
status/group behavior before Phase 1.

## Phase 1 — SYS_PROCINFO and ps

### Fixed process ABI

Proposed shared ABI, exactly 64 bytes with an explicit size assertion:

```c
typedef struct {
    int64_t  pid, ppid, pgid, sid;
    uint32_t state;       /* 0=free, 1=running, 2=stopped, 3=zombie, 4=done(reserved) */
    uint32_t reserved;    /* zero */
    uint64_t cpu_ticks;   /* cumulative scheduler ticks */
    char     name[16];    /* NUL-terminated, unused bytes zero */
} proc_info_t;
```

Offsets: identities 0/8/16/24, state 32, reserved 36, ticks 40, name 48.
Never copy the internal record or its pointers. Shared fixed-width structures
and size assertions follow `src/include/syscall_abi.h:82` and `:94`.

| Predicate | Public result |
| --- | --- |
| unused or unpublished | Excluded, including staged/incomplete reservations. |
| published, not exited, stopped | STOPPED (2). |
| published, not exited, not stopped | RUNNING (1), including scheduler-blocked processes. |
| published, exited, uncollected | ZOMBIE (3). |
| exited and collected/nonwaitable | Excluded. FREE and DONE are not emitted. |

`SYS_PROCINFO=36`: `(uint64_t index, proc_info_t *buffer) -> 1 / 0 / -errno`.
Index counts enumerable records in physical slot order, starting at zero.
Indices beyond the last return zero; check large values without truncation or
index arithmetic overflow. Success writes one complete record; zero/error
leaves the buffer unchanged. Validate the full writable 64-byte user range
first, even for an out-of-range index; invalid pointers return `SYSCALL_EFAULT`.
Refresh ticks outside the process lock, snapshot under it, release, then copy
out as in `src/kernel/syscall.c:1269`. Never nest scheduler and process locks.

Enumeration is bounded and best-effort across calls, not an atomic whole-table
snapshot. Churn may skip or repeat identities. Export a public bound of 64 tied
by a kernel assertion to `PROCESS_CAPACITY`; user programs must not include the
private process-table header. `ps` stops at zero/error or the bound.

Build `user/ps.c` as `build/ps.elf`, reuse `user/tools/start.asm` with
`TOOL_ENTRY=ps_main` and `user/shell.ld`, and stage `/bin/ps`. Follow explicit
source/header/linker and archive dependencies in `Makefile:376` and `:433`.
Print PID, PPID, PGID, SID, STATE, NAME; support the reserved DONE label without
expecting the kernel to emit it. Use BSS buffers, bounded integer formatting,
short-write handling, nonzero error exit, and the 512-byte user stack budget.
Use stack-usage reports, including callees, as the build pattern already does
(`Makefile:377`); no hosted runtime, red zone or SIMD (`Makefile:17`).

Gate: proposed `test-s9-ps-host` exercises actual projection/dispatcher with
mocked range validation and user formatting; indexes, exclusion, end-of-list,
zero fill, invalid writable ranges, states, short writes and cap termination.
Proposed `test-s9-ps` boots the real shell, creates running and stopped background
jobs, checks observed PID/PGID/SID/parentage, and checks zombie visibility with a
controlled parent that delays wait collection. Assert omission after collection,
no signal/status consumption, prompt recovery and hostile Ring 3 buffers
(unmapped, read-only, kernel, cross-page, overflow). BIOS and UEFI, no data disks.

## Phase 2 — SYS_SYSINFO and sysinfo

### Sources and prerequisites

PMM offers a coherent `pmm_get_stats()` under rank 4 (`src/mm/pmm.h:48`,
`src/mm/pmm.c:373`). However, `total_pages` is highest managed physical address
divided by page size, not a sum of RAM regions (`src/mm/pmm.c:60`, `:82`). Holes
are therefore included. Do not label that span installed RAM.

Recommendation: add an explicitly defined managed-RAM total derived from the
validated boot memory map, capped at PMM's supported coverage and excluding
holes/MMIO. Define exactly which RAM types it includes before Phase 2 coding;
keep free RAM as currently free PMM pages times 4096. Compute used as total minus
free only after proving both counters describe compatible sets. This needs a
Phase 2 metric prerequisite, not silent reinterpretation of existing PMM getters.

A global timer getter exists (`src/arch/x86_64/apic.c:280`), but it is not a
verified uptime source: the common handler increments a shared volatile counter
on every CPU (`:76`), and APs start the same timer/vector (`:253`;
`src/kernel/thread.c:883`). It can scale with CPU count and has unsynchronized
multi-writer increments. Do not divide this counter by 100 and call it uptime.

Recommendation: add a separate BSP-owned monotonic tick counter with atomic
publication/read and an exported immutable calibrated tick frequency. Do not
change the existing counter's consumers without a separate audit. Define uptime
from BSP timer start, not power-on or wall-clock time; firmware/early boot time
and time lost while IRQs are masked are not measured. Calibration records the
requested frequency and main currently requests 100 Hz
(`src/arch/x86_64/apic.c:239`; `src/kernel/main.c:5001`). Validate SMP-independent
progress. This time-source prerequisite requires approval before implementation.

`smp_get_cpu_count()` exists (`src/arch/x86_64/smp.c:150`); bring-up finalizes the
count from successful AP arrivals (`:642`, `:653`) and starts schedulers (`:140`).
Use successfully initialized scheduler CPUs including the BSP; do not report raw
firmware entries. Confirm count stability at the syscall's reachable boot stage.

Recommendation: task count means enumerable user processes, including zombies
and the caller, excluding unpublished reservations and kernel/idle threads.
Count under the process lock using exactly Phase 1's predicate. Label it
"Processes" in user output. A whole-kernel thread count needs separate machinery
and is out of this milestone's default scope (`src/kernel/process_table.h:5`;
`src/kernel/thread.h:43`).

### Proposed system ABI

Reserve number 37 for `SYS_SYSINFO`, subject to rechecking the shared number
table before implementation. Signature: `(sysinfo_t *buffer) -> 0 / -errno`.
Proposed fixed 48-byte layout, to freeze after the source-semantic gate:

```c
typedef struct {
    uint64_t total_ram_bytes; /* managed RAM definition approved above */
    uint64_t free_ram_bytes;  /* free PMM frames * PAGE_SIZE */
    uint64_t uptime_ticks;    /* BSP elapsed timer ticks */
    uint64_t tick_hz;         /* also the unit for proc_info_t.cpu_ticks */
    uint32_t cpu_count;       /* initialized scheduler CPUs, BSP included */
    uint32_t task_count;      /* enumerable user processes, zombies included */
    uint64_t reserved;        /* zero */
} sysinfo_t;
```

Expose ticks plus frequency so top does not hardcode 100 Hz or lose precision to
whole seconds. Assert size/offsets. Validate the complete writable buffer first;
collect coherent PMM metrics, time, CPU count and process count separately, with
no locks held across subsystems or copy-out. The full structure is a best-effort
combination of subsystem snapshots, not a globally atomic observation.

`user/sysinfo.c` prints managed total/free RAM, elapsed uptime, CPUs and Processes.
Use integer conversion, BSS storage, bounded output and the Phase 1 build pattern.
Gate: proposed `test-s9-sysinfo-host` covers units, overflow, zero frequency
defense, reserved bytes, count predicate and EFAULT. Proposed `test-s9-sysinfo`
covers real Ring 3 reads, memory sanity, process-count transitions and monotonic
time at SMP=1/4/8 under BIOS/UEFI with AP counts. Compare elapsed time with bounded
host/QEMU tolerance; specifically reject CPU-count-multiplied uptime. No data disks.

## Phase 3 — top

Reuse both ABIs. Two bounded BSS process snapshots keyed by PID; deduplicate PID
within each enumeration pass. Missing/new PIDs have no CPU delta on first sight.
Use `100 * delta(cpu_ticks) / delta(uptime_ticks)` for percent of one CPU, with
overflow-safe integer arithmetic. Label this normalization; do not divide by CPU
count or claim total system utilization from user-process rows. A single-threaded
process nominally reaches 100%; asynchronous snapshots can produce anomalous
intervals. Reject zero/reversed time and decreasing CPU counters; display unknown
for invalid intervals rather than wrapping or inventing a measurement. Stable
PID ordering breaks ties. Do not infer process exit merely from one missed row.

No screen-clear syscall is required: `SYS_WRITE` routes terminal output through
`console_terminal_write()` (`src/fs/vfs.c:25`), which implements cursor positioning,
line erase and clear-screen (`src/drivers/console.c:284`, `:298`, `:303`, `:308`).
The shell already emits clear/home (`user/shell/ui.c:277`). `SYS_TERMCTL` supplies
dimensions and modes (`src/kernel/syscall.c:192`, `:217`); `SYS_INPUT_READ` provides
bounded 1..1000 ms waits (`src/include/terminal.h:34`, `src/kernel/syscall.c:227`).
Use timed input for refresh and `q` to quit, keep ISIG enabled, and cap displayed
rows/columns to reported dimensions. Redirected/plain output should use a bounded
one-shot summary without control sequences. No terminal mutation is needed for
the initial implementation. Verify stop/continue and redraw after foreground
resume; do not suppress TTIN or acquire the terminal from a background job.

Gate: proposed `test-s9-top-host` covers PID churn, deltas, overflow, zero intervals,
formatting, dimensions, write failure and bounded memory/stack use. Proposed
`test-s9-top` checks timed refresh, busy versus stopped process deltas, q/Ctrl-C,
Ctrl-Z/bg/fg behavior, resize/dimension handling within existing API limits,
redirection and prompt recovery. BIOS/UEFI no-data-disk runs must inspect terminal
screen state as well as serial strings; existing screen/input testing provides
the pattern (`scripts/test_shell.py:1`, `:37`). No new syscall is planned.

## Verification and evidence conventions

All new target names above are proposals. Use actual subsystem code in host
fixtures and identify every mocked lock, scheduler, clock or syscall adapter.
Host passes establish bounded logic, not IRQ/SMP safety. Ring 3 probes establish
real validation/dispatch and lifecycle behavior. Physical claims require separate
device observations (`AGENTS.md:415`; `docs/plans/S8_PLAN.md:697`).

New S9 QEMU runners use disposable ISOs, BIOS plus paired read-only OVMF code and
disposable vars, bounded waits, captured logs and guaranteed cleanup. Validate
final argv on every launch: only the intended ISO and firmware; reject data disks,
injected drive/blockdev/device backends and extra arguments. The no-data-disk
S8 pattern is `scripts/test_s8_jobs.py:18`, `:48`, `:63`. Unit-test the preflight's
rejection paths. Existing regressions keep their documented disposable fixtures;
`test-shell` currently depends on an NVMe fixture (`Makefile:252`).

Regression bar for implementation: `make test-shell`, `make test-shell-host`,
`make test-pipeline-host`, `make test-s8-jobctl`, `make test-s8-jobs`,
`make test-s8-terminal`, `make test-nmi`, and `make clean` followed by `make`.
Phase 0 additionally runs metadata/group/stop/orphan host tests and scheduler/lock
regressions; Phase 2 adds relevant PMM/boot-memory checks. Use WSL Ubuntu-24.04
at `/mnt/c/Sources/FortressOS` (`AGENTS.md:136`, build/run section). Record exact
commands, firmware, SMP, commit/image identity, logs, result and limitations.
An unrun target is not a pass. SMP guest CPU count alone does not prove cross-core
process accounting; run a controlled AP process for that claim.

After each accepted phase, create/update `docs/roadmap/shell-s9-phaseN.md`,
`docs/roadmap/README.md`, and AGENTS.md status/test tables with actual evidence.
After Phase 1 acceptance, the S9 status can say Phase 1 complete and Phases 2/3
remain, noting the accepted Phase 0 prerequisite. Do not write completion language
for this plan. Preserve historical S8 evidence. No source files, tests or roadmap
status are changed by this planning task.

## Open decisions and implementation risks

1. Approve Phase 0's value-only refresh and final-tick publication after auditing
   every queue, migration and exit path. Remote reads become safe only when all
   authoritative tick writes follow the agreed synchronization discipline.
2. Accept the longer 64-slot occupancy caused by durable zombies. Tests must prove
   release on wait/parent exit and teardown in either order without leaking slots.
3. Approve the exact managed-RAM region set. Existing total-memory getters cannot
   substantiate an installed-RAM claim; do not ship that ambiguity as a label.
4. Approve a separate BSP timebase, its atomic access and timer-start epoch. It is
   a tick-based elapsed estimate, not a new precision clock or wall-clock service.
5. Confirm process-only task count and one-CPU percentage normalization. Kernel
   utilization, per-process RSS, load average and threads are deferred.
6. Verify terminal behavior on framebuffer and serial output independently. Clear
   and cursor APIs exist, but their presence does not establish top acceptance.
7. Freeze sysinfo layout and shared enumeration bound before Phase 2/1 respectively;
   no hidden ABI changes in Phase 3. No claim in this plan required source changes
   or tests to investigate; unresolved behavior remains a gate, not an assumption.
