# Cross-Cutting Subsystem Notes

Evidence and implementation notes that span multiple phases, kept here
instead of duplicated into each phase file. If you're looking for a
phase-specific claim, check the phase file first — this file is for NMI
delivery, boot console, interactive input, and general Dell hardware
acceptance that isn't tied to a single numbered phase.

## NMI transition and physical-boot diagnostics

- MADT type 4 NMI records are validated and applied to the bootstrap CPU's
  LAPIC LINT pins, with processor-ID matching and conflict detection.
  Undeclared pins stay masked. x2APIC/type-10 NMI routing is not
  implemented.
- `make test-nmi` uses QEMU TCG QMP injection plus hardware GDB breakpoints
  at zero-byte assembly labels. It never patches code, synthesizes INT 2,
  or widens the transition windows. It verifies 5 boundaries x 4 rounds x 2
  firmware modes, then requires the complete boot suite to finish.
  Evidence: `build/nmi-*.json` and `build/nmi-*.log`. This covers QEMU
  delivery, not physical NMI injection, nested fault/NMI scenarios or SMP.
  **Note for SMP work:** this entire verification is currently BSP-only —
  see `SMP_DESIGN.md` Piece 2 (SM9) for the plan to extend it per-CPU.
- Framebuffer logging begins before PMM/GDT tests. COM1 loopback failure
  disables UART output; transmitter waits are bounded so absent hardware
  cannot hang boot.
- The current PMM explicitly manages RAM below 2 GiB, reserving higher RAM
  until allocator/audit capacity is expanded. Its bitmap is selected within
  managed RAM. (Superseded by Phase 9H for the 32 GiB case — see
  `phase-9h-ram.md`.)
- `make test-boot-diagnostics`: UEFI, 8 GiB, no COM1, no NVMe fixture;
  verifies progress to PCI discovery and captures
  `build/boot-8g-no-uart.png`.
- Storage fixture assertions run only against QEMU NVMe vendor/device IDs.
  Both hardware and fixture paths launch /bin/shell after diagnostics. The
  physical NVMe is still not mounted; /mnt is only available via the USB
  path (Phase 9G) or the QEMU ext2 fixture.

### `make test-nmi` detail

Injects real external NMIs through QMP at five debugger hardware-breakpoint
locations: before saving user RSP, after saving it, after switching to the
kernel stack, before restoring user RSP, and immediately before SYSRET with
user RSP already active. Labels add no instructions or delays. The harness
checks the hardware frame's exact interrupted RIP/CS/RFLAGS/RSP/SS, checks
execution on IST2, verifies all GPRs and scratch values are preserved,
compares 128 bytes below user RSP, executes IRET and compares restored
state. The exit probe also checks successful SYSRET to CPL3. Four rounds
per firmware provide 40 verified injections total, followed by full-suite
completion. JSON evidence and serial logs are in `build/nmi-bios.*` and
`build/nmi-uefi.*`.

Testing exposed masked LINT1 routing: QEMU's external NMI delivery respects
that mask. The kernel now parses MADT Local APIC NMI records and applies
the declared bootstrap-CPU routes, rather than assuming both LINT pins can
always stay masked. Invalid pin/flag encodings are tested; conflicting
applicable routes are rejected. The harness requires this normal boot
configuration; it performs no guest memory or register writes. References:
QEMU GDB documentation and QEMU hw/intc/apic.c.

This closes the QEMU transition-window delivery gap. It does not establish
physical-hardware delivery, all nested-fault cases, SWAPGS correctness or
SMP safety.

## Boot-console scrolling

The console caches character/colour cells in static RAM (512 x 256 cells,
1.5 MiB; viewport capped to this grid). Scrolling never reads framebuffer
MMIO. Only changed cells are rendered, and each scroll advances
min(8, max(1, rows/4)) rows so several subsequent log lines need no screen
movement. Output remains synchronous and immediately visible, including
before PMM/heap initialization. Wrapping is deferred until the next
printable character; an explicit newline following a full-width line
advances exactly once.

`make test-console` checks pixel output, colour preservation, scroll
batching, zero redraws for blank lines, control characters, one-cell
screens and padded framebuffer bounds under ASan/UBSan. BIOS/UEFI full boot
suites also pass.

Replaced uncached framebuffer-to-framebuffer scrolling with a static RAM
text cache. Rendering compares character and colours and skips unchanged
cells; blank cells ignore irrelevant foreground-colour differences.
Scrolling advances up to eight rows per batch, reducing screen movement
frequency during logs. The fixed 1.5 MiB cache requires no early allocator
and supports a 512 x 256 text viewport. Framebuffer writes remain uncached;
this is not GPU acceleration or write-combining support. After testing the
rebuilt ISO on the Latitude, the user reported that it feels like Linux
booting; no timing benchmark was collected.

## Interactive input and shell

`make` includes a separate freestanding C ELF `/bin/shell` in initramfs.
Boot launches it as a normal Ring 3 process after the acceptance suite.

Historical baseline: in early Phase 9, files were read-only, no editor or
command discovery existed, lines were capped at 191 bytes (overflow
discarding the entire command), and arrows/function keys were ignored.

Shell S0–S2 (implemented 2026-09-25; see `shell-s0-s2.md`) modernized this
into a modular, comfortable interactive terminal environment:

- **Modular architecture:** split `user/shell.c` into dedicated components
  under `user/shell/`: `builtins` (registry and dispatch), `io` (syscall wrappers
  and formatting), `lineedit` (pure editor state machine and RAM history),
  `ui` (ANSI rendering, prompt, viewport, paste mode), and `fileedit` (extracted
  line-based text editor).
- **Input and timed wait ABI:** `SYS_INPUT_READ` (17) provides raw, non-echoed
  input with indefinite (-1), nonblocking (0), or bounded wait (1..1000 ms).
  Input IRQs publish without locking across context switches; the BSP timer
  performs bounded wakeups. Input remains BSP-affine. Queue/UART drops surface
  `-20` (`INPUT_LOST`), invalidating the pending command rather than executing
  partial input.
- **Terminal control ABI:** `SYS_TERMCTL` (16) reports terminal geometry,
  drop counter, and display generation. Supports process-level output endpoint
  selection: mirror (0), local framebuffer (1), serial (2), or plain (3).
  Interactive writes bypass `dmesg_append`. Framebuffer console interprets a
  bounded CSI ANSI subset (cursor movement `A`/`B`/`C`/`D`, positioning `H`/`f`,
  erase `K`/`J`, SGR colors `m`, and cursor show/hide `?25h`/`?25l`).
- **Full line editing:** 4096-byte input limit, horizontal viewport, cursor
  insertion/deletion, Backspace/Delete, Home/End, word erase (`Ctrl+W`), line
  erase (`Ctrl+U`/`Ctrl+K`), single kill buffer yank (`Ctrl+Y`), clear screen
  (`Ctrl+L`), draft cancellation (`Ctrl+C`), and empty-line exit (`Ctrl+D`).
- **In-memory history and search:** bounded by 1000 entries and 256 KiB,
  consecutive duplicate suppression, `history` and `history clear`. Up/Down
  navigates history with draft restoration. `Ctrl+R` provides reverse incremental
  search; Enter accepts the match for editing, and a second Enter executes.
- **Bracketed paste review:** pasted newlines/tabs are converted to spaces;
  requires explicit review and two Enter presses before execution.
- **Keyboard decoding & AltGr:** decodes arrows, Home, End, Delete, Left/Right
  Ctrl, and Belgian AZERTY AltGr operator mappings (`|`, `\`, `{}`, `[]`, `~`).

`SYS_READ(0, buffer, count)` checks all user pages for write permission
before waiting. It returns available bytes as a short read, without
waiting for newline. A scheduler predicate and BLOCKED-list insertion occur
under the scheduler lock with IRQs disabled. Producers publish input before
waking readers; a resumed reader rechecks availability. IRQ exclusion also
spans predicate-to-dequeue. No lock crosses a context switch, no ISR
allocates/logs/switches, and the IDT dispatcher owns each keyboard/UART
EOI. Blocked tasks remain visible to process liveness/wait APIs. This
input/scheduler contract is bootstrap-CPU-only; SMP and concurrent
address-space mutation need further synchronization — see `SMP_DESIGN.md`
Pieces 3 and 4.

### Verification

- `make test-input`: actual decoder/FIFO under ASan/UBSan; modifiers,
  Pause, PrintScreen, extended keys, wraparound and overflow.
- `make test-shell-host`: consolidated ASan/UBSan suite testing keyboard/queue,
  framebuffer console CSI parsing, and actual shell editor logic (bounds,
  history memory budgets, reverse search, bracketed paste, overflow, and hostile bytes).
- `make test-shell-integration` / `make test-shell`: BIOS/UEFI PS/2 events and
  serial RX through real emulated devices, stdin pointer checks, Backspace/Shift,
  arrows, history/search, paste, terminal modes, sleeping task/timer progress,
  and process restarts with stable physical free-page and stack-slot counts.
  Logs: `build/shell-*.log`.
- `scripts/test_shell_no_uart.py`: UEFI 8 GiB with COM1 absent and non-fixture NVMe
  identity, confirming framebuffer `echo hello`, keyboard events, and sleeping input on the
  hardware boot path. Screenshot: `build/shell-keyboard-only.png`. Physical
  Dell interaction is verified per `docs/roadmap/shell-s0-s2.md`.
- Existing BIOS/UEFI storage acceptance and 40 exact-boundary NMI tests
  pass.

References for the driver/test protocol: Intel EC firmware 8042
documentation
(https://intel.github.io/ecfw-zephyr/reference/kbchost/index.html), QEMU
PS/2 implementation
(https://github.com/qemu/qemu/blob/master/hw/input/ps2.c), and QMP
input-send-event
(https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html).

## Dell Latitude 5590 physical acceptance (general, 2026-09-16 & 2026-09-18)

User-supplied testing confirms hardware operation on a Latitude 5590 (Core
i5-8350U, 32 GiB installed RAM, 256 GB NVMe, Intel UHD 620), booted from
USB:

- **2026-09-16:** PS/2 set 2 -> set 1 / IRQ1, COM1 RX absent, interactive
  shell `help`, `ls`, `cat etc/motd`, and missing-file error handling
  verified. `wsl -d Ubuntu-24.04 -- make test-storage test-shell test-power`
  passed BIOS/UEFI storage and shell checks, keyboard-only UEFI 8 GiB, and
  ordinary read-only-boot shutdown/reboot (QEMU exit code 0). The kernel and
  ISO also built with the existing strict compiler flags.
- **2026-09-18:**
  - **Belgian AZERTY (Bug H4):** see `phase-9e-exec-and-files.md` for the
    fix; verified here on hardware.
  - **System V AMD64 ELF ABI:** see `phase-9e-exec-and-files.md`.
  - **Visuals:** Limine splash wallpaper and kernel boot logo / emblem
    verified.
  - **Power:** ACPI S5 shutdown and multi-tier reset confirmed functional.

These are manual hardware observations, supplementing the automated QEMU
and host test suites. Cached text scrolling avoids framebuffer reads; keep
early/no-UART output working.

## Physical boot: visible early diagnostics (root-cause note)

The framebuffer console previously started after PMM/VMM/heap tests. The
PMM also audited a fixed 2 GiB capacity after sizing itself for all
installed RAM, so a larger machine could halt before any screen output.
Initialization now starts screen logging early and explicitly limits
managed RAM to the supported low 2 GiB at that stage (superseded for the
32 GiB case by Phase 9H). Higher RAM remains unavailable to allocation
until the relevant phase. UART writes also stop waiting after a bounded
poll or failed loopback test.

A QEMU UEFI regression with 8 GiB and COM1 absent reaches PCI discovery
after all memory, process and initramfs tests; its framebuffer screenshot
is saved. Subsequent user photos confirm successful diagnostics and
interactive shell boot on the Latitude. They do not isolate the original
black-screen root cause. Physical disks are excluded from QEMU-specific
storage fixture tests using the controller vendor/device identity.

## Hardware facts recorded here (not tied to one phase)

- **H1:** Dell 5590 photo: keyboard input and IRQ1 initialization reported
  working. Code: `keyboard_init` clears translation while
  selecting/querying set 2, then sets bit 6 to deliver translated set 1.
  Preserve the sequence; it is not proof of the firmware's initial bit
  value.
- **H2:** Code: NVMe doorbells use CAP.DSTRD-derived stride (`4 << DSTRD`)
  and dynamic mapping extent. No physical DSTRD measurement is established
  here; never hardcode QEMU's value.
- **H6:** The internal physical NVMe filesystem is not mounted by the
  kernel by default. `/mnt` is provided by the USB data partition when a
  supported stick is attached and selected (Phase 9G). Initramfs file reads
  prove neither physical disk I/O nor persistence.
- **H6a:** Phase 9F code + user report (2026-09-18): the raw image includes
  an ext2 data partition; the user flashed it with Rufus and reported no
  `/mnt`. Kernel USB storage support was absent at that point. USB boot and
  image verification do not establish USB partition mounting or
  persistence on their own; Phase 9G supplied that missing path.
- **H7:** Code: ACPI FADT/DSDT S5 and reset fallbacks exist (`power.c`);
  this is limited parsing, not a general AML interpreter. Port `0x604` is a
  QEMU mechanism. Physical ACPI S5 shutdown and multi-tier reset confirmed
  functional on Dell 5590.
- **H8:** Recorded QEMU evidence: 40 exact-boundary NMIs on IST2; no proof
  of physical NMI injection, nested-fault completeness, SWAPGS or SMP
  safety.
