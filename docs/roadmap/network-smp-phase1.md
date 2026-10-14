# Network SMP Phase 1 evidence

2026-10-14. **PASS: generic handoff/lifetime mechanism and inert CPU-0 owner.**
Production networking, socket CPU guards, spawn pinning, TCP ISNs and reboot
policy are unchanged. Phase 2 has not begun; this is not AP networking or
physical acceptance.

## Prerequisite reconciliation

The user explicitly authorized reconciling PROTECTED.md with the existing
`056a12d` implementation: SHA-256 boot-secret/tuple/TSC ISNs, no quiet period,
and conditional hardware entropy assurance. The unconditional startup banner
remains a known overstatement, not security evidence. No TCP code was changed.

[Phase 0 evidence](network-smp-phase0.md) remains qualified: `gate-8tsepefx`
passed 12/12 baseline boots and 61 negatives; `gate-4fs51srj` passed all guest
cases but **failed** source preservation because AGENTS.md changed concurrently.
Neither that failure nor the concurrent update was overwritten. Three original
host targets passed; the missing raw-serial test adapters were added to
`tests/net_eth_host.c`, after which `make test-net-eth-host` passed ASan/UBSan.

## Implemented mechanism and safety audit

`src/kernel/handoff.c` / `handoff.h` provide copied scalar requests, stable
slot/uint64 generation tokens, per-actor registration, offer/admission, claim,
pending steps, immutable completion, collection, cancellation, caller release,
abandonment and sole-owner reclamation. No caller stack, user address, file or
TCB pointer is retained in a request. Main, idle and process constructors zero
the new registration; children do not inherit their parent's active operation.

The implementation uses bounded scans of 64 records rather than a separate
runnable-token ring. At most 32 records are QUEUED; data can use at most 24
credits, leaving eight for control, which can borrow idle data capacity.
Claiming immediately releases execution credit. Retained pending/completed
records therefore do not strand control execution credits.

Monotonic tickets are assigned at first offer, FIFO within each class. The owner
can grant an offered ticket while its caller is stopped; unoffered PREPARED
records do not block later callers. Eligible classes alternate at claim, and
pending/ack cursors rotate independently. The inert owner bounds each turn to
eight claims, eight pending steps and eight acknowledgement inspections, then
yields. Internal saturation returns the internal WAIT result to adapters that
sleep/retry; it is not a new public EAGAIN outcome.

Caller interest survives until collection/release or abandonment; owner interest
survives caller death. A second operation from the same actor cannot begin until
its retained record is reclaimed. Cancellation before claim publishes one
terminal result without side effects. Claimed or pending cancellation requires
an explicit owner step, permitting future typed cleanup before completion.
An accepted positive result survives a later cancellation. Generation exhaustion
retires the slot; ticket exhaustion fails closed. Terminal service failure
rejects new work and cannot be restarted by calling start again. Claimed memory
remains retained until its owner acknowledges it; an unresponsive owner is not
recovered by freeing its storage.

Predicates acquire-read atomic generation/state, admission sequence or owner
hint, and never take a lock under a scheduler lock. Completion publishes reply
fields before release state. The ordinary Rank-1 handoff lock never nests with
scheduler/process/network locks. Wakes occur after unlock; only fixed-size
kernel scalar assignments occur while locked. Pending storage alone does not
keep the owner runnable: `handoff_notify` publishes service readiness, serialized
against owner hint clearing. Future network input/resource/deadline adapters
must supply those notifications; no generic protocol timer is implemented here.

TCB value registration is abandoned unlocked before process descriptor teardown,
before thread termination, and idempotently in the detached reaper before
address-space/stack destruction. No scheduler switching or ownership protocol
was changed. The existing dirty workspace spawn-pinning changes were preserved.

Each request record is 80 bytes (the host symbol is 0x1400 bytes for 64 records).
Compile-time record/pool/registration bounds preserve the Phase 0 ledger and
576 KiB ceiling. Phase 1 allocates no payload slabs and has no hot allocation.
File/endpoint references, 64 KiB staging, copy pins, reservation leases and
receive/accept commit/abort transactions are **Phase 2 prerequisites**, not
implemented or accepted by this gate.

## Final host and guest gates

Invocation in WSL Ubuntu-24.04: `make test-network-smp-phase1`.

Host evidence: `build/network-smp-phase1/host-7cydyifs/manifest.json` and its
ASan/UBSan and ThreadSanitizer logs. Both **PASS** using actual mechanism code,
pthreads lock adapters and a condition-variable scheduler adapter. The final
campaign has 48 concurrent producers / **14,400 requests**, forced saturation
before owner startup, and exactly **14,474 reclaimed records** including the
deterministic cases. Coverage includes unavailable owner, cancellation before
and after claim, pending cleanup acknowledgement, caller death, reversed
completions, FIFO/class fairness, retained-record and credit saturation, stale
tokens, exhaustion, failure quarantine, rejection of restart, and pending idle
notification. No allocation-failure claim applies to this static mechanism.

TSan uses `setarch x86_64 -R`; exact compile/run argv and binary/source hashes
are in the manifest. `host-dpfb_mn1` retains the earlier **FAIL** from TSan's
startup `unexpected memory mapping`, before test execution. The same binary's
successful `tsan-no-aslr.log` is also retained. Later successful campaigns do not
erase that infrastructure failure. Host adapters do not prove real scheduling.

Final guest evidence: `build/network-smp-phase1/gate-r23svl5l/manifest.json`.
**6/6 PASS**, source preservation **PASS**, baseline disk hash unchanged;
`exclusions.json`: **6 argv audits / 31 negative exclusions PASS**.

| Firmware | SMP | Actual caller CPU mask | Owner CPU | Exact reclaimed records |
| --- | ---: | --- | ---: | ---: |
| BIOS | 1 | 0x1 | 0 | 124 |
| BIOS | 4 | 0xf | 0 | 125 |
| BIOS | 8 | 0xff | 0 | 125 |
| UEFI | 1 | 0x1 | 0 | 124 |
| UEFI | 4 | 0xf | 0 | 125 |
| UEFI | 8 | 0xff | 0 | 125 |

The private fixture syscall goes through real Ring 3 entry and the normal
dispatcher signal/return-validation tail. Snapshot-only staged launch comes from
`scripts/test_network_smp_phase0.py`; ordinary production commands remain pinned.
Each inert owner step asserts CPU 0. Foreground entry logs plus the atomic caller
mask establish AP execution, including simultaneous pending callers on all APs
at SMP=8. Seven real pending producers plus one foreground control caller fit
the shell's eight-job limit. The separate **64-record saturation case uses
synthetic actor identities**, not 64 Ring 3 processes.

Every case forces publication before owner sleep, completion before caller
enrollment, cancellation and STOP/CONT/KILL recovery, contention, saturation and
exact drainage. SMP=4/8 additionally force a remote wake while the outgoing
context is still busy and before `switch_context` saves it; a missed rendezvous
traps rather than passing. Generic owner waits have no timer-rescue hook.
Pending release/cancellation uses explicit notifications. Foreground probe
results use real shell exit status because UART text can interleave with Done
notifications. Final pool/caller-mask/count assertions are separate oracles.

Exact source identities (HEAD plus dirty/untracked sources) are in `sources.json`.
This report and the final AGENTS.md entropy-assurance wording were updated
after the successful campaign. The captured snapshot hashes remain authoritative.
Snapshot overrides, serial logs, redacted command transcripts, per-boot argv,
paired OVMF variables and disposable disks are retained. Every launch passes the
strict storage audit before Popen. Each boot cleanly shuts down and its extracted
root passes Linux `e2fsck -fn`; QEMU is bounded and reaped on failure as well.
The ordinary pinned `ifconfig` query and shell recovery pass in every case.

Final instrumented kernel SHA-256:
`fa3dd704ad3f01769927d99e3d2d3337ea94c66ea1b07cceb0d28d6d27e59ce5`.
Initramfs:
`00bc84c58aab92fb52828a7e64c897b3ce3c058840d6e637a2eb0aa42afca992`.
Baseline disposable disk:
`e793f18ba6826e8eef44815a4b7a330d482238c757170f44b8a69c7529115ccf`.

## Release-build and regression boundary

`build/network-smp-phase1/release-verification.json` records the uninstrumented
strict kernel/initramfs build in isolated `guest-workspace-fkqk7eqw`: **PASS**.
Symbols/strings and archive contents confirm no fixture owner, private syscall
hook, CPU provenance strings, exhaustion injection or probe ELF. No shared
release ISO/image was rebuilt. Kernel SHA-256:
`db0efc3ccd45eaaf5c118791c22a412805537bb6b1be62a8b28ddc54b69779f0`;
initramfs:
`9cb74330513af287364c221aa14eb69309e5ea04102170c11750fdf11cfb5f25`.

`make test-resched-return-host test-s8-stops-host test-net-eth-host` passed;
output is retained in `build/network-smp-phase1/regressions.log`. Python fixture
scripts compile. These host regressions retain their mocked scheduling limits.

## Retained preparation failures

All earlier gate directories and their FAIL manifests/logs remain:

- `gate-70d4yvof`: saturation fixture forgot to wake its frozen owner.
- `gate-g2nm_042`: concurrent UART text split the background-start marker.
- `gate-42qb8jvo`: runner assumed job 1 instead of the actual shell job ID.
- `gate-d_9wjxoj`: eight background jobs left no foreground control slot.
- `gate-p7ibbehb`: SMP=1 fixture waited for cancellation acknowledgement while
  a pending signal prevented sleeping, starving the owner. Unwind now releases
  caller interest immediately; owner acknowledgement survives it. The private
  syscall was also moved into the normal dispatcher return path.
- `gate-1pfkttk6`: BIOS/SMP=1 passed; an asynchronous prompt was mistaken for the
  following background launch response. Commands now require their echoed line.
- `gate-wo_54eas`: two BIOS cases passed; SMP=8 split PASS text with a Done
  notification. Exit status replaced text as the foreground result oracle.
- `gate-o8e74ch_`: invalid reuse-workspace path rejected before build/boot.

`gate-5160e8lt` and `gate-8vxbos69` each passed six guest cases and source/argv
audits before the final pending-idle refinement. The final campaign above uses
the refined mechanism; those prior passes do not substitute for its rerun.

No network protocol integration, file/endpoint-reference acceptance, wire audit,
measured idle CPU result, e1000e matrix, general AP syscall acceptance or physical
acceptance follows from this inert-owner gate. Phase 2 and later phases retain
their own explicit gates.
