# Phase 9E — Program Execution, ABI, Directory Ops & Bug H4

Status: COMPLETE. See `AGENTS.md` status table for current summary; this file
holds the detailed implementation notes and evidence.

## Phase 9E Saved File Management & Bug H4 Resolution (2026-09-18)

Implemented directory operations (`mkdir`), file rename/move (`rename`), and
deletion (`unlink`) across VFS and writable ext2, along with user syscalls
(`SYS_MKDIR` = 11, `SYS_UNLINK` = 12, `SYS_RENAME` = 13) and interactive
Ring 3 shell commands (`mkdir`, `rm`, `mv`).

- **Directory lifecycle:** Ext2 directory creation allocates dedicated data
  block and initializes standard `.` (self) and `..` (parent) records.
  Parent `links` count is incremented on creation and decremented on removal.
- **Safety checks:** Directory unlinking enforces that directories are empty
  (only `.` and `..` permitted; returns `-VFS_ENOTEMPTY` /
  `SYSCALL_ENOTEMPTY` otherwise).
- **Directory reparenting:** Cross-directory renames update `..` directory
  entry in the moved directory to point to the new parent, with
  corresponding link count adjustments.
- **On-disk reclamation:** Unlinked inodes have their data blocks returned to
  the block bitmap, inode marked free in the inode bitmap, block pointers
  and size cleared, `i_links_count` set to 0, and `i_dtime` deletion
  timestamp recorded.
- **Bug H4 fix:** Corrected Belgian AZERTY layout scancode decoding. Number
  row scancodes 2..13 now use `shift ^ s->caps` as Shift-Lock for digits
  `1234567890`. Shifted lookup takes precedence over alphabet table,
  preventing accented keys (`0x03`, `0x08`, `0x0A`, `0x0B`, `0x28`) from
  falsely generating uppercase letters. Added ISO scancode 86 (`<` / `>`).
- **Verification:** `make test-input` and `make test-console` passed under
  ASan/UBSan. `make test-ext2` passed all 8 geometries. `make test-ext2-write`
  verified 3-boot persistence across BIOS and UEFI with zero `e2fsck -fn`
  errors. `make test-storage`, `make test-shell`, and `make test-power`
  passed.

## Phase 9E — Program Execution from Shell, Exit Status & System V AMD64 ABI (2026-09-18)

Phase 9E adds the ability for the interactive Ring 3 shell to load, execute,
pass arbitrary string arguments to, and wait on standalone user ELF binaries
from VFS (`/bin/hello`), while isolating CPU faults, tracking exit status,
and adhering strictly to the standard System V AMD64 ELF ABI.

### Implementation Details

- **System Calls**: `SYS_SPAWN` (nr 9: path, argv pointer -> child PID) and
  `SYS_WAIT` (nr 10: child PID, status pointer -> 0 on success).
- **Standard System V AMD64 Process Stack**: In
  `process_setup_user_stack()`, string arguments are packed at the high end
  of the initial 4 KiB user stack page
  (`USER_STACK_TOP_VIRT = 0x00007FFFF0001000ULL`) via HHDM virtual
  translation (`vmm_phys_to_virt(stack_phys)`). Below the strings, the
  initial pointer table is written: `[RSP] = argc`, `[RSP+8] = argv[0]`,
  ..., `argv[argc] = NULL`, `envp[0] = NULL`, `AT_NULL` auxiliary vector pair
  (`0, 0`). `RSP` is strictly 16-byte aligned (`RSP % 16 == 0`).
- **Register Initialization & ABI**: At process entry
  (`user_process_trampoline`), `RDI = argc`, `RSI = argv`, `RDX = 0`
  (standard `rtld` termination handler), with all other GPRs sanitized to
  zero. Kernel boot tests using `process_spawn_with_arg()` maintain scalar
  `RDI` mode selection compatibility for `init.asm` test modes 0..7.
- **Process Waiting & Reclamation**: `process_wait_child()` uses
  single-threaded parent predicate `child_done` with `sched_wait_until()`,
  waking via `sched_wake_all()`. Parent reaps dead resources via
  `sched_reap_dead()`. Up to 64 active child records are tracked in
  `g_child_records` under the scheduler spinlock.
- **Fault Isolation**: Processes faulting on CPU exceptions (e.g. #PF vector
  14, #GP vector 13) are recorded with status `128 + vector` by the
  exception handler, reported to the user as
  `[PROCESS] Faulted (exception vector <N>)` without bringing down the
  parent shell or kernel.
- **Shell Argument Parsing & Status Tracking (`$?`)**: User-space shell
  command parser tokenizes whitespace-delimited arguments
  (`run /path [args...]`), passing `argv[]` array to `SYS_SPAWN`. Shell
  tracks `last_status` updated on every command and child termination.
  `echo $?` expands to decimal exit code.
- **Command Chaining**: Shell command parser supports conditional chaining:
  `&&` executes subsequent command only if previous succeeded
  (`last_status == 0`), while `||` executes only if previous failed
  (`last_status != 0`).
- **Driver Hardening**: In `serial_init()`, receiver FIFO is drained inside
  loopback mode, followed by a bounded poll for data ready. This eliminates
  false loopback failures and serial silencing caused by UEFI/OVMF firmware
  debug noise during boot.

### Verification Environment & Evidence

- **Automated Shell Integration Suite (`make test-shell`)**:
  - `PASS bios`: Tested `/bin/hello` execution with no arguments
    (`run /bin/hello`), numeric argument (`run /bin/hello 42`), string
    argument (`run /bin/hello world`), status query (`echo $?` -> `42` /
    `0`), command chaining (`&&` executed on success, skipped on failure;
    `||` executed on failure, skipped on success), negative error paths
    (`/missing`, `/bin`), and zero-leak resource audit (`free_pages` and
    `g_stack_slots_bitmap` unchanged across child lifecycles). Log:
    `build/shell-bios.log`.
  - `PASS uefi`: Validated identical command sequences and resource
    assertions under UEFI with paired OVMF firmware. Log:
    `build/shell-uefi.log`.
  - `PASS keyboard-only UEFI 8 GiB`: Validated hardware boot path without
    COM1 UART.
- **Subsystem Regression Coverage**:
  - `make test-input`: Passed FIFO and scancode decoding.
  - `make test-console`: Passed cached redraw and scrolling checks.
  - `make test-storage`: Passed BIOS and UEFI GPT and ext2 Ring 3 read/audit
    tests.
  - `make test-nmi`: Passed 40 exact-boundary NMI delivery cycles across all
    5 syscall transitions in BIOS and UEFI.
  - `make test-ext2`: Passed host ASan/UBSan matrix with injected failures
    across 8 configurations.

## Dell hardware acceptance (from the 2026-09-16 & 2026-09-18 combined report)

- **System V AMD64 ELF ABI:** `run /bin/hello testing ...` verified passing
  command-line arguments across the user/kernel boundary with proper
  16-byte stack alignment, on physical Latitude 5590 hardware.
- **Belgian AZERTY (Bug H4):** Shift-Lock on top number row with Caps Lock ON
  verified producing digits `1234567890`. Accented keys unshifted produce
  base characters without falsely emitting uppercase letters. European ISO
  `<` / `>` key (scancode 0x56) verified.

Full general Dell acceptance narrative (PS/2, console, power) lives in
`subsystems.md` since it spans multiple phases, not just 9E.
