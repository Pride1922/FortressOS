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
    │       ├── gdt.c            # GDT setup and TSS IST1 initialization
    │       ├── gdt_flush.asm    # lgdt, segment reloads (CS/DS/SS/ES), and ltr
    │       ├── idt.h            # IDT descriptor and interrupt_frame_t definitions
    │       ├── idt.c            # IDT table setup and exception panic diagnostics
    │       └── interrupts.asm   # 32 assembly exception stubs and register preservation
    ├── drivers/
    │   ├── serial.c         # UART 16550 COM1 port I/O driver (115200 8N1)
    │   └── serial.h         # Serial driver headers and port I/O inlines (inb, outb, io_wait)
    ├── include/
    │   ├── boot_info.h      # Kernel-owned boot information and memory map snapshot
    │   ├── limine.h         # Official Limine bootloader protocol specification
    │   ├── string.h         # Freestanding memory and string manipulation prototypes
    │   └── types.h          # Standard freestanding primitive types (uint8_t, size_t, bool)
    ├── kernel/
    │   ├── boot_info.c      # Boot metadata deep-copying and verification
    │   └── main.c           # Kernel entry point (kmain), validates Limine tags, memory & FB
    ├── lib/
    │   └── string.c         # Freestanding memset, memcpy, memmove, memcmp, strlen
    └── mm/
        ├── pmm.c            # Physical Memory Manager bitmap frame allocator
        ├── pmm.h            # PMM public prototypes, page macros, and metrics
        ├── vmm.c            # Virtual Memory Manager 4-level paging and CR3 management
        └── vmm.h            # VMM public prototypes, PTE flags, and query APIs
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
    │   ├── Mapping Query API: vmm_get_physical_address(), vmm_is_mapped()
    │   ├── Stack Guard Pages: Unmapped virtual pages directly below stacks to catch overflow via #PF
    │   ├── Higher-half kernel remapping, HHDM remapping, and boot data preservation during CR3 switch
    │   └── Switching to independent kernel CR3, TLB invalidation, and NX / RW permission tests
    │
    ▼
[Phase 4B] Kernel Heap Allocator
    │   ├── Dynamic memory primitives: kmalloc(), kfree(), and krealloc() backed by virtual paging
    │   └── Heap stress tests: repeated allocation/free cycles, coalescing, and reallocation expansion
    │
    ▼
[Phase 5] ACPI Discovery & APIC Timer
    │   ├── Limine RSDP query, RSDT/XSDT validation, and MADT parsing
    │   ├── Mask legacy 8259 PIC and setup dedicated APIC spurious interrupt handler
    │   ├── Local APIC (LAPIC) and I/O APIC setup and MMIO mapping
    │   └── Periodic APIC Timer calibration (verifying continuous ticks and EOI)
    │
    ▼
[Phase 6] Kernel Threads & Scheduling
    │   ├── Thread Control Block (TCB) and assembly context switching
    │   ├── Checkpoint 1: Cooperative multitasking (two kernel threads yielding via thread_yield())
    │   └── Checkpoint 2: Preemptive round-robin scheduler driven by timer, spinlocks & idle thread
    │
    ▼
[Phase 7] User Space & Ring 3 Syscalls (The First Milestone)
        ├── Checkpoint 1: Ring 3 transition via iretq (User CS 0x23, User SS 0x1B, User RSP/RIP)
        ├── Checkpoint 2: First system call (serial print syscall via int 0x80 or syscall)
        ├── Checkpoint 3: Initramfs / embedded ELF user executable loading
        ├── Checkpoint 4: Clean process exit system call
        ├── Checkpoint 5: Fast syscall hardening (syscall / sysret / IA32_EFER / STAR / LSTAR)
        └── Acceptance Test: Hello World in Ring 3 + deliberate illegal access to kernel memory
            (user program faults and terminates cleanly without crashing or panicking the kernel)
```

---

## 6. Limine Protocol Reference Notes

- Modern Limine requests are placed in the `.requests` section between `.requests_start_marker` and `.requests_end_marker`.
- The `linker.ld` must protect these markers with `KEEP(*(.requests_start_marker))` and `KEEP(*(.requests_end_marker))`.
- Base Revision is set to 3 (`LIMINE_BASE_REVISION(3)`). The kernel verifies support at runtime via `LIMINE_BASE_REVISION_SUPPORTED`.
- Always check `request.response != NULL` before accessing fields.
