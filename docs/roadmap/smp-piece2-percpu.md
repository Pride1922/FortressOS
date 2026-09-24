# SMP Piece 2 — Per-CPU storage

Status: **implemented and verified in QEMU and on Dell Latitude 5590 (UEFI, 8 CPUs, 2026-09-23)**; results below.
Pieces 3-6 are not implemented.

## Implementation and contracts

- `cpu_local_t`, reached through GS, contains CPU identity, current thread,
  syscall scratch/RSP0, timer-preemption count, interrupt nesting, and local
  fault/NMI records. CPU 0 is installed during early GDT initialization,
  before the kernel IDT and scheduler. APs install GS before their IDT.
- Every CPU has a separate GDT and TSS. Selector 0x28 names a different
  descriptor/table on each CPU. CPU-local IST1/IST2 each have a 4 KiB guard
  and 16 KiB usable stack. AP kernel stacks have the same geometry. Static
  lifetime avoids AP allocations and retains storage after a timeout.
  Capacity is bounded by `MAX_DETECTED_CPUS` (64), not a guessed CPU count.
- All scheduler mutable state is in distinct CPU-indexed containers or GS
  fields. AP containers remain inactive. No AP runqueue, migration, parallel
  scheduling, or concurrent allocator support is implied. Thread IDs and
  virtual stack slots still require Piece 4's ownership design before use
  on APs. Existing rank enforcement remains BSP-only for Piece 3.
- All guard mappings and a kernel-owned MADT snapshot are prepared by BSP
  before any AP release. Release is sequential, with a bounded wait for each
  CPU; failure stops further releases. APs do not allocate, modify mappings,
  acquire subsystem locks, or log through console/dmesg. The existing
  PMM cap → kernel CR3 → high-memory unlock ordering is preserved.
- Limine hands off on its AP stack. Assembly enables NX, changes to the
  kernel CR3 and immediately switches to the private kernel stack without
  touching the old stack again. GS/GDT/TSS/IDT setup precedes local probes.
- AP-local LAPIC setup reuses BSP's established MMIO mapping, validates
  MSR/MADT agreement, masks timers and ordinary LVT sources, and installs
  MADT NMI routes without allocation or logging. APs park with IF clear.
- AP startup checks GS, loaded GDTR/TR, CR3 and stack range; a software
  `INT 2` checks IST2 plumbing (not hardware NMI evidence). A push into the
  kernel-stack guard triggers a real #PF followed by #DF on IST1. Recovery
  is restricted to that CPU's active boot probe. Unexpected AP faults store
  a CPU-local diagnostic and halt; they never enter BSP test hooks/locks.

## Syscall and interrupt entry

Global syscall scratch/RSP0 storage is removed. SWAPGS precedes the first
GS access; user RSP is saved at GS:8 and kernel RSP loaded from GS:16 before
any stack write. User returns swap back; kernel test recovery keeps kernel
GS. Interrupt entry reads the actual GS base, retaining its swap decision
in saved callee-preserved registers. This handles NMI arrival on either side
of SWAPGS despite saved CS being Ring 0. Frame layout and call alignment are
unchanged. FSGSBASE is explicitly disabled; no API sets a user GS base.

Interrupt nesting belongs to the CPU. Context switches suspend the outgoing
context's nesting count and restore it when that context resumes. NMI output
uses bounded raw UART polling with a CPU-local timeout latch, no locks or
shared serial-state writes. Concurrent UART text can interleave; the CPU-local
NMI records retain the evidence independently of text output.

## Verification

Run Linux commands in WSL Ubuntu-24.04 at `/mnt/c/Sources/FortressOS`.

| Date | Command | Environment | Result |
| --- | --- | --- | --- |
| 2026-09-23 | `make -j4` | WSL GCC/NASM; ISO and raw image | PASS; raw image GPT/FAT/ext2 validation passed |
| 2026-09-23 | `python3 scripts/test_smp_percpu.py` | QEMU TCG q35, BIOS/UEFI × 1/4/8 CPUs | PASS all six boots; distinct state/stacks, BSP byte comparisons, real AP #DF, hardware NMI on every CPU, shell |
| 2026-09-23 | `python3 scripts/test_nmi_transitions.py` | QEMU BIOS/UEFI, BSP | PASS 56 exact-boundary NMIs across seven probes; boot suites completed |
| 2026-09-23 | `python3 scripts/test_shell.py` | QEMU BIOS/UEFI | PASS IRQ input, sleeping readers/timer progress, syscall validation, restarts and resource counts |
| 2026-09-23 | `python3 scripts/test_shell_no_uart.py` | QEMU UEFI, 8 GiB, COM1 absent | PASS keyboard-only shell, real PS/2 input and sleeping reader |
| 2026-09-23 | `python3 scripts/test_storage_boot.py` | QEMU BIOS/UEFI, snapshot NVMe fixture | PASS GPT, ext2 Ring 3 and allocation-set audits |

The final runners were invoked directly after `make -j4` to keep the same
built image throughout verification. Earlier implementation checks also ran
`make test-smp-discovery` (1/4 CPUs, PASS) and the original five-probe NMI
suite (40 deliveries, PASS). Those earlier runs are not substituted for the
final results above. No physical disk was written.

The AP NMI test initially exposed missing local LAPIC NMI routing, corrected
before the final run. Simultaneous raw UART NMI reports can interleave with
BSP text; the runner now captures boot markers before NMI injection and uses
CPU-local records to establish hardware delivery, rather than parsing mixed
UART output. Physical UART serialization remains outside this piece.

`make test-smp-percpu` uses QEMU TCG q35, BIOS and paired OVMF code/disposable
vars, 1/4/8 vCPUs, and a snapshot of `build/nvme_gpt.img`. It inspects actual
CPU-local memory and descriptor contents through read-only GDB requests,
compares the BSP's entire IST1/IST2 stack regions and GDT/TSS before/after
AP startup, checks AP #DF recovery on the correct stack, injects hardware
NMIs via QMP, checks every CPU's NMI count/IST2 address, and reaches the
shell. Logs/reports: `build/smp-percpu-{bios,uefi}-{1,4,8}.{log,json}`.

`make test-nmi` injects at seven exact BSP syscall boundaries, four rounds
under each firmware (56 total), including both sides of SWAPGS. It verifies
saved RIP/frame, restored registers, and unchanged user stack/scratch/RSP0.
Logs/reports: `build/nmi-{bios,uefi}.{log,json}`. APs do not execute syscalls,
so this is not AP syscall-transition evidence. AP hardware-NMI delivery is
separately covered by `test-smp-percpu` (SM9).

## Dell Latitude 5590 physical acceptance (2026-09-23)

Physical UEFI boot on Dell Latitude 5590 (OEM: DELL) with 32 GiB RAM confirmed
Piece 2 bring-up and subsequent system stability end-to-end:
- ACPI MADT and Limine SMP agreed on 8 CPUs (BSP LAPIC ID 0).
- APs 1–7 sequentially configured private GS, GDT, TSS, and IST stacks,
  executed the real IST1 double-fault overflow/recovery probe and IST2 probe,
  and reported online before parking with `IF=0`.
- All 8 CPUs were accounted for at the Piece 2 ready checkpoint.
- Post-boot operations remained fully functional: PS/2 keyboard initialized
  with IRQ1 US mapping, xHCI enumerated root ports, SanDisk 3.2 Gen 1 on
  SuperSpeed port 0x12 was probed and classified as `SYNC_BACKED`, and GPT
  partition `sdap2` mounted read-write at `/mnt`.
- Interactive Ring 3 shell reached and processed real user keyboard input
  (`dmesg /mnt/boot.log`).

Physical external-NMI delivery via hardware button/BMC remains unverified
(QEMU QMP results do not transfer); APs remain parked, and Piece 3 requires
its own explicit authorization before implementation.
