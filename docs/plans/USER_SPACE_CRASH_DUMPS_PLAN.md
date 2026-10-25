# FortressOS — User-Space Crash Core Dumps (Issue #39)

## 1. Executive Summary

With the introduction of multi-threaded applications and userland pthreads (Issue #12), robust diagnostics for unhandled exceptions in Ring 3 are essential for troubleshooting concurrency faults, memory access violations, stack corruption, and illegal instructions.

Issue #39 introduces a bounded, zero-heap, non-allocating **User-Space Crash Core Dumper** (`src/kernel/crash_dump.c`, `src/kernel/crash_dump.h`) in FortressOS. When any unhandled CPU exception occurs in user mode (CPL 3), the kernel automatically captures full architectural register state, decoded condition flags, the faulting instruction stream, and the user stack window, outputting diagnostics immediately to COM1 serial and appending them to the kernel `dmesg` ring buffer.

Crucially, this implementation eliminates the AP exception handling defect in `idt.c` (where non-BSP cores halted on any unhandled interrupt frame), ensuring fault isolation across all CPU cores in multi-core SMP environments.

---

## 2. Architecture & Control Flow

```
                      +-------------------------------------------------+
                      |    CPU Exception in User Space (CPL 3)          |
                      |    (#DE, #UD, #NP, #SS, #GP, #PF, #MF, #XM)     |
                      +-------------------------------------------------+
                                               |
                     +-------------------------v-------------------------+
                     |  Check User Signal Handler (sigaction / signal)   |
                     +---------------------------------------------------+
                          /                                       \
                [Custom Handler Registered]                [SIG_DFL / Unhandled]
                        /                                           \
         +------------------------------+             +-------------------------------+
         | Deliver Signal Frame (v1)    |             |  Execute Crash Core Dumper    |
         | (signal_deliver_handler)     |             |  (crash_dump_user)            |
         +------------------------------+             +-------------------------------+
                        | (fails / recursive)                         |
                        +---------------------------------------------+
                                               |
                               +---------------+---------------+
                               |                               |
                +--------------v--------------+ +--------------v---------------+
                |   Architectural Registers   | |    Bounded Memory Windows    |
                |  - RAX..R15, RIP, RSP       | |  - Code at RIP (16 bytes)    |
                |  - RFLAGS (decoded flags:   | |  - Stack at RSP (16 QWORDs)  |
                |    CF, PF, ZF, SF, IF, OF)  | |  - Safe user-range validation|
                |  - CR2, Vector mnemonic,    | |    prevents secondary faults |
                |    decoded page fault bits  | +------------------------------+
                +-----------------------------+                |
                               \                               /
                                +--------------+---------------+
                                               |
                               +---------------v---------------+
                               | Non-Allocating Dual Output:   |
                               | 1. Immediate COM1 Serial UART |
                               | 2. Kernel dmesg Ring Buffer   |
                               +-------------------------------+
                                               |
                               +---------------v---------------+
                               | Clean Process Termination     |
                               | (exit status: 128 + POSIX sig)|
                               +-------------------------------+
```

---

## 3. Exception to Signal Mapping & POSIX Status

Unhandled exceptions map deterministically to POSIX signals and termination codes:

| Vector | Mnemonic | POSIX Signal | Exit Status | Trigger Condition |
|---|---|---|---|---|
| **0** | `#DE` (Divide Error) | `SIGFPE` (8) | **136** | Integer division by zero |
| **4** | `#OF` (Overflow Trap) | `SIGFPE` (8) | **136** | `INTO` instruction overflow |
| **5** | `#BR` (Bound Range) | `SIGSEGV` (11) | **139** | `BOUND` range exceeded |
| **6** | `#UD` (Invalid Opcode) | `SIGILL` (4) | **132** | Illegal opcode or `ud2` instruction |
| **7** | `#NM` (Device Not Avail) | `SIGFPE` (8) | **136** | Coprocessor/FPU unavailable |
| **11** | `#NP` (Segment Not Present)| `SIGBUS` (7) | **135** | Segment descriptor not present |
| **12** | `#SS` (Stack Fault) | `SIGBUS` (7) | **135** | Stack segment limit violation |
| **13** | `#GP` (General Protection)| `SIGSEGV` (11) | **139** | Non-canonical address or privileged instruction |
| **14** | `#PF` (Page Fault) | `SIGSEGV` (11) | **139** | Unmapped page, write violation, or NX exec |
| **16** | `#MF` (x87 FP Error) | `SIGFPE` (8) | **136** | x87 floating point exception |
| **17** | `#AC` (Alignment Check) | `SIGBUS` (7) | **135** | Unaligned memory access |
| **19** | `#XM` (SIMD FP Error) | `SIGFPE` (8) | **136** | SSE/AVX floating point exception |

---

## 4. Key Architectural Contracts

### 4.1 SMP Core Independence & AP Isolation Bug Fix
Prior to Issue #39, `src/arch/x86_64/idt.c:255` halted the AP (`if (cpu->id != 0) { for(;;) cli; hlt; }`) **before** checking `if ((frame->cs & 3) == 3)`. As a result, any user thread running on an AP that encountered a fault halted the core indefinitely.

In this implementation, the Ring 3 check `(frame->cs & 3) == 3` executes unconditionally before AP kernel exception isolation, ensuring that user threads on all cores (BSP and APs) cleanly generate crash dumps and terminate without affecting sibling threads or freezing CPUs.

### 4.2 Safe Memory Range Validation (Zero Secondary Faults)
The crash dumper never dereferences user addresses blindly:
- **Code Window**: Validates `[RIP, RIP + 16)` using `vmm_validate_user_range(pml4, rip, 16, false)`. If unmapped or crossing unmapped pages, prints `<instruction address unmapped or inaccessible>`.
- **Stack Window**: Iterates word-by-word through 16 QWORDs (128 bytes) starting at `RSP`. Each address is validated prior to dereference. If a page boundary is unmapped or the stack is invalid, it prints `<unmapped page boundary reached>` and terminates the stack walk gracefully.

### 4.3 Gate G1 Stack Safety & Zero Heap
- **Zero Heap Allocations**: Pure static/stack formatting; `kmalloc` is strictly forbidden in the fault handler.
- **Fixed-Buffer Formatting**: Max stack frame usage across all dumper functions is $\le 256$ bytes (`crash_dump_user` measured at 256 B, helper routines $\le 112$ B), strictly conforming to the Gate G1 stack ceiling ($\le 1024$ B).
- **Concurrency Serialization**: Multi-core crash dumps are serialized using `SPINLOCK_RANKED(5, "user_crash_dump")` to prevent interleaved serial UART characters when multiple threads fault concurrently.

### 4.4 Dual Output Channels
- **COM1 Serial**: Emitted immediately via `serial_raw_puts` without relying on console mutexes, guaranteeing diagnostic delivery even if console subsystems are degraded.
- **Kernel Log Buffer (`dmesg`)**: Appended to the circular kernel buffer via `dmesg_append_str`, allowing inspection via `/bin/dmesg` or persistence via `/bin/boot-snapshot`.

---

## 5. Verification Matrix

| Suite / Test | Environment | Scope | Result |
|---|---|---|---|
| `make test-crash-dump-host` | Linux host (WSL2), ASan/UBSan | Validates vector-to-signal translation, mnemonic naming, `#PF` error bit decoding, and `RFLAGS` condition decoding under AddressSanitizer and UndefinedBehaviorSanitizer. | **100% PASS** |
| `make test-crash-dump` (BIOS, SMP=1) | QEMU q35, SeaBIOS | Tests `/bin/crash-test` across NULL dereference (`#PF`/`SIGSEGV`), division by zero (`#DE`/`SIGFPE`), illegal opcode (`#UD`/`SIGILL`), custom signal handler recovery, recursive fault handling, and `sudo dmesg` log retention. | **100% PASS** |
| `make test-crash-dump` (BIOS, SMP=4) | QEMU q35, SeaBIOS | Multi-core AP fault isolation, SMP preemption stability, and serialized core dump verification. | **100% PASS** |
| `make test-crash-dump` (UEFI, SMP=1) | QEMU q35, OVMF | UEFI environment verification, page protection fault decoding, exit code 139 verification. | **100% PASS** |
| `make test-crash-dump` (UEFI, SMP=4) | QEMU q35, OVMF | Full multi-firmware SMP=4 concurrency and fault isolation verification. | **100% PASS** |
| `make test-pthread` | QEMU BIOS & UEFI × SMP=1/4 | Regression test ensuring pthreads synchronization, thread lifecycle, and process termination contracts remain fully intact. | **100% PASS (4/4)** |
