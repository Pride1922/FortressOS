# FortressOS — Userland Threads & pthreads Architectural Plan (Issue #12)

**Status:** COMPLETE / IMPLEMENTED  
**Date:** 2026-10-25  
**Issue:** [#12](https://github.com/Pride1922/FortressOS/issues/12) (`Userland threads / pthreads: Shared-address-space threads; credentials move to shared process object`)  
**Scope:** Shared-address-space threading model, credential & descriptor decoupling to `process_t`, kernel threading & synchronization primitives (Syscalls 89–94: `SYS_THREAD_CREATE`, `SYS_THREAD_EXIT`, `SYS_THREAD_JOIN`, `SYS_THREAD_SELF`, `SYS_THREAD_YIELD`, `SYS_FUTEX`), freestanding Ring 3 `<pthread.h>` userspace library, host ASan/UBSan unit test suite, and multi-firmware BIOS/UEFI QEMU acceptance suite.

---

## 1. Executive Summary & Problem Statement

### 1.1 The Single-Threaded Process Limitation
Prior to Issue #12, FortressOS modeled processes as 1:1 TCB-to-address-space entities. Every process had a single thread of execution (`tcb_t`), holding private credentials (`creds_t`), a private file descriptor table (`fd_table`), and a private page directory pointer (`cr3` / `pml4_virt`). 

As noted in `docs/plans/CREDENTIAL_SNAPSHOT_PROTOCOL.md` (line 366):
> "If multiple user threads sharing an address space are implemented, credentials must move out of `tcb_t` into a shared `process_t` object. The binding in `process_table.c` will bind to `process_t.creds`, preserving the exact same snapshot and publication protocols."

Furthermore, without shared address space threads and lightweight userland synchronization primitives (`futex`), FortressOS user programs could not utilize POSIX `pthread` concurrency, multi-threaded computation, or thread-safe synchronization abstractions (mutexes, condition variables, barriers, and once-initialization).

### 1.2 Objectives
1. **Shared Process Abstraction (`process_t`)**:
   - Decouple process-wide state (credentials `creds_t`, open file descriptor table `fd_table`, current working directory `cwd`, address space `cr3`/`vmm_space`, thread reference counts) from per-thread execution context (`tcb_t` containing RIP, RSP, registers, kernel stack, signal state, thread-specific ID).
   - Ensure credential binding (`process_record_bind_creds` and `process_record_publish_creds`) attaches canonically to `process_t.creds`, preserving all invariant guarantees of the FortressOS credential publication protocol.
2. **Kernel Threading & Synchronization Primitives**:
   - Implement Syscalls 89–94:
     - `SYS_THREAD_CREATE` (89): Spawns a new concurrent thread sharing the caller's CR3, PML4, credentials, and file descriptors with allocated kernel stack and initialized user trampoline frame.
     - `SYS_THREAD_EXIT` (90): Cleanly terminates calling thread, decrements active thread counts, records exit value, and wakes join waiters without retiring shared address space or closing descriptors unless it is the final thread.
     - `SYS_THREAD_JOIN` (91): Waits for target thread termination using lockless scheduler wait predicates and retrieves thread exit status.
     - `SYS_THREAD_SELF` (92): Returns caller TID.
     - `SYS_THREAD_YIELD` (93): Voluntarily yields current execution quantum.
     - `SYS_FUTEX` (94): Fast userspace locking primitive supporting `FUTEX_WAIT` (with optional millisecond timeout) and `FUTEX_WAKE` operations.
3. **Freestanding Userspace `<pthread.h>`**:
   - Deliver full POSIX-compatible pthreads API in Ring 3 adhering to Gate G1 ($\le 1024$ B stack usage, zero dynamic heap allocations):
     - `pthread_create`, `pthread_join`, `pthread_exit`, `pthread_self`, `pthread_equal`, `pthread_yield`, `pthread_detach`.
     - `pthread_mutex_init`, `pthread_mutex_lock`, `pthread_mutex_trylock`, `pthread_mutex_unlock`, `pthread_mutex_destroy`.
     - `pthread_cond_init`, `pthread_cond_wait`, `pthread_cond_timedwait`, `pthread_cond_signal`, `pthread_cond_broadcast`, `pthread_cond_destroy`.
     - `pthread_barrier_init`, `pthread_barrier_wait`, `pthread_barrier_destroy`.
     - `pthread_spin_init`, `pthread_spin_lock`, `pthread_spin_trylock`, `pthread_spin_unlock`, `pthread_spin_destroy`.
     - `pthread_once`.
4. **Verification**:
   - Host test harness `tests/pthread_host.c` validating pthreads algorithms under GCC/Clang AddressSanitizer/UndefinedBehaviorSanitizer.
   - Userland test command `/bin/pthread-test` validating create/join, mutex serialization, condition variables, barriers, and once-initialization in live Ring 3.
   - Multi-firmware acceptance suite `scripts/test_pthread.py` running automated boots under QEMU across BIOS and UEFI with SMP=1 and SMP=4.

---

## 2. Protected Contracts & Invariants

1. **Rank-1 Lock Discipline (`PROTECTED.md` §Lock ranks)**:
   - Runqueue locks (`sched-0` .. `sched-N`), `g_process_lock`, and other Rank 1 spinlocks must NEVER be acquired while holding another Rank 1 lock.
   - Scheduler wait predicates evaluated under `sched_lock` by `sched_wait_until` must NEVER acquire any spinlocks; predicates are completely lock-free, utilizing atomic loads (`__atomic_load_n`) with Acquire semantics against release-published state.
   - No spinlock may remain held across `switch_context` or `sched_wait_until`.
2. **Credential Publication Integrity (`CREDENTIAL_SNAPSHOT_PROTOCOL.md`)**:
   - Process credentials are held in `process_t.creds`. When a process spawns, `process_record_bind_creds(p->tid, &proc->creds)` attaches the process registry to `process_t.creds`.
   - Syscall actor evaluation (`syscall_actor`) resolves credentials via `curr->process ? curr->process->pid : curr->tid`, guaranteeing that all threads of a process share identical privileges and observe atomic credential modifications simultaneously.
3. **Memory & Address Space Lifecycle (`PROTECTED.md` §Virtual memory)**:
   - When secondary threads spawn, `vmm_space_add_sched_ref(cr3)` increments scheduler reference counting.
   - When a secondary thread terminates via `SYS_THREAD_EXIT`, `vmm_space_retire(cr3)` is NOT called; only when `__atomic_sub_fetch(&proc->thread_count, 1)` reaches zero (the final thread) does `thread_exit()` retire the address space.
   - In `sched_reap_dead()`, PML4 destruction and descriptor closing occur only when the process reference count `__atomic_sub_fetch(&proc->ref_count, 1)` reaches zero.
4. **System V AMD64 ABI Conformance**:
   - New user stacks must be 16-byte aligned before `iretq` execution to satisfy System V AMD64 ABI requirements for SIMD/SSE operations. `sys_thread_create` masks `stack_top &= ~0xFULL`.
5. **Gate G1 Stack & Heap Ceilings**:
   - Kernel and user thread routines adhere strictly to stack frames $\le 1024$ bytes.
   - Zero heap allocation is performed on wait/wake/futex paths. Pre-allocated static waiter tables (`MAX_FUTEX_WAITERS = 64`, `MAX_THREAD_JOIN_RECORDS = 64`) serve all synchronization primitives.

---

## 3. Structural Architecture

### 3.1 The `process_t` Object
Defined in [`src/kernel/thread.h`](../../src/kernel/thread.h):
```c
struct process {
    uint64_t       pid;             /* Thread group leader / main process ID */
    spinlock_t     lock;            /* Process metadata lock */
    uint32_t       ref_count;       /* Active thread references + process handle */
    uint32_t       thread_count;    /* Number of alive threads */
    creds_t        creds;           /* Canonical process-shared credentials bound to registry */
    struct file   *fd_table[MAX_PROCESS_FDS];
    uint32_t       fd_flags[MAX_PROCESS_FDS];
    char           cwd[256];
    uintptr_t      cr3;
    uint64_t      *pml4_virt;
    struct vmm_space *vmm_space;
    uint64_t       exit_code;
    bool           has_exited;
};
```

### 3.2 Thread Control Block (`tcb_t`) Decoupling
In [`src/kernel/thread.h`](../../src/kernel/thread.h), `tcb_t` receives a pointer to the shared process object:
```c
typedef struct tcb {
    uint64_t       rsp;              /* Saved stack pointer (MUST be first field at offset 0) */
    uint64_t       tid;
    uint64_t       parent_pid, pgid, sid;
    signal_state_t signals;
    process_t     *process;         /* Shared process object (NULL for kernel threads) */
    creds_t        creds;           /* Private until binding; then process_table value APIs only */
    ...
} tcb_t;
```

All file descriptor operations (`fd_alloc`, `fd_alloc_exact`, `fd_get`, `fd_free`, `fd_dup`, `fd_dup2`, `fd_close_cloexec`, `fd_clone_table`) dynamically route to `proc->process->fd_table` and `proc->process->fd_flags` when `proc->process` is present.

---

## 4. Syscall ABI Specification

Defined in [`src/include/syscall_abi.h`](../../src/include/syscall_abi.h):

| Syscall Number | Name | Arguments | Return Value | Description |
|---|---|---|---|---|
| 89 | `SYS_THREAD_CREATE` | `uintptr_t entry`, `uintptr_t stack`, `uintptr_t arg`, `uint64_t *out_tid` | `tid` or `-errno` | Spawns new thread sharing address space, credentials, and file descriptors |
| 90 | `SYS_THREAD_EXIT` | `uint64_t exit_val` | Does not return | Terminates current thread and publishes exit value |
| 91 | `SYS_THREAD_JOIN` | `uint64_t tid`, `uint64_t *out_val` | `0` or `-errno` | Waits for target thread to exit and collects return value |
| 92 | `SYS_THREAD_SELF` | None | `uint64_t tid` | Returns current thread ID |
| 93 | `SYS_THREAD_YIELD` | None | `0` | Yields current scheduler quantum |
| 94 | `SYS_FUTEX` | `uint32_t *uaddr`, `int op`, `uint32_t val`, `const timespec_t *timeout` | `count` or `-errno` | Fast userspace locking primitive (`FUTEX_WAIT` / `FUTEX_WAKE`) |

### 4.1 Futex Operation Codes
```c
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
```

---

## 5. Userspace `<pthread.h>` Library Implementation

Located in [`user/pthread.h`](../../user/pthread.h):

- **Mutex Implementation**:
  - `pthread_mutex_t` uses atomic compare-and-swap on a 32-bit state variable (`0` = unlocked, `1` = locked without waiters, `2` = locked with waiters).
  - Fast-path lock: `__atomic_compare_exchange_n(&m->state, 0, 1)`. Fast-path unlock: `__atomic_compare_exchange_n(&m->state, 1, 0)`.
  - Contended slow-path falls back to `SYS_FUTEX` with `FUTEX_WAIT` and `FUTEX_WAKE`.
- **Condition Variable Implementation**:
  - `pthread_cond_t` utilizes an atomic monotonically incrementing 32-bit sequence counter.
  - `pthread_cond_wait` atomically unlocks the mutex, waits via `SYS_FUTEX` (`FUTEX_WAIT`) on the sequence value, and re-acquires the mutex upon waking.
  - `pthread_cond_signal` and `broadcast` increment the sequence counter and wake waiters via `SYS_FUTEX` (`FUTEX_WAKE`).
- **Barrier Implementation**:
  - `pthread_barrier_t` manages cycle generation and arrival counts, waking all waiting threads when the threshold count is reached.
- **Once-Initialization**:
  - `pthread_once` coordinates single-execution routines using states `0` (uninitialized), `1` (in-progress), and `2` (done) with futex sleep if another thread is initializing.

---

## 6. Verification & Test Evidence

### 6.1 Host ASan/UBSan Unit Suite (`make test-pthread-host`)
Host unit test suite [`tests/pthread_host.c`](../../tests/pthread_host.c) verifies userspace pthread algorithms under compiler instrumentation:
- **Test 1**: Mutex mutual exclusion with 8 concurrent threads executing 1,000 serialized increments each. Verified exact counter value $8,000$.
- **Test 2**: Condition variable coordination between producer and consumer threads.
- **Test 3**: Barrier synchronization across 4 concurrent threads.
- **Test 4**: `pthread_once` single-execution guarantee across 8 threads.
- **Result**: **100% PASS** with 0 memory errors, 0 leaks, 0 undefined behaviors.

### 6.2 Userland Concurrency Suite (`/bin/pthread-test`)
Stand-alone Ring 3 binary [`user/pthread_test.c`](../../user/pthread_test.c) executed in live FortressOS environment:
- `[1/5] Testing pthread_create & pthread_join... PASS`
- `[2/5] Testing pthread_mutex (concurrent counter)... PASS (counter=2000)`
- `[3/5] Testing pthread_cond_wait & pthread_cond_signal... PASS`
- `[4/5] Testing pthread_barrier... PASS`
- `[5/5] Testing pthread_once single-init guarantee... PASS (run_count=1)`
- `=== All Userland Thread Tests PASSED ===`

### 6.3 Multi-Firmware QEMU Acceptance Suite (`make test-pthread`)
Automated multi-firmware test harness [`scripts/test_pthread.py`](../../scripts/test_pthread.py):
- BIOS Firmware (SMP=1): **PASS**
- BIOS Firmware (SMP=4): **PASS**
- UEFI Firmware (SMP=1): **PASS**
- UEFI Firmware (SMP=4): **PASS**
- **Result: 4/4 Matrix 100% PASS**
