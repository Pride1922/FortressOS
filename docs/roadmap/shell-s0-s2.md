# Shell S0–S2: terminal foundation, editing and RAM history

Implementation date: 2026-09-25. Design and approved scope:
[SHELL_DESIGN.md](../plans/SHELL_DESIGN.md). Physical Dell acceptance is pending.

## Delivered

- Shared syscall constants are separated from kernel IDT declarations.
- Shell I/O, builtin registry/help, the existing file editor, and the new command
  editor/UI are separate modules with explicit Makefile dependencies.
- `SYS_INPUT_READ` provides raw, non-echoed input with indefinite, nonblocking or
  bounded waits. Input IRQs publish only; the BSP timer performs the wakeup using
  the existing scheduler protocol. Input remains BSP-affine.
- PS/2 arrows, Home/End/Delete, Ctrl and AltGr produce complete terminal sequences.
  Queue insertion is all-or-none for synthesized keys. Detected queue/UART/keyboard
  loss invalidates the pending command instead of executing missing-byte input.
- `SYS_TERMCTL` reports geometry/loss/display generation and selects per-process
  output. Terminal writes bypass the kernel log and support a bounded CSI subset.
  Early boot, kernel and NMI output keep their independent path.
- The editor has a 4096-byte input limit, horizontal viewport, insertion/deletion,
  Ctrl+A/E/W/U/K/Y/L, Ctrl+C cancellation and empty-line Ctrl+D exit.
- Up/Down recall and restore drafts; Ctrl+R searches, Enter accepts for editing and
  a second Enter executes. Escape/Ctrl+G restores the pre-search draft.
- History is bounded by both 1000 entries and 256 KiB, suppresses consecutive
  duplicates, and supports `history`/`history clear`.
- Bracketed paste joins CR/LF/tab as spaces and requires explicit review with two
  Enter presses after its end marker. Ctrl+C also cancels an incomplete paste.
- Existing `run`, status/chaining, file editor and power operations remain supported.

## Explicit limits

History is RAM-only and disappears on shell restart/reboot. There is no new quoting,
cwd/completion/environment/pipeline/job-control implementation in this milestone.
Ctrl+C cancels prompt input; it does not yet signal a running child. Long input does
not raise the existing executable argv limits. Overflow retains the visible buffer
but requires Ctrl+C before execution resumes. Initial text support remains ASCII.

Default output mirrors framebuffer/UART using a conservative width when UART exists.
`terminal local` uses full framebuffer width; `terminal serial` selects serial output;
`terminal plain` avoids shell escape rendering. Keyboard/UART still share legacy
input: use one input source/operator at a time. An isolated Escape expires in about
100 ms, subject to scheduler latency. Normal idle reads remain asleep indefinitely.
Kernel output triggers repaint on the next input event, not a periodic shell wakeup.
Only bracketed paste is recognizable as paste; raw UART bytes are not distinguishable
from keystrokes. The embedded file editor retains its older editing interface.

## Verification

Run under WSL Ubuntu-24.04 at `/mnt/c/Sources/FortressOS`.

| Command | Result and scope |
| --- | --- |
| `make -j4` | PASS: strict kernel/user build, ISO/raw image, GPT/FAT/ext2 image verification |
| `make test-shell-host` | PASS: ASan/UBSan on actual editor, decoder/queue and framebuffer code; editing/search/history limits, paste, loss, hostile bytes, colors/cursor/erase bounds |
| `python3 scripts/test_shell.py bios` | PASS: real PS/2/UART, raw/timed reads, framebuffer cells/cursor, search/history/paste, log separation, spawn/wait, idle timer progress, restart/resource counts |
| `python3 scripts/test_shell.py uefi` | PASS: same checks with paired OVMF code/temporary vars |
| `python3 scripts/test_shell_no_uart.py` | PASS: UEFI 8 GiB, no UART, physical-device emulated keyboard, framebuffer text and screenshot, sleeping reader |
| `SHELL_TEST_CPUS=4 python3 scripts/test_shell.py bios` | PASS: full shell suite with four CPUs; shell/input remain BSP-affine |
| `SHELL_TEST_CPUS=8 python3 scripts/test_shell.py uefi` | PASS: full shell suite with eight CPUs; shell/input remain BSP-affine |
| `python3 scripts/test_power.py` | PASS: BIOS shutdown and reboot, QEMU exits cleanly |
| `python3 scripts/test_ext2_write.py` | PASS: BIOS and UEFI, three boots each, create/edit/save/readback/truncate/directory operations and offline `e2fsck -fn` after each boot, disposable NVMe fixture |
| `python3 scripts/test_smp_percpu.py` | PASS: BIOS/UEFI 1/4/8 CPUs, distinct GS/GDT/TSS/stacks, BSP isolation, AP #DF and real hardware-NMI injection in QEMU |

New shell logs include firmware and CPU count (`build/shell-<firmware>-<n>cpu.log`);
earlier runs used `build/shell-bios.log`/`shell-uefi.log`. Framebuffer evidence is
`build/shell-keyboard-only.png`. Existing storage and SMP runners retain their own
logs/JSON. These are generated local artifacts, not committed binary evidence.

The initial integration harness mistook prompt repaint for command completion; it
now preserves CR bytes while decoding and waits for a newline-prefixed fresh prompt.
The no-UART breakpoint now follows `input_read_timeout`. A separate orchestration
attempt ran independent make processes that collided while rebuilding the ISO; those
launches were discarded, the image rebuilt once, and scripts rerun successfully
against it. Do not run separate image-producing make processes concurrently.

No syscall entry/return assembly, lock ranks, VMM ownership, storage authorization
or raw-write fixture policy changed. Compiler stack-usage files show the largest
individual shell frame is the existing `cat` frame (576 bytes); the editor/history
arenas are in BSS, not on the one-page user stack. Existing call chains remain well
below that stack budget. The kernel input wait retains its stack until resumption.

## Dell Latitude 5590 acceptance checklist

Use the newly built `bin/fortress.img` and the normal read-only boot entry. This
milestone's keyboard/history tests need no writable filesystem. Record the commit,
firmware, keyboard layout and any unexpected key output; photos of failures help.

1. **Startup and cursor:** boot to `fortress>`, run `help`, `ls /bin`, and
   `run /bin/hello`. The cursor should be visible and output should remain readable.
   If a detected UART limits the viewport, run `terminal local` to use the full screen.
2. **Insertion and deletion:** type `echo ac`, press Left, type `b`, then Enter.
   Expect `abc`. Recall with Up, use Home/End and Left/Right, and use Delete versus
   Backspace on characters in the middle. No adjacent text should disappear merely
   because the cursor moves.
3. **Ctrl bindings:** type `echo one two`; Ctrl+W removes `two`, Ctrl+Y restores it.
   Ctrl+A/E moves to the start/end; Ctrl+U removes text before the cursor; Ctrl+K
   removes text after it. Ctrl+L clears and redraws without losing the command.
   Ctrl+C cancels a draft and gives a fresh prompt.
4. **History/draft:** run `echo first`, then `echo second`. Type `echo draft` without
   Enter, use Up/Down through history, and Down back to the draft. Run `history` and
   check entries. `history clear` removes the earlier entries.
5. **Search:** run `echo first` again. Ctrl+R, type `first`, then Enter: the command
   is on the editable line but has not run. Enter again executes it. Repeat starting
   with a draft and cancel search with Escape or Ctrl+G; the original draft returns.
6. **Long line and bottom edge:** type `echo ` and hold a letter until the command
   exceeds the old 191-byte limit (around 250–300 characters). Move Home/End and edit
   the middle. The viewport should scroll horizontally and Enter prints the full
   argument. Repeat near the bottom of the screen. If you deliberately exceed 4096
   bytes, expect `[lost/full: Ctrl+C]`; Enter must not run a truncated command.
7. **Belgian layout:** `layout azerty`; verify H4 Caps Lock top-row digits 1–0 and
   ordinary Shift/letter behavior. Try the combinations below, then release AltGr
   and verify normal letters still work. Switch to `layout us` and back to confirm
   modifier state remains usable. Report the actual printed legend/key used if a
   combination differs from your keyboard.

   | Character | Implemented physical combination (Belgian legends from KBDBE.DLL) |
   | --- | --- |
   | `|` | AltGr+`&` (`1`) |
   | `@` | AltGr+`é` (`2`) |
   | `#` | AltGr+`"` (`3`) |
   | `{` | AltGr+`'` (`4`); also AltGr+`ç` (`9`) |
   | `[` | AltGr+`(` (`5`); also AltGr+`^` |
   | `^` | AltGr+`§` (`6`) |
   | `}` | AltGr+`à` (`0`) |
   | `]` | AltGr+`$` |
   | `` ` `` | AltGr+`µ` |
   | `~` | AltGr+`=` |
   | `\` | AltGr+`<` (ISO key next to Left Shift); also AltGr+`)` |

8. **Numeric Keypad (NumPad):** verify typing digits 0–9, `.`, `+`, `-`, `*`, `/`,
   and Keypad Enter directly from the keypad with NumLock active (default on boot).
   Press NumLock to toggle and verify navigation events (Up/Down/Left/Right/Home/End/Ins/Del).
9. **Recovery and restart:** Escape alone must not wedge the next key. Ctrl+D on
   an empty prompt or `exit` restarts the supervised shell. History should be empty
   after restart (apart from newly entered commands). Finish with `shutdown`, and
   separately verify `reboot` still works.

Optional serial-terminal test: paste a bracketed multiline `echo` command. Nothing
executes while pasting; the review prompt appears, the first Enter accepts it and
the second executes. This is not a clipboard test on the framebuffer-only Dell.

Do not mark hardware acceptance complete until these observations are supplied.
