# FortressOS - Architectural Review & Technical Debt Tracking

This document records architectural invariants, technical debt analysis, and proactive designs for upcoming milestones based on system reviews.

---

## 1. Physical & Virtual Memory Auditing: Moving Beyond Counter Deltas

### Limitation of Counter-Based Auditing
Currently, Phase 7 and Phase 8 verify resource recovery across loops by comparing counters:
```c
size_t baseline_free_pages = pmm_get_free_pages();
size_t baseline_allocated_tables = vmm_get_allocated_table_frames();
/* ... execute lifecycle operations ... */
assert(pmm_get_free_pages() == baseline_free_pages);
assert(vmm_get_allocated_table_frames() == baseline_allocated_tables);
```
While effective against gross unbalances, counter equality alone cannot detect symmetric leaks where a leaked frame in one subsystem is coincidentally masked by an untracked or premature deallocation in another.

### Golden Snapshot Set Auditing Architecture
To achieve true leak verification:
1. **Steady-State Bitmap Snapshot**: At steady-state idle, snapshot the exact 64 KiB PMM allocation bitmap into a golden buffer.
2. **Page Table Walk Tree Hash**: Compute a 64-bit cryptographic/CRC hash over the Master Kernel PML4's active entries (higher half: entries 256..511).
3. **Exact Set Diffing**: After running 10 cycles of process creation and teardown, diff the PMM allocation bitmap byte-for-byte against the golden snapshot:
   - Any bit flipped from 0 to 1 indicates an unmapped leaked physical frame.
   - Any bit flipped from 1 to 0 indicates an unauthorized free of kernel or bootloader memory.

---

## 2. Per-CPU State & GS-Base Refactoring (The `g_tss_rsp0` Debt)

### Current Uniprocessor State
Presently, privilege transitions (Ring 3 $\to$ Ring 0) and interrupt entries rely on a single global variable `g_tss_rsp0` and a single TSS linked in GDT selector `0x28`:
- `syscall_entry.asm` swaps stacks by loading the single kernel stack from `g_tss_rsp0`.
- In Ring 3 $\to$ Ring 0 interrupts, CPU hardware reads `TSS.RSP0` from the boot TSS descriptor.

### Target Multi-Core Per-CPU Architecture
Before bringing up Application Processors (APs) in SMP, `g_tss_rsp0` must be replaced with the AMD64/x86_64 `SWAPGS` mechanism:

```c
typedef struct cpu_local {
    struct cpu_local *self;          /* %gs:0  - Self pointer */
    uint64_t          kernel_rsp0;   /* %gs:8  - Active kernel stack for syscall entry */
    uint64_t          scratch_rsp;   /* %gs:16 - Temporary scratch during SWAPGS */
    uint32_t          cpu_id;        /* %gs:24 - Logical CPU core ID */
    uint32_t          lapic_id;      /* %gs:28 - Hardware Local APIC ID */
    struct thread    *current_thread;/* %gs:32 - Pointer to active TCB */
    tss_t             tss;           /* Dedicated per-CPU Task State Segment */
} __attribute__((aligned(64))) cpu_local_t;
```

#### Fast Syscall Transition with `SWAPGS`:
```nasm
syscall_entry_stub:
    swapgs                          ; Switch GS base from User to Kernel cpu_local
    mov     [gs:16], rsp            ; Save user RSP into scratch slot
    mov     rsp, [gs:8]             ; Load dedicated per-CPU kernel_rsp0
    push    qword [gs:16]           ; Push user RSP onto kernel stack
    push    r11                     ; Push user RFLAGS
    push    rcx                     ; Push user RIP
    ; ... execute syscall ...
    swapgs                          ; Restore user GS base before sysretq
    sysretq
```

---

## 3. Lock Hierarchy & Runtime Rank Enforcement

### Defined Hierarchy Order
To eliminate deadlocks, locks must strictly be acquired in descending order:
$$\text{L1: } \texttt{g\_sched\_lock} \longrightarrow \text{L2: } \texttt{g\_heap\_lock} \longrightarrow \text{L3: } \texttt{g\_vmm\_lock} \longrightarrow \text{L4: } \texttt{g\_pmm\_lock}$$

### Debug-Mode Runtime Hierarchy Validator
Without runtime checking, inverted lock acquisition (e.g. an interrupt or VMM path requesting dynamic heap allocation while `g_vmm_lock` is held) causes latent deadlocks under load.

#### Proposed Debug Rank Validator:
```c
typedef enum {
    LOCK_RANK_SCHED = 1,
    LOCK_RANK_HEAP  = 2,
    LOCK_RANK_VMM   = 3,
    LOCK_RANK_PMM   = 4,
} lock_rank_t;

typedef struct {
    volatile uint32_t lock;
    uint32_t          rflags;
    lock_rank_t       rank;
    const char       *name;
} ranked_spinlock_t;

/* In debug builds, verify that no lock with rank >= new lock's rank is held on this CPU */
static inline void ranked_lock_acquire(ranked_spinlock_t *l) {
    uint32_t held_mask = get_cpu_held_lock_mask();
    uint32_t invalid_mask = ~((1U << l->rank) - 1);
    if (held_mask & invalid_mask) {
        panic_lock_inversion(l->name, l->rank, held_mask);
    }
    set_cpu_held_lock_bit(l->rank);
    spin_lock_irqsave(&l->lock, &l->rflags);
}
```

---

## 4. SMP-Safe TLB Shootdown for Page Table Teardown

### The Non-Current PML4 Landmine
In uniprocessor execution, `vmm_destroy_pml4()` safely reclaims user page tables by asserting `cr3 != target_pml4`.

In SMP, a multi-threaded process or child address space may simultaneously be active on other CPU cores. Tearing down page tables while another core holds translations in its TLB causes memory corruption and page-fault panics:
- **IPI Shootdown Protocol**: Before freeing intermediate page tables or changing existing page permissions in an active address space, an inter-processor interrupt (IPI) must be dispatched to all cores present in the process CPU affinity mask (`cpumask_t`).
- Cores execute `invlpg` or reload `CR3` before acknowledging completion to the initiating core.

---

## 5. Phase 9C.2 (ext2) Hardening & DoS Discipline

Extending the bounds and $W\oplus X$ discipline developed in ELF loading and GPT parsing to ext2:
1. **Superblock Validation**:
   - `s_blocks_per_group` must be $> 0$ and consistent with block group descriptor count.
   - `s_log_block_size` must strictly resolve to 1024, 2048, or 4096 bytes.
   - Total blocks and total inodes must fit within disk partition capacity.
2. **Indirect Block Cycle & Depth Traversal**:
   - Indirect, bi-indirect, and tri-indirect block traversal must enforce strict cycle detection (preventing malicious circular block references like `i_block[12] -> LBA K -> LBA K`).
   - Indirect block references must strictly lie within partition boundaries.
   - Directory traversal must enforce maximum depth limits (e.g. 32 levels) to prevent stack overflow from circular directory hard links.

---

## 6. GPT Specification Conformance & Verification Status

1. **Permitted Entry Sizes**: Validated against the UEFI specification: explicitly limited to 128, 256, or 512 bytes (rejecting arbitrary intermediate 8-byte multiples).
2. **BIOS Parity Status**: Phase 9C.1 has passed full verification under UEFI (`OVMF_CODE_4M.fd`). BIOS verification (`make run-bios`) is tracked as outstanding until executed.

---

## 7. Extended Roadmap Entries

```
[Phase 9C.3] Minimal PS/2 Keyboard & Blocking Input Queue
    ├── 8042 controller init, scancode set detection, IRQ1 via I/O APIC
    ├── Ring buffer keyqueue with blocking read (wait queue, not busy poll)
    ├── Serial input mirroring (so you can test in QEMU without PS/2)
    └── Acceptance: type "hello" on real laptop, see it echoed in console

[Phase 9C.4] Minimal Line Editor / REPL Primitive (kernel-side)
    ├── Non-canonical line discipline: backspace, left/right, home/end
    ├── No history, no tab-completion (deliberately minimal)
    └── Acceptance: edit a 10-line buffer in Ring 0, echo back

[Phase 9D.5] "flatfs" — Tiny Writable FS for Scratch Storage
    ├── Single-file, append + truncate only, fixed max size (e.g. 64 KiB)
    ├── Lives on its own GPT partition, format tool runs on first write
    └── Acceptance: write "hello\n", reboot laptop, read back "hello\n"
```
