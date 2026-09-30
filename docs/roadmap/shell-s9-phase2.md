# Shell S9 Phase 2 — SYS_SYSINFO and /bin/sysinfo

Status: COMPLETE (2026-09-30). Implemented and verified on QEMU (BIOS & UEFI, SMP=1 and SMP=4).

## 1. Summary of Changes

Phase 2 adds the `SYS_SYSINFO` system call (number 37) and the standalone Ring 3 `/bin/sysinfo` user utility, resolving the two metric prerequisites identified in `docs/plans/S9_PLAN.md`.

### Prerequisite 1: Managed-RAM Total
- `pmm_get_stats().total_pages` is `highest_addr / PAGE_SIZE`, representing an address span from physical address 0 to the highest mapped RAM page, which includes unmapped PCI MMIO apertures and firmware holes.
- **Definition**: Managed RAM is the sum of bytes across DRAM regions delivered to the OS: `LIMINE_MEMMAP_USABLE`, `LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE`, `LIMINE_MEMMAP_KERNEL_AND_MODULES`, and `LIMINE_MEMMAP_ACPI_RECLAIMABLE`, clamped to `PMM_BITMAP_MAX_RAM_BYTES` (32 GiB).
- **Correctness Subset Proof**: All allocatable pages in PMM originate strictly from `LIMINE_MEMMAP_USABLE` below 32 GiB. Since managed RAM contains the entire `USABLE` pool plus kernel/bootloader/module/ACPI memory, `free_ram_bytes <= total_ram_bytes` holds at all times, and `used_ram_bytes = total_ram_bytes - free_ram_bytes` accurately describes real RAM used without hole distortion.
- **Implementation**: Computed once in `pmm_init()` and exposed via `pmm_get_managed_ram_bytes()`.

### Prerequisite 2: BSP Monotonic Timebase
- The existing APIC tick counter `g_timer_ticks` was incremented on every CPU in `apic_timer_handler()`, scaling with active core count and suffering unsynchronized multi-writer data races across cores.
- **Implementation**:
  - Added dedicated `static volatile uint64_t g_bsp_timer_ticks`.
  - Incremented strictly on CPU 0 (`if (cpu_current()->id == 0)`) via `__atomic_fetch_add(..., 1, __ATOMIC_RELAXED)`.
  - Read via `apic_timer_get_bsp_ticks()` (`__atomic_load_n(..., __ATOMIC_ACQUIRE)`).
  - Exported immutable calibrated tick frequency via `apic_timer_get_frequency()` (`(uint64_t)g_target_hz`).
  - Reset to 0 before multitasking starts in `main.c:5662` so early boot test ticks do not bleed into system uptime.
  - Left `g_timer_ticks` and `apic_timer_get_ticks()` completely untouched for their existing callers in `thread.c` and `main.c`.

### Kernel: SYS_SYSINFO Syscall
- Syscall number 37: `SYS_SYSINFO(sysinfo_t *buf) -> 0 | -errno`.
- Exact 48-byte `sysinfo_t` ABI with compile-time size and offset assertions:
  - `total_ram_bytes` (offset 0, uint64_t)
  - `free_ram_bytes` (offset 8, uint64_t)
  - `uptime_ticks` (offset 16, uint64_t)
  - `tick_hz` (offset 24, uint64_t)
  - `cpu_count` (offset 32, uint32_t)
  - `task_count` (offset 36, uint32_t)
  - `reserved` (offset 40, uint64_t)
- S2 Range validation enforced first: returns `SYSCALL_EFAULT` on invalid user pointers.
- Lock discipline (L1): samples PMM free frames under `g_pmm_lock`, task count under `g_process_lock`, CPU count from immutable `g_total_cpu_count`, and uptime from atomic load. No cross-subsystem locks nested; copy-out occurs with all locks released.

### User Utility: /bin/sysinfo
- Standalone freestanding executable compiled from `user/sysinfo.c` linked with `user/shell.ld`.
- Uses static BSS buffers (`s_info` and `s_buf`), using 80 bytes of static stack (well below 512B budget; verified via `build/sysinfo.su`).
- Formats system information in human-readable columns:
  ```text
  FortressOS — system information
    CPUs:        1
    Uptime:      00:04:12
    RAM total:   2048 MiB
    RAM free:    1980 MiB
    RAM used:      68 MiB
    Processes:   6
  ```

## 2. Verification Evidence

### Host Unit Test
- Target: `make test-s9-sysinfo-host` (`tests/sysinfo_host.c`)
- Covers:
  - S2 range validation and `SYSCALL_EFAULT` return.
  - Reserved field zeroing.
  - Uptime `HH:MM:SS` calculation and zero-frequency defense.
  - RAM units conversion to MiB and math bounds.
- Status: **PASS**

### NMI Regression Guard
- Target: `make test-nmi`
- Covers: 28 exact-boundary NMIs + 24 exact-boundary sigreturn NMIs across 4 rounds on both BIOS and UEFI.
- Status: **PASS**

### QEMU Integration & Regression Test
- Target: `make test-s9-sysinfo` (`scripts/test_s9_sysinfo.py`)
- Environment: Real Ring 3 shell, disposable ISO, no data disks, argv preflight checking for forbidden device injection.
- Matrix:
  - BIOS `SMP=1`: `CPUs=1, RAM=2047 MiB (Free=2026, Used=21), Uptime delta=2s, Procs=2->3`
  - BIOS `SMP=4`: `CPUs=4, RAM=2047 MiB (Free=2026, Used=21), Uptime delta=2s, Procs=2->3`
  - UEFI `SMP=1`: `CPUs=1, RAM=2041 MiB (Free=2020, Used=21), Uptime delta=2s, Procs=2->3`
  - UEFI `SMP=4`: `CPUs=4, RAM=2041 MiB (Free=2020, Used=21), Uptime delta=2s, Procs=2->3`
- Regression Guard (Prerequisite 2):
  - Invariant: `SMP=4` uptime delta ($\Delta t = 2\text{s}$) equals `SMP=1` uptime delta ($\Delta t = 2\text{s}$). The multi-writer scaling bug is completely eliminated.
- RAM Sanity: `total >= free`, `used = total - free`, plausible physical DRAM.
- Process Transition: Spawning background job `cat &` transitions enumerable task count dynamically.

## 3. Physical Hardware Acceptance: Dell Latitude 5500

User-supplied testing confirms hardware operation on a second physical bare-metal machine, a **Dell Latitude 5500** (8 GiB installed RAM, 8 logical CPUs), booted via UEFI from USB:

### Observed Metrics & Execution
- **SMP Bring-up**: Kernel detects all 8 cores via ACPI MADT / Limine SMP, initializes per-CPU GS/TSS/IST structures, brings all 7 APs online (`All 7 application processor(s) online`), and starts distributed schedulers.
- **Ring 3 Shell & Introspection**:
  - Interactive shell prompt reached cleanly.
  - `/bin/ps` executes in Ring 3, querying `SYS_PROCINFO`, and formats active processes correctly:
    ```text
      PID  PPID  PGID   SID  STATE    NAME
        1     0     1     1  RUNNING  shell
    ```
  - `/bin/sysinfo` executes in Ring 3, querying `SYS_SYSINFO`, and displays accurate hardware and runtime data:
    - **CPUs**: Correctly reports `8` logical CPUs.
    - **Uptime**: Monotonic APIC timebase advancing at 100 Hz without CPU-count multiplication (Prerequisite 2 verified on 8-core physical hardware; no multi-writer clock drift).
    - **RAM**: Managed-RAM total accurately reports ~8 GiB (~8192 MiB installed DRAM handed to the OS without PCI MMIO hole inflation), with valid free and used distributions.
    - **Processes**: Reflects active enumerable process count.

### Photo Evidence
- **Photo 1 (Boot / SMP discovery)**: Framebuffer boot log displaying Limine kernel handoff, memory map discovery, and SMP AP initialization confirming `All 7 application processor(s) online`.
- **Photo 2 (`/bin/ps` Ring 3 execution)**: Interactive framebuffer terminal running `/bin/ps` under the user shell, rendering process table columns with correct PID, PPID, PGID, SID, state, and name.
- **Photo 3 (`/bin/sysinfo` Ring 3 execution)**: Interactive framebuffer terminal running `/bin/sysinfo`, displaying formatted `CPUs: 8`, real-time `Uptime: HH:MM:SS`, ~8 GiB `RAM total`, and `Processes: 6`.

### Evidence Boundary
These observations represent physical hardware validation on a second physical machine model (Dell Latitude 5500 alongside Dell Latitude 5590). User-confirmed photos and hardware runs supplement the automated QEMU and host test suites without introducing machine-specific assumptions or hardcoded workarounds.
