# FortressOS — SMP Cross-CPU User Process Spawn & Exception Routing

## 1. Fixed Bug: Cross-CPU User-Process Spawn onto an Idle AP (Resolved 2026-09-30)

### Problem
A user process spawned onto a CPU other than the BSP via `process_spawn_internal(target_cpu, ...)` did not run if the target CPU was idle. The process metadata record was published with state `THREAD_RUNNING` (`state=1`), but it never executed, accumulating zero ticks indefinitely. Discovered during S9 Phase 0 (`make test-s9-metadata SMP=4`).

### Root Cause
The process was successfully queued by the BSP, the target AP was awakened from idle via `IPI_VECTOR_RESCHED`, and the AP scheduler switched context into Ring 3 at entry `_start` (`user_process_trampoline`). However, upon executing its very first system call (`int 0x80`), execution entered `isr_exception_handler` in `src/arch/x86_64/idt.c`:

```c
/* Hardware or software interrupts (vectors >= 32, excluding syscall 0x80) */
if (frame->vector >= 32 && frame->vector != 0x80) {
    ...
    return;
}

/* APs unexpected exception isolation: keep unexpected faults out of BSP test hooks. */
if (cpu->id != 0) {
    cpu->fault_vector = frame->vector;
    cpu->fault_error = frame->error_code;
    cpu->fault_rip = frame->rip;
    for (;;) __asm__ volatile("cli; hlt");
}
```

Because vector 128 (`0x80`) was explicitly excluded from the hardware IRQ block (`frame->vector != 0x80`), it fell through to the AP exception-isolation check. On an AP (`cpu->id != 0`), `int 0x80` was treated as an unexpected kernel exception: the AP recorded `fault_vector = 128` (`0x80`) and permanently parked itself with `cli; hlt`. The syscall was never dispatched, LAPIC timer ticks stopped, and the task remained stuck in `THREAD_RUNNING` with zero ticks.

### Fix
In `src/arch/x86_64/idt.c`, relocated the `if (frame->vector == 0x80)` system call dispatch block ahead of the `if (cpu->id != 0)` AP exception isolation block.

### Verification Evidence
Verified with `python3 scripts/test_s9_metadata.py`:
- `PASS S9 metadata bios SMP=1`
- `PASS S9 metadata uefi SMP=1`
- `PASS S9 metadata bios SMP=4`
- `PASS S9 metadata uefi SMP=4`
- `PASS S9 metadata bios SMP=8`
- `PASS S9 metadata uefi SMP=8`

---

## 2. OPEN — User-Mode Exception on an AP Parks Core Instead of Terminating Process

### Problem
A Ring 3 process that faults on a CPU other than the BSP takes the AP exception-isolation path (`isr_exception_handler`, `idt.c`: `if (cpu->id != 0)`) and parks the AP in `cli; hlt`, instead of terminating the faulting process via the user-fault path used on the BSP (`(frame->cs & 3) == 3`).

This is the same class of ordering issue as the fixed `int 0x80` dispatch bug: the AP kernel fault-isolation check runs before user-mode fault handling.

### Status and Impact
- **Status**: OPEN / Latent.
- **Impact**: No current production or test feature intentionally faults a user process on an AP, so this does not block current milestones.
- **Required Architecture / Design**:
  The fix is a behavior definition, not a trivial ordering change:
  1. Decide and specify how a user-mode fault on an AP transitions into process termination (signal/SIGSEGV metadata publication, parent notification, durable zombie records).
  2. Address-space teardown and descriptor release on a non-BSP core under the L4 detach-under-lock rule and deferred CR3 cleanup invariants.
  3. Route `(frame->cs & 3) == 3` faults to that path before the AP-isolation park.

### Protected Contracts Touched
- `src/arch/x86_64/idt.c`: user-fault dispatch and exception gate ordering.
- Process termination on an AP and cross-core exit signaling.
- Memory/CR3 ownership & L4 lock discipline (§9-adjacent — requires its own scoped authorization).
