# S8 prerequisite: process-group lifetime

Implemented 2026-09-28 as a distinct prerequisite to Phase 3. Test execution
is left to the user; no runtime acceptance is claimed. Phase 3 terminal
ownership and ingress work are not part of this checkpoint.

## Reading-pass decision: Shape A

Before this change, groups existed only as PGID/SID fields in process records
(also cached in TCBs). `group_exists` scanned used, non-exited records, including
unpublished spawn reservations. That preserved a group after leader exit while
other live/staged members remained, but did not retain an empty group for a
deferred consumer. Child-record generations described child status, not groups.

Production PIDs come from `g_global_next_tid` atomic increments in `thread.c`;
there is no free-ID allocator. `process_record_begin` rejects IDs outside the
positive signed 64-bit range. Monotonic PID allocation does not make group
lifetimes monotonic: a non-session-leader P can create PGID P, join another
group so P becomes empty, then use `setpgid(0, 0)` to recreate PGID P. This
requires no PID reuse. Therefore a bare (PGID, signal) deferred event is unsafe.

An event targets the group captured at input arrival. Comparing it with the
foreground group at drainage would incorrectly lose valid events after handoff.
Signal recipients are current members of that captured group at publication,
not a snapshot of individual member PIDs. Members that leave are not targeted.

## Primitive and boundaries

All production implementation is in `src/kernel/process_table.c`, using the
existing rank-1 process lock. A fixed 128-slot group store owns PGID, SID,
per-slot generation, member count and atomic external reference count.
Live and staged process reservations own membership from begin until
regroup/abort/exit/forget. Child exit-status records do not retain group identity.
Repeated exit/forget does not decrement membership twice.

Thread-context `process_group_acquire` validates SID and a populated target.
An owned `process_group_ref_t` pins its slot and generation. The owner must
release it exactly once using `process_group_release_ref`; plain structure
copies do not create ownership. Callers must provide unowned output storage
to acquire/retain; the API cannot detect an overwritten ownership token.

`process_group_try_retain` clones an already-owned reference with one atomic
CAS attempt, no lock or scan. The source reference must remain owned throughout
the call. A Phase 3 foreground handle protected by BSP IRQ exclusion meets that
requirement. Failure (contention or saturation) leaves output unchanged and must
feed Phase 3's overflow/drop accounting. This API does not reacquire stale
copies. Acquire can return ENOMEM on refcount saturation/contention as well.

An empty referenced group reserves its numeric PGID: joining or recreating it
returns EPERM until its last external reference is released. Reuse then advances
the slot generation; exhausted generations retire the slot. Group-store exhaustion
returns ENOMEM before publishing membership or child reservations. Existing-group
joins still work at capacity. No heap allocation, scheduler changes, or TCB
lifetime extension is introduced.

`process_group_signal` is a trusted kernel, thread-context API. It validates the
retained identity and publishes to its members under the process lock through
the same signal helper as numeric kill. Empty/stale targets return ESRCH. It
does not consult the current foreground group or require the original caller
to remain alive. Future user-facing operations must enforce session permissions;
this kernel handle is not a new userspace capability ABI.

Phase 3 must retain/release foreground and queued-event ownership explicitly;
ordinary IRQs may only clone an already-owned handle. Phase 4/5 must preserve
job identity across their user/kernel boundary using this primitive or a
validated ABI built on it, rather than re-derive numeric group lifetime rules.
No shell job handle ABI or terminal syscall is implemented by this checkpoint.

## Verification handoff

Commands actually run by the agent:

- `wsl -d Ubuntu-24.04 -- make build/kernel/process_table.o`: PASS, strict kernel compilation.
- Host test `gcc ... -fsyntax-only`: PASS; no test execution.

Run in WSL Ubuntu-24.04 at `/mnt/c/Sources/FortressOS`:

```sh
make test-s8-groups-host
make test-s8-process-host test-s8-signals-host test-s8-stops-host
make test-s8-process test-s8-signals test-s8-stops
```

The new target compiles actual process-table code with pthread lock adapters,
ASan/UBSan, and bounded compiler/execution timeouts. It covers namesake PGID
recreation, stale-generation rejection, session validation, captured group-wide
publication, staged membership and abort, leader exit, STOP/CONT publication,
capacity/transaction rollback, concurrent clones/releases and publication racing
exit, reference saturation, and exact empty-store cleanup. It does not establish
IRQ, scheduling, terminal ingress, or hardware correctness. Existing integration
targets provide the process/signal regression coverage; results remain pending.
