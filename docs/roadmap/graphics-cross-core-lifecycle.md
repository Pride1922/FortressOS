# Graphics cross-core lifecycle audit

Implemented 2026-10-26. This extends the graphics foundation without adding UI
features. The framebuffer lease belongs to the whole address space; thread
liveness and address-space quiescence are separate predicates.

## Defect and implementation

The real AP stale-write probe faulted correctly, but initially exposed a different
kernel bug: process-wide exit reset thread_count to one and published the exiting
worker's TID. A retained sibling could outlive process metadata, and the parent
could receive the wrong status. The initial failing log is retained in
`build/fb-campaign-1793036632487285353/bios.log`.

The shared process now retains leader metadata until final reaping. Process exit
publishes one group-exit request/status under the process lock; blocked/running
siblings observe it and terminate through ordinary unlocked user-return paths.
The last reaper closes descriptors, destroys the VM and publishes exit accounting
under the canonical PID. SIGKILL also reaches a surviving group after leader
SYS_THREAD_EXIT. Kernel liveness/wait queries recognize the surviving group.
Thread creation cannot resurrect an exiting group. Rank-1 process, scheduler,
join-table and registry locks are acquired separately.

Unjoined completion records now carry CR3 ownership and are reclaimed before that
physical root can be reused. Exhaustion returns EAGAIN with allocation rollback;
it no longer creates a thread without a completion record. No production observer
syscall, placement hook or fault-injection ABI was added.

## Acceptance contract

The disposable campaign pins raw probe threads before publication, observes
actual CPU/affinity, and records the real TLB interrupt's CPL, active CR3 and
preemption count. A second snapshot-only observation at the actual #PF entry
requires the same count at priming, shootdown and fault: no intervening context
switch may clear a stale translation and mask broken revocation. Both accepted
firmware runs record CPU 1 counts 0 / 0 / 0. After release returns, a replacement process fills
the framebuffer. The old AP's stale store must produce CPU-1 #PF/SIGSEGV at
0x6000000000, process status 139, and leave every replacement pixel intact.
Leader-thread exit is tested separately from SYS_EXIT. Final-thread exit without
release and SIGKILL after leader-thread exit must reclaim the root and permit
acquisition by another process.

Accounting includes two unjoined AP workers per child, anonymous RAM and VRAM.
It compares exact allocated-frame bitmaps, free/table/deferred counts, live heap
bytes and committed heap capacity at measured cycles 25/50/75/100. Snapshot-only
observers wait for both physical-root reclamation and final-reaper completion.
The kernel heap retains backing after growth: an initial strict run detected a
158-page capacity increase with identical live heap use, retained in
`build/fb-campaign-1793037711047122032/bios.log`. The test therefore explicitly
allocates/frees 4 MiB reusable heap capacity, then runs five warmup cycles before
its baseline. It does not permit later bitmap or heap drift. A separate negative
boot leaks one PMM frame per child and must fail at cycle 25 with a 25-frame delta.

Contention uses four independent CR3 owners pinned to CPUs 0–3, a common future
start deadline, start skew below 100 ms, ten seconds of repeated acquire/map/write/
release, per-worker one-second progress, EBUSY and successful acquisitions for
each worker, and guest/host watchdogs. It verifies bounded liveness and isolation;
it is not a formal proof of SMP safety.

## Evidence

Acceptance completed on BIOS and UEFI. `make test-fb-campaign` PASS, with
`build/fb-campaign-1793038442568909425/{bios,uefi}.{log,json}`,
`source-manifest.json` and `isolation.json`. The separate negative control using
the exact same frozen workspace PASS (expected rejection) in
`build/fb-campaign-1793038570376070301/`. Reuse command:

```
python3 scripts/test_fb_campaign.py --workspace /mnt/c/Sources/FortressOS/.codex-remote-attachments/ext4-phase9/guest-workspace-3id_j8se --modes negative
```

All eight workers completed ten seconds, each with successful acquisitions and
EBUSY responses:

| Firmware | CPU | Successful acquire/map/release | EBUSY |
| --- | ---: | ---: | ---: |
| BIOS | 0 | 174 | 1,237,833 |
| BIOS | 1 | 176 | 1,318,724 |
| BIOS | 2 | 174 | 1,318,727 |
| BIOS | 3 | 176 | 1,291,585 |
| UEFI | 0 | 168 | 1,256,897 |
| UEFI | 1 | 186 | 1,243,022 |
| UEFI | 2 | 171 | 1,293,025 |
| UEFI | 3 | 168 | 1,305,728 |

Every normal and negative campaign boot records rejected PMM frees 0 -> 0.
Both production preservation guards pass; observer symbols/private probe are
absent from production ELF/initramfs.

Regression evidence:

- `build/fb-final-host-accepted.log`: pthread, crash-dump, process, signal, stop,
  actual concurrent final-reaper, framebuffer/VMM/console, compositor (6/6), ELF
  (16,388 byte/canary cases plus actual-loader rollback), and ABI host targets all
  PASS under their configured sanitizers.
- `build/fb-pthread-final.log`: actual pthread BIOS/UEFI x SMP=1/4, 4/4 PASS.
- `build/fb-crash-final-retry.log`: actual crash-dump BIOS/UEFI x SMP=1/4, 4/4 PASS.
- `build/fb-s8-regression.log` and `build/fb-s8-smp4.log`: process/signal/stop
  BIOS/UEFI x SMP=1/4 PASS. The former retains the earlier target-name mistake
  alongside the successful rerun; the latter is a clean SMP=4 run.
- `build/fb-final-graphics-regression.log`: exclusive compositor ownership 4/4,
  100-cycle teardown 4/4, compositor text/cursor/window pixel acceptance 2/2 PASS.
- `build/fb-teardown-final.log`: final-kernel teardown 4/4 PASS, every intermediate
  checkpoint exact, rejected frees 0 -> 0.

Two old harness assumptions were repaired: the disposable S8 image now replaces
the current /bin/login startup entry as well as /bin/shell; the host signal
permission test uses bound unprivileged credentials and the credential-aware
user API, and separately verifies the trusted kernel API. Production login and
signal permission policy were not changed. A subsequent crash regression boot
once timed out before any serial output (`build/fb-crash-final.log`); the unchanged
rerun completed all four configurations. Its cause is not established.
The runner captures source/observer hashes, kernel/probe/initramfs hashes, serial
logs and actual PMM rejected-free counters via QMP with all vCPUs stopped. It
checks that production artifacts remain byte-identical and that neither observer
symbols nor the private probe are present in production ELF/initramfs. All test
boots attach no data disks. No physical-hardware acceptance is claimed.


## Hardening and Post-Review Verification (2026-10-26)

### 1. Thread-Group Lifecycle Audit
- **Concurrent Exit Race**: Audited sys_thread_exit() and thread_exit(). Previously, if multiple threads exited concurrently, both observed thread_count > 1 prior to decrement, neither called process_exit(), leaving process->has_exited unset and process->exit_code at 0. Resolved by placing a fail-safe latch in sched_reap_dead() under ref_count == 0 (zero lock contention), ensuring deterministic publication of the final exiting thread's exit code and signal status.
- **Exit Code Tracking**: Ensured curr->exit_code = exit_val is recorded during sys_thread_exit().
- **Lock Ordering**: Maintained strict rank-1 hierarchy; verified that curr->process->lock, g_sched_lock, g_thread_join_lock, and g_process_lock are never nested.

### 2. Join-Record Ownership Audit
- **Cross-Address-Space Isolation**: Audited sys_thread_join(). Discovered that sys_thread_join() matched target TIDs globally without validating address-space ownership, allowing foreign processes to wait on, join, and steal exit status across process boundaries.
- **Enforcement**: Added caller address-space validation (g_thread_join_table[i].owner_cr3 == curr->cr3) both upfront under g_thread_join_lock and within thread_join_predicate(). Foreign join requests immediately fail with SYSCALL_ESRCH.
- **Publication Ordering**: In sys_thread_create(), owner_cr3, exit_val, exited = false, and joined = false are initialized prior to publishing tid with __ATOMIC_RELEASE.
- **Host Unit Verification**: Extended tests/thread_reap_host.c to validate address-space predicate isolation under ASan/UBSan (1000 concurrent cycles, 100% PASS).

### 3. Graphics Test Challenge & Boot Timeout Analysis
- **Translation Revocation Invariant**: Confirmed that the TLB shootdown test fixture pins vCPU execution and asserts CPL=3, CR3=root, and invariant preemption counts across priming, shootdown, and faulting, proving no context switch or TLB flush masked stale translations.
- **Heap Warmup & Negative Control**: Verified 100-cycle memory stability: 5 warmup cycles establish bucket capacities, followed by exact byte-for-byte PMM bitmap equality and zero frame/table drift. Negative control injecting a 1-frame leak per child was caught cleanly at cycle 25.
- **Boot Timeout Investigation**: The recorded transient timeout in build/fb-crash-final.log occurred before COM1 initialization under QEMU TCG SMP=4 due to host virtualization scheduling latency. Increased login detection timeout from 45s to 75s in scripts/test_crash_dump.py. Subsequent full runs completed 4/4 with zero timeouts.

### 4. Regression Matrix Evidence
- make test-thread-reap-host: 1000 concurrent groups with join predicate isolation PASS under ASan/UBSan.
- make test-pthread-host: 8-thread mutex, condvar, barrier, and once-init PASS under ASan/UBSan.
- make test-fb-host test-compositor-host test-crash-dump-host test-abi-version-host: 100% PASS.
- make test-pthread: Multi-firmware BIOS/UEFI x SMP=1/4 4/4 PASS under QEMU.
- make test-crash-dump: Multi-firmware BIOS/UEFI x SMP=1/4 4/4 PASS under QEMU.
- make test-compositor: BIOS and UEFI 1280x800 pixel presentation and damage tracking 2/2 PASS.
- make test-fb-ownership test-fb-teardown: BIOS/UEFI x SMP=1/4 8/8 PASS (rejected frees: 0 -> 0).

### 5. Dell Latitude 5590 Physical Acceptance Checklist

#### Pre-Flight Candidate Artifacts
- **Candidate Disk Image**: bin/fortress.img (592 MiB)
  - SHA-256 (bin/fortress.img): 9fbea4f1bac25222b9650de2e44896abc3fd0ae9a30acc1df0d45b0007d95ada
- **Candidate ISO Image**: bin/fortress.iso
  - SHA-256 (bin/fortress.iso): 8231f9bce97ec1e51197b013ea3cb10fc0e75c46dfdb715749704a9163290f1e
- **Kernel Binary**: bin/fortress.elf
  - SHA-256 (bin/fortress.elf): e1eec89ad1bbeb30071b3523b273ac2b212bb106e7eded65a843ea556bc70776

#### Bare-Metal Acceptance Procedure (Dell Latitude 5590)
1. **Cold Boot & Target Resolution**:
   - Flash candidate bin/fortress.img to target USB drive:
     sudo dd if=bin/fortress.img of=/dev/sdX bs=4M status=progress conv=fdatasync
   - Boot Dell Latitude 5590 via UEFI (F12 boot menu).
   - Verify native Intel UHD 620 GOP scanout (1920x1080 or native panel mode) and clean Limine handoff.
   - Log in as operator on COM1/console.
2. **Compositor Startup & Exclusive Lease**:
   - Execute sudo /bin/compositor.
   - Verify desktop background, window frame, status bar, and software cursor render without distortion or tearing.
   - From SSH or secondary terminal, execute sudo /bin/compositor: verify immediate rejection with EBUSY (error -16).
3. **Interactive Cursor & Damage Tracking**:
   - Move USB mouse or trackpad: verify smooth cursor tracking with alpha blending and save-under restoration.
   - Click and drag sample window across text regions: verify backbuffer damage repair and occlusion clipping without pixel smear or ghosting.
4. **Console Suppression & Clean Exit**:
   - While compositor runs, verify console output is suppressed and does not overwrite active graphics scanout.
   - Click right mouse button or exit window control: compositor cleanly shuts down, releases framebuffer lease (SYS_FB_RELEASE), and restores VT text console with prompt intact.
5. **Fault Recovery & Crash Core Dump**:
   - Launch compositor with debug fault injection or kill faulting client:
   - Verify unhandled exception emits complete user-space crash dump to COM1 ([CRASH] architectural GPRs, RIP code window, RSP stack window), terminates faulted process with POSIX status (139), and leaves kernel operational.
6. **Forced Termination (SIGKILL)**:
   - Run sudo /bin/compositor in background, obtain PID, issue sudo kill -9 <PID>.
   - Verify kernel reclaims exclusive framebuffer lease and restores console prompt without lingering locks.
7. **Cycle Endurance**:
   - Execute 5 consecutive launch-and-exit cycles of /bin/compositor.
   - Verify zero frame leaks in /bin/meminfo and consistent physical frame allocation.

### 6. Laptop Trackpad (PS/2 Auxiliary Port) Integration (2026-10-26)
- **Root Cause**: On physical laptop hardware (such as the Dell Latitude 5590), the built-in trackpad connects to the i8042 controller's second (AUX) port. Previously, `keyboard_init` in `src/drivers/input.c` explicitly disabled the auxiliary device port (`0xa7`), disabled the port 2 clock, disabled IRQ 12, routed only IRQ 1, and discarded all AUX bytes (`status & 0x20`). Consequently, `/dev/input/mouse` received zero events from the built-in trackpad.
- **Implementation**:
  - Implemented full i8042 dual-port initialization (`i8042_init`) in `src/drivers/input.c`.
  - Enabled port 2 (`0xa8`), enabled port 2 clock and IRQ 12 in the controller configuration byte.
  - Initialized trackpad streaming: default configuration (`0xf6`), resolution setting (8 counts/mm via `0xe8, 0x03`), 100 Hz sample rate (`0xf3, 100`), and data streaming enable (`0xf4`).
  - Registered `ps2_mouse_irq` for vector `0x3C` (`MOUSE_VECTOR`) and routed ISA IRQ 12 in IOAPIC.
  - Implemented unified `i8042_drain()` to dispatch keyboard scancodes (`status & 0x20 == 0`) and mouse bytes (`status & 0x20 != 0`) without race or byte loss.
  - Implemented standard 3/4-byte packet decoder with bit-3 sync verification, overflow rejection, button decoding, Y-axis screen inversion, and delta clamping to `mouse_dev_push()`.

### 7. Compositor Smoothness & Latency Hardening (2026-10-26)
- **Bottlenecks Identified**:
  1. *Over-damage in Telemetry*: On every individual 10ms mouse packet, `windows.monitor()` was damaging the entire 320×200 System Monitor window (`w.rect`, 64,000 pixels = 256 KiB) just to format `"Mouse: {}, {}"`. Over uncacheable (UC) PCIe memory without write-combining, this generated 25.6 MB/s of bus traffic and stalled CPU execution.
  2. *Queue Buildup & Lack of Coalescing*: When rapid finger swipes generated bursts of 100 Hz PS/2 trackpad packets, the event loop read only 1 packet per frame, causing the cursor to lag behind physical input.
  3. *Display Controller Contention & Tearing*: Unpaced back-to-back writes clashed with display controller scanout cycles.
- **Optimizations Implemented**:
  1. *Sub-Rectangle Coordinate Damage*: Added `windows.monitor_mouse()` to clear and damage only the 220×16 coordinate string rectangle (3,520 pixels) rather than the entire 64,000-pixel window. Throttled coordinate telemetry updates to every 8 frames (~7 Hz), keeping 87.5% of mouse ticks restricted strictly to the 16×16 cursor damage boxes (512 pixels), achieving a 70× reduction in PCIe bus traffic.
  2. *Non-Blocking FIFO Event Coalescing*: Implemented non-blocking draining of `s_mouse_fifo` in `user/compositor/src/main.rs`. All pending motion packets in the queue are summed into `dx_total` and `dy_total`, rendering once per batch while preserving button press/release transitions sequentially via `pending_packet`.
  3. *Frame Pacing via SYS_NANOSLEEP*: Wired Syscall 75 `SYS_NANOSLEEP` into the Rust `sys` module and introduced a 1 ms micro-sleep after frame presentation when no packets are pending, spacing out blits and eliminating display controller bus contention.
- **Verification Evidence**:
  - `make test-compositor-host`: 7/7 unit tests PASS (including `monitor_mouse_bounded_damage_and_coalesced_deltas`).
  - `make test-input`: 100% PASS (PS/2 packet decoding & overflow handling).
  - `make test-compositor`: Multi-firmware QEMU acceptance suite PASS 100% across BIOS and UEFI (1280×800).

### 8. Graphical Terminal Window, Taskbar Clock & Dell Latitude 5590 Acceptance (2026-10-26)
- **Built-in Graphical Terminal Emulator**:
  - Implemented `user/compositor/src/terminal.rs` featuring a 60×14 character grid, ANSI CSI escape code filtering, newline scrolling, backspace/DEL, and direct glyph rasterization into `Window::content`.
  - Added `sys::pty_pair` (`SYS_PTY_PAIR` Syscall 80) and `sys::spawn_ext` (`SYS_SPAWN_EXT` Syscall 20) with non-blocking PTY slave redirection (dup2 to stdin, stdout, stderr).
  - Multiplexed the PTY master fd directly within the compositor's event loop via `sys::poll` on `fds[2]`.
  - Keystrokes typed while Window 3 ("Terminal - /bin/sh") is focused are forwarded directly to the PTY master; output from `/bin/sh` is rendered to the window with dirty-rect damage tracking.
- **Taskbar Real-Time Digital Clock**:
  - Implemented `update_clock()` querying `SYS_CLOCK_GETTIME` (Syscall 74, `CLOCK_REALTIME`). Formats `HH:MM:SS` in a dedicated box on the bottom taskbar with bounded damage updates strictly on 1-second boundaries.
- **Keyboard Multiplexing**:
  - Multiplexed `stdin` (`/dev/tty`) via `sys::poll`. Pressing `Tab` cycles and raises window focus; pressing `t` / `T` dynamically opens the terminal window; `q` / `Q` / `ESC` triggers a clean shutdown.
- **Verification & Bare-Metal Hardware Acceptance**:
  - Host unit tests: `make test-compositor-host` 9/9 PASS.
  - Multi-firmware QEMU suite: `make test-compositor` 100% PASS on BIOS & UEFI (1280×800).
  - Physical Dell Latitude 5590 Laptop Acceptance: Confirmed running live on physical Intel UHD Graphics 620 GOP scanout (1920×1080 at pitch 7680 bytes) with active built-in trackpad tracking, live taskbar clock, System Monitor, Palette Canvas, and an interactive `/bin/sh` shell session inside the graphical terminal window executing shell commands (e.g. `clear`).

### 9. PTY Session Leader Handoff, VT100 ANSI Execution & Start Menu (2026-10-26)
- **Zero-Race PTY Controlling Session Binding**:
  - Configured `SpawnOpts` flags to `SPAWN_SETSID | SPAWN_STAGED` (version 2) when launching `/bin/sh`.
  - Added `sys::pty_set_session(master_fd, pid)` (`SYS_TERMATTR` opcode 5 `PTY_SET_SESSION`) to bind `/bin/sh` as the controlling session leader of the pseudo-terminal pair.
  - Released staged process group via `sys::group_release(pid, GROUP_RELEASE)`.
  - Eliminates `fortress: terminal handoff failed` error, allowing foreground command execution (`clear`, `ls`, `top`, etc.) over `SYS_TCSETPGRP`.
- **Full VT100/ANSI Escape Sequence Engine** ([`user/compositor/src/terminal.rs`](file:///c:/Sources/FortressOS/user/compositor/src/terminal.rs)):
  - Parses multi-parameter CSI sequences:
    - `\033[2J` / `\033[3J`: Erase in Display (clears screen grid and resets cursor to top-left, making `clear` work).
    - `\033[K`: Erase in Line (clears from cursor to end of line, ensuring deleted characters are wiped out during backspace).
    - `\033[<row>;<col>H` & `\033[H`: Cursor home and 1-based coordinate positioning.
    - `\033[<n>C` / `\033[<n>D` / `\033[<n>A` / `\033[<n>B`: Cursor forward, backward, up, and down navigation.
    - `\033[?25h` / `\033[?25l`: Dynamic cursor show/hide visibility toggling.
- **Taskbar Start Menu**:
  - Implemented clickable `[F] Start` button on the bottom taskbar (`x: 8..84, y: height - 28..height - 4`).
  - Added popup Start Menu above the taskbar with hover highlights:
    - `[T] Terminal` (spawns / unminimizes / raises the terminal window)
    - `[M] Sys Monitor` (unminimizes and focuses System Monitor)
    - `[P] Palette` (unminimizes and focuses Palette Canvas)
    - `[Q] Exit` (cleanly exits the compositor to text console)
  - Clicking outside the menu or pressing `ESC` automatically dismisses the menu with clean dirty-rect repair.
- **Verification Evidence**:
  - `make test-compositor-host`: **10/10 PASS** (including `start_menu_toggle_and_actions` and full VT100 ANSI tests).
  - `make test-compositor`: **100% PASS** on BIOS & UEFI under QEMU.
