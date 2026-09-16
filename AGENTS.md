# FortressOS - AI Agent & Developer Architecture Guide

Welcome to **FortressOS**, a modern, bare-metal, 64-bit operating system kernel targeting `x86_64` UEFI hardware utilizing the modern **Limine Boot Protocol** (v7/v8 specification).

This document serves as the single source of truth for AI agents (and human systems programmers) interacting with, extending, and maintaining this codebase.

---

## 1. Project Vision & Architecture

### High-Level Architecture
- **Target Architecture:** `x86_64` (AMD64 / Intel 64).
- **Target Platform:** Modern UEFI firmware via Limine bootloader (with fallback support for BIOS).
- **Boot Protocol:** Limine protocol (Base Revision 3 / v8.x).
- **Memory Model:** Higher-half kernel mapped at `0xffffffff80000000`. Limine provides a Higher Half Direct Map (HHDM) allowing direct access to all physical memory offset by `hhdm_request.response->offset`.
- **Toolchain Paradigm:** Strictly freestanding C11 and NASM x86_64 assembly. No C standard runtime (`-nostdlib`, `-ffreestanding`).

### Core Design Principles
1. **Zero Undefined Behavior & Predictable Execution:** Memory structures, page tables, and hardware registers must be explicitly typed, aligned, and bounded.
2. **Strict Freestanding Environment:** Never include hosted libc headers (`<stdio.h>`, `<stdlib.h>`, `<string.h>`). Freestanding types are declared in `types.h` and compiler built-ins.
3. **Headless & Diagnostic First:** Early diagnostics are emitted to 16550 UART COM1 (`0x3F8`) serial output before and during framebuffer setup.
4. **Modularity & Layered Isolation:** Hardware abstractions (UART, GDT, IDT, PMM, VMM, APIC) must reside in isolated drivers/subsystems with explicit public APIs.

---

## 2. Directory Structure

```
FortressOS/
├── .gitignore               # Ignores build outputs, ISOs, and external bootloader binaries
├── AGENTS.md                # System context, coding conventions, and architectural roadmap
├── Makefile                 # Automated compilation, bootloader fetch, ISO packaging, and QEMU run
├── limine.conf              # Limine bootloader configuration menu and kernel path
├── linker.ld                # x86_64 higher-half linker script (4KiB section alignment, Limine markers)
└── src/
    ├── arch/
    │   └── x86_64/
    │       ├── boot.asm         # Early assembly crt0 entry stub, aligns stack, invokes kmain
    │       ├── gdt.h            # GDT, TSS, and segment selector structures
    │       ├── apic.c           # Local APIC and APIC Timer initialization & MMIO access
    │       ├── apic.h           # LAPIC registers, offsets, MSRs, and timer prototypes
    │       ├── context.asm      # Low-level switch_context and thread_trampoline assembly stubs
    │       ├── gdt.c            # GDT setup and TSS IST1 initialization
    │       ├── gdt_flush.asm    # lgdt, segment reloads (CS/DS/SS/ES), and ltr
    │       ├── idt.h            # IDT descriptor, interrupt_frame_t, and IRQ handler registry
    │       ├── idt.c            # IDT table setup, exception diagnostics, and IRQ dispatch
    │       ├── interrupts.asm   # 32 assembly exception stubs and register preservation
    │       ├── msr.h            # MSR read/write inlines, register addresses, and bit flags
    │       └── syscall_entry.asm# Low-level fast syscall entry stub and sysretq dispatcher
    ├── drivers/
    │   ├── acpi.c           # RSDP, RSDT/XSDT validation, and MADT parsing
    │   ├── acpi.h           # ACPI table headers, RSDP, and MADT structure definitions
    │   ├── block.c          # Abstract block device subsystem & device registry
    │   ├── block.h          # block_dev_t descriptor, sector operations, and registration API
    │   ├── ioapic.c         # I/O APIC discovery, MMIO registers, and redirection table masking
    │   ├── ioapic.h         # I/O APIC controller definitions and routing prototypes
    │   ├── nvme.c           # PCIe NVMe storage driver, Admin/IO queues, dynamic doorbells
    │   ├── nvme.h           # NVMe register structures, SQE/CQE, and sector read/write/flush API
    │   ├── pci.c            # PCI configuration access (ECAM MCFG & legacy 0xCF8/0xCFC)
    │   ├── pci.h            # PCI device descriptors, class codes, and configuration prototypes
    │   ├── pic.c            # 8259 PIC masking and disable logic
    │   ├── pic.h            # 8259 PIC port definitions and mask queries
    │   ├── serial.c         # UART 16550 COM1 port I/O driver (115200 8N1)
    │   └── serial.h         # Serial driver headers and port I/O inlines (inb, outb, io_wait)
    ├── fs/
    │   ├── gpt.c            # GPT partition table parser, Protective MBR, and bounded partition devices
    │   ├── gpt.h            # GPT header, partition entry structures, and GUID definitions
    │   ├── tarfs.c          # Read-only USTAR archive parser for initramfs
    │   ├── tarfs.h          # USTAR tar format headers
    │   ├── vfs.c            # Virtual File System tree, lookup, file descriptors, and stat/readdir
    │   └── vfs.h            # VFS node structures, file handle descriptors, and public API
    ├── include/
    │   ├── boot_info.h      # Kernel-owned boot information and memory map snapshot
    │   ├── limine.h         # Official Limine bootloader protocol specification
    │   ├── string.h         # Freestanding memory and string manipulation prototypes
    │   └── types.h          # Standard freestanding primitive types (uint8_t, size_t, bool)
    ├── kernel/
    │   ├── boot_info.c      # Boot metadata deep-copying and verification
    │   ├── elf.c            # Strict ELF64 executable validation, mapping, and loading
    │   ├── elf.h            # ELF64 header, program header, limits, and loader API
    │   ├── embedded_init.asm# Embedded user init ELF binary blob via incbin
    │   ├── main.c           # Kernel entry point (kmain), validates Limine tags, memory & FB
    │   ├── syscall.c        # System call dispatcher, range validation, and handlers
    │   ├── syscall.h        # System call numbers, ABI register mappings, and error codes
    │   ├── thread.c         # Cooperative thread scheduler, runqueue, and thread lifecycle
    │   └── thread.h         # TCB structure, thread_state_t, and scheduler prototypes
    ├── lib/
    │   └── string.c         # Freestanding memset, memcpy, memmove, memcmp, strlen
    ├── mm/
    │   ├── heap.c           # Dynamic kernel heap allocator with boundary tags and free list
    │   ├── heap.h           # Heap public prototypes, block structures, and alignment macros
    │   ├── pmm.c            # Physical Memory Manager bitmap frame allocator
    │   ├── pmm.h            # PMM public prototypes, page macros, and metrics
    │   ├── vmm.c            # Virtual Memory Manager 4-level paging and CR3 management
    │   └── vmm.h            # VMM public prototypes, PTE flags, and query APIs
    └── user/
        ├── init.asm         # Standalone ELF64 user init program (Ring 3 execution test)
        └── linker.ld        # User-space linker script with 4 KiB page-separated segments
```

---

## 3. Build, Run, and Debug Instructions

### Prerequisites
- **Toolchain:** `gcc`, `ld` (GNU Binutils), `nasm`, `make`.
- **Packaging:** `xorriso` (for ISO creation), `git` (for fetching Limine bootloader).
- **Virtualization:** `qemu-system-x86_64`, `ovmf` (UEFI firmware).

*On Ubuntu / Debian / WSL2:*
```bash
sudo apt-get update
sudo apt-get install -y build-essential nasm xorriso qemu-system-x86 ovmf git curl
```

### Build Commands
- **Build Kernel & Bootable ISO:**
  ```bash
  make
  ```
  Produces `bin/fortress.elf` and `bin/fortress.iso`. Automatically clones Limine binary dependencies if missing.

- **Clean Build Artifacts:**
  ```bash
  make clean
  ```

- **Full Clean (including downloaded Limine/OVMF):**
  ```bash
  make distclean
  ```

### Run Commands
- **Run in QEMU (UEFI Mode - Default):**
  ```bash
  make run
  ```
  Launches QEMU configured with `-M q35 -m 2G -serial stdio` and OVMF firmware. Early serial output appears directly in the host terminal.

- **Run in QEMU (Legacy BIOS Mode):**
  ```bash
  make run-bios
  ```

### Debugging with GDB
To debug kernel initialization step-by-step:
1. Launch QEMU frozen at startup waiting for a GDB connection:
   ```bash
   make debug
   ```
2. In a separate terminal, launch GDB and connect to QEMU's GDB stub:
   ```bash
   gdb bin/fortress.elf -ex "target remote :1234" -ex "break _start" -ex "continue"
   ```

---

## 4. Coding Standards for AI Agents & Contributors

### Freestanding C Rules
1. **Never `#include` Hosted Headers:**
   - Permitted: Compiler built-in freestanding headers (`<stdint.h>`, `<stddef.h>`, `<stdbool.h>`, `<stdarg.h>`) or project-local headers (`"types.h"`).
   - Prohibited: `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<unistd.h>`, `<sys/...>`.
2. **Compiler Flags Enforcement:**
   Every source file is compiled with strict flags:
   `-ffreestanding -fno-stack-protector -fno-stack-check -fno-lto -fPIE -m64 -march=x86-64 -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone -Wall -Wextra -Werror`
   - `-mno-red-zone`: Mandatory for x86_64 kernels so interrupts do not clobber the 128-byte red zone below `RSP`.
   - `-mno-sse -mno-sse2`: Disables SIMD instructions until the kernel explicitly enables FXSAVE/SSE in CR0/CR4.
3. **Explicit Pointer Arithmetic & Physical/Virtual Conversions:**
   - Physical memory addresses must NEVER be dereferenced directly.
   - When accessing physical memory, translate using Limine's HHDM offset:
     ```c
     void *virt_addr = (void *)((uintptr_t)phys_addr + hhdm_offset);
     ```
   - Always cast pointer arithmetic to `uintptr_t` or `uint8_t *`.
4. **Inline Assembly Conventions:**
   - Use GNU inline assembly with explicit volatile attributes, output/input operands, and `"memory"` clobbers where register state or memory side effects occur.
   - Example:
     ```c
     static inline void outb(uint16_t port, uint8_t val) {
         __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port) : "memory");
     }
     ```
5. **Assembly Stubs (`src/arch/x86_64/`):**
   - Written in NASM syntax (`[bits 64]`, `default rel`).
   - Must adhere to the System V AMD64 ABI:
     - Function arguments passed in: `RDI`, `RSI`, `RDX`, `RCX`, `R8`, `R9`.
     - Callee-preserved registers: `RBX`, `RSP`, `RBP`, `R12`, `R13`, `R14`, `R15`.
     - Stack alignment: `RSP` must be 16-byte aligned before any `call` instruction.
6. **Concurrency & Lock Hierarchy Rules:**
   - **Non-Recursive Spinlocks**: `spinlock_t` uses atomic test-and-set with interrupt flag (`RFLAGS`) preservation (`spin_lock_irqsave` / `spin_unlock_irqrestore`). They are strictly **non-recursive**; acquiring an already-held lock on the same CPU will deadlock. Internal `_unlocked` helpers are used across subsystems to avoid self-recursion.
   - **Strict Hierarchy Order**: Locks must always be acquired in descending order:
     `g_sched_lock` (L1) -> `g_heap_lock` (L2) -> `g_vmm_lock` (L3) -> `g_pmm_lock` (L4).
   - **Context Switch Invariant**: No spinlock may EVER remain held across `switch_context()`. Specifically, `g_sched_lock` is explicitly released with `__atomic_clear(&g_sched_lock.lock, __ATOMIC_RELEASE)` before calling `switch_context()`. However, hardware interrupts MUST remain strictly disabled across `TSS.RSP0`, `CR3`, and the `switch_context` stack exchange until the incoming thread restores its saved `RFLAGS`.
   - **Reaper Invariant & Detached Deallocation**: Complex cross-subsystem cleanup (`sched_reap_dead()`) decouples dead nodes under lock and cleans them up outside the lock. The reaper strictly asserts that the executing context is not the dead thread, does not use the dead thread's stack slot, and does not run under the dead process's CR3. Finding active CR3 matching a dead process indicates a critical scheduler lifecycle bug and causes an immediate diagnostic panic.
7. **Stack Guard Page Architecture & Fault Escalation:**
   - **Linear Growth Scope**: Dedicated thread stacks include a 4 KiB unmapped bottom guard page (`0xFFFFFFFFA0000000ULL`). This catches linear contiguous stack growth. It does not catch frame skips exceeding 4096 bytes without compiler stack-clash probes.
   - **Double Fault (#DF) Escalation**: In Ring 0 with `IST=0`, Vector 14 (`#PF`) delivers on the active stack pointer `RSP`. Pushing the `#PF` exception frame onto an already-exhausted stack causes a nested fault, which hardware escalates to Vector 8 (`#DF`). Because Vector 8 is bound to `IST1`, execution safely lands on the dedicated 16 KiB emergency IST1 stack for diagnostic panic logging. (Note: IST1 provides an emergency recovery stack for diagnostics; it does not guarantee prevention of every triple fault if the IST1 mapping, TSS, IDT, handler code, or diagnostic path is corrupted. Furthermore, once Ring 3 is entered, interrupts and exceptions crossing privilege levels switch to `TSS.RSP0` rather than using the faulting user stack).

---

## 5. Architectural Roadmap for Future Modules

Future tasks should follow this sequenced implementation order:

```
[Phase 1] Serial & Early Logging (COMPLETE)
    │
    ▼
[Phase 2] GDT & IDT (COMPLETE)
    │   ├── 64-bit GDT with Kernel CS (0x08), Kernel DS (0x10), TSS (Selector 0x28)
    │   ├── Dedicated 16 KiB IST1 stack linked to Double Fault (#DF, Vector 8)
    │   ├── IDT with 256 64-bit Interrupt Gates (0x8E) and uniform assembly ISR stubs
    │   └── Rich serial panic dumps (Page Fault CR2 decode, register context)
    │
    ▼
[Phase 3] Physical Memory Manager (PMM) (COMPLETE)
    │   ├── Parse Limine memory map (usable RAM & bootloader reclaimable)
    │   ├── Frame Allocator (compact 64 KiB Bitmap placed via HHDM at 0x100000)
    │   ├── pmm_alloc_page(), pmm_free_page(), pmm_alloc_pages(), pmm_free_pages()
    │   └── Memory statistics and self-tests (distinct pages, contiguous, reclaim)
    │
    ▼
[Phase 3.5] Foundation Hardening & Freestanding Lib (COMPLETE)
    │   ├── Freestanding string.h / string.c (memset, memcpy, memmove, memcmp, strlen)
    │   ├── Complete 256 IDT gate coverage with distinct vector numbers and unexpected IRQ logging
    │   ├── PMM audit (64 KiB bitmap storage at 0x100000 reserved, frame 0 guarded, 2 GiB capacity verified)
    │   ├── Automated Makefile dependency tracking (-MMD -MP) and -g debug symbols
    │   └── Framebuffer 32bpp format verification & bounds clipping in main.c
    │
    ▼
[Phase 4A] Virtual Memory Manager (VMM) & 4-Level Paging (COMPLETE)
    │   ├── x86_64 4-Level Paging (PML4, PDPT, PD, PT) structure management
    │   ├── Ownership Rules: VMM strictly owns page-table frames; callers own mapped physical frames
    │   ├── Mapping & Query API with canonical address validation (vmm_map, vmm_unmap, vmm_is_mapped, vmm_get_physical_address)
    │   ├── Stack Guard Pages: Deterministic unmapped 4 KiB guard pages directly below boot stack and IST1 stack (#PF trap)
    │   ├── Selective HHDM mapping (RAM-only; multi-GiB MMIO holes skipped) & explicit uncacheable Framebuffer MMIO
    │   ├── Parent-table user permission propagation and stack-safe NX execution enforcement testing
    │   ├── Intermediate Table Lifecycle: Kernel VMM retains allocated intermediate tables on unmapping to prevent churn;
    │   │   complete address-space destruction (vmm_destroy_pml4) & refcounted table reclamation are explicitly deferred to Phase 7
    │   └── Switching to independent kernel CR3, TLB invalidation, and NX / RW permission tests
    │
    ▼
[Phase 4B] Kernel Heap Allocator (COMPLETE)
    │   ├── 16-byte aligned boundary tags (header & footer) with O(1) bidirectional coalescing
    │   ├── Embedded doubly linked free list with first-fit search and block splitting
    │   ├── Dynamic virtual memory expansion (512 MiB window at 0xFFFFFFFFB0000000)
    │   ├── Non-contiguous physical frame allocation via PMM with transactional rollback on exhaustion
    │   ├── Full API semantics: kmalloc, kfree, kcalloc (overflow check), krealloc (data preservation)
    │   ├── Diagnostic panic traps on invalid metadata or detected double-free
    │   └── Comprehensive verification suite: alignment, splitting, both-neighbour coalescing, stress test
    │
    ▼
[Phase 5] ACPI Discovery & APIC Timer (COMPLETE)
    │   ├── Limine RSDP query, RSDT/XSDT validation, and MADT parsing (LAPIC, CPUs, I/O APICs, ISOs)
    │   ├── Mask legacy 8259 PIC (0x21=0xFF, 0xA1=0xFF) and setup dedicated APIC spurious interrupt handler
    │   ├── Local APIC (LAPIC) MMIO uncacheable page mapping (PTE_PCD|PTE_PWT|PTE_NX), SVR=0x1FF, and TPR=0
    │   ├── Periodic APIC Timer PIT-assisted calibration (100 Hz), dynamic IRQ dispatch, and EOI verification
    │   └── I/O APIC discovery, GSI validation and initial mask readback; external device delivery remains unverified
    │
    ▼
[Phase 6] Kernel Threads & Scheduling
    │   ├── Checkpoint 1: Cooperative Multitasking (COMPLETE)
    │   │   ├── Thread Control Block (TCB) with offset-0 rsp, tid, state, and 16 KiB stacks
    │   │   ├── Low-level switch_context (System V callee-preserved regs & RFLAGS atomicity)
    │   │   ├── thread_trampoline with register parameter threading (R12=entry, R13=arg)
    │   │   ├── Voluntary yielding (thread_yield), clean exit (thread_exit), and dead thread reaper
    │   │   └── Verification: Two worker threads ping-ponging 10 rounds, clean return to kmain, heap audit
    │   ├── Checkpoint 2: Preemptive Round-Robin Scheduler (COMPLETE)
    │   │   ├── Timer interrupt preemption driven by 100 Hz APIC Timer ticks (20 ms quantum)
    │   │   ├── Scheduler spinlocks with interrupt flags save/restore (spin_lock_irqsave / spin_unlock_irqrestore)
    │   │   ├── Dedicated idle thread (sti; hlt loop) executed when runqueue is empty
    │   │   ├── Single-owner preemptive EOI acknowledgement before switching stacks to prevent APIC priority lockout
    │   │   └── Verification: Two CPU-bound worker threads with zero manual yields advance concurrently across samples
    │   └── Checkpoint 3 / Hardening Review: Safety, Synchronization & Dedicated Stacks (COMPLETE)
    │       ├── EOI Lifecycle: Eliminated global flags; LAPIC EOI acknowledged directly on timer entry
    │       ├── Subsystem Synchronization: spinlock_t with IRQ save/restore guarding Heap, PMM, and VMM
    │       ├── Page-Backed Thread Stacks: Dedicated virtual window (0xFFFFFFFFA0000000) with unmapped 4 KiB guard pages
    │       ├── Lock Hierarchy: Lockless detached reaping avoiding nested scheduler-heap/VMM inversions
    │       └── Lifecycle Stress Test: 36 concurrent threads with dynamic heap alloc/free, stack recycling, and heap audit
    │
    ▼
[Phase 7] User Space & Ring 3 Syscalls (The First Milestone)
        ├── Checkpoint 0: Process Virtual Address Space Lifecycle & Teardown (COMPLETE)
        │   ├── User PML4 creation with lower-half zeroing (0..255) and higher-half kernel mirroring (256..511)
        │   ├── Process isolation: user mappings strictly private and invisible across address spaces & kernel PML4
        │   ├── TSS RSP0 privilege transition hook (gdt_set_tss_rsp0 / gdt_get_tss_rsp0)
        │   ├── Recursive multi-level teardown (vmm_destroy_pml4) with intermediate table & user frame reclamation
        │   ├── Invariant guards: destruction of master kernel PML4 or active CR3 rejected
        │   └── Zero-leak audit: 100% intermediate table & physical frame recovery verified under UEFI & BIOS
        ├── Checkpoint 1: Ring 3 Transition via iretq & Trap Hook (COMPLETE)
        │   ├── User GDT segment validation (Kernel CS 0x08, Kernel DS 0x10, User DS 0x1B, User CS 0x23)
        │   ├── Dedicated 16 KiB kernel TSS.RSP0 stack arming for privilege transitions (Ring 3 -> Ring 0)
        │   ├── IDT Vector 0x80 configured as User Interrupt Gate (0xEE, DPL=3)
        │   ├── Atomic privilege switch via enter_user_mode assembly stub (SS:0x1B, RSP:user_stack, RFLAGS:0x202, CS:0x23, RIP:user_entry)
        │   ├── Test user payload: stack push/pop, 64-bit arithmetic, int 0x80 syscall trap
        │   ├── ABI-compliant trap recovery via test_user_mode_helper and isr_exception_handler redirect
        │   └── Verification: Captured CPL=3 (CS 0x23), RPL=3 (SS 0x1B), verified arithmetic RAX, 100% zero-leak teardown
        ├── Checkpoint 2: First System Call & Bidirectional Execution (COMPLETE)
        │   ├── System call ABI (int 0x80): RAX=nr, RDI=fd/arg1, RSI=buf/arg2, RDX=count/arg3, return in RAX
        │   ├── SYS_WRITE (nr 1) with UART serial driver integration and SYS_EXIT (nr 0)
        │   ├── Strict user buffer validation (vmm_validate_user_range):
        │   │   ├── Pointer wrap-around and canonical lower-half (< 0x0000800000000000) bounds checking
        │   │   ├── 4-level page table walk across all spanned 4 KiB pages verifying PTE_PRESENT and PTE_USER
        │   │   └── Rejection of non-canonical, kernel addresses, unmapped pages, and oversized buffers (> 16 KiB)
        │   ├── True bidirectional execution: syscall handler sets frame->rax and iretq resumes user mode in Ring 3
        │   ├── Preemption isolation: RFLAGS=0x002 (IF=0) during manual address-space test execution
        │   ├── TSS.RSP0 stack restoration invariant: preserved across transitions and restored before teardown
        │   └── Verification suite: 8 distinct Ring 3 test assertions (valid write, cross-page mapped buffer,
        │       cross-page unmapped fault, kernel pointer rejection, oversized buffer, zero-length, invalid fd, clean exit)
        ├── Checkpoint 3: Embedded ELF64 User Executable Loading (COMPLETE)
        │   ├── Strict executable format contract: ET_EXEC only (rejects ET_DYN, PIE, and PT_INTERP)
        │   ├── Strict W^X memory security enforcement: segments with both PF_W and PF_X rejected (ELF_ERR_PERM)
        │   ├── Overflow-safe arithmetic bounds checking on all offsets, file sizes, memory sizes, and virtual ranges
        │   ├── Segment overlap, page-zero (vaddr < PAGE_SIZE), and stack/guard collision prevention
        │   ├── Total mapped pages cap (MAX_ELF_PAGES = 1024) preventing memory exhaustion attacks
        │   ├── Transactional loading & failure rollback: unmapped frames freed before recursive vmm_destroy_pml4()
        │   ├── Standalone user ELF compilation pipeline (user/init.asm, user/linker.ld -> build/init.elf -> embedded_init.o)
        │   ├── User process execution in Ring 3: verified .data initialized value, .bss zeroing & writeability,
        │   │   SYS_WRITE serial output, and clean termination via SYS_EXIT(77)
        │   └── Comprehensive negative validation & 5-cycle repeated load/teardown audit with 0 memory leaks
        ├── Checkpoint 4: General Process Exit & Lifecycle Management (COMPLETE)
        │   ├── Process Spawning (process_spawn): dedicated user PML4 (CR3), page-backed kernel stack, and user stack
        │   ├── User Trampoline (user_process_trampoline): drops to Ring 3 with RFLAGS.IF=1 and zeroed register state
        │   ├── Preemptive Multi-Tasking: timer ticks safely preempt user processes, switching CR3 and TSS.RSP0
        │   ├── General Process Termination (SYS_EXIT / process_exit): records exit status, transitions to TERMINATED,
        │   │   and context switches to another runnable context without returning to dead user code
        │   ├── Safe Deferred Reclamation (Reaper / sched_reap_dead): non-self-destructing cleanup in separate context,
        │   │   switching away from dead CR3, reclaiming intermediate tables, user frames, kernel stack slots, and TCBs
        │   └── Comprehensive Verification: 5-cycle repeated preemptive process spawn/exit stress test with 0 memory leaks
        ├── Acceptance Test: Preemption & Fault Isolation (PASSED IN BIOS & UEFI QEMU)
        │   ├── Concurrent CPU-Bound User Preemption: Direct evidence of timer-driven preemption (5-6 preemptions per worker,
        │   │   11-12 timer ticks consumed, 5 switches between runnable workers) with verified exit codes 77 and 88
        │   ├── Ring 3 Fault Isolation: Deliberate illegal read of supervisor kernel memory (0xFFFFFFFF80000000) caught via #PF
        │   │   (Vector 14), terminated by CPU exception convention (exit code 142 = 128 + Vector 14) without panicking kernel, while healthy peer finished cleanly
        │   ├── Reaper Invariants & Safe Reclamation: Zero-delta resource checks in BIOS and UEFI (0 tables, 0 frames, 0 stack slots leaked)
        │   │   with active CR3/stack collision invariant assertions
        │   └── Bounded circular exit records (MAX_EXIT_RECORDS=64) with FIFO replacement policy
        ├── Checkpoint 5: Fast Syscall Hardening via syscall / sysret (COMPLETE)
        │   ├── Hardware MSR Configuration: IA32_EFER.SCE (bit 0), IA32_STAR (Kernel CS 0x08, User CS 0x23, User SS 0x1B),
        │   │   IA32_LSTAR (syscall_entry_stub), and IA32_SFMASK (masks IF, TF, DF, and arithmetic flags)
        │   ├── Return State Hardening & Canonical Policy: Return RIP and RSP bounds-checked strictly against canonical lower-half limits
        │   │   (PAGE_SIZE <= rip, rsp < 0x0000800000000000ULL) before loading user RSP, mitigating Intel CVE-2012-0217 (#GP in Ring 0).
        │   │   0x0000800000000000ULL is non-canonical and explicitly rejected. Invalid return state is handled on the kernel stack,
        │   │   aborting without ever executing sysretq
        │   ├── RFLAGS Security Sanitization: User flags sanitized before sysretq, stripping IOPL (bits 12-13), NT (bit 14), TF (bit 8),
        │   │   and VM (bit 17), while forcing IF=1 (0x200) and reserved bit 1 = 1 (0x002)
        │   ├── Non-Maskable Interrupt (NMI) IST2 Strategy: Vector 2 configured with dedicated 16 KiB emergency stack + 4 KiB guard page
        │   │   (IST2) in TSS/IDT. Handler is strictly reentrant and lockless, avoiding scheduler and subsystem spinlocks.
        │   │   (Note: Delivery during the 2-instruction entry/exit race window is architecturally configured via IST2 but remains unverified by active NMI injection)
        │   ├── User Stack Invariant: syscall_entry_stub performs zero pushes, calls, or writes on the user stack before switching RSP
        │   ├── Concurrency Contract: g_tss_rsp0 follows scheduled thread; explicitly single-CPU in Phases 1-7, prepared for GS base in SMP
        │   ├── Syscall ABI Specification: RCX and R11 documented as clobbered by hardware; callee-preserved registers honored
        │   ├── Dual-Interface Support: Reference int 0x80 preserved; negative parity verified for EFAULT, EINVAL, EBADF, and ENOSYS
        │   └── Comprehensive Verification Suite:
        │       ├── Mode 4 functional test passed with exit code 99
        │       ├── Hostile return test suite: exact non-canonical boundary (0x0000800000000000), mid non-canonical (0x8000000000000000),
        │       │   canonical kernel space (0xFFFF800000000000 / 0xFFFFFFFF80000000), page-zero (< 0x1000), and RFLAGS sanitization verified
        │       ├── Exact canonical upper boundary (0x00007FFFFFFFFFF8) acceptance verified
        │       ├── Preempted concurrent workers (Modes 5 & 6): 120 fast syscalls executed under 100 Hz timer preemption (3 switches across 6 ticks)
        │       └── 100% zero-leak resource audit under BIOS and UEFI QEMU (0 tables, 0 frames, 0 stack slots)
        │
        ▼
[Phase 8] Virtual File System & Interactive Shell
    ├── Step 8A: Basic Framebuffer Text Console (COMPLETE)
    │   ├── Linear 32bpp framebuffer rendering with 8x16 monochrome bitmap font
    │   ├── Text console primitives: newline (\n), carriage return (\r), backspace (\b), tab (\t), printable ASCII
    │   ├── Software row scrolling with bottom line blanking and cursor boundary clamping
    │   ├── Dual output mirroring: serial_putc mirrors to console_putc if console is initialized
    │   ├── Thread & IRQ-safe synchronization via dedicated spinlock_t g_console_lock (spin_lock_irqsave)
    │   ├── Tokyo Night theme palette (Foreground: 0x00C0CAF5, Background: 0x001A1B26)
    │   └── Comprehensive verification: 160x50 character grid, cursor movements, 55-line scroll test, and banner rendering in BIOS and UEFI
    └── Step 8B: Initramfs, Minimal VFS & File Descriptors (COMPLETE)
        ├── Limine module request for initramfs.tar (USTAR format) with memory reserved by bootloader
        ├── Snapshot module metadata (initramfs_vaddr, initramfs_paddr, initramfs_size) and verify magic/checksum
        ├── Strict read-only USTAR parser rejecting non-USTAR, corrupt checksums, octal overflow, and unsupported types
        ├── VFS node abstraction (vfs_node_t) separated from open file object (file_t) ensuring independent seek offsets
        ├── Per-process file descriptor table (fd_table[32]) with O(1) allocation and automated cleanup on exit (fd_close_all)
        ├── Directory enumeration API (vfs_readdir / sys_readdir) supporting future shell ls
        ├── Hardened system calls: sys_open, sys_close, sys_read, sys_stat, sys_readdir
        │   ├── Strict user destination buffer validation (vmm_validate_user_range with write_req = true)
        │   ├── Proper EOF detection, short reads, zero-length reads, and EBADF / ENOENT / EFAULT returns
        ├── NMI & Panic Reentrancy: Dedicated lockless serial_raw_* path prevents console spinlock deadlocks
        ├── Standard archive contents: /bin/init, /bin/hello, /etc/motd, /docs/readme.txt
        └── Comprehensive verification: VFS hierarchy lookup, directory enumeration, dual open independent offsets,
            Ring 3 acceptance suite (Mode 7, exit code 88), and 100% zero-leak resource audit under BIOS and UEFI QEMU
    │
    ▼
[Phase 9] Storage Track (NVMe & ext2)
    ├── Phase 9A: PCI Discovery & MMIO BAR Decoding (COMPLETE)
    │   ├── PCI configuration access: PCIe ECAM via ACPI MCFG with segment/bus range awareness & legacy 0xCF8/0xCFC fallback
    │   ├── Non-destructive inspection of firmware-assigned BARs (distinguish 32-bit vs 64-bit Memory & I/O spaces)
    │   ├── Hardware enumeration: identify Mass Storage (0x01), Non-Volatile Memory (0x08), NVM Express (0x02)
    │   └── Acceptance Test: Identify QEMU NVMe controller, verify class codes, and decode 64-bit MMIO BAR (PASSED in UEFI & BIOS)
    ├── Phase 9B.1: NVMe Initialization and Reads (COMPLETE)
    │   ├── Single-controller, single-namespace, single I/O queue pair with bounded polling
    │   ├── Contiguous DMA allocation via PMM, PRP entry management, and DMA-buffer lifetime guarantees
    │   ├── Controller capabilities (CAP), Admin queues (ASQ/ACQ), Identify Controller & Namespace geometry
    │   ├── Bounded polling command execution with timeout recovery (never free DMA memory while controller active)
    │   └── Acceptance Test: Identify namespace geometry, read known test sector patterns across LBAs, 70-read wraparound stress test, and out-of-range rejection (PASSED in UEFI & BIOS)
    ├── Phase 9B.2: Writes and Flush (COMPLETE)
    │   ├── NVMe Write (`NVME_NVM_OP_WRITE`) and Flush (`NVME_NVM_OP_FLUSH`) synchronous commands on IOSQ 1
    │   ├── Dynamic doorbell mapping calculation: covers offsets up to `0x1000 + 3 * (4 << CAP.DSTRD) + 4` (Dell Latitude safe)
    │   ├── Queue limit abstraction: checks `(CAP.MQES + 1) >= 32` as controller capacity limit rather than fixed 2048 requirement
    │   ├── Timeout & DMA quarantine safety: controller quiesce on error; permanently quarantines DMA memory if quiesce fails
    │   ├── Raw-sector write isolation: disabled by default on normal boots (`ENABLE_NVME_PERSISTENCE_TEST`) to protect partition tables
    │   ├── Explicit documentation qualifications:
    │   │   - Fixed-port poweroff (`outw(0x604, 0x2000)`) is a QEMU test harness mechanism, not general ACPI shutdown for the Latitude 5590.
    │   │   - A clean QEMU restart demonstrates persistence in that emulator environment, not physical power-loss resilience.
    │   └── Acceptance Test: Write to disposable disk image, flush, 70-write wraparound test, verify neighbours, restart QEMU, verify 100% 512-byte persistence across reboot (PASSED in UEFI & BIOS)
    ├── Phase 9C.1: GUID Partition Table (GPT) & Bounded Block Devices (COMPLETE)
    │   ├── Fixture Separation: isolated raw NVMe persistence disk fixture (`build/nvme_raw.img`) from partitioned GPT fixture (`build/nvme_gpt.img`); gated raw pattern tests via `ENABLE_NVME_RAW_PATTERN_TESTS`
    │   ├── Bounded Partition-Array Validation: strict entry size (128..512 bytes, 8-byte aligned), max entry count (1..128), overflow-safe byte limit (64 KiB), disk capacity fit, and non-overlap against headers and usable space prior to allocation
    │   ├── Exact CRC32 Calculation: computed strictly over `num_partition_entries * sizeof_partition_entry` exact bytes, excluding sector padding
    │   ├── Deterministic Backup Policy: complete 5-case outcome matrix (Valid/Consistent -> Primary; Invalid/Valid -> in-memory read-only fallback; Valid/Invalid -> degraded-mode Primary; Valid/Inconsistent -> reject ambiguity; Both Invalid -> reject disk)
    │   ├── All-or-Nothing Staging & Publication: whole-table bounds and pairwise overlap validation in memory before registering partition block devices; read-only callbacks (`write_sector = NULL`, `flush = NULL`)
    │   ├── Block Device Abstraction: geometry and capacity accessors (`block_get_sector_size`, `block_get_sector_count`, `block_get_capacity_bytes`, `block_read_sector`, `block_unregister_dev`)
    │   ├── Bounded Block Device Adapter: exposes discovered partitions (e.g. `nvme0n1p1`) with sector translation and strict capacity bounds checking
    │   └── Acceptance Test Suite:
    │       - Live NVMe ext2 partition discovery (`GPT_GUID_LINUX_FS`) at LBA 2048..10239 (8192 sectors, 4 MiB)
    │       - Verified relative LBA 0 read, ext2 superblock magic `0xEF53` at LBA 2, and last sector LBA 8191
    │       - Strict rejection of reads at capacity boundary (LBA 8192), out-of-bounds (LBA 99999), arithmetic overflow (`UINT64_MAX`), and write/flush attempts
    │       - Expanded 7-Case Negative Test Suite: N1 (bad primary array fallback to backup in memory), N2 (both invalid rejection), N3 (ambiguity rejection on inconsistent headers), N4 (valid-CRC overlapping partition rejection without publishing), N5 (valid-CRC out-of-range partition rejection without publishing), N6 (oversized entry count rejection), and N7 (isolated parent dispatch: verified rejected reads never reach parent driver)
    │       - Dynamic kernel heap integrity audit verified with 0 memory leaks (PASSED in UEFI & BIOS)
    ├── Phase 9C.2: Read-Only ext2 Filesystem (NEXT)
    │   ├── Superblock (0xEF53), block groups, inode table, directory traversal, and direct/indirect block reading
    │   └── Acceptance Test: Mount at /mnt alongside working root initramfs and read /mnt/hello.txt via VFS
    ├── Steps 8C–8D: Interactive Console & Shell
    │   ├── Step 8C: PS/2 keyboard controller & I/O APIC IRQ1 routing with non-busy blocking read wait queue
    │   └── Step 8D: Ring 3 interactive shell (fortress> prompt, ls, cat, help, and program launching)
    └── Phase 9D: Writable ext2 Filesystem
        ├── Block/inode allocation, directory entry insertion, file creation and writes
        └── Acceptance Test: Create and reopen files after reboot (persistent storage)
```

---

## 6. Limine Protocol Reference Notes

- Modern Limine requests are placed in the `.requests` section between `.requests_start_marker` and `.requests_end_marker`.
- The `linker.ld` must protect these markers with `KEEP(*(.requests_start_marker))` and `KEEP(*(.requests_end_marker))`.
- Base Revision is set to 3 (`LIMINE_BASE_REVISION(3)`). The kernel verifies support at runtime via `LIMINE_BASE_REVISION_SUPPORTED`.
- Always check `request.response != NULL` before accessing fields.
