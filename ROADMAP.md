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

## Phase 9D review follow-up (2026-09-17)

Fixed shutdown synchronization without a writable mount, removed failed-mount
placeholder pointers, and reserved detached mount nodes before dirtying disk.
Truncation now reserves its bitmap scratch space before detachment and checks
all reclamation errors. Inode-initialization flush failure taints immediately,
without rollback writes. Metadata mapping rejection returns EIO; reserved GDT
expansion blocks are excluded from file data. Shutdown synchronization returns
failure on taint or I/O failure and freezes writes after a clean marker.

`wsl -d Ubuntu-24.04 -- make test-ext2` passed all eight sanitizer geometries,
including 14 fresh-image regression scenarios covering mount OOM/write/flush
failure, read-only/no-mount shutdown, allocation ownership, truncation OOM and
reclamation failure, tainted no-I/O behavior, and shutdown freeze/flush failure.
`make test-ext2-write` passed BIOS/UEFI three-boot persistence and both offline
`e2fsck -fn` audits per firmware on disposable clones. These tests do not claim
crash atomicity, torn-sector recovery, or physical writable-disk acceptance.

## Phase 9E Saved File Management & Bug H4 Resolution (2026-09-18)

Implemented directory operations (`mkdir`), file rename/move (`rename`), and deletion
(`unlink`) across VFS and writable ext2, along with user syscalls (`SYS_MKDIR` = 11,
`SYS_UNLINK` = 12, `SYS_RENAME` = 13) and interactive Ring 3 shell commands (`mkdir`, `rm`, `mv`).

- **Directory lifecycle:** Ext2 directory creation allocates dedicated data block and
  initializes standard `.` (self) and `..` (parent) records. Parent `links` count is
  incremented on creation and decremented on removal.
- **Safety checks:** Directory unlinking enforces that directories are empty (only `.` and `..`
  permitted; returns `-VFS_ENOTEMPTY` / `SYSCALL_ENOTEMPTY` otherwise).
- **Directory reparenting:** Cross-directory renames update `..` directory entry in the moved
  directory to point to the new parent, with corresponding link count adjustments.
- **On-disk reclamation:** Unlinked inodes have their data blocks returned to the block bitmap,
  inode marked free in the inode bitmap, block pointers and size cleared, `i_links_count` set
  to 0, and `i_dtime` deletion timestamp recorded.
- **Bug H4 fix:** Corrected Belgian AZERTY layout scancode decoding. Number row scancodes 2..13
  now use `shift ^ s->caps` as Shift-Lock for digits `1234567890`. Shifted lookup takes precedence
  over alphabet table, preventing accented keys (`0x03`, `0x08`, `0x0A`, `0x0B`, `0x28`) from
  falsely generating uppercase letters. Added ISO scancode 86 (`<` / `>`).
- **Verification:** `make test-input` and `make test-console` passed under ASan/UBSan. `make test-ext2`
  passed all 8 geometries. `make test-ext2-write` verified 3-boot persistence across BIOS and
  UEFI with zero `e2fsck -fn` errors. `make test-storage`, `make test-shell`, and `make test-power`


## Phase 9G.1 xHCI Controller & USB Device Enumeration (2026-09-19)

Completed Milestone 9G.1 (xHCI controller initialization, DMA rings, root port discovery/reset, device addressing, descriptor validation, and device configuration) across both QEMU (BIOS and UEFI) and physical bare-metal Dell Latitude 5590 hardware:

- **9G.1a Discovery**: PCI discovery of xHCI controller (`0000:00:14.0`, Intel Sunrise Point-LP `8086:9D2F`, 64-bit non-prefetchable BAR0 at `0xEF330000`).
- **9G.1b Reset & MMIO**: Sized aperture (64 KiB), validated operational registers, verified BIOS-to-OS ownership handoff (extended capability at offset `0x846C`), halted and reset controller (`CNR=0`).
- **9G.1c Rings**: Command Ring and Event Ring with ERST, Link TRB toggle cycle, and synchronous No-Op command verification via Command Completion Events.
- **9G.1d Ports**: Protocol capability mapping (12 USB 2.0 ports, 6 USB 3.0 ports). Root port scan detected 4 connected devices: Port 0x5 (High-Speed), Port 0x7 (Full-Speed), Port 0x9 (High-Speed), Port 0xA (Full-Speed). Bounded port reset and speed negotiation.
- **9G.1e Device Addressing & Configuration**:
  - DCBAA and scratchpad buffers initialized.
  - Multi-port scan loop probes attached USB 2.0 ports and filters non-mass-storage devices.
  - Port 0x5: Internal laptop webcam (`if_cls=0x0E`, USB Video Class) detected, cleanly rejected, and Slot 1 disabled.
  - Port 0x7: Full-speed device cleanly rejected.
  - Port 0x9: Physical Kingston/Phison USB flash drive (`VID=0x13FE`, `PID=0x4200`) detected on Slot 3:
    - Control Transfers on EP0 verified (TRT 16-bit shift fixed).
    - Device Descriptor read: `bMaxPacketSize0=64`, `bcdUSB=0x0200`.
    - Configuration Descriptor parsed (expanded buffer up to 2048 bytes).
    - Interface validated: Class `0x08` (Mass Storage), SubClass `0x06` (SCSI transparent command set), Protocol `0x50` (Bulk-Only Transport).
    - Bulk endpoints identified: Bulk-In EP `0x81` (max packet 512), Bulk-Out EP `0x02` (max packet 512).
    - `SET_CONFIGURATION(1)` command issued and completed successfully.
- **Verification**: `make test-usb-descriptors` passed 100% (8 host ASan/UBSan unit tests + BIOS/UEFI QEMU absent/present matrix). Bare-metal Dell Latitude 5590 boot confirmed working with interactive Ring 3 shell reached.

## Phase 9G.2 Read-Only USB Mass Storage Block Device (2026-09-19)

Completed Milestone 9G.2 (Bulk-Only Transport, SCSI engine, uniform block device registration, and GPT partition discovery) across QEMU (BIOS and UEFI) and physical bare-metal Dell Latitude 5590 hardware:

- **Bulk Transfer Rings**: Configured xHCI transfer rings for Bulk-In (Endpoint ID / DCI 3) and Bulk-Out (DCI 4) via `Configure Endpoint` command (Type 12) with Input Context slot indexing `(dci + 1) * ctx_dwords`.
- **Bulk-Only Transport (BOT)**:
  - 31-byte CBW (`0x43425355` "USBC") submission on Bulk-Out.
  - Data transfer stage on Bulk-In/Bulk-Out with cacheline flushing.
  - 13-byte CSW (`0x53425355` "USBS") reading on Bulk-In with signature, tag matching, and status validation.
- **SCSI Engine**:
  - `INQUIRY` (0x12): Reported Product "USB DISK 2.0".
  - `TEST UNIT READY` (0x00): Automatic `REQUEST SENSE` (0x03) recovery for initial Unit Attention.
  - `READ CAPACITY (10)` (0x25): Dell Kingston USB drive reported 30,320,640 sectors (16 GB / 14.46 GiB), 512 bytes/sector.
  - `READ (10)` (0x28): Verified logical sector reads.
- **Uniform Block Device & GPT Discovery**:
  - Registered block device `sda` via `block_register_usb()`.
  - Sector 0 read verified with Protective MBR signature `0xAA55`.
  - GPT partition table parsed: published `sdap1` (ESP FAT32, 64 MiB) and `sdap2` (Linux FS ext2, 64 MiB).
  - xHCI controller and DMA rings remain active at runtime for block I/O.
- **Verification**: `make test-usb-block` passed 100% (8 host ASan/UBSan unit tests + BIOS/UEFI QEMU absent/present matrix). Bare-metal Dell Latitude 5590 photo confirmed `sda`, `sdap1`, and `sdap2` registration and interactive Ring 3 shell reached.

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
    ├── Phase 9C.4: Ring 3 Shell and Minimal Editor (COMPLETE; Dell interaction confirmed)
    │   ├── /bin/shell from initramfs: help, ls, cat, echo, edit, exit/restart; blocking stdin and user-space line editing
    │   ├── In-memory editor (`edit /path`): static BSS line buffer (64 lines x 128 chars), p/a/i/d/c/stats/help commands
    │   ├── No history, no tab-completion (deliberately minimal)
    │   └── Acceptance: read a file into a Ring 3 editor and modify its in-memory buffer (PASSED in BIOS and UEFI QEMU)
    ├── Phase 9C.5: Power Management & Keyboard Layout Switching (COMPLETE)
    │   ├── ACPI S5 shutdown (FADT PM1a/PM1b_CNT and DSDT _S5 package parsing) & emulator ports
    │   ├── Multi-tier reboot: ACPI reset, 8042 reset pulse, chipset PCI reset (0xCF9), and triple fault
    │   ├── SYS_REBOOT (reboot/shutdown) and SYS_KBD_LAYOUT syscalls
    │   └── Ring 3 shell commands: reboot, shutdown, poweroff, and layout (us/azerty)
    └── Phase 9D: Bounded Writable ext2 Filesystem (COMPLETE)
        ├── Explicit opt-in writable mount (`ext2_mount_rw`), preserving read-only defaults on hardware
        ├── Ordered 3-stage allocation with NVMe flush barriers (bitmap reservation + flush -> zero init + flush -> reference link + flush)
        ├── Ordered 3-stage truncation with pre-validation (bounds/metadata/duplicate check -> inode detachment + flush -> block reclamation + flush)
        ├── Prefix durability on writes: positive byte counts returned only for fully flushed prefixes; flush/I/O failure taints mount (`-EIO`)
        ├── Strict separation of read-only policy (`-EROFS` / `-11`) from tainted failure state (`-EIO` / `-9`), avoiding collision with `SYSCALL_ENOENT` (`-5`)
        ├── Superblock clean/dirty tracking: `s_state = 0` (EXT2_VALID_FS cleared) on RW mount, restored to clean `1` on `ext2_sync_all()` during clean shutdown
        ├── Feature audit: sparse-super group protection (powers of 3, 5, 7), external xattr pre-rejection (`i_file_acl != 0` -> `-EOPNOTSUPP`), `BTREE_DIR` and double/triple indirection rejection
        ├── Directory entry insertion (`vfs_create`), record splitting, and directory block growth
        ├── Resource pre-reservation in VFS: `file_t` descriptor and cached nodes allocated before destructive truncation or disk mutations (`vfs_open_ext`)
        ├── Ring 3 text editor safe saving (`w`), path length bounds (256), and unmodified status on error
        └── Acceptance: `make test-ext2` (host ASan/UBSan across 8 configurations with failure injection) and `make test-ext2-write` (BIOS & UEFI 3-boot persistence, editor create/truncate, and host `e2fsck -fn` with 0 errors)
    │
    ▼
[Phase 9E] Program Execution from Shell, Exit Status & System V AMD64 ABI (COMPLETE)
    ├── Bounded spawn/wait interface (SYS_SPAWN nr 9, SYS_WAIT nr 10)
    ├── Standard System V AMD64 ELF process stack layout (16-byte aligned RSP, argc, argv[0..argc-1], NULL, envp NULL, AT_NULL auxv)
    ├── Argument string storage packed at top of user stack page via kernel HHDM mapping
    ├── Initial user register state: RDI = argc, RSI = argv, RDX = 0
    ├── Kernel boot tests backwards-compatibility: process_spawn_with_arg retains scalar RDI mode selector
    ├── VFS ELF execution: process_spawn_from_vfs loads binaries from TarFS or ext2
    ├── Single-threaded parent wait on g_child_records (up to 64 tracked children)
    ├── Safe deferred reaper reclamation upon wait or parent termination
    ├── Ring 3 shell integration: run /path [args...] tokenizes arbitrary string arguments
    ├── Shell exit status tracking ($? via echo $? and last_status variable)
    ├── Shell command chaining: logical AND (&&) and logical OR (||) execution
    ├── Fault trap isolation: CPU exceptions (128 + vector) caught and reported without crashing shell/kernel
    ├── Serial driver hardening: bounded RX FIFO drain during loopback self-test eliminates UEFI OVMF boot noise
    └── Automated acceptance: make test-shell passing BIOS, UEFI, and UEFI 8 GiB keyboard-only mode with zero-leak resource audit
    │
    ▼
[Phase 9F] Dual-Boot Raw Disk Image Packaging (COMPLETE; USB persistence pending 9G)
    ├── Dual-partition GPT disk image layout (130 MiB total, Protective MBR + Primary & Backup GPT)
    ├── Partition 1: EFI System Partition (FAT32, 64 MiB, LBAs 2048..133119) formatted via mformat/mcopy
    │   ├── Populated with Limine UEFI loaders (BOOTX64.EFI, BOOTIA32.EFI), BIOS code (limine-bios.sys),
    │   └── Kernel binary (fortress.elf), initramfs (initramfs.tar), config (limine.conf), and splash (splash.png)
    ├── Partition 2: Persistent Storage (ext2, 64 MiB, LBAs 133120..264191) formatted via mke2fs (-b 1024)
    │   └── Pre-populated with README.txt and welcome notes; intended USB /mnt mount requires Phase 9G
    ├── Limine BIOS Stage 1 & Stage 2 deployment via limine bios-install (embedded into MBR LBA 0 and GPT partition gap)
    ├── Tooling & Automation: scripts/create_boot_img.py with automatic verification (--verify)
    ├── Makefile Integration: make img, make run-img (UEFI), make run-img-usb (USB storage), make run-img-bios (BIOS)
    └── Recorded image/boot verification (does not establish kernel USB storage access):
        ├── Automated GPT CRC, FAT32 directory structure, and offline e2fsck -fn partition verification (0 errors)
        ├── QEMU UEFI and BIOS boot to interactive shell prompt with 100% test pass
        └── Image can be flashed to USB; physical USB filesystem access and persistence remain unimplemented
    │
    ▼
[Phase 9G] USB Storage & Real /mnt Persistence (COMPLETE)
    ├── 9G.1: xHCI controller init, DMA rings, root port scan, device addressing and descriptors (Dell verified)
    ├── 9G.2: Bulk-Only Transport, SCSI engine, block_dev_t registration, GPT discovery (Dell verified)
    ├── 9G.3: Production /mnt mount via PARTUUID + USB provenance, read-only (Dell verified)
    ├── 9G.4: Writable mount, durability classification, BOT stall recovery, persistence across power cycle (Dell verified on Kingston; strong paths QEMU-verified)
    └── Acceptance: QEMU three-boot persistence + offline e2fsck on disposable images; Dell persistence confirmed on Kingston 13FE:4200
    │
    ▼
[Phase 9G.5] USB Topology Expansion (NEXT)
    ├── 9G.5a: Multiple xHCI controller enumeration (independent per-controller state)
    ├── 9G.5b: SuperSpeed enumeration (USB 3.x port link state, slot/endpoint context layout)
    ├── 9G.5c: Strong durability verification on a USB 3.x device
    ├── 9G.5d: Persistence verification on a second device class
    └── 9G.5e: Hubs (deferred until a hub-attached device requires it)
    │
    ▼
[Following] Accounts/permissions, then installer
```

### Phase 9G handoff and evidence boundary (2026-09-18)

After flashing `fortress.img` with Rufus, the user reported no `/mnt`. Review
identified the missing runtime path: the kernel has no USB controller or
mass-storage driver, and `/mnt` is mounted from `nvme0n1p1` only by the QEMU
storage acceptance suite. The boot image's ext2 filesystem is partition 2.
The `run-img*` targets attach a separate NVMe fixture, so their shell boot and
any fixture `/mnt` do not prove access to the image's USB data partition.
Phase 9F completion covers image packaging, not physical USB persistence.
The README embedded in that partition describes intended behavior, not proof
of implemented USB access.

Antigravity's next implementation is Phase 9G, beginning with enumeration
and read-only USB access. The staged implementation scope, protected contracts
and acceptance checklist are in [AGENTS.md §2](AGENTS.md#phase-9g-implementation-handoff).
Each stage records its actual results here. The original planning update did
not establish implementation or test acceptance; subsequent evidence follows.

**Scope discipline:** Planning estimate: 9G.1 is expected to be the largest single driver effort since NVMe. Commit 9G.1a through 9G.1e as separate changes, each verified in QEMU before merging. If any checkpoint exceeds two focused sessions without a working artifact meeting its required evidence, stop implementation, document the specific blocker and evidence, and reassess scope before proceeding. Do not begin 9G.2 until 9G.1e produces a valid device descriptor and a validated directly attached BOT mass-storage interface on both QEMU and the Dell; awaiting hardware verification is a recorded blocker, not a pass.

**9G stop condition:** Use two weeks of active implementation effort after 9G.1e acceptance as a provisional review budget for 9G.2 and the read-only mount in 9G.3, not a delivery promise. If no read-only `/mnt` mount works on the Dell by that review point, stop and identify whether the bottleneck is xHCI complexity, hardware divergence or the existing storage stack. Record completed artifacts, failed checks and a revised scope/estimate before resuming. Exclude and record time awaiting hardware access separately. USB mounting/persistence may be explicitly deferred to a follow-up phase; the existing boot-image path remains available, and deferred acceptance must remain marked incomplete.

The handoff now defines inter-stage guarantees in AGENTS.md's fourth table
column, a USB 2.0/direct-attachment/boot-time-only scope, exact read-only SCSI
commands, and bounded event-ring polling compatible with ext2's lock contract.
9G.1 is split into PCI-only discovery (9G.1a), MMIO/reset (9G.1b), No-Op command
completion (9G.1c), port inspection (9G.1d), and descriptor enumeration (9G.1e).
The original handoff starts with 9G.1a and requires bounded state-dump diagnostics before
the first transfer. Known hardware unknowns and their measurement stages are
listed in AGENTS.md. These are planned deliverables, not new driver code.

Each 9G.1 checkpoint also names likely failure symptoms and bounded responses.
Diagnostic printing is thread-context only after locks are released; timeout
paths publish a preallocated snapshot and pending flag. Writable selection is
planned as an explicit boot-menu opt-in using `usb_data=PARTUUID=<guid>` plus
`usb_data_mode=rw`, with a read-only default and duplicate-target rejection.
Labels or marker files alone do not authorize writes. Boot-argument support
and the menu entry are implementation work, not existing functionality.

The consolidated "What 9G does NOT do" list in AGENTS.md excludes SuperSpeed,
external hubs, hot-plug/reconnection, UAS, other USB classes, suspend/resume,
multiple LUNs and runtime host-controller reset recovery. Direct-attached
SuperSpeed should be a separate follow-up after 9G; external hubs remain later
work. Initial reset, root-port management and safe failure remain required.

9G.2 owns logical-sector compatibility; 9G.3 owns larger-device GPT policy and
read-only mount selection; 9G.4 owns the writable path and real device flush.
New `test-usb-*` runners must assert that no extra data disk is attached,
allowing only the USB fixture plus firmware code/vars. `test-img-*` boot evidence
must remain separate from storage acceptance.

Completion requires all of the following:

- Read the intended USB partition at `/mnt` with no NVMe fixture attached;
  preserve shell startup when the stick is absent, unsupported or unreadable.
- Test exact-size images and images copied to larger disposable devices,
  including the backup-GPT location mismatch. Preserve physical disk exclusions
  and require explicit selection/opt-in for writable USB mounting.
- On disposable USB images under BIOS and UEFI: create/save and cleanly shut
  down, reboot/read/overwrite and cleanly shut down, then reboot/read again.
  Check the ext2 partition offline with `e2fsck -fn` after clean shutdowns.
- Exercise transfer failures, timeouts, invalid descriptors and bounds, and
  safe handling of device disappearance without releasing DMA still in use.
- Record separate Dell acceptance: selected USB device, `/mnt` file read,
  save, clean shutdown and persisted contents after reboot. Keep the internal
  NVMe outside this test. QEMU results alone cannot close physical acceptance.

**Post-completion note (2026-09-19):** The scope discipline and stop
conditions above were the plan as issued. Phase 9G completed without
triggering either the per-checkpoint review or the two-week stop condition;
each stage landed with its acceptance evidence recorded. Physical writable
persistence was achieved on the Kingston USB 2.0 stick; the strong durability
path remains QEMU-only pending SuperSpeed support (Phase 9G.5). The handoff
language above is preserved as the historical record.

### Phase 9G.1a — PCI-only xHCI discovery (2026-09-18)

Implemented `pci_report_xhci()` using the existing read-only PCI lookup. It
reports the first class/subclass/interface 0x0c/0x03/0x30 match, segment/BDF,
vendor/device ID and firmware-assigned BAR0 base, width and prefetch flag.
Unsupported headers, absent/unassigned BARs, I/O BARs and unsupported memory
BAR types return with a diagnostic. No controller BAR mapping/sizing, command
register writes, firmware handoff, reset, DMA or USB transfers are performed.
BAR aperture and controller accessibility remain unverified for 9G.1b.

The report appears immediately before shell startup through the serial output
path that also mirrors to the framebuffer. Missing NVMe now skips the NVMe
checks instead of halting, allowing PCI-only tests without a storage fixture.
The existing QEMU identity gate and physical NVMe storage exclusion remain.

`wsl -d Ubuntu-24.04 -- make test-usb-discovery` passed all four cases:
BIOS/UEFI, each with xHCI present and absent. All reached the interactive shell
without NVMe or other data disks. The runner validates its final QEMU arguments,
uses ISO boot and disposable paired OVMF vars, bounds waits and terminates QEMU.
Evidence: `build/usb-discovery-{bios,uefi}-{present,absent}.log` and `.stderr`.
This verifies PCI metadata and continuation, not malformed-BAR injection,
USB-device enumeration, USB I/O or physical hardware behavior.

Regression: `wsl -d Ubuntu-24.04 -- make test-shell test-storage` passed
BIOS/UEFI shell and storage checks plus keyboard-only UEFI 8 GiB without COM1.
The kernel compiled with the existing strict warning/freestanding flags.
Final `wsl -d Ubuntu-24.04 -- make test-usb-discovery img` repeated all four
discovery cases successfully and rebuilt `bin/fortress.img`; the image
builder's MBR/GPT/FAT checks and offline ext2 `e2fsck` verification passed.

**Dell photo evidence (2026-09-18):** user-supplied `Photo 1.jpg` shows
`[USB 9G.1a] First xHCI controller: 0000:00:14.0 vendor=0x8086 device=0x9D2F`
and `BAR0=0xEF330000 memory64 prefetch=no`, followed by the discovery-complete
message, `[BOOT] Interactive shell ready.` and the `fortress>` prompt. COM1 RX
is unavailable, so this also establishes visible framebuffer reporting on
the Dell. QEMU storage fixture tests are shown as skipped.

This confirms physical PCI discovery and boot progression to the shell prompt.
The photo does not show a typed command, so post-change keyboard interaction
is not newly verified. BAR extent, controller MMIO, ownership handoff/reset,
USB enumeration and persistence remain unverified. These addresses/IDs are
observations of this Dell, not constants for the driver. Next: 9G.1b MMIO/reset.

**Dell 9G.1b hardware verification (2026-09-18):** user confirmed that Phase 9G.1b
passed on physical Dell Latitude 5590 hardware:
- Controller BAR0 sized and mapped in dedicated UC/NX virtual window.
- Capability offsets validated against the aperture.
- BIOS-to-OS ownership handoff semaphore successfully negotiated; SMIs disabled.
- Host controller halted and reset via HCRST; CNR cleared to 0.
- Reset readback values (USBCMD, USBSTS, PAGESIZE) validated.
- Virtual window safely unmapped, bus mastering left disabled, no firmware DMA leaked.
- Interactive PS/2 keyboard confirmed functional at the `fortress>` shell prompt.

Next checkpoint: 9G.1c command and event rings.

**Dell 9G.1c hardware verification (2026-09-18):** user confirmed that Phase 9G.1c
passed on physical Dell Latitude 5590 hardware:
- Command Ring and Event Ring DMA pages allocated and bound to `CRCR`, `ERSTSZ`, `ERSTBA`, and `ERDP`.
- PCI Bus Mastering enabled dynamically during transfer execution.
- Controller started (`USBCMD.RS = 1`); hardware posted a Port Status Change Event for attached Port 5 (`ctrl=0x8801`).
- Port status change event consumed and acknowledged via `ERDP`.
- No-Op Command TRB (type 23) executed via Doorbell 0; matching Command Completion Event (`XHCI_COMP_SUCCESS`, type 33) received.
- Controller cleanly halted, bus mastering disabled, DMA frames reclaimed safely without leaks.
- Interactive PS/2 shell confirmed functional.

Next checkpoint: 9G.1d root port inspection and reset.

**Dell 9G.1d hardware verification (2026-09-18):** user confirmed that Phase 9G.1d
passed on physical Dell Latitude 5590 hardware:
- Traversed Supported Protocol capabilities; mapped USB 2.0 and USB 3.x ports.
- Scanned 18 root ports (`PORTSC`), detected connected device on Port 5.
- Verified port power and executed bounded port reset on Port 5.
- Verified port enablement (`PED = 1`) and successfully decoded High-Speed (480 Mbps) speed.
- Selected Port 5 for subsequent device addressing; isolated non-target ports.
- Interactive PS/2 shell confirmed functional.

Next checkpoint: 9G.1e device addressing and descriptor parsing.

---

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

Follow-up integration: `wsl -d Ubuntu-24.04 -- make test-storage test-shell
test-power` passed BIOS/UEFI storage and shell checks, keyboard-only UEFI 8 GiB,
and ordinary read-only-boot shutdown/reboot (QEMU exit code 0). The kernel and
ISO also built with the existing strict compiler flags.

### Phase 9D — Bounded Writable ext2 Filesystem Verification (2026-09-17)

Phase 9D delivers bounded write support on the ext2 block layer, enabling file creation,
truncation, block reclamation, and persistence to NVMe storage.

- **Implementation Details**:
  - Direct block and single-indirect block allocation and writes (`vfs_write`).
  - Directory entry insertion (`vfs_create`).
  - File truncation (`vfs_truncate`) with a 3-stage contract: scratch pre-allocation,
    Stage 2 detachment (inode size/pointers zeroed and flushed first), and Stage 3
    block reclamation with per-block error validation and taint marking.
  - Unsupported structures (double/triple indirect blocks, non-regular files) are
    explicitly pre-rejected with `-EFBIG` / `-EOPNOTSUPP`.
  - Atomic mount staging: `/mnt` VFS node is linked to the hierarchy only after the
    dirty marker is persisted and flushed to disk; mount failure unwinds cleanly.
  - Clean shutdown lifecycle: `ext2_sync_all()` returns `bool`. If the filesystem is
    tainted, it refuses to mark the filesystem clean; on success, it sets `s_state = EXT2_VALID_FS`
    and freezes further writes (`fs->read_only = true`).
  - Line editor in `user/shell.c` expanded to 8 KiB with save-protection guards
    (`editor_save_disabled`) against truncated reads.
  - Explicit write opt-in: writes are disabled by default; enabled only when
    `-fw_cfg name=opt/fortress/write_test,string=1` is provided (or `WRITE_TEST=1`).

- **Verification Environment & Evidence**:
  - **Environment**: WSL2 `Ubuntu-24.04` on Windows 11 host (x86_64, Linux 6.6 kernel).
    QEMU `q35`, 2 GiB RAM, PCIe NVMe controller (`serial=fortress0`), GPT with
    a 1024-byte-block ext2 partition. These prior results were supplied by the user;
    they are not new test runs by the implementation agent.
  - **Automated 3-Boot Persistence Suite (`make test-ext2-write`)**:
    - Ran disposable GPT NVMe fixtures across both legacy BIOS and UEFI
      (paired with OVMF 4M firmware).
    - Boot 1: created `/mnt/written.txt` via Ring 3 shell, saved, verified NVMe flush,
      and clean ACPI S5 shutdown (QEMU exit code 0). Offline `e2fsck -fn` passed with 0 errors.
    - Boot 2: verified persisted multi-line content, truncated and overwrote with
      shorter content, clean shutdown. Offline `e2fsck -fn` passed with 0 errors.
    - Boot 3: verified cross-boot persistence of truncated state with zero stale lines.
  - **Interactive Manual Verification**:
    - Booted via `make run-bios WRITE_TEST=1` in WSL Ubuntu-24.04 (from PowerShell).
    - Verified kernel log: `[ext2] Writable mount complete at /mnt`.
    - Created `/mnt/test.txt` via `edit /mnt/test.txt`, entered multi-line text in append mode,
      saved via `w` (`[EDIT] Saved 58 bytes (2 lines) to /mnt/test.txt`), quit with `q`,
      and verified contents via `cat /mnt/test.txt`.
    - Executed clean shutdown via `poweroff`.
    - User reports persistence on reload. The clean-marker/freeze behavior is
      established by code and automated offline checks, not solely by shutdown output.
  - **Host Fault-Injection Matrix (`make test-ext2`)**:
    - 14 deterministic regression scenarios in `tests/ext2_host.c` under ASan/UBSan,
      covering exact write failure counts, OOM allocations, and shutdown freeze invariants.

### Phase 9E — Program Execution from Shell, Exit Status & System V AMD64 ABI (2026-09-18)

Phase 9E adds the ability for the interactive Ring 3 shell to load, execute, pass arbitrary string arguments to, and wait on standalone user ELF binaries from VFS (`/bin/hello`), while isolating CPU faults, tracking exit status, and adhering strictly to the standard System V AMD64 ELF ABI.

- **Implementation Details**:
  - **System Calls**: `SYS_SPAWN` (nr 9: path, argv pointer -> child PID) and `SYS_WAIT` (nr 10: child PID, status pointer -> 0 on success).
  - **Standard System V AMD64 Process Stack**: In `process_setup_user_stack()`, string arguments are packed at the high end of the initial 4 KiB user stack page (`USER_STACK_TOP_VIRT = 0x00007FFFF0001000ULL`) via HHDM virtual translation (`vmm_phys_to_virt(stack_phys)`). Below the strings, the initial pointer table is written: `[RSP] = argc`, `[RSP+8] = argv[0]`, ..., `argv[argc] = NULL`, `envp[0] = NULL`, `AT_NULL` auxiliary vector pair (`0, 0`). `RSP` is strictly 16-byte aligned (`RSP % 16 == 0`).
  - **Register Initialization & ABI**: At process entry (`user_process_trampoline`), `RDI = argc`, `RSI = argv`, `RDX = 0` (standard `rtld` termination handler), with all other GPRs sanitized to zero. Kernel boot tests using `process_spawn_with_arg()` maintain scalar `RDI` mode selection compatibility for `init.asm` test modes 0..7.
  - **Process Waiting & Reclamation**: `process_wait_child()` uses single-threaded parent predicate `child_done` with `sched_wait_until()`, waking via `sched_wake_all()`. Parent reaps dead resources via `sched_reap_dead()`. Up to 64 active child records are tracked in `g_child_records` under the scheduler spinlock.
  - **Fault Isolation**: Processes faulting on CPU exceptions (e.g. #PF vector 14, #GP vector 13) are recorded with status `128 + vector` by the exception handler, reported to the user as `[PROCESS] Faulted (exception vector <N>)` without bringing down the parent shell or kernel.
  - **Shell Argument Parsing & Status Tracking (`$?`)**: User-space shell command parser tokenizes whitespace-delimited arguments (`run /path [args...]`), passing `argv[]` array to `SYS_SPAWN`. Shell tracks `last_status` updated on every command and child termination. `echo $?` expands to decimal exit code.
  - **Command Chaining**: Shell command parser supports conditional chaining: `&&` executes subsequent command only if previous succeeded (`last_status == 0`), while `||` executes only if previous failed (`last_status != 0`).
  - **Driver Hardening**: In `serial_init()`, receiver FIFO is drained inside loopback mode, followed by a bounded poll for data ready. This eliminates false loopback failures and serial silencing caused by UEFI/OVMF firmware debug noise during boot.

- **Verification Environment & Evidence**:
  - **Automated Shell Integration Suite (`make test-shell`)**:
    - `PASS bios`: Tested `/bin/hello` execution with no arguments (`run /bin/hello`), numeric argument (`run /bin/hello 42`), string argument (`run /bin/hello world`), status query (`echo $?` -> `42` / `0`), command chaining (`&&` executed on success, skipped on failure; `||` executed on failure, skipped on success), negative error paths (`/missing`, `/bin`), and zero-leak resource audit (`free_pages` and `g_stack_slots_bitmap` unchanged across child lifecycles). Log: `build/shell-bios.log`.
    - `PASS uefi`: Validated identical command sequences and resource assertions under UEFI with paired OVMF firmware. Log: `build/shell-uefi.log`.
    - `PASS keyboard-only UEFI 8 GiB`: Validated hardware boot path without COM1 UART.
  - **Subsystem Regression Coverage**:
    - `make test-input`: Passed FIFO and scancode decoding.
    - `make test-console`: Passed cached redraw and scrolling checks.
    - `make test-storage`: Passed BIOS and UEFI GPT and ext2 Ring 3 read/audit tests.
    - `make test-nmi`: Passed 40 exact-boundary NMI delivery cycles across all 5 syscall transitions in BIOS and UEFI.
    - `make test-ext2`: Passed host ASan/UBSan matrix with injected failures across 8 configurations.

### Phase 9G.1e — xHCI Device Addressing & Descriptors (2026-09-18)

Phase 9G.1e implements device slot assignment, device addressing, Default Control Pipe (EP0) transfer ring management, USB descriptor querying and parsing, Mass Storage BOT class validation, and device configuration (`SET_CONFIGURATION(1)`).

- **Implementation Details**:
  - `src/drivers/xhci_dev.h`, `src/drivers/xhci_dev.c`:
    - Clean separation of memory: DCBAA table, scratchpad buffer array (up to 128 pages based on `HCSPARAMS2`), Input Context (32-byte or 64-byte based on `HCCPARAMS1.CSZ`), Output Context, EP0 Transfer Ring (256 TRBs with Link TRB), and bounce buffer.
    - Pre-initialization: `CONFIG.MaxSlotsEn` and `DCBAAP` programmed while the controller is halted, following the xHCI specification Section 4.2.
    - Single running pipeline: controller starts once in `xhci_verify_rings()`, executes No-Op at index 0, verifies root ports in `xhci_discover_and_reset_ports()`, and proceeds directly to device enumeration without halting, preserving controller internal cycle states and dequeue indices.
    - Command execution via Doorbell 0: issues `ENABLE_SLOT` (acquires Slot ID), registers Output Context in DCBAA, sets up Input Context (Slot Context + EP0 Context with speed and port routing), and issues `ADDRESS_DEVICE`.
    - EP0 Control Transfers: Setup Stage TRB (IDT=1), Data Stage TRB (pointing to DMA bounce buffer), and Status Stage TRB (IOC=1). Rings Doorbell for Slot ID (Target=1).
    - Descriptor Parsing:
      - Reads initial 8 bytes of Device Descriptor: extracts and validates `bMaxPacketSize0` (8, 16, 32, 64).
      - Issues `EVALUATE_CONTEXT` if the negotiated packet size differs from the initial speed default.
      - Reads full 18-byte Device Descriptor: captures `idVendor`, `idProduct`, `bNumConfigurations`.
      - Reads 9-byte Configuration Descriptor header, validates `wTotalLength` (9..512), and reads full configuration descriptor.
      - Validates Interface: requires class `0x08` (Mass Storage), subclass `0x06` (SCSI transparent command set), protocol `0x50` (Bulk-Only Transport). Rejects other device classes cleanly.
      - Locates Bulk-In and Bulk-Out endpoints, verifies max packet size (e.g. 512 bytes for High-Speed).
      - Issues standard USB request `SET_CONFIGURATION(1)`.
    - Clean teardown and DMA quarantine: all allocated PMM pages are freed upon successful clean shutdown; if any command or transfer fails/times out, frames are quarantined to prevent DMA memory corruption.

- **Verification Evidence**:
  - **Host ASan/UBSan Unit Test Suite (`python3 scripts/test_xhci_dev_host.py`)**:
    - `PASS`: normal enumeration, descriptor reads, BOT class validation, and SET_CONFIGURATION(1).
    - `PASS`: Enable Slot failure handled cleanly.
    - `PASS`: Address Device failure handled cleanly.
    - `PASS`: Bad descriptor header rejected cleanly.
    - `PASS`: Malformed device descriptor rejected cleanly.
    - `PASS`: Non-mass-storage class device rejected cleanly.
    - `PASS`: Device without bulk endpoints rejected cleanly.
    - `PASS`: SET_CONFIGURATION failure handled cleanly.
  - **QEMU Full Matrix Suite (`make test-usb-descriptors`)**:
    - `PASS usb-descriptors-bios-absent`: boots cleanly without xHCI, shell prompt reached, PS/2 echo responsive.
    - `PASS usb-descriptors-bios-present`: QEMU `qemu-xhci` with `usb-storage` attached to USB 2.0 port. Discovers Port 1, High-Speed (480 Mbps), issues Enable Slot (Slot ID 1), Address Device, reads descriptors (`VID=0x46F4 PID=0x0001 EP0_MAX=64 Bulk-In=0x81 Bulk-Out=0x02`), issues `SET_CONFIGURATION(1)`, and boots to interactive shell with PS/2 echo.
    - `PASS usb-descriptors-uefi-absent`: paired OVMF 4M UEFI firmware boots cleanly without xHCI.
    - `PASS usb-descriptors-uefi-present`: full UEFI boot with `qemu-xhci` and `usb-storage` enumeration verified.

### Phase 9G.3 — Production `/mnt` Mount & Command Line Partition Selection (2026-09-19)

Phase 9G.3 delivers production storage initialization independent of QEMU acceptance fixtures, mounting the persistent ext2 data partition (`sdap2`) read-only at `/mnt` using explicit GPT partition GUID (`PARTUUID`) matching and USB device provenance verification.

- **Implementation Details**:
  - **Limine Command Line Capture**:
    - Instantiated `struct limine_kernel_file_request kernel_file_request` under Base Revision 3 protocol.
    - Deep-copied `kernel_file->cmdline` into kernel-owned storage in `boot_info_t` (`boot_info.cmdline`, up to 512 bytes, null-terminated).
  - **Bounded Command Line & GUID Parser (`src/fs/usb_mount.h`, `src/fs/usb_mount.c`)**:
    - `gpt_str_to_guid()`: converts standard 36-char mixed-endian UUID string `XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX` into `gpt_guid_t`, functioning as exact inverse to `gpt_guid_to_str()`.
    - `usb_mount_parse_cmdline()`: parses whitespace-delimited tokens for `usb_data=PARTUUID=<guid>` and `usb_data_mode=ro|rw` (default `ro`). Non-PARTUUID selection (e.g. filesystem labels or raw disk names) is strictly rejected as malformed.
  - **Storage Initialization & Provenance Verification**:
    - Hooked directly into boot sequence in `kmain()` after `xhci_boot_probe()`.
    - Checks controller initialization state (`usb_is_initialized()`); if absent, cleanly logs diagnostic and leaves `/mnt` unmounted.
    - Checks if `/mnt` is already mounted (preventing conflicts with test fixtures).
    - Filters candidates by USB BOT parent device provenance (`part->parent->name == "sda"`), strictly excluding internal NVMe devices (`nvme0n1`).
    - Resolves candidates against target GUID:
      - 0 matches: logs `Partition PARTUUID=... not found on supported USB storage; /mnt left unmounted`.
      - > 1 matches: logs `Ambiguous candidates: multiple partitions matched PARTUUID=...; /mnt left unmounted`.
      - 1 match: verifies GPT policy (rejects invalid/ambiguity, accepts consistent primary, degraded primary, or backup fallback).
    - Enforces read-only policy for Phase 9G.3 (if `mode=rw` is requested, notes that writable persistence is deferred to 9G.4 and proceeds with read-only mount).
    - Reports device provenance: `[USB 9G.3] Selected USB device: sda, partition: sdap2 (PARTUUID=...)`.
    - Mounts ext2 partition read-only via `ext2_mount(&part->block_dev, "/mnt")` and logs `[USB 9G.3] PASS: Mounted sdap2 read-only at /mnt`.
  - **Dual-Boot Image Builder Update (`scripts/create_boot_img.py`)**:
    - Generates and prints `[IMG] Data partition PARTUUID: <UUID>`.
    - Generates image-specific `limine.conf` with:
      - Default entry: `/FortressOS (UEFI x86_64)` with `kernel_cmdline: usb_data=PARTUUID=<UUID> usb_data_mode=ro`.
      - Writable entry: `/FortressOS (Persistent Storage - Writable: PARTUUID=<UUID>)` with `kernel_cmdline: usb_data=PARTUUID=<UUID> usb_data_mode=rw`.
    - Deploys to all 4 standard ESP configuration locations.

- **Verification Evidence**:
  - **Host ASan/UBSan Unit Test Suite (`scripts/test_usb_mount_host.py`)**:
    - `PASS`: `gpt_str_to_guid` and `gpt_guid_to_str` bidirectional roundtrip test.
    - `PASS`: `usb_mount_parse_cmdline` with valid RO/RW, default modes, extra arguments, malformed targets, and invalid syntax.
    - `PASS`: `usb_mount_production_storage` candidate selection: 0 matches, non-USB parent rejection, ambiguous clones rejection, GPT policy rejection, degraded primary acceptance, and RW fallback to RO.
  - **QEMU Full Matrix Integration Suite (`make test-usb-mount`)**:
    - `PASS usb-mount-bios-absent`: boots cleanly without xHCI, shell prompt reached and responsive to keyboard echo.
    - `PASS usb-mount-bios-present`: boots raw disk image `bin/fortress.img` as an emulated USB flash drive under legacy BIOS. Discovers xHCI, addresses BOT device on Slot 1 Port 1, registers `sda`, parses GPT (`sdap1`, `sdap2`), reads Limine cmdline, mounts `sdap2` read-only at `/mnt`, drops to interactive shell, verifies `ls /mnt` lists `README.txt`, and `cat /mnt/README.txt` prints persistent storage banner.
    - `PASS usb-mount-uefi-absent`: paired OVMF 4M UEFI firmware boots cleanly without xHCI, shell prompt reached and responsive.
    - `PASS usb-mount-uefi-present`: boots raw disk image `bin/fortress.img` as an emulated USB flash drive under UEFI firmware. Fully mounts `sdap2` at `/mnt`, and verifies `ls /mnt` and `cat /mnt/README.txt` via QMP.
  - **Bare-Metal Dell Latitude 5590 Hardware Acceptance (2026-09-19)**:
    - Booted from physical USB flash drive flashed with Rufus.
    - Discovered xHCI controller 8086:9D2F, enumerated Kingston/Phison flash drive on Port 9, registered `sda` (30,320,640 sectors).
    - Verified Sector 0 MBR signature `0xAA55`.
    - GPT parsed with both Primary and Backup valid and consistent; published `sdap1` (ESP FAT32) and `sdap2` (ext2 data).
    - Kernel command line read from Limine (`usb_data=PARTUUID=79C710... usb_data_mode=ro`).
    - Matched `sdap2` against target PARTUUID with USB BOT parent provenance (`sda`).
    - Successfully mounted `sdap2` read-only at `/mnt` (`[USB 9G.3] PASS: Mounted sdap2 read-only at /mnt`).
    - Interactive Ring 3 shell prompt reached and responsive. Photographic evidence confirmed.


### Phase 9G.4 — USB Writable Persistence & Durability Classification (2026-09-19)

Phase 9G.4 completes the USB storage stack: `/mnt` mounts read-write on real hardware, files written from the shell persist across a full power cycle, and durability is classified per-device with an explicit disclosure when the device cannot be classified strongly. The work spans three sub-problems — BOT stall recovery, SCSI cache-policy discovery, and mount eligibility — each verified independently before the whole path was exercised end-to-end.

**What was built**

*BOT stall recovery (Commit 1b).*
- `xhci_bot_endpoint_reset()` issues Stop Endpoint, Reset Endpoint, Set TR Dequeue Pointer (with DCS bit set), and CLEAR_FEATURE(ENDPOINT_HALT) in sequence.
- `xhci_bot_transfer()` attempts a single bounded endpoint reset on a stall before latching the device offline.
- `latched_offline` is distinct from `transport_failed`; both prevent further BOT submissions and retain DMA allocations until reboot.

*SCSI cache-policy discovery (Commit 2).*
- `MODE SENSE(6)` and `MODE SENSE(10)` caching page `0x08` support, requesting current values only. `MODE SELECT` is not implemented and device cache settings are not modified.
- Fallback sequence: `MODE SENSE(6)` first, then `MODE SENSE(10)` if the page is not found or the command is rejected. Transport failures stop the probe; command rejections do not.
- `xhci_scsi_probe_cache_policy()` parses the mode header, block descriptor length, page code, and page length independently, validates each against transferred byte count, and reads `WCE` / `RCD` / write-protect. Malformed, truncated, missing, or conflicting reports mean unknown.
- A separate `SYNCHRONIZE CACHE(10)` probe with `IMMED=0` records whether the device supports durable flushing. Command rejection is captured as `command_failed`, not `transport_failed`.

*Durability classification (Commit 3).*
- `xhci_bot_probe_durability()` runs after block registration and before mount. It stores one of: `SYNC_BACKED`, `WRITE_THROUGH`, `ASSUMED_WRITE_THROUGH`, `READ_ONLY`, or `UNKNOWN` (probe not completed).
- Classification rules: explicit `WCE=0` → `WRITE_THROUGH`; working sync → `SYNC_BACKED`; `WCE=1` with failed sync → `READ_ONLY`; no page and no sync with healthy transport → `ASSUMED_WRITE_THROUGH`.
- `xhci_bot_flush_barrier()` is mode-aware: `SYNC_BACKED` requires the command to succeed; `WRITE_THROUGH` succeeds immediately; `ASSUMED_WRITE_THROUGH` attempts sync and succeeds even if the device rejects, failing only on transport loss. Modes latch to `READ_ONLY` on transport failure and are not silently changed.
- The `[USB DURABILITY]` boot dump prints the raw MODE SENSE attempts, WCE/RCD, sync result, classification, and mount eligibility. When classification is `ASSUMED_WRITE_THROUGH`, a three-line disclosure states that the device does not report cache policy, that write-through is assumed matching Linux and Windows, and that power-loss during writes may lose data.

*Mount eligibility & normal sync (Commit 4).*
- `usb_mount_production_storage()` permits RW for `SYNC_BACKED`, `WRITE_THROUGH`, and `ASSUMED_WRITE_THROUGH`; RO otherwise. It still requires explicit `usb_data_mode=rw`, a matching PARTUUID, USB parent provenance, a strictly consistent or degraded-primary GPT policy, and a successful flush preflight.
- `usb_mount_sync()` flushes the mounted block device without marking the ext2 filesystem clean. It is deliberately separate from `ext2_sync_all()` (which sets `EXT2_VALID_FS` and freezes writes for shutdown).

**Verification Evidence**

*Host ASan/UBSan unit suites:*
- `python3 scripts/test_xhci_bot_host.py`: stall recovery paths, MODE SENSE(6/10) parsing matrices (legal short replies, WCE on/off, write-protect, wrong page/subpage, bad lengths, zero sense, conflicting responses), and the full durability policy table.
- `python3 scripts/test_usb_mount_host.py`: mount eligibility across all durability modes, degraded GPT, missing write/flush callbacks, ext2 RW failure fallback, and `usb_mount_sync()` success/failure.

*QEMU three-boot persistence:*
- `make test-usb-persistence` passed BIOS and paired-OVMF UEFI three-boot create/read/overwrite/delete cycles on disposable 130 MiB USB images, with offline `e2fsck -fn` returning zero after every clean shutdown. The runner validates final QEMU argv (only the disposable USB data device, with read-only firmware and disposable vars permitted). These are clean-shutdown tests with substring content assertions; they do not establish physical power-loss resilience.

**Dell 9G.4 hardware verification (2026-09-19):** user confirmed that Phase 9G.4 passed on physical Dell Latitude 5590 hardware with the Kingston USB DISK 2.0 (VID `0x13FE` PID `0x4200`, 30,320,640 sectors, 512 bytes/sector):
- `MODE SENSE(6) page 0x08: not found` and `MODE SENSE(10) page 0x08: not found`.
- `SYNCHRONIZE CACHE test: failed/unsupported`; command-failed CSW, sense `0/0/0`.
- `Classification: ASSUMED_WRITE_THROUGH`, disclosure printed at boot.
- `Mount mode: read-write`; `[USB 9G.4] PASS: Mounted sdap2 read-write at /mnt`.
- A file written via the editor on the Dell survived a full power cycle (power off, stick physically removed, reinserted, rebooted). Photographic evidence recorded.
- `e2fsck -fn /dev/sda2` on the stick from Linux reported 0 errors on two consecutive runs: one with the device mounted (kernel-cached view, warning noted), one after `umount` (raw on-disk view). File and block counts stable (14 files, 2091/65536 blocks).

The SanDisk USB 3.x stick was also tested and correctly identified as a SuperSpeed device on Port 0x12, then skipped by 9G's scope. This is a correct scope exclusion, not a failure. Strong durability paths (`WRITE_THROUGH`, `SYNC_BACKED`) remain QEMU-verified only.

**What 9G.4 does NOT establish**

- **Strong durability on physical hardware.** No tested stick reports a caching page or accepts `SYNCHRONIZE CACHE`. The `WRITE_THROUGH` and `SYNC_BACKED` paths are QEMU-verified only.
- **Physical power-loss tolerance.** The `ASSUMED_WRITE_THROUGH` disclosure states the boundary explicitly: clean shutdown is assumed durable; power-loss during writes may lose data. Abrupt-stop tests are simulated (mock volatile
  cache, injected flush failures), not physical.
- **USB 3.x devices.** A SanDisk USB 3.x stick was tested on the Dell and correctly identified as a SuperSpeed device on Port 0x12, then skipped by 9G's scope. SuperSpeed support is required to reach the BOT layer with this device.

**Why the strong path is unverified on hardware**

The SanDisk was tested specifically to exercise `WRITE_THROUGH` or `SYNC_BACKED`. It did not enumerate as a USB 2.0 mass-storage device because it is USB 3.x, and 9G's scope explicitly excludes SuperSpeed. This makes SuperSpeed support a prerequisite for physical verification of the strong durability paths — not a convenience, a dependency. That observation motivates Phase 9G.5.

### Phase 9G.5 — USB Topology Expansion (IN PROGRESS)

SuperSpeed support, multiple-controller enumeration, and hub support. Sequenced as three sub-projects, each with its own acceptance criteria and its own hardware target.

### Phase 9G.5a — Multiple xHCI Controllers (COMPLETE, 2026-09-19)

FortressOS now enumerates and initializes every xHCI controller the platform exposes, instead of stopping at the first match. On machines with a single controller the behaviour is unchanged; on machines with two, a mass-storage device on either controller is reachable and mountable.

Delivered in three commits:

- **Commit 1 — `xhci_controller_t` struct and array.** The six controller-scoped statics in `xhci.c` (`s_rings_io`, `s_dma`, `s_dev_dma`, `s_bot_rings`, `s_flush_error`, `g_dump_record`) moved into a single `xhci_controller_t` type, held in `s_controllers[XHCI_MAX_CONTROLLERS]`. Only index 0 was used; no PCI
  collection, no loop, no per-controller initialization. External signatures, call graph, and log strings unchanged.

- **Commit 2 — bounded enumeration in PCI discovery.**
  `pci_find_all_devices()` added to `pci.c`, iterating the same topology as the existing `pci_find_device()` but collecting all matches up to a caller-supplied maximum. `pci_report_xhci()` now reports every controller as `xHCI controller N/M: BDF=..., vendor=..., device=...`. Only controller 1 is still initialized. Test runners updated to assert the new log format.

- **Commit 3 — per-controller init loop and active-device selection.**
  `xhci_boot_probe()` now enumerates all controllers and runs the existing init sequence for each via a new private helper `xhci_init_one_controller()`. A new file-scope pointer `s_active_usb_controller` tracks the controller whose mass-storage device is currently registered as `sda`. Per-controller MMIO windows replace the single shared `XHCI_PROBE_VIRT` mapping. A private `xhci_dump_controller_state()` enables per-controller diagnostics without routing through the active pointer. Only the first successfully registered device becomes active; subsequent mass-storage devices are logged as `Mass-storage device found on controller N, but only one active device is supported` and left unregistered.

**Verification:**

- **QEMU:** dual-controller boot with a single stick on the second controller; all five USB suites pass under BIOS and UEFI; three-boot persistence passes with zero filesystem errors.
- **Dell Latitude 5590 (one xHCI controller at `0000:00:14.0`):**
  behaviour identical to commit 2; single `1/1` line; the rest of the boot log unchanged; Kingston mounts RW with `ASSUMED_WRITE_THROUGH`.
- **Dell Latitude 5530 (two xHCI controllers at `0000:00:14.0` and `0000:00:14.2`):** both controllers enumerated and initialized. With the Kingston plugged into a controller-2 port, the device is found on controller 2, registered as `sda`, and mounted read-write at `/mnt`. SuperSpeed device on Port 0x10 correctly skipped as unsupported. USB hub on Port 0x1 correctly rejected as class 0x09. Prior to commit 3, controller 2 was invisible and `/mnt` was never mounted.

**Not established:** the guard path for a second simultaneously attached mass-storage device. The guard logic (`s_active_usb_controller` check before registration) is in the code and structurally verified, but a two-stick boot was not photographed with the guard message visible in the log.

**Deferred:** the "no scrollback on boot" limitation of the framebuffer console means the head of the boot log (the `1/2` and `2/2` lines) is not photographable from hardware without a kernel-log buffer. This is independent of 9G.5a and tracked as a future work item (`dmesg`-style log capture).

**9G.5b — SuperSpeed enumeration. (NEXT)** USB 3.x port link state, SuperSpeed slot and endpoint context layout (`MaxBurstSize`, `MaxPacketSize` 1024 for bulk, no `Evaluate Context` for EP0), and SuperSpeed descriptor handling. Test target: SanDisk enumerates on Port 0x12 as a USB 3.0 BOT device with `speed=SuperSpeed`. Success criterion is the `[USB 9G.1e] PASS: BOT Mass Storage device found on Slot N (Port 0x12)` line on the Dell.

**9G.5c — Strong durability on the SanDisk.** Once the SanDisk enumerates, its MODE SENSE and SYNCHRONIZE CACHE behavior can be read. Success criterion is a classification other than `ASSUMED_WRITE_THROUGH` — either `WRITE_THROUGH` (explicit `WCE=0`) or  SYNC_BACKED` (working flush). If the SanDisk also reports no cache policy, the strong path remains QEMU-only and that is documented as a device-class finding, not a driver defect.

**9G.5d — Persistence on the SanDisk.** Same three-boot test as the Kingston, with `e2fsck` clean after each clean shutdown. This closes the "verified on two independent devices" claim for the persistence path.

**9G.5e — Hubs (deferred).** USB 2.0 and USB 3.x hub support, recursive enumeration, downstream port power sequencing. No hot-plug. Deferred until there is a specific device reachable only through a hub. A USB-C docking hub is available for testing when the sub-project begins.

The consolidated "What 9G.5 does NOT do" list remains as stated in the 9G handoff: SuperSpeed Plus (USB 3.1+ 10 Gbps), UAS, other USB classes, suspend/resume, multiple LUNs, and runtime host-controller reset recovery remain out of scope.