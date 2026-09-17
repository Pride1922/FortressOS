# FortressOS - AI Agent & Developer Architecture Guide

## 1. Read This First

Before editing, read [§4 invariants](#4-coding-standards-and-invariants), the
subsystem's public header, and the matching [§7 recipe](#7-how-to-add).
Read [§9 protected contracts](#9-do-not-touch-without-discussion) before changing
boot, synchronization, address-space ownership or test isolation.

| Change touches | Read next |
| --- | --- |
| Locking, scheduling, blocking | L1–L4, S3; [spinlock.h](src/include/spinlock.h), [thread.h](src/kernel/thread.h) |
| Syscalls or user pointers | S1–S4; [syscall.h](src/kernel/syscall.h), [vmm.h](src/mm/vmm.h); §7.1 |
| Interrupts or input | I1–I3; [idt.h](src/arch/x86_64/idt.h), [ioapic.h](src/drivers/ioapic.h); §7.2 |
| Storage or VFS | M1–M4; [block.h](src/drivers/block.h), [vfs.h](src/fs/vfs.h), [ext2.h](src/fs/ext2.h); §7.3–7.4 |
| Tests or user programs | [Makefile](Makefile), §3 and §7.5–7.6 |
| Hardware assumptions or workarounds | [§8 evidence](#8-hardware-facts-and-verification-boundaries), then the driver |

Use `rg` / `rg --files` to locate implementations and callers. Trace indirect
calls too; a textual search alone does not establish locking or IRQ safety.
This guide states contracts; headers/code define implemented APIs. If they
conflict, identify the discrepancy before changing behavior or claiming support.

## 2. Project Overview and Current Work

FortressOS is a freestanding C11/NASM x86_64 kernel using Limine v8, base revision 3,
with UEFI and BIOS boot. Kernel virtual base: `0xffffffff80000000`; HHDM offset
comes from boot metadata. Hardware subsystems have explicit, separate APIs.
Early COM1 and framebuffer diagnostics must work before the heap is available.

History lives in [ROADMAP.md](ROADMAP.md); qualifications and technical debt in
[ARCH_REVIEW.md](ARCH_REVIEW.md). Keep new implementation instructions here,
checkpoint history there, and verification claims tied to actual evidence.

| Checkpoint | Status / acceptance |
| --- | --- |
| Latest recorded completion: 9D bounded writable ext2 | Explicit opt-in writable mount (`ext2_mount_rw`), direct block and single-indirect allocation and writes, directory entry insertion (`vfs_create`), truncation (`vfs_truncate`) with block reclamation, emergency read-only remount on metadata/flush failure, double/triple indirect pre-rejection, host ASan/UBSan matrix suite with injected failure coverage, offline `e2fsck -fn` verification (0 errors), and BIOS/UEFI 3-boot persistence in QEMU. |
| 9C.5 power & layout | Power/reset and US/AZERTY switching implemented (`9a3c4b4`); `make test-power` exercises QEMU power commands. Dell 5590 manual verification confirmed working ACPI S5 shutdown, reboot, and partial Belgian AZERTY layout switching (accented keys é/è/ç/à open bug; see H4). |
| 9C.3 / 9C.4 shell & editor | Blocking keyboard/serial input, Ring 3 shell, and in-memory editor complete. |
| Next: accounts and installation | Define identity/permission enforcement and installer target selection; accept with persistent account setup and a launched application. Not implemented yet. |

## 3. Build, Run, Debug and Verify

Run Linux tools in WSL `Ubuntu-24.04` at `/mnt/c/Sources/FortressOS` (or a Linux checkout).
Prerequisites: GCC/binutils, NASM, make, xorriso, git, QEMU x86, OVMF,
Python 3, e2fsprogs; GDB for interactive debugging. No hosted runtime in the OS.

```bash
sudo apt-get install -y build-essential nasm xorriso qemu-system-x86 ovmf git curl e2fsprogs python3 gdb
make                         # bin/fortress.elf, bin/initramfs.tar, bin/fortress.iso
make run                     # QEMU q35, 2 GiB, COM1, paired OVMF when available
make run-bios                # Legacy BIOS
make debug                  # Frozen QEMU, GDB port 1234
gdb bin/fortress.elf -ex "target remote :1234" -ex "break _start" -ex "continue"
```

From PowerShell: `wsl -d Ubuntu-24.04 -- make` (workspace is the current directory).
`make clean` removes build/ISO outputs; `make distclean` also removes downloaded
Limine/OVMF. Use only when needed. `make` fetches missing Limine dependencies.

| Target | Scope / evidence |
| --- | --- |
| `make test-input` | Host ASan/UBSan: decoder, modifiers and bounded FIFO |
| `make test-console` | Host ASan/UBSan: pixel output, wrapping, scrolling and bounds |
| `make test-ext2` | Host ASan/UBSan: actual ext2/VFS, malformed images, I/O/OOM paths |
| `make test-ext2-write` | QEMU ext2 file creation, editor save, host `e2fsck -fn` integrity, and cross-boot persistence on disposable NVMe GPT fixture |
| `make test-storage` | BIOS/UEFI GPT/ext2, Ring 3 reads, allocation-set audits; `build/storage-*.log` |
| `make test-shell` | BIOS/UEFI IRQ1/IRQ4 interaction, sleeping readers, restart counts; also UEFI 8 GiB without COM1 |
| `make test-nmi` | 5 exact syscall boundaries × 4 rounds × 2 firmware modes; `build/nmi-*.json` and `.log` |
| `make test-boot-diagnostics` | UEFI 8 GiB, no COM1; progress to PCI discovery and framebuffer capture |
| `make test-power` | QEMU shutdown/reboot command tests; physical ACPI S5 confirmed separately on Dell 5590 (see §8 H7), not by this target |

Choose tests relevant to the change, then required integration coverage. Report
commands actually run and their limits; an existing test target is not a new pass.
QEMU storage tests use disposable fixtures/snapshots; never point raw-write tests
at a real disk. `build/nvme_raw.img` and `build/nvme_gpt.img` serve different tests.

## 4. Coding Standards and Invariants

### Freestanding and ABI rules

- Kernel/user code: project headers or compiler freestanding headers only;
  no hosted `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<unistd.h>` or `<sys/...>`.
  Host tools/tests are separate and may use their host runtime.
- Keep Makefile strict warnings (`-Wall -Wextra -Werror`), freestanding flags,
  `-mno-red-zone`, and disabled x87/MMX/SSE. SIMD state management is not implemented.
  Kernel uses `-fPIE`; the standalone C shell overrides with `-fno-pie`.
- NASM: `[bits 64]`, `default rel`. SysV C arguments: RDI, RSI, RDX, RCX, R8, R9;
  preserve RBX/RBP/R12–R15/RSP; align RSP to 16 bytes **before** every call.
- Inline assembly needs correct operands/clobbers and `volatile` for hardware
  effects; include `"memory"` when required. Use explicit fixed-width types,
  alignment and overflow-safe bounds before dereferencing or computing offsets.

### Locking and lifecycle

| ID | Binding invariant | How to check |
| --- | --- | --- |
| L1 | Acquire increasing ranks: sched **or** ext2 (1) → heap (2) → VMM (3) → PMM (4) → console (5). Sched/ext2 cannot nest. Release LIFO. | Trace nested calls from `spin_lock_irqsave`; verify `SPINLOCK_RANKED` values and `spin_debug_selftest`. Violations panic/deadlock, not warnings. |
| L2 | No spinlock across `switch_context`. Keep IRQs disabled through target TSS.RSP0, CR3 and stack exchange until saved incoming flags restore. | Check every switch site for `spin_unlock_noirq` and `spin_debug_assert_unheld`; exercise preemption and sleeping reads. |
| L3 | Locks are non-recursive; `_unlocked` helpers avoid reacquisition. Saved 64-bit RFLAGS belongs to the caller. Tracking is bootstrap-CPU-only. | Inspect public-to-public calls, error exits and saved flags; never share an IRQ-save token. |
| L4 | Detach dead tasks under sched lock; free outside it, on another stack and CR3. | Inspect `sched_reap_dead` assertions and all unwind paths; run lifecycle/reclamation tests. |

### Syscalls and user memory

| ID | Binding invariant | How to check |
| --- | --- | --- |
| S1 | ABI: RAX number/result; RDI/RSI/RDX/R10/R8/R9 arguments. Fast entry clobbers RCX/R11. Normal dispatch writes `frame->rax`; return uses the existing stub. | Compare `syscall.h`, dispatch and Ring 3 callers; preserve assembly frame layout. |
| S2 | Validate every user range via `vmm_validate_user_range` before access; kernel-written buffers require `write_req=true`. Bound sizes and strings. | Every syscall case that dereferences a user pointer calls `vmm_validate_user_range` first. Grep for `frame->rdi`/`rsi`/`rdx` and confirm each is preceded by a validate call. Trace every pointer, page crossing and arithmetic operation; test unmapped, read-only, kernel, zero-length and overflow cases. |
| S3 | Fast entry masks IF and switches from user RSP before any stack access. | Preserved in `syscall_entry.asm` (SWAPGS, RSP switch, IF masked by SFMASK). |
| S3a | Blocking stdin does NOT enable IF before sleeping. Predicate check, BLOCKED insertion and dequeue are IRQ-excluded. | Follow `input_read` → `sched_wait_until`; verify IRQ state on resume and a blocked reader with advancing timer. |
| S3b | Sleep releases all locks before switching. Wake rechecks predicate. | Check every switch site for `spin_unlock_noirq` and verify wait predicate is checked in loop. |
| S3c | Do not invent a `wait_queue_sleep` API — see `input_read` / `sched_wait_until`. | Use existing `sched_wait_until` / `sched_wake_all` primitives; do not add generic ad-hoc blocking helpers. |
| S4 | Validate canonical lower-half RIP/RSP (strictly below `0x0000800000000000`, at least one page); sanitize return RFLAGS before SYSRET. | Keep IOPL/NT/TF/VM stripped and IF/bit 1 forced; run hostile-state cases and `make test-nmi` for entry/exit changes. Canonical does not mean mapped. |

### Interrupts and deferred work

| ID | Binding invariant | How to check |
| --- | --- | --- |
| I1 | Ordinary device IRQ handlers do bounded draining/queue publication/wakeup only: no allocation, blocking, context switch or normal logging. Timer preemption is a deliberate exception. | Every `idt_register_hardware_handler` callback must not call `kmalloc`, `vmm_map`, `sched_wake_all` (except the timer path), or `console_*`/`serial_*`. Grep the handler body. |
| I2 | Exactly one EOI owner. `idt_register_hardware_handler` makes the dispatcher own EOI. Timer uses `idt_register_handler` and issues EOI **before** scheduling. Spurious APIC IRQ gets no EOI. | Every `lapic_eoi()` call site is either inside the IDT dispatcher (via `g_needs_eoi`) or in `apic_timer_handler` before scheduling. No other caller. Check registration and handler together; never convert timer registration blindly. |
| I3 | ISR publication precedes wakeup; processing occurs in a runnable thread. Timer/idle schedules it; no universal deferred-work-at-next-tick API exists. NMI remains lockless/non-scheduling and uses raw UART. | Trace producer/consumer and wake races; inspect NMI transitive calls for subsystem locks or console output. |

### Memory, ownership and storage boundaries

| ID | Binding invariant | How to check |
| --- | --- | --- |
| M1 | Never dereference raw physical addresses. Use runtime HHDM translation for mapped RAM; map MMIO explicitly with the driver's required cache/NX flags. | No `(void *)phys_addr` or `*(phys_addr)` cast appears outside the HHDM translate helper. Grep for casts to `void *` and confirm each is either an HHDM translation (`vmm_phys_to_virt`) or an explicit MMIO map. |
| M2 | HHDM offset is boot-provided, never a constant. Use kernel-owned boot metadata after handoff. | Inspect `boot_info` and `vmm_phys_to_virt`; reject missing Limine responses before reading fields. |
| M3 | VMM owns tables; caller owns data frames. Destroy refuses kernel/active CR3, never frees shared higher half; `free_user_frames=true` requires exclusively owned, singly mapped leaf frames. | Read `vmm.h` ownership/prevalidation contract; check rollback and exact allocation-set/table audits, not just equal counts. |
| M4 | Bound all block/partition/parser arithmetic and hardware waits; publish only fully validated state. Never free DMA memory while a controller may still use it. | Inspect lower-layer dispatch on rejected requests, NVMe quiesce/quarantine, GPT staging, ext2 malformed-input tests and rollback. |

Thread stacks have a 4 KiB lower guard and 16 KiB usable space. A guard catches
contiguous downward exhaustion, not every large frame skip. Ring 0 #PF (IST=0)
uses active RSP; Ring 3 privilege transitions use TSS.RSP0. #DF uses IST1 and NMI
uses IST2. These diagnostic stacks do not guarantee survival if their mappings,
TSS, IDT, handler or diagnostic path is damaged. Keep that qualification.

## 5. File Map

```
FortressOS/
├── .gitignore               # Ignores build outputs, ISOs, and external bootloader binaries
├── AGENTS.md                # Task routing, binding invariants, recipes and evidence
├── ROADMAP.md               # Detailed checkpoint history and archived verification notes
├── ARCH_REVIEW.md           # Architecture audit, limits and technical debt
├── scripts/                 # Host/QEMU verification and disposable disk fixtures
├── tests/                   # Host tests and mocks
├── Makefile                 # Automated compilation, bootloader fetch, ISO packaging, and QEMU run
├── limine.conf              # Limine bootloader configuration menu and kernel path
├── linker.ld                # x86_64 higher-half linker script (4KiB section alignment, Limine markers)
├── src/
│   ├── arch/
│   │   └── x86_64/
│   │       ├── boot.asm         # Early assembly crt0 entry stub, aligns stack, invokes kmain
│   │       ├── gdt.h            # GDT, TSS, and segment selector structures
│   │       ├── apic.c           # Local APIC and APIC Timer initialization & MMIO access
│   │       ├── apic.h           # LAPIC registers, offsets, MSRs, and timer prototypes
│   │       ├── context.asm      # Low-level switch_context and thread_trampoline assembly stubs
│   │       ├── gdt.c            # GDT, TSS RSP0, IST1 (#DF) and IST2 (NMI)
│   │       ├── gdt_flush.asm    # lgdt, segment reloads (CS/DS/SS/ES), and ltr
│   │       ├── idt.h            # IDT descriptor, interrupt_frame_t, and IRQ handler registry
│   │       ├── idt.c            # IDT table setup, exception diagnostics, and IRQ dispatch
│   │       ├── interrupts.asm   # Exception/IRQ stubs and register preservation
│   │       ├── msr.h            # MSR read/write inlines, register addresses, and bit flags
│   │       └── syscall_entry.asm# Low-level fast syscall entry stub and sysretq dispatcher
│   ├── drivers/
│   │   ├── acpi.c           # RSDP, RSDT/XSDT validation, and MADT parsing
│   │   ├── acpi.h           # ACPI table headers, RSDP, and MADT structure definitions
│   │   ├── block.c          # Abstract block device subsystem & device registry
│   │   ├── block.h          # block_dev_t descriptor, sector operations, and registration API
│   │   ├── console.c/.h     # Cached framebuffer text console
│   │   ├── font.h           # Embedded 8x16 font
│   │   ├── input.c/.h       # IRQ input and blocking stdin
│   │   ├── input_buffer.h   # Bounded FIFO
│   │   ├── keyboard.c/.h    # Translated scancodes and layout tables
│   │   ├── power.c/.h       # ACPI shutdown and reset fallbacks
│   │   ├── ioapic.c         # I/O APIC discovery, MMIO registers, and redirection table masking
│   │   ├── ioapic.h         # I/O APIC controller definitions and routing prototypes
│   │   ├── nvme.c           # PCIe NVMe storage driver, Admin/IO queues, dynamic doorbells
│   │   ├── nvme.h           # NVMe register structures, SQE/CQE, and sector read/write/flush API
│   │   ├── pci.c            # PCI configuration access (ECAM MCFG & legacy 0xCF8/0xCFC)
│   │   ├── pci.h            # PCI device descriptors, class codes, and configuration prototypes
│   │   ├── pic.c            # 8259 PIC masking and disable logic
│   │   ├── pic.h            # 8259 PIC port definitions and mask queries
│   │   ├── serial.c         # UART 16550 COM1 port I/O driver (115200 8N1)
│   │   └── serial.h         # Serial driver headers and port I/O inlines (inb, outb, io_wait)
│   ├── fs/
│   │   ├── ext2.c/.h        # Read-only ext2 mount and file operations
│   │   ├── gpt.c            # GPT partition table parser, Protective MBR, and bounded partition devices
│   │   ├── gpt.h            # GPT header, partition entry structures, and GUID definitions
│   │   ├── tarfs.c          # Read-only USTAR archive parser for initramfs
│   │   ├── tarfs.h          # USTAR tar format headers
│   │   ├── vfs.c            # Virtual File System tree, lookup, file descriptors, and stat/readdir
│   │   └── vfs.h            # VFS node structures, file handle descriptors, and public API
│   ├── include/
│   │   ├── boot_info.h      # Kernel-owned boot information and memory map snapshot
│   │   ├── limine.h         # Official Limine bootloader protocol specification
│   │   ├── spinlock.h       # IRQ-save locks and rank contract
│   │   ├── string.h         # Freestanding memory and string manipulation prototypes
│   │   └── types.h          # Standard freestanding primitive types (uint8_t, size_t, bool)
│   ├── kernel/
│   │   ├── boot_info.c      # Boot metadata deep-copying and verification
│   │   ├── elf.c            # Strict ELF64 executable validation, mapping, and loading
│   │   ├── elf.h            # ELF64 header, program header, limits, and loader API
│   │   ├── embedded_init.asm# Embedded user init ELF binary blob via incbin
│   │   ├── main.c           # Kernel entry point (kmain), validates Limine tags, memory & FB
│   │   ├── spinlock.c       # Bootstrap-CPU lock discipline checks
│   │   ├── syscall.c        # System call dispatcher, range validation, and handlers
│   │   ├── syscall.h        # System call numbers, ABI register mappings, and error codes
│   │   ├── thread.c         # Preemptive scheduler, run/wait queues and process lifecycle
│   │   └── thread.h         # TCB structure, thread_state_t, and scheduler prototypes
│   ├── lib/
│   │   ├── crc32.c/.h       # GPT CRC32
│   │   └── string.c         # Freestanding memset, memcpy, memmove, memcmp, strlen
│   └── mm/
│       ├── heap.c           # Dynamic kernel heap allocator with boundary tags and free list
│       ├── heap.h           # Heap public prototypes, block structures, and alignment macros
│       ├── pmm.c            # Physical Memory Manager bitmap frame allocator
│       ├── pmm.h            # PMM public prototypes, page macros, and metrics
│       ├── vmm.c            # Virtual Memory Manager 4-level paging and CR3 management
│       └── vmm.h            # VMM public prototypes, PTE flags, and query APIs
└── user/                    # Standalone programs, outside src/
    ├── init.asm             # Standalone ELF64 user init program (Ring 3 execution test)
    ├── hello.asm            # Standalone hello program
    ├── shell.c              # Interactive Ring 3 shell
    ├── shell_start.asm      # Shell entry and ABI alignment
    ├── shell.ld             # Shell ELF segment layout
    └── linker.ld            # Assembly test programs, page-separated segments
```

## 6. Limine Notes

- Base revision 3; verify `LIMINE_BASE_REVISION_SUPPORTED` and non-NULL responses.
- Keep requests between `.requests_start_marker` / `.requests_end_marker` and
  linker `KEEP` directives. Read [boot_info.h](src/include/boot_info.h) for snapshots.
- Initramfs module backing memory stays reserved (`KERNEL_AND_MODULES`); tarfs
  nodes reference it directly. Copying metadata does not copy module contents.

## 7. How to Add

### 7.1 A syscall

Canonical examples: `SYS_STAT` in [syscall.c](src/kernel/syscall.c); blocking `SYS_READ` in [input.c](src/drivers/input.c).

1. Read `syscall.h`, caller code and S1–S4; choose an unused number and update ABI docs.
2. Add the handler and dispatch case; define argument bounds and negative errors.
3. Validate every user buffer/string before access; request writable pages for outputs.
4. For blocking: follow `input_read`/`sched_wait_until` exactly (see S3/S3a/S3b). Do not paraphrase the contract — read the invariant.
5. Return through dispatch/`frame->rax`; terminal process/power operations use their existing non-returning lifecycle.
6. Add Ring 3 success/error/boundary coverage in a suitable test program; extend the appropriate BIOS/UEFI runner (often `test-shell`).
7. Audit acquired resources on failure/exit; use PMM bitmap/table/mapping and heap checks where ownership changes. Run NMI tests if entry/exit changes.

### 7.2 An IRQ handler

Canonical example: `keyboard_irq` / `serial_irq` in [input.c](src/drivers/input.c); timer is the explicit I1/I2 exception.

1. Read `idt.h`, `ioapic.h`, device source and I1–I3; choose a nonconflicting vector.
2. Initialize bounded device buffers and register the handler before enabling delivery.
3. Use `idt_register_hardware_handler` for ordinary device IRQs; omit EOI in its body. Exception: spurious APIC vector is registered via a path that suppresses EOI (`idt_register_handler` without EOI). If your handler must not acknowledge, use that path and document why.
4. Route ISA through `ioapic_route_isa` to respect MADT overrides; serialize IOAPIC access with IRQs disabled as its header requires.
5. Drain a bounded batch, publish queue/flag state, wake waiters; defer substantial work to a thread. No printing/allocating/switching in the device handler.
6. Test real device delivery, repeated events, overflow and sleep/wake races; verify timer progress and firmware coverage. Direct calls alone do not prove routing.

### 7.3 A block device

Canonical examples: [block.c](src/drivers/block.c), [nvme.c](src/drivers/nvme.c), bounded partition adapter in [gpt.c](src/fs/gpt.c).

1. Read `block.h`, M1/M4 and the driver lifecycle before allocating MMIO/DMA resources.
2. Provide actual `sector_size`/`sector_count`; check multiplication/addition overflow before capacity/range use.
3. Implement bounded sector callbacks and applicable flush; read-only devices leave write/flush NULL.
4. Validate partition-relative bounds before parent dispatch; validate all entries before registry publication.
5. Register only fully initialized devices; unregister and unwind on failure, respecting references and DMA quiescence/quarantine.
6. Test first/last/out-of-range I/O, failure cleanup and registry state using mocks/disposable images; never enable raw patterns on hardware or GPT fixtures.

### 7.4 A VFS node or file operation

Canonical examples: [vfs.c](src/fs/vfs.c), [tarfs.c](src/fs/tarfs.c), [ext2.c](src/fs/ext2.c).

1. Read `vfs.h`/filesystem headers: shared `vfs_node_t` differs from `file_t` with its own open offset/reference count.
2. Define backing-store lifetime, node ownership and supported operations; respect ext2's boot-lifetime read-only mount contract.
3. Bound paths, names, directory records and read sizes; return the existing errors/EOF semantics.
4. Wire callbacks and per-process descriptors without sharing offsets between independent opens.
5. Close/unwind references on failure and exit; keep `fd_close_all`/reaper cleanup valid.
6. Test dual opens, short reads/EOF, directory iteration, malformed backing data and Ring 3 writable-output validation. Use host tests plus relevant boot tests.

### 7.5 A test / make target

Canonical examples: [test_ext2.py](scripts/test_ext2.py), [test_shell.py](scripts/test_shell.py), [test_nmi_transitions.py](scripts/test_nmi_transitions.py).

1. Choose host sanitizer coverage for parsers/pure logic, QEMU for CPU/device delivery, manual hardware evidence for physical claims.
2. Test behavior and failure boundaries using actual subsystem code; label mocks explicitly.
3. Add a `test-X` Makefile target with real build/fixture dependencies; keep destructive tests explicitly gated.
4. Bound waits, use disposable/snapshot disks, pair OVMF code/vars, capture logs and always terminate test QEMU.
5. Assert specific results (not just a boot banner); include resource ownership checks when allocating/reclaiming.
6. Record exact invocation, result and evidence boundary in the task report/roadmap; update this routing table only if needed.

### 7.6 A user program in initramfs

Canonical examples: [shell.c](user/shell.c), [shell_start.asm](user/shell_start.asm), [shell.ld](user/shell.ld), Makefile `USER_SHELL_ELF`.

1. Place sources under root `user/`, read the syscall ABI, and use freestanding flags with no red zone/SIMD/host runtime.
2. Provide a correctly aligned entry stub and loader-supported static ELF64 with page-separated permission segments.
3. Add explicit ELF source/header/linker dependencies and an initramfs archive dependency.
4. Copy the ELF to staging `/bin/<name>` and regenerate USTAR; preserve strict tar format constraints.
5. Launch through the existing `process_spawn` lifecycle/test harness. Merely adding `/bin/foo` does not add a shell exec command; no exec syscall exists yet.
6. Verify Ring 3 execution, syscall results, faults/exit and deferred resource reclamation in BIOS/UEFI.

## 8. Hardware Facts and Verification Boundaries

Preserve empirically-derived and spec-derived workarounds with their evidence labels.
Do not remove one merely to match a datasheet; investigate discrepancies and record
device/firmware/repro. Observation, implemented behavior and unverified claims
are distinct. Do not turn an example or a source-code comment into physical acceptance evidence.

| ID | Evidence / constraint |
| --- | --- |
| H1 | **Dell 5590 photo:** keyboard input and IRQ1 initialization reported working. **Code:** `keyboard_init` clears translation while selecting/querying set 2, then sets bit 6 to deliver translated set 1. Preserve the sequence; it is not proof of the firmware's initial bit value. |
| H2 | **Code:** NVMe doorbells use CAP.DSTRD-derived stride (`4 << DSTRD`) and dynamic mapping extent. No physical DSTRD measurement is established here; never hardcode QEMU's value. |
| H3 | **5590 photo:** ECAM segment 0 buses 0..127, base `0xF0000000`. Parse MCFG, never assume this address/range or apply it to another Dell. No 5530 acceptance record is established here. |
| H4 | **Code + user report (2026-09-16):** `layout azerty` selects Belgian AZERTY (Punt). Digits require Shift. Accented unshifted keys (é è ç à on scancodes 0x03, 0x08, 0x0A, 0x0B) are reported wrong on hardware; suspected signed-char pipeline issue in decoder/ring/console. Caps is Shift-XOR, not full Shift-Lock. AltGr absent; arrows/function keys ignored; Caps LED unsynchronized. Full coverage unverified. |
| H5 | **Known software limit:** PMM manages low 2 GiB despite 32 GiB installed on the 5590. Higher RAM is unavailable to allocation. |
| H6 | **Boot policy/photo:** physical NVMe filesystem is not mounted; `/mnt` is the QEMU ext2 fixture. Initramfs file reads prove neither physical disk I/O nor persistence. |
| H7 | **Code:** ACPI FADT/DSDT S5 and reset fallbacks now exist (`power.c`); this is limited parsing, not a general AML interpreter. Port `0x604` is a QEMU mechanism. Physical ACPI S5 shutdown and multi-tier reset confirmed functional on Dell 5590. |
| H8 | **Recorded QEMU evidence:** 40 exact-boundary NMIs on IST2; no proof of physical NMI injection, nested-fault completeness, SWAPGS or SMP safety. |

### Dell Latitude 5590 physical acceptance (2026-09-16)

User-supplied boot photos confirm the shell on a Latitude 5590 (Core i5-8350U,
32 GiB installed RAM, 256 GB NVMe, Intel UHD 620), booted from a Rufus-written
USB. The latest photo shows PS/2 set 2 -> set 1 / IRQ1 ready, COM1 RX unavailable,
and the Ring 3 shell responding to keyboard input. `help` prints the command
list, `ls` lists `docs/`, `etc/`, `bin/`, and `cat etc/motd` prints the welcome
file and returns to the prompt. `cat motd` correctly reports a missing file.
These are manual observations, supplementing the automated QEMU tests.
The user reported improved boot-console responsiveness; no timing benchmark was
collected. Cached text scrolling avoids framebuffer reads; keep early/no-UART
output working. Detailed console, NMI and input test notes are in [ROADMAP.md](ROADMAP.md).

## 9. Do Not Touch Without Discussion

Discuss intentional changes to these contracts before implementation unless the
current task already explicitly authorizes them. Routine edits preserving them
need no extra approval. Preserve the behavior, not arbitrary lines of code.

- Limine request markers and linker `KEEP` placement.
- Boot/entry stack alignment (System V ABI, 16-byte before call).
- Interrupt-frame layout (register preservation order in `interrupts.asm` and `syscall_entry.asm`).
- Syscall transition windows (no stack writes before RSP switch, canonical RIP/RSP validation before SYSRET).
- Dependency ordering of subsystem initialization in `kmain`.
- Lock ranks, no-lock-across-switch rule, IRQ-excluded sleep/wakeup and CR3/stack ownership.
- Evidence-backed hardware workarounds (including any `empirical: Dell` comments): read their evidence first.
- The 2 GiB PMM cap without a coordinated allocator/audit/mapping plan.
- `ENABLE_*` raw-write gates, disposable fixture separation, hardware storage exclusions and DMA quarantine.
- Shared kernel PML4 ownership, boot-module backing lifetime, and current single-CPU assumptions.
