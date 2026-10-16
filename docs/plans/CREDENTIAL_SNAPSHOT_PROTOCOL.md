# FortressOS — Credential Snapshot and Publication Protocol Design Review

Status: COMPLETE — Authoritative Design Review Document (Roadmap Item #5).  
Implemented in: `src/include/creds.h`, `src/kernel/thread.h`, `src/kernel/process_table.h`, `src/kernel/process_table.c`, `src/kernel/permissions_syscalls.inc`, `src/fs/vfs.c`, `src/kernel/syscall.c`.  
Verified by: `make test-process-registry-host`, `make test-perm-creds-host`, `make test-perm-spawn-host`, `make test-perm-phase5-host`, 16 spawn fault injection cuts (`src/mm/memory_boot_test.c`), and Dell Latitude 5590 physical acceptance.

---

## 1. Purpose and Scope

This document reviews the architectural design and operational contracts of the FortressOS credential snapshot, publication, and cross-CPU synchronization protocol. It closes roadmap item #5 ("Credential snapshot/publication protocol design review"), which was called out as a prerequisite in [PERMISSIONS_PLAN.md](PERMISSIONS_PLAN.md) §2.1 and verified across Permissions Phases 0–5.

### What This Document Covers
- How credentials are encapsulated, bound, published, and read coherently across CPUs without holding pointers to thread control blocks (`tcb_t`) past their protected lifetimes.
- The optimistic concurrency publication protocol (expected-old comparison under lock) that prevents partial or corrupt credential states.
- The lifecycle interaction between process creation, failure rollback (the 16 spawn fault injection cuts), and process exit detachment.
- The lock ordering constraints governing the process table lock ($G$), filesystem locks, scheduler locks, and memory subsystem locks.
- The concrete protocol rules followed by all credential consumers (VFS path resolution, signal authorization, process inspection, spawn/setuid, and credential mutation syscalls).

### What This Document Does Not Cover
- Discretionary Access Control (DAC) permission algorithms (mode bit masks, sticky bits, directory setgid traversal; covered in [PERMISSIONS_PLAN.md](PERMISSIONS_PLAN.md)).
- The user-facing syscall number mappings and register conventions (defined in `src/include/syscall_abi.h` and [PERMISSIONS_PLAN.md](PERMISSIONS_PLAN.md) Phase 2).
- The on-disk user authentication database schema and crypt hashing algorithms (covered in [PERMISSIONS_PLAN.md](PERMISSIONS_PLAN.md) Phase 3).

---

## 2. Where Credentials Live

In FortressOS, credential state is encapsulated by value in the `creds_t` structure, defined in [`src/include/creds.h`](../../src/include/creds.h):

```c
#define CREDS_MAX_GROUPS 16

typedef struct creds {
    uint32_t uid, euid, suid;
    uint32_t gid, egid, sgid;
    uint32_t groups[CREDS_MAX_GROUPS];
    uint16_t ngroups, umask;
    uint32_t reserved;
    uint64_t cap_effective;
} creds_t;

_Static_assert(sizeof(creds_t) == 104, "credential value size");
_Static_assert(offsetof(creds_t, groups) == 24, "credential groups offset");
_Static_assert(offsetof(creds_t, ngroups) == 88, "credential count offset");
_Static_assert(offsetof(creds_t, reserved) == 92, "credential reserved offset");
_Static_assert(offsetof(creds_t, cap_effective) == 96, "credential caps offset");
```

### Field Responsibilities
- **`uid`, `euid`, `suid`**: Real, Effective, and Saved User IDs. DAC checks test `euid`; set-ID checks test `uid` or `suid`; signal delivery authorization tests `euid`, `uid`, and `suid`.
- **`gid`, `egid`, `sgid`**: Real, Effective, and Saved Group IDs.
- **`groups[16]` / `ngroups`**: Bounded array of supplementary group IDs (at most `CREDS_MAX_GROUPS = 16`). Unused entries are guaranteed to be zero.
- **`umask`**: File mode creation mask (default `0022`; low 9 bits `0777` honored).
- **`reserved`**: Padding required to align `cap_effective` to an 8-byte boundary; guaranteed zero by `creds_valid()`.
- **`cap_effective`**: 64-bit capability bitmask (defined bits `CAP_DAC_OVERRIDE` through `CAP_FSETID`, masked by `CAP_ALL = (1ULL << 12) - 1`).

### TCB Embedding and Process Lifetime
Credentials reside directly inside the thread control block ([`src/kernel/thread.h`](../../src/kernel/thread.h)):

```c
typedef struct tcb {
    ...
    signal_state_t signals;
    creds_t creds; /* Private until binding; then process_table value APIs only. */
    ...
} tcb_t;
```

Storage is allocated as part of the `tcb_t` heap structure. However, its access lifecycle is strictly separated into three phases:
1. **Construction Phase (Private)**: While a newly spawned task is being populated by `process_spawn_internal`, `p->creds` is private to the spawning CPU. No lock is held, and no other CPU can observe `p->creds`.
2. **Bound Phase (Registry Publication)**: Prior to scheduling or staging, `process_record_bind_creds(p->tid, &p->creds)` registers the storage pointer inside the global process table. Once bound and published, **direct field reads and writes on `p->creds` are forbidden across the kernel**. All reads must use `process_record_creds()`, and all mutations must use `process_record_publish_creds()`.
3. **Detachment Phase (Exit/Reaping)**: When the process exits (`process_record_exit_accounted_times`) or aborts (`process_record_abort`), the registry pointer is cleared (`p->creds = NULL`) under $G$ before the TCB is returned to the heap.

> [!IMPORTANT]
> **One `tcb_t` Per Process Constraint**:  
> In FortressOS today, every user thread is an independent process (1 TCB = 1 process owning its descriptor table, virtual address space, and credentials). If shared-address-space multithreading is introduced in the future, credentials **must move to a shared `process_t` object** before thread creation is allowed. A per-thread credential pointer in a multi-threaded process is strictly prohibited.

---

## 3. The Snapshot Model

FortressOS enforces a strict **value-snapshot discipline** for all credential observations.

### Syscall Entry Snapshot
When a system call requires credential authority (e.g., VFS lookup, file creation, permission queries), the calling thread takes a local stack copy via `syscall_actor()`:

```c
/* One bounded actor snapshot; G is released before any filesystem work. */
static bool syscall_actor(creds_t *out) {
    tcb_t *curr = thread_current();
    return curr && process_record_creds(curr->tid, out);
}
```

Inside [`src/kernel/process_table.c`](../../src/kernel/process_table.c):

```c
bool process_record_creds(uint64_t pid, creds_t *out) {
    if (!out) return false;
    uint64_t irq = spin_lock_irqsave(&g_process_lock);
    process_record_t *p = find(pid);
    bool ok = p && p->published && !p->exited && p->creds;
    if (ok) *out = *p->creds;
    spin_unlock_irqrestore(&g_process_lock, irq);
    return ok;
}
```

### Properties of the Snapshot
1. **Whole-Value Copy**: All 104 bytes of `creds_t` are copied into caller stack memory under the global process lock `g_process_lock` ($G$).
2. **Zero Escaped Pointers**: No pointer to the TCB, no pointer to `p->creds`, and no internal array pointers escape the critical section.
3. **Tear-Free Coherence**: Because the copy occurs while holding $G$, a reader cannot observe a torn credential (such as a new `euid` paired with an old `cap_effective` mask or partial `groups` array).
4. **Lifetime Decoupling**: Once copied to the stack, the snapshot is fully detached from the target's lifecycle. Even if the process terminates, its TCB is reaped, and its PML4 root is unmapped on another core, the local `creds_t` stack variable remains valid and immutable for the duration of the system call.

---

## 4. The Publication Model

Credentials are never modified in place through unprotected pointer writes. All credential transitions (via `sys_setresuid`, `sys_setresgid`, `sys_setgroups`, `sys_capset`, `sys_umask`, or program execution) follow an **optimistic compare-and-swap publication protocol**.

```
[Reader/Writer CPU]                                      [Process Registry (G)]
       |                                                           |
       |-- 1. Snapshot old creds: syscall_actor(&old) ------------>| (takes G, copies value, drops G)
       |                                                           |
       |-- 2. Pure value computation: creds_setresuid(&old, ...)-> | (OUTSIDE G: zero locks, zero I/O)
       |      Validates inputs, transitions, capability drops      |
       |                                                           |
       |-- 3. Commit: process_record_publish_creds(pid, old, next)->| (takes G)
       |                                                           |-- Check: p->published && !p->exited
       |                                                           |-- Full byte compare: *p->creds == *old?
       |                                                           |   [MATCH] -> *p->creds = *next; return true
       |                                                           |   [MISMATCH] -> return false (EAGAIN)
       |<-- 4. Return status (0 or EAGAIN) -----------------------| (drops G)
```

### The Expected-Old Publication Routine
[`src/kernel/process_table.c`](../../src/kernel/process_table.c) implements `process_record_publish_creds`:

```c
bool process_record_publish_creds(uint64_t pid, const creds_t *expected, const creds_t *next) {
    if (!creds_valid(expected) || !creds_valid(next)) return false;
    creds_t value = *next;
    uint64_t irq = spin_lock_irqsave(&g_process_lock);
    process_record_t *p = find(pid);
    bool ok = p && p->published && !p->exited && p->creds;
    if (ok) {
        const uint8_t *a = (const uint8_t *)p->creds, *b = (const uint8_t *)expected;
        for (unsigned i = 0; i < sizeof(*expected); ++i) {
            if (a[i] != b[i]) { ok = false; break; }
        }
    }
    if (ok) *p->creds = value;
    spin_unlock_irqrestore(&g_process_lock, irq);
    return ok;
}
```

### The "No Partial Publication" Invariant
A credential transition is atomic. It either fully commits or fails without side effects:
- `creds_valid()` validates reserved bit zeroes, capability mask limits (`cap_effective & ~CAP_ALL == 0`), and `ngroups <= 16`.
- The pure transformation functions in [`src/kernel/creds.c`](../../src/kernel/creds.c) (`creds_setresuid`, `creds_setresgid`, `creds_setgroups`, `creds_capset`, `creds_umask`) write to an output buffer only upon complete success. Any error (`CREDS_EPERM`, `CREDS_EINVAL`) leaves the output untouched.
- If concurrent modification occurred between the initial snapshot and publication commit, the 104-byte `memcmp` detects the mismatch and returns `false`, causing `credential_commit` to return `SYSCALL_EAGAIN`.

### Spawn Path Rollback Integration
During program spawn (`process_spawn_from_vfs_group`), the child process is constructed through multiple staged allocations before it is published to a scheduler runqueue. FortressOS verifies this lifecycle using **16 deterministic fault injection cuts** in [`src/mm/memory_boot_test.c`](../../src/mm/memory_boot_test.c):

| Fault Injection Cut | Injection Point | Unwind Action | Credential Invariant |
| --- | --- | --- | --- |
| `SPAWN_FAULT_VMM_USER_PML4` | User PML4 allocation | `process_record_abort` | Unbound, slot freed |
| `SPAWN_FAULT_ELF_SEGMENT_PMM` (P0/P1) | ELF physical frame alloc | `process_record_abort` | Unbound, slot freed |
| `SPAWN_FAULT_ELF_SEGMENT_MAP` (P0/P1) | ELF page table mapping | `process_record_abort` | Unbound, slot freed |
| `SPAWN_FAULT_SIGRESTORER_PMM/MAP` | Signal trampoline setup | `process_record_abort` | Unbound, slot freed |
| `SPAWN_FAULT_USER_STACK_PMM/MAP` | User stack setup | `process_record_abort` | Unbound, slot freed |
| `SPAWN_FAULT_KSTACK_PMM/MAP/ALLOC` | Kernel stack creation | `process_record_abort` | Unbound, slot freed |
| `SPAWN_FAULT_TCB_KMALLOC` | TCB memory allocation | `process_record_abort` | Unbound, slot freed |
| `SPAWN_FAULT_FD_INIT` | Standard descriptor setup | `process_record_abort` | Unbound, slot freed |
| `SPAWN_ACTIONS_PARTIAL` | Open/dup2 action failure | `process_record_abort` | Unbound, slot freed |
| `SPAWN_FAULT_SCHED_REF` | VMM space reference increment | `process_record_abort` | Bound, detached via abort |

If any cut triggers or an invalid executable/action is encountered:
1. `process_spawn_from_vfs_group` calls `process_record_abort(pid)`.
2. Under $G$, `process_record_abort` resets `p->creds = NULL`, `p->signals = NULL`, leaves the process group, and sets `p->used = false`.
3. Memory resources (PML4, kstack, TCB, file structures) are freed.
4. **Result**: A failed spawn never leaves a dangling, half-initialized, or orphaned credential record in the registry.

---

## 5. The Cross-CPU Read Model

In an SMP environment, a reader running on CPU $A$ may need to inspect the credentials of a target process whose TCB was created or is executing on CPU $B$ (e.g., during `kill`, `sysinfo`, `ps`, or `top`).

### The Protocol for Cross-CPU Observation
1. The caller provides only the target's numeric `uint64_t pid`.
2. The reader invokes `process_record_creds(pid, &snapshot)` or `process_signal_send_creds(caller_pid, target_pid, sig)`.
3. The registry acquires `g_process_lock` ($G$) with local interrupts disabled (`spin_lock_irqsave`).
4. The target slot is located in `processes[]` via `find(pid)`.
5. If the target is unused, exited, or unpublished, the read safely returns `false` / `ESRCH`.
6. If the target is valid, `snapshot = *p->creds` copies all 104 bytes into CPU $A$'s stack frame.
7. $G$ is released (`spin_unlock_irqrestore`).

### Memory Safety: Why TCB Pointers Must Never Be Retained
- A TCB's physical memory is freed by `sched_reap_dead()` on its executing CPU once `has_exited` is marked and its execution reference drops.
- While `vmm_space_t` maintains `sched_refs` (tasks holding the CR3 root) and `op_refs` (transient page walks/mappings), **these reference counters protect the paging structures, NOT the `tcb_t` struct or its `creds` field**.
- Retaining a raw `tcb_t *` across scheduling yields, syscalls, or lock releases exposes the system to use-after-free corruption if the target terminates.
- By clearing `p->creds = NULL` under $G$ at the exact moment of process exit (`process_record_exit_accounted_times`) and process teardown (`process_record_forget`), any concurrent cross-CPU reader holding $G$ either observes the complete, valid credential value or observes `p->creds == NULL` and returns `ESRCH`. Stale or torn pointer dereferencing is physically impossible.

---

## 6. Lock Ordering and Exclusion

FortressOS strictly ranks all synchronization primitives ([AGENTS.md](../../AGENTS.md) §4 L1; [PROTECTED.md](../../PROTECTED.md)).

### The Global Process Lock ($G$)
`g_process_lock` is an **ordinary Rank-1 spinlock**. It serializes access to process identity, credential bindings, process group topologies, and signal action states.

### Strict Non-Nesting Contracts
```
Rank 1: g_process_lock (G)  <--- CANNOT NEST --->  Rank 1: ext2_lock / e4_lock / run_lock
Rank 1: g_process_lock (G)  <--- CANNOT NEST --->  Rank 1: per-CPU sched_lock
Rank 1: g_process_lock (G)  <--- CANNOT NEST --->  Rank 1: network locks (g_net_stack_lock, etc.)
```

1. **No Process Lock Across Filesystem Locks or I/O**:
   $G$ must never be held during any VFS directory lookup, inode read, disk write, BOT USB polling, or Ext4 journal transaction. Syscall handlers must acquire $G$, snapshot the actor credentials into a stack variable, release $G$, and only then enter the VFS layer.
2. **No Process Lock Across Scheduling (`switch_context`)**:
   Holding $G$ across a context switch or during a blocking wait is an invariant violation (L2).
3. **No Process Lock Across User Buffer Copying**:
   User memory verification (`vmm_validate_user_range`) and copy operations (`copy_user_string`, `memcpy` to/from Ring 3) must execute completely outside $G$. If a page fault were to occur while holding $G$, deadlock would ensue.

---

## 7. Specific Consumers and Protocols

Every kernel subsystem that touches credentials adheres to a specific operational protocol:

| Consumer | Actor (Caller) | Target | Lock Held | Lifetime / Storage | Failure Mode |
| --- | --- | --- | --- | --- | --- |
| **VFS Permission** (`vfs_permission`) | Current syscall thread | Filesystem node | None ($G$ dropped before VFS call) | Value copy on stack (`const creds_t *actor`) | `-VFS_EACCES` / `-VFS_EPERM`; no node or disk mutation |
| **Signal Delivery** (`process_signal_send_creds`) | Current syscall thread | Target PID or PGID | $G$ (`g_process_lock`) | Value check under $G$ via `permission_signal()` | `SYSCALL_ESRCH` (dead/absent) or `SYSCALL_EPERM` (auth failed); zero signals queued |
| **Process Inspection** (`ps`, `top`, `sysinfo`) | User inspection tool | Any system PID | $G$ (`g_process_lock`) | Snapshot copied to caller buffer under $G$ | Slot skipped if uncommitted or uncollected |
| **Spawn / Set-ID** (`process_spawn_from_vfs_group`) | Parent process | Child process being spawned | $G$ during snapshot, bind, and commit | Child `tcb_t.creds` populated; bound before commit | `process_record_abort` cleans slot; zero leaks |
| **Credential Syscalls** (`setresuid`, `capset`, etc.) | Current process | Self | $G$ during initial snapshot and final commit | Stack calculation; optimistic commit | `EINVAL` / `EPERM` (invalid) or `EAGAIN` (stale CAS); creds unchanged |

### 1. VFS Permission Checks
- Handlers in `src/kernel/syscall.c` (e.g., `sys_stat`, `sys_open`, `sys_chmod`) call `syscall_actor(&actor)`.
- The stack-allocated `&actor` pointer is passed down the VFS call chain (`vfs_lookup_creds`, `vfs_open_mode_creds`, `vfs_permission`).
- The filesystem driver acquires its private rank-1 lock (`ext2_lock`, `e4_lock`, `run_lock`) and evaluates DAC rules against the node's authoritative metadata (`node->uid`, `node->gid`, `node->mode`).
- $G$ is never held; filesystem locks never nest with $G$.

### 2. Signal Authorization
- `process_signal_send_creds(caller_pid, selector, sig)` is invoked.
- $G$ is acquired.
- The caller record `self` and target records `p` are located.
- `permission_signal(self->creds, p->creds, sig)` verifies authority:
  - Allowed if `self->creds->cap_effective & CAP_KILL`.
  - Allowed if `self->creds->uid == p->creds->uid` or `self->creds->uid == p->creds->suid`.
  - Allowed if `self->creds->euid == p->creds->uid` or `self->creds->euid == p->creds->suid`.
  - Staged child processes attached to signals are eligible targets.
- If authorized, `signal_publish(p, sig)` marks the signal in `p->signals->pending_mask`.
- Woken PIDs are collected in an array. $G$ is released before waking threads.

### 3. Process Inspection
- Ring 3 tools invoke `SYS_SYSINFO` or `SYS_PROCLIST`.
- `process_record_snapshot_pid` or `process_record_snapshot` acquires $G$, checks `p->published && !p->exited`, and extracts scalar data (`pid`, `parent`, `pgid`, `sid`, `cpu_ticks`, `state`, `name`).
- No TCB pointer or credential pointer is ever exposed to the user.

### 4. Process Spawn and Set-ID Execution
- In `process_spawn_from_vfs_group`:
  1. `process_record_begin` reserves child PID under $G$.
  2. `process_record_creds(curr->tid, &actor)` snapshots parent credentials under $G$.
  3. `vfs_open_exec_creds` opens the binary using `&actor`.
  4. `spawn_credentials(&actor, &image_metadata, &exec_creds)` determines child credentials:
     - Sets UID/GID from image if setuid/setgid bits are set and the mount permits suid (`!(mnt_flags & VFS_MNT_NOSUID)`).
     - Ordinary spawn preserves dropped capabilities: if parent dropped capabilities, child never restores them, even if UID is 0.
  5. `process_spawn_internal` initializes child TCB and sets `child->creds = exec_creds`.
  6. `process_record_bind_creds(child->tid, &child->creds)` binds child credentials under $G$.
  7. `process_record_attach_signals` attaches signal state under $G$.
  8. `process_record_commit(child->tid)` marks child published under $G$.
  9. If any allocation or setup step fails, `process_record_abort(child->tid)` runs under $G$, detaching credentials and freeing the reservation cleanly.

### 5. Credential Mutation Syscalls
- Syscalls 66–70 (`setresuid`, `setresgid`, `setgroups`, `capset`, `umask`) implemented in [`src/kernel/permissions_syscalls.inc`](../../src/kernel/permissions_syscalls.inc):
  1. Validate argument ranges and user buffers outside $G$.
  2. Take current snapshot: `syscall_actor(&old)`.
  3. Run pure value transition: `creds_setresuid(&old, r, e, s, &next)`.
  4. If transition fails, return `SYSCALL_EPERM` or `SYSCALL_EINVAL` immediately (credentials unaltered).
  5. Call `credential_commit(&old, &next)` -> `process_record_publish_creds(self->tid, &old, &next)`.
  6. If publication succeeds, returns `0`. If concurrent modification occurred, returns `SYSCALL_EAGAIN`.

---

## 8. The Signal Authorization Path and Global Consolidation

During Permissions Phase 0, a source audit identified races in the historical PID-shard locking architecture (self-signal vs. group-signal deadlocks, tick accounting corruption during slot reuse). The architecture was formally **consolidated onto the single global rank-1 `g_process_lock` ($G$)**.

### Rationale for Single Global $G$
- Process capacity in FortressOS is bounded (`PROCESS_CAPACITY = 64`).
- Group signaling, parent/child relationships, and session topologies require inspecting multiple records atomically. Sharded locks introduced severe lock ranking anomalies and complex acquisition ordering.
- Consolidating onto a single ordinary Rank-1 spinlock ensures that every registry lookup, signal delivery check, credential validation, and status update executes within a uniform, deadlock-free critical section.

### The Wake-After-Unlock Discipline
A critical invariant in FortressOS is that **the process lock ($G$) must never nest with a per-CPU scheduler lock** (both are Rank 1). However, signaling a thread often requires waking it up (`thread_wake_for_signal`), which acquires that thread's CPU `sched_lock`.

To resolve this without rank inversion, [`src/kernel/process_table.c`](../../src/kernel/process_table.c) strictly enforces the **wake-after-unlock discipline**:

```c
static int64_t signal_send_common(uint64_t caller, int64_t selector, uint64_t sig, bool user) {
    ...
    uint64_t irq = spin_lock_irqsave(&g_process_lock);
    ...
    uint64_t woken[PROCESS_CAPACITY];
    unsigned woken_count = 0;
    ...
    /* Inside G critical section: */
    signal_publish(p, sig);
    if (woken_count < PROCESS_CAPACITY) woken[woken_count++] = p->pid;
    ...
    spin_unlock_irqrestore(&g_process_lock, irq);

    /* OUTSIDE G: safe to acquire scheduler locks */
    for (unsigned i = 0; i < woken_count; ++i) {
        thread_wake_for_signal(woken[i]);
    }
    return result;
}
```

### Atomic Action Take and Stop-State Publication
In `take_action` (called by `process_signal_take_action` and `process_signal_take_control`):
- $G$ is acquired.
- Pending, unblocked, unignored signals are evaluated.
- If a default stop signal (`SIGSTOP`, `SIGTSTP`, `SIGTTIN`, `SIGTTOU`) is taken:
  - `p->signals->pending_mask` is updated.
  - `stopped_locked(p, sig)` transitions `p->stopped = true` and publishes the child stop event to `children[]`.
  - The signal action and previous mask are snapshotted to caller variables.
- All state changes commit atomically under the **same single $G$ critical section**, preventing discrepancies between pending masks, handler frames, and child status reports.

---

## 9. Forbidden Patterns

The following patterns represent architectural violations. Any pull request or modification introducing them must be rejected:

1. **No Retained `tcb_t` Pointers**:  
   Never store a `tcb_t *` across system call boundaries, in filesystem data structures, or in global registries. TCB lifetimes are managed exclusively by the scheduler.
2. **No Unsynchronized Field Reads**:  
   Never read `target->creds.euid` or any credential field directly without holding $G$ or holding an exclusively owned private snapshot. Direct field access causes torn reads under SMP.
3. **No Process Lock Held Across I/O or Scheduling**:  
   Never hold `g_process_lock` across VFS operations, block I/O, USB polling loops, memory allocation (`kmalloc`), page table manipulations, or `switch_context`.
4. **No Lock Nesting with Rank-1 Locks**:  
   Never acquire $G$ while holding `ext2_lock`, `e4_lock`, `run_lock`, a per-CPU `sched_lock`, or a network stack lock.
5. **No Partial Publication on Transition Failure**:  
   Never write half-computed credential fields to the TCB. All changes must be fully derived, validated via `creds_valid()`, and committed atomically via `process_record_publish_creds()`.
6. **No Escaping Pointers from Registry**:  
   `process_record_creds()` must always copy by value into caller storage (`*out = *p->creds`). It must never return `p->creds` directly.

---

## 10. Forward-Looking Constraints

As FortressOS evolves, future milestones must respect the following design boundaries:

1. **Multi-Threaded User Processes (1:N or M:N Threading)**:  
   If multiple user threads sharing an address space are implemented, credentials **must move out of `tcb_t` into a shared `process_t` object**. The binding in `process_table.c` will bind to `process_t.creds`, preserving the exact same snapshot and publication protocols. This structural refactoring is an explicit precondition for userland threads.
2. **Thread-Specific CPU Accounting**:  
   When thread-level accounting (`CLOCK_THREAD_CPUTIME_ID`, `pthread_getcpuclockid`) is introduced, cycle counters (`utime_tsc`, `stime_tsc`) remain thread-scoped inside `tcb_t`. Credential and security contexts remain strictly process-scoped.
3. **Cross-Core Sockets and Filesystem Operations**:  
   When network socket descriptors or filesystem descriptors are manipulated across application cores (AP and BSP), the actor credential snapshot captured at syscall entry must continue to govern the operation. No cross-core message or handoff may carry a raw pointer to a remote CPU's credential state.

---

## 11. Evidence and Implementation References

### Authoritative Implementation Sources
- [`src/include/creds.h`](../../src/include/creds.h): Canonical 104-byte value structure, capability bit definitions (`CAP_*`), keep flags, and helper prototypes.
- [`src/kernel/creds.c`](../../src/kernel/creds.c): Pure functional transformation algorithms (`creds_setresuid`, `creds_setresgid`, `creds_setgroups`, `creds_capset`, `creds_umask`).
- [`src/kernel/thread.h`](../../src/kernel/thread.h): TCB embedding and spawn lifecycle flags.
- [`src/kernel/process_table.h`](../../src/kernel/process_table.h): Registry public prototypes (`process_record_bind_creds`, `process_record_creds`, `process_record_publish_creds`).
- [`src/kernel/process_table.c`](../../src/kernel/process_table.c): Global $G$ implementation, expected-old compare-and-swap, exit detachment, and signal authorization.
- [`src/kernel/permissions_syscalls.inc`](../../src/kernel/permissions_syscalls.inc): Syscall adapters for `setresuid`, `setresgid`, `setgroups`, `capset`, `capget`, `umask`.
- [`src/fs/vfs.c`](../../src/fs/vfs.c): Actor-aware path resolution (`vfs_lookup_creds`) and permission verification (`vfs_permission`).
- [`src/mm/vmm.c`](../../src/mm/vmm.c) / [`src/mm/vmm.h`](../../src/mm/vmm.h): Address space lifecycle separation (`sched_refs`, `op_refs`, deferred teardown).

### Automated Verification Suites
- **`make test-process-registry-host`** ([`tests/process_registry_audit_host.c`](../../tests/process_registry_audit_host.c)):  
  Simulates multithreaded concurrent writers and readers under ASan/UBSan. Exercises 20,000 continuous credential updates against 20,000 concurrent reader snapshots, verifying that every snapshot matches either state $A$ or state $B$ with zero torn reads. Verifies invalid bit rejection, expected-old CAS mismatch detection, and clean detachment upon abort/exit/reuse.
- **`make test-perm-creds-host`** ([`tests/perm_creds_host.c`](../../tests/perm_creds_host.c)):  
  Host ASan/UBSan exhaustive test suite verifying 70,496 credential transformation matrix cases across all user ID permutations, capability drops, and group bounds.
- **`make test-perm-spawn-host`** / **`make test-sudo`**:  
  Verifies credential inheritance during spawn, dropped-capability preservation, and secure set-ID privilege transitions under BIOS and UEFI QEMU.
- **Spawn Fault Injection Harness** ([`src/mm/memory_boot_test.c`](../../src/mm/memory_boot_test.c)):  
  Validates that all 16 spawn fault injection cuts trigger clean `process_record_abort` execution with zero leaks and complete credential detachment.

### Phase Reports
- [Permissions Phase 0: Values and Registry Audit Checkpoint](../roadmap/permissions-phase0-values.md)
- [Permissions Phase 1: Permissive Wiring and Call-Site Verification](../roadmap/permissions-phase1-wiring.md)
- [Permissions Phase 2: Enforcement and Metadata Syscalls](../roadmap/permissions-phase2-gates.md)
- [Permissions Phase 3: Persistent Accounts, Login, and Credential Syscalls](../roadmap/permissions-phase3-gates.md)
- [Permissions Phase 4: Set-ID Execution, Sudo, and Nosuid Isolation](../roadmap/permissions-phase4-gates.md)
- [Permissions Phase 5: Hardening and Physical Acceptance Review](../roadmap/permissions-phase5-gates.md)
- [Dell Latitude 5590 Physical Acceptance Report](../roadmap/permissions-phase5-dell.md)

---

## 12. Known Limitations

The following limitations are intrinsic to the current implementation and documented for architectural clarity:

1. **Fixed Process Table Capacity**:  
   The process table is bounded to `PROCESS_CAPACITY = 64` entries. Exceeding 64 concurrent processes causes `process_record_begin` to fail with `EAGAIN`/`ENOMEM`.
2. **Synchronous Syscall Rejection on Contention**:  
   If concurrent tasks mutate the same process's credentials simultaneously, the loser of the optimistic compare-and-swap in `process_record_publish_creds` receives `SYSCALL_EAGAIN`. The kernel does not execute a busy-wait spin retry; retry policy is deferred to the calling application runtime.
3. **Volatile In-Memory Storage on Live Media**:  
   Until persistent storage is mounted and initialized via the Persistent Root Installer (Phase 4/5), runtime credential mutations (e.g., changing passwords or adding groups) do not persist across reboots.
