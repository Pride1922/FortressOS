# Nano SSH viewport correction (2026-10-17)

The reported dark editor screen showed a shortcut bar and empty-line markers
while the title and typed text were absent. A focused real OpenSSH reproduction
confirmed nano rendered **50 rows into a 30-row SSH terminal**. `SYS_TERMCTL`
describes the local framebuffer; using its height caused the SSH terminal to
scroll the title and first text rows away. The text was present in the wire
output, so another foreground-color change would not fix this failure.

Nano now reads `SYS_TERMATTR(1, TERM_GET_WINSIZE)` for its output PTY at startup
and during its existing periodic refresh, falling back to the existing console
information when the fd does not support a PTY window query. SIGWINCH is ignored
because dimensions are polled. Header, status and shortcut bars are clipped to
the window width so long filenames or small windows cannot add wrapped rows.
The header also resets rendition and selects bright white before inversion.
No kernel syscall, signal or PTY ownership behavior changes.

## Verification

- `python3 scripts/test_nano_ssh.py bios` against the pre-fix release image:
  expected failure `(50, 30)` at the frame-height assertion. Evidence retained
  in `build/nano-ssh/bios-5u_q36yh/`, with exact QEMU/SSH argv and raw output.
- `make test-nano-host`: ASan/UBSan PASS, including narrow-frame coverage at
  20/40/60/80 columns with long title/status strings and no wrapped rows.
  The older badge-content test now uses 120 columns to fit all badges.
- `make test-nano-ssh`: BIOS and UEFI PASS. Real OpenSSH password authentication,
  negotiated 100×30 viewport, visible title and typed text on the intended rows,
  explicit bright-white text, live resize to 60×18 without termination/wrapping,
  Ctrl+O save and Ctrl+X return to shell, followed by an independently computed
  SHA-256 comparison for `NanoSSHVisible\n` read from the saved file.
  Passing evidence: `build/nano-ssh/bios-rs8efili/` and
  `build/nano-ssh/uefi-8hrr25es/`; `screen-*` files are decoded ANSI row text,
  not native Windows Terminal screenshots.
- `make -j4`: strict freestanding build and ISO/raw-image integrity checks PASS.
- `make test-nano`: existing local-console BIOS/UEFI acceptance PASS, including
  save/reopen/discard, read-only and piped input, line numbers, goto, replace,
  undo/redo, multi-buffer switching and nanorc/regex coverage.

The new test copies regular NVMe and ISO fixtures, records exact argv/raw SSH
output, uses copied OVMF variables and always terminates/reaps OpenSSH and QEMU.
It attaches no physical disks. Fixture setup is shared with SSH acceptance.
Initial harness iterations exposed long-command observation/public-key setup
and prompt/readback assumptions; their failed artifacts remain in the same
build directory. The final test uses password authentication, terminal-state
assertions and file hashes rather than requiring a noninteractive exec channel
to omit its shell banner. This is QEMU/OpenSSH evidence, not physical laptop or
native Windows Terminal acceptance.
