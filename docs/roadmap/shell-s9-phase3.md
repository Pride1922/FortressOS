# Shell S9 Phase 3 — /bin/top (The Finale)

Status: COMPLETE (2026-09-30). Implemented and verified on host, QEMU (BIOS & UEFI, SMP=1 and SMP=4), and bare-metal Dell hardware.

## 1. Summary of Changes

Phase 3 implements the `/bin/top` standalone Ring 3 user program, completing the Shell S9 — System Introspection milestone.
`top` requires no new syscalls: it builds a live, interactive system view purely on top of the existing `SYS_PROCINFO` (36), `SYS_SYSINFO` (37), and console terminal control (`SYS_TERMCTL`, `SYS_INPUT_READ`, `SYS_WRITE`).

### Architecture & Refresh Loop
- **Snapshots**: Maintains two bounded static BSS snapshots of the process list (`s_prev_procs` and `s_curr_procs`, capacity `PROC_INFO_MAX = 64`) and system info (`s_prev_sysinfo` and `s_curr_sysinfo`).
- **PID Churn & Deduplication**:
  - Within each pass, `SYS_PROCINFO(0, 1, ...)` results are deduplicated by PID to handle table churn.
  - A PID with no prior sample (or on the initial frame) reports CPU% as `unknown`.
  - Missing PIDs in the current cycle do not corrupt previous state or crash enumeration.
- **CPU% Math**:
  - Formula: $\text{CPU\%} = \frac{100 \times \Delta(\text{cpu\_ticks})}{\Delta(\text{uptime\_ticks})}$ (normalized to one CPU core).
  - Computed in permille with 1 decimal place format (`permille / 10 . permille % 10`).
  - Overflow-safe integer math protecting against 64-bit multiplications.
  - Rejects zero intervals ($\Delta t = 0$), reversed time ($\Delta t < 0$), and decreasing CPU tick counters by safely assigning `unknown` without arithmetic wrap-around.
- **Stable Sorting & Deterministic Tie-breaking**:
  - Primary sort key: CPU% descending (known percentages before `unknown`).
  - Tie-breaking: PID ascending (lower PID first).
- **Stack Budget**:
  - All large structures (snapshots, line buffers, screen buffers) are allocated in static BSS.
  - Frame stack usage: `top_render_frame` uses 96 bytes, `top_main` uses 128 bytes (verified via `-fstack-usage` in `build/top.su`), well below the 512-byte Ring 3 stack budget.

### Terminal & Redirection Behavior
- **Interactive Terminal**:
  - Clears screen and homes cursor before each redraw using console ANSI sequences (`\033[2J\033[H`).
  - Formatted columns:
    ```text
    FortressOS top — up 00:04:12
    CPUs: 1   Tasks: 4
    Mem:  2048 MiB total, 1980 free, 68 used
      PID  STATE       CPU%  NAME
       61  RUNNING     49.0  /bin/hello
       63  RUNNING      1.8  /bin/top
       57  RUNNING      0.0  shell
       62  STOPPED      0.0  /bin/cat
    ```
  - Bounded input wait (`sys_input_read(..., 1000)`):
    - Redraws automatically every 1000 ms.
    - Quits cleanly on `q` or `Q`.
    - Handles `Ctrl-C` (`SIGINT`) with immediate prompt recovery.
    - Preserves job control: on `Ctrl-Z` (`SIGTSTP`), top stops cleanly. On `bg`, top stops on `SIGTTIN` without stealing the terminal. On `fg`, top resumes in foreground and redraws cleanly.
- **Redirected / Plain Mode**:
  - When stdout is not a terminal (`SYS_TERMCTL(TERM_ISATTY)` returns 0) or terminal is set to `TERM_PLAIN`:
  - Produces a bounded one-shot summary.
  - Suppresses all control sequences (no `\033[2J\033[H`).
  - Exits immediately with 0.
- **Dimension Capping**:
  - Respects terminal dimensions queried via `SYS_TERMCTL(TERM_GET)`.
  - Truncates lines exceeding `term.cols`.
  - Caps displayed rows to `term.rows` (leaves space for header).

## 2. Verification Evidence

### Host Unit Test
- Target: `make test-s9-top-host` (`tests/top_host.c`, compiled with ASan and UBSan)
- Coverage:
  - Delta matching, initial cycle, and PID churn.
  - Decreasing counter rejection, zero interval, and reversed interval -> `unknown`.
  - Overflow bounds on arithmetic.
  - Sorting by CPU% descending and deterministic tie-breaking by PID ascending.
  - Frame rendering, escape codes, and row/column dimension capping.
  - `top_main` interactive, redirected, and write failure handling.
- Result: **PASS** (zero leaks, zero sanitizer warnings).

### QEMU Integration & Acceptance Test
- Target: `make test-s9-top` (`scripts/test_s9_top.py`)
- Environment: Real Ring 3 shell, disposable ISO, no data disks, argv preflight checking for forbidden device injection.
- Matrix:
  - **BIOS SMP=1**: All 5 stages PASS.
  - **BIOS SMP=4**: All 5 stages PASS.
  - **UEFI SMP=1**: All 5 stages PASS.
  - **UEFI SMP=4**: All 5 stages PASS.
- Stages Verified:
  1. *Stage 1 (Redirected one-shot)*: `/bin/top | /bin/head -n 5` produces header/rows without `\x1b[2J`, prompt recovers.
  2. *Stage 2 (Interactive timed refresh & q)*: At least two full refresh cycles without keystrokes; `q` exits cleanly, prompt recovers.
  3. *Stage 3 (Busy vs stopped CPU% deltas)*:
     - `/bin/hello --spin &` achieves nonzero CPU% (e.g. ~49.0%–100%).
     - `/bin/cat &` + `kill %2 STOP` achieves `0.0%` CPU% and `STOPPED` state.
  4. *Stage 4 (Ctrl-C quit)*: `\x03` immediately terminates top, prompt recovers.
  5. *Stage 5 (Ctrl-Z, bg, fg lifecycle)*: `\x1a` stops top (`[n] Stopped`), `bg` resumes in background, `fg` brings top to foreground with clean redraw, `q` exits cleanly.

### NMI Regression Guard
- Target: `make test-nmi`
- Coverage: 28 exact-boundary NMIs + 24 exact-boundary sigreturn NMIs across 4 rounds on both BIOS and UEFI.
- Result: **PASS**.

## 3. Physical Hardware Acceptance: Dell Bare-Metal

User-supplied testing confirms bare-metal hardware operation on Dell hardware booted via UEFI from USB:

- **Introspection Suite Verified**: All three Shell S9 user utilities (`/bin/ps`, `/bin/sysinfo`, `/bin/top`) confirmed fully functional in Ring 3 on physical hardware.
- **Live Terminal Redraw**: `/bin/top` executes live in the interactive framebuffer console:
  - Header uptime advances monotonically at 100 Hz in real time without core scaling.
  - Process list updates dynamically with accurate task counts.
  - CPU% deltas calculate smoothly across sample intervals.
  - Clean ANSI frame clearing and cursor repositioning without visual artifacts or screen corruption.
  - Interactive keystroke handling (`q` exit) returns cleanly to the shell prompt on physical PS/2 keyboard.

### Evidence Boundary
These observations represent physical hardware validation on bare-metal Dell hardware confirming `ps`, `sysinfo`, and `top` operational in Ring 3. User-confirmed live hardware execution completes the final acceptance criteria for the Shell S9 milestone across both automated test harnesses and physical hardware.

