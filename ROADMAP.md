# FortressOS Roadmap and Checkpoint History

This file preserves the detailed roadmap and verification notes migrated from
AGENTS.md at commit `9a3c4b4`. Historical statements describe their checkpoint;
for example, later phases supersede earlier deferred-work notes. They are not
new instructions or proof that every current revision has passed every test.

For binding implementation rules, task routing, current hardware evidence and
next acceptance targets, read [AGENTS.md](AGENTS.md). For audit limits and
technical debt, read [ARCH_REVIEW.md](ARCH_REVIEW.md). Code and public headers
remain the implementation reference. Some old notes (including the default
keyboard layout) are corrected in AGENTS.md; the record below is preserved.

## Detailed checkpoint roadmap

Recorded implementation sequence and planned work:

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
        │   │   (Verified: make test-nmi injects 20 external NMIs per BIOS/UEFI boot at five exact syscall instruction boundaries; checks IST2, saved RIP/RSP, GPRs, unchanged user stack, IRET and SYSRET.)
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
    │   ├── RAM-cached character/colour cells, changed-cell redraw and batched scrolling (up to 8 rows)
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
    │   ├── Bounded Partition-Array Validation: supported entry sizes (128, 256, or 512 bytes), max entry count (1..128), overflow-safe byte limit (64 KiB), disk capacity fit, and non-overlap against headers and usable space prior to allocation
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
    │       - Dynamic kernel heap integrity audit verified with 0 memory leaks (PASSED in UEFI and BIOS via `make test-storage`)
    ├── Phase 9C.2: Read-Only ext2 Filesystem (COMPLETE)
    │   ├── Superblock (0xEF53), block groups, inode table, directory traversal, and direct/indirect block reading
    │   ├── Boot-time /mnt mount, lazy bounded node cache, direct through triple-indirect lookup, sparse reads
    │   ├── Unsupported feature/geometry rejection, bounded directory records, transactional mount allocation
    │   ├── ASan/UBSan host tests: 1/2/4 KiB blocks, 512/4096-byte sectors, 128/256-byte inodes, corruption and OOM/I/O errors
    │   └── BIOS/UEFI: Ring 3 exact file read/print/close, 10 cycles with exact PMM bitmap, mapping fingerprint and heap audits
    ├── Phase 9C.3: Minimal PS/2 Keyboard & Blocking Input Queue (COMPLETE; Dell input confirmed)
    │   ├── Bounded 8042 initialization, set 2 selection/query, translated set 1, IRQ1 via I/O APIC
    │   ├── Ring buffer keyqueue with blocking read (wait queue, not busy poll)
    │   ├── COM1 RX IRQ4 feeds the same 256-byte FIFO; bounded ISR drains, drop-new overflow
    │   └── Acceptance: QEMU IRQ1/IRQ4 in BIOS/UEFI; Dell PS/2 typing, help, ls and cat confirmed
    ├── Phase 9C.4: Ring 3 Shell (COMPLETE; Dell interaction confirmed); Minimal Editor (PENDING)
    │   ├── /bin/shell from initramfs: help, ls, cat, echo, exit/restart; blocking stdin and user-space line editing
    │   ├── No history, no tab-completion (deliberately minimal)
    │   └── Acceptance: read a file into a Ring 3 editor and modify its in-memory buffer
    ├── Phase 9C.5: Power Management & Keyboard Layout Switching (COMPLETE)
    │   ├── ACPI S5 shutdown (FADT PM1a/PM1b_CNT and DSDT _S5 package parsing) & emulator ports
    │   ├── Multi-tier reboot: ACPI reset, 8042 reset pulse, chipset PCI reset (0xCF9), and triple fault
    │   ├── SYS_REBOOT (reboot/shutdown) and SYS_KBD_LAYOUT syscalls
    │   └── Ring 3 shell commands: reboot, shutdown, poweroff, and layout (us/azerty)
    └── Phase 9D: Writable ext2 Filesystem
        ├── Block/inode allocation, directory entry insertion, file creation and writes
        └── Acceptance Test: Create and reopen files after reboot (persistent storage)
```

---


## Archived implementation and verification notes

## Architectural audit and ext2 implementation scope

See `ARCH_REVIEW.md` for implemented checks, supported ext2 format and deferred work.
`make test-ext2` runs the actual ext2/VFS sources under host ASan/UBSan;
`make test-storage` verifies the complete boot suite in BIOS and UEFI, saving logs
in `build/storage-bios.log` and `build/storage-uefi.log`. QEMU uses snapshot disk
writes; raw-sector pattern tests remain disabled by default.

Lock ranks increase on acquisition: scheduler/ext2 (1, mutually exclusive),
heap (2), VMM (3), PMM (4), console (5). Tracking uses bootstrap-CPU storage,
with IRQs disabled before inspecting it. Release must be LIFO. Saved 64-bit
RFLAGS belongs to each caller. Diagnostics use raw UART. This is not SMP-ready.

### NMI transition and physical-boot diagnostics

- MADT type 4 NMI records are validated and applied to the bootstrap CPU's
  LAPIC LINT pins, with processor-ID matching and conflict detection. Undeclared
  pins stay masked. x2APIC/type-10 NMI routing is not implemented.
- `make test-nmi` uses QEMU TCG QMP injection plus hardware GDB breakpoints at
  zero-byte assembly labels. It never patches code, synthesizes INT 2, or widens
  the transition windows. It verifies 5 boundaries x 4 rounds x 2 firmware modes,
  then requires the complete boot suite to finish. Evidence: `build/nmi-*.json`
  and `build/nmi-*.log`. This covers QEMU delivery, not physical NMI injection,
  nested fault/NMI scenarios or SMP.
- Framebuffer logging begins before PMM/GDT tests. COM1 loopback failure disables
  UART output; transmitter waits are bounded so absent hardware cannot hang boot.
- The current PMM explicitly manages RAM below 2 GiB, reserving higher RAM until
  allocator/audit capacity is expanded. Its bitmap is selected within managed RAM.
- `make test-boot-diagnostics`: UEFI, 8 GiB, no COM1, no NVMe fixture; verifies
  progress to PCI discovery and captures `build/boot-8g-no-uart.png`.
- Storage fixture assertions run only against QEMU NVMe vendor/device IDs.
  Both hardware and fixture paths launch /bin/shell after diagnostics. The physical
  NVMe is still not mounted; /mnt is only available on the QEMU ext2 fixture.

### Boot-console scrolling

The console caches character/colour cells in static RAM (512 x 256 cells,
1.5 MiB; viewport capped to this grid). Scrolling never reads framebuffer MMIO.
Only changed cells are rendered, and each scroll advances min(8, max(1, rows/4))
rows so several subsequent log lines need no screen movement. Output remains
synchronous and immediately visible, including before PMM/heap initialization.
Wrapping is deferred until the next printable character; an explicit newline
following a full-width line advances exactly once.

`make test-console` checks pixel output, colour preservation, scroll batching,
zero redraws for blank lines, control characters, one-cell screens and padded
framebuffer bounds under ASan/UBSan. BIOS/UEFI full boot suites also pass.

### Interactive input and shell

`make` includes a separate freestanding C ELF `/bin/shell` in initramfs. Boot
launches it as a normal Ring 3 process after the acceptance suite. Try:

```
help
ls /
ls /bin
cat /etc/motd
cat /docs/readme.txt
echo hello
```

`exit` terminates/reaps the process and launches a fresh shell. Files are read-only;
no editor, command execution/exec, disk installation, accounts or USB HID driver
is provided by this step. Keyboard layout is Belgian AZERTY (Shift/Caps Lock,
Backspace, Enter; arrows and function keys ignored, Caps LED not synchronized).
Line length is bounded to 191 bytes; overflow discards the entire command.
The console supports erasing across wrapped rows. Serial CR/LF and DEL are
normalized; echo and line editing occur in user space, not interrupt handlers.

`SYS_READ(0, buffer, count)` checks all user pages for write permission before
waiting. It returns available bytes as a short read, without waiting for newline.
A scheduler predicate and BLOCKED-list insertion occur under the scheduler lock
with IRQs disabled. Producers publish input before waking readers; a resumed
reader rechecks availability. IRQ exclusion also spans predicate-to-dequeue.
No lock crosses a context switch, no ISR allocates/logs/switches, and the IDT
dispatcher owns each keyboard/UART EOI. Blocked tasks remain visible to process
liveness/wait APIs. This input/scheduler contract is bootstrap-CPU-only; SMP and
concurrent address-space mutation need further synchronization.

Verification:
- `make test-input`: actual decoder/FIFO under ASan/UBSan; modifiers, Pause,
  PrintScreen, extended keys, wraparound and overflow.
- `make test-shell`: BIOS/UEFI PS/2 events and serial RX through real emulated
  devices, stdin pointer checks, Backspace/Shift, file commands and error paths,
  sleeping task/timer progress, descriptors closed, three process restarts with
  stable physical free-page and stack-slot counts. Logs: `build/shell-*.log`.
- The same target tests UEFI 8 GiB with COM1 absent and non-fixture NVMe identity,
  confirming framebuffer `echo hello` and sleeping input on the hardware boot
  path. Screenshot: `build/shell-keyboard-only.png`. Physical Dell interaction
  was subsequently confirmed by the user photo described below.
- Existing BIOS/UEFI storage acceptance and 40 exact-boundary NMI tests pass.

References for the driver/test protocol: Intel EC firmware 8042 documentation
(https://intel.github.io/ecfw-zephyr/reference/kbchost/index.html), QEMU PS/2
implementation (https://github.com/qemu/qemu/blob/master/hw/input/ps2.c), and
QMP input-send-event (https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html).

### Dell Latitude 5590 physical acceptance (2026-09-16)

User-supplied boot photos confirm the shell on a Latitude 5590 (Core i5-8350U,
32 GiB installed RAM, 256 GB NVMe, Intel UHD 620), booted from a Rufus-written
USB. The latest photo shows PS/2 set 2 -> set 1 / IRQ1 ready, COM1 RX unavailable,
and the Ring 3 shell responding to keyboard input. `help` prints the command
list, `ls` lists `docs/`, `etc/`, `bin/`, and `cat etc/motd` prints the welcome
file and returns to the prompt. `cat motd` correctly reports a missing file.
These are manual observations, supplementing the automated QEMU tests.

The welcome file is from the boot initramfs. The photo explicitly reports that
QEMU storage fixture tests were skipped: physical NVMe filesystem mounting,
reads/writes and persistence remain unverified. This photo does not verify
physical NMI injection, every key/modifier, or blocked-reader resource counters.
The PMM still manages only the low 2 GiB despite 32 GiB being installed.
