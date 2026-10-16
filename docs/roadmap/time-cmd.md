# POSIX `/bin/time` and Per-Process CPU Time Accounting

## 1. Overview & Scope

This document records the architectural design, kernel implementation, syscall ABI extensions, and verification evidence for per-process CPU time accounting and the Ring 3 `/bin/time` tool in FortressOS.

Following the addition of the wall-clock time subsystem (`SYS_CLOCK_GETTIME` 74, `SYS_NANOSLEEP` 75), `/bin/time` provides full POSIX time accounting by reporting:
1. `real`: Wall-clock elapsed time measured via `CLOCK_MONOTONIC` using high-resolution hardware TSC interpolation.
2. `user`: CPU time spent executing user-mode code.
3. `sys`: CPU time spent executing inside the kernel on behalf of the process (syscalls and interrupts).

---

## 2. Kernel Accounting Architecture

### 2.1 Per-TCB CPU-Time Fields
`struct tcb` in `src/kernel/thread.h` was extended with four 64-bit accounting fields:
- `uint64_t utime_tsc`: Total accumulated user-mode TSC cycles.
- `uint64_t stime_tsc`: Total accumulated kernel-mode (system) TSC cycles.
- `uint64_t last_tsc`: Timestamp (via `apic_poll_clock_read()` / `rdtsc`) of the most recent accounting transition.
- `bool in_user_mode`: Boolean flag indicating current execution state (`true` = user mode, `false` = kernel mode).

All fields are initialized to zero at allocation (`thread_create_internal`, `sched_init`), with `last_tsc` seeded to current TSC.

### 2.2 Mode Transition Hooks
Kernel hooks update the accumulators at every privilege and execution boundary without heap allocation or unbounded locking:
1. **Syscall Entry (`user -> kernel`)**:
   `syscall_dispatch()` invokes `time_account_user_to_kernel(curr)` immediately upon entry, adding elapsed user cycles (`now - last_tsc`) to `utime_tsc`, setting `in_user_mode = false`, and updating `last_tsc = now`.
2. **Syscall Exit (`kernel -> user`)**:
   `syscall_dispatch()` invokes `time_account_kernel_to_user(curr)` before returning to the assembly exit stub (both normal return and `sys_sigreturn`), crediting elapsed cycles to `stime_tsc`, setting `in_user_mode = true`, and updating `last_tsc = now`.
3. **Interrupt Entry (`user -> kernel`)**:
   `isr_exception_handler()` checks `(frame->cs & 3) != 0`. If the interrupt arrived while executing in user mode, it calls `time_account_user_to_kernel(curr)`, charging the interrupted user execution to `utime_tsc` and switching accounting to kernel mode.
4. **Interrupt Exit (`kernel -> user`)**:
   In `src/arch/x86_64/interrupts.asm`, the `.signal_done` label inspects `test byte [rsp + 144], 3` (CS RPL). If returning to user mode, it calls `time_account_enter_user()`, charging the interrupt processing duration to `stime_tsc` and resetting mode to user.
5. **Context Switch (`sched_post_switch`)**:
   When switching out `previous`, `time_account_switch_out(previous)` charges elapsed cycles to `previous->utime_tsc` or `previous->stime_tsc` according to `previous->in_user_mode`. When switching in the incoming thread, `time_account_switch_in(curr)` refreshes `curr->last_tsc = rdtsc()`, guaranteeing that sleeping, waiting, or preemption intervals are never charged to the resumed task.
6. **User Process Trampoline**:
   In `src/arch/x86_64/context.asm`, `user_process_trampoline` invokes `time_account_enter_user` immediately prior to pushing the user iretq frame and executing `iretq`.
7. **Process Exit (`process_exit`)**:
   `process_exit()` closes the final accounting window before detaching, updating `stime_tsc`, and passing exact `utime_tsc` and `stime_tsc` values to `process_record_exit_accounted_times()`.

### 2.3 Lock Rank Discipline
`g_process_lock` and per-CPU scheduler locks (`sched_lock`) are both Rank 1 locks. Per `PROTECTED.md`, they **must never nest**.
In `process_get_times()`:
1. `g_process_lock` is acquired to validate target PID existence and caller permissions.
2. If target is already exited (`target->exited == true`), its final CPU totals are read directly from `process_record_t` and `g_process_lock` is released.
3. If target is alive, `g_process_lock` is released before calling `thread_get_cpu_times()`, which acquires per-CPU `sched_lock` independently. If the target exited during the window between release and lookup, `g_process_lock` is safely re-acquired to read the finalized exit record.

---

## 3. Syscall ABI Extensions

### 3.1 `SYS_TIMES` (Syscall 76)
- **Signature**: `int64_t sys_times(int64_t pid, tms_t *buf);`
- **Parameters**:
  - `pid`: Process identifier (`0` = calling process, `> 0` = target process).
  - `buf`: Userland pointer to a `tms_t` structure.
- **Structure Layout**:
```c
typedef struct {
    int64_t tms_utime_ns;   /* user time in nanoseconds */
    int64_t tms_stime_ns;   /* system time in nanoseconds */
    int64_t tms_cutime_ns;  /* children user time in nanoseconds */
    int64_t tms_cstime_ns;  /* children system time in nanoseconds */
} tms_t;
_Static_assert(sizeof(tms_t) == 32, "tms_t size");
```
- **Permission Policy**:
  - Querying self (`pid == 0` or `pid == caller_pid`) is always permitted.
  - Parent querying its child (`target->parent == caller_pid`) is always permitted.
  - Callers with `euid == 0` or `CAP_KILL` capability are permitted.
  - Callers with matching real/effective user ID are permitted.
  - Unauthorized callers receive `-SYSCALL_EPERM`.
  - Non-existent or already reaped processes return `-SYSCALL_ESRCH`.
- **Zombie CPU Retention**:
  When a child exits, its CPU totals remain stored in `process_record_t` as a zombie until reaped by `SYS_WAITPID`.

### 3.2 `WNOWAIT` Support in `SYS_WAITPID`
Added `#define WNOWAIT 4u` to `src/include/syscall_abi.h`.
When `options & WNOWAIT` is passed:
- `process_record_wait()` verifies if an eligible child has exited or changed state.
- It returns the child PID and writes its exit status to `status`.
- It does **not** consume the child record (`c->used` remains `true`, `c->reported_seq` is not updated).
- This allows `/bin/time` to wait for child termination, inspect its zombie CPU times via `SYS_TIMES`, and subsequently reap it via standard `waitpid(child_pid, &status, 0)`.

---

## 4. Userland Tool: `/bin/time`

Implemented in `user/tools/time.c`:
- **CLI Options**:
  - `-p`: POSIX output format.
  - `-h`, `--help`: Usage help.
  - `--`: Delimiter terminating option parsing.
- **Output Formats** (written to `stderr`):
  - **Default (Linux-style)**: Tab-separated minutes and seconds:
    ```text
    real	0m1.234s
    user	0m0.891s
    sys	0m0.102s
    ```
  - **POSIX (`-p`)**: Space-separated seconds with two decimal places:
    ```text
    real 1.23
    user 0.89
    sys 0.10
    ```
- **Exit Status Propagation**: `/bin/time` propagates the exact exit status of the child (or `128 + signal` if terminated by signal).

---

## 5. Verification & Test Evidence

### 5.1 Host Unit Test Suite (`make test-time-cmd-host`)
Built with GCC, `-fsanitize=address,undefined -fno-omit-frame-pointer`:
- Format conversion tests across zero, sub-second, multi-second, multi-minute, and negative durations.
- CLI argument parsing (`-p`, `-h`, `--help`, unrecognized flags, missing command).
- Full mocked syscall execution verifying `WNOWAIT` pre-reap call, zombie `SYS_TIMES` sampling, reap call, and status propagation (`exit 7`, `exit 137` on `SIGKILL`, `exit 127` on `ENOENT`).
- **Result**: 100% PASS.

### 5.2 Multi-Firmware QEMU Integration Suite (`make test-time-cmd`)
Executed across all 4 matrix configurations:
1. **BIOS, SMP=1**: PASS
   - `time sleep 1`: `real 0m1.002s`, `user 0m0.000s`, `sys 0m0.001s`.
   - `time -p sleep 1`: `real 1.00`, `user 0.00`, `sys 0.00`.
   - Exit code propagation: `time which time` -> 0, `time which nonexistent` -> 1.
   - CPU-bound accounting: `time sha256sum /bin/shell`: `real 0m0.008s`, `user 0m0.004s`, `sys 0m0.001s` (`user > 0`, `sys > 0`).
   - Per-process isolation verified.
2. **BIOS, SMP=4**: PASS
   - Verified AP core preemption, TSC cycle accounting, and clean shutdown.
3. **UEFI, SMP=1**: PASS
   - Verified with paired OVMF 4M firmware (`OVMF_CODE_4M.fd`, `OVMF_VARS_4M.fd`).
4. **UEFI, SMP=4**: PASS
   - Verified multi-core UEFI guest environment.

### 5.3 Regression Suite
- `make test-time-host`: 100% PASS
- `make test-date-sleep-host`: 100% PASS
- `make test-time`: 100% PASS
- `python3 scripts/test_process_table_host.py`: All 6 test suites (`--metadata`, `--groups`, `--signals`, `--stops`, `--orphans`) 100% PASS with 4,000 concurrent lifecycles.
- Strict uninstrumented kernel build and `git diff --check`: PASS with zero warnings/errors.
