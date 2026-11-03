# Dell gterm entry-stack crash — 2026-11-03

The Dell log records `/bin/gterm` PID 25 terminating with SIGSEGV/#PF at
RIP `0x40829e`, CR2/RSP `0x7fffefffcf78`. Instruction bytes are
`48 c7 04 24 00 00 00 00`, a write to the stack during the entry prologue.
The pre-fix `build/gterm.elf` resolves this address to `rust_main`; its disassembly
matches the reported bytes exactly. Repeated 4096-byte stack probes precede the
first heap-initialization call. The local 16384-byte PTY read buffer causes that
entry frame to exhaust the bounded process stack before the UI starts.

The PTY buffer now uses a heap vector after successful heap initialization.
Its size, read batching, processing and polling behavior are unchanged. Stack
mappings, guard behavior and process/kernel privilege boundaries are unchanged.
Do not replace this with `Box::new([0; 16384])`, which can create a large temporary.

Verification:

- Freestanding release build using the compositor's existing target/linker config.
- Actual binary entry frame: **568 bytes**, within the 1024-byte budget.
- `scripts/check_gterm_stack.py` rejects the old crashing binary and accepts the
  fixed release binary; `make test-gterm-stack` checks the built artifact.
- `make test-compositor-host`: **80/80 PASS**, including gterm color, scrollback,
  selection, search and clipboard tests. Host execution alone cannot catch this
  crash because the host stack is larger, hence the binary stack gate.
- No physical or full graphical guest acceptance of this fix is claimed yet.

Evidence: `build/gterm-stack-fix/`, including the supplied Dell log. No new
installer or repository package is published as part of this fix.

The same Dell log shows successful NVMe initialization, consistent GPT copies,
installed-root admission and no reported ext4 read failures. Bitmap read cycles
`4881517942` at `1897212679 Hz` represent about **2.57 accumulated seconds**;
checksum cycles `709402710` represent about **0.374 accumulated seconds**. These
are on-demand accumulated counters, not a timestamped total mount duration.
The three user-measured cold boots are 21, 20 and 19.85 seconds (median 20),
and installation took 135 seconds. Per-stage timing is still needed to attribute
the remaining boot delay.

The supplied commands also show root-owned mode 04755 `/bin/sudo` and successful
absolute-path password authentication/elevation. Earlier plain `sudo` failures
are unresolved: the same-command plain/absolute comparison and shell resolution
output are still needed to distinguish lookup from a transient account snapshot.


## Authenticated Dell SSH follow-up

Authenticated SSH to the same running Dell confirms real UID 1000 and wheel
membership. `type sudo` resolves `/bin/sudo`, and PATH is `/bin:/sbin`.
Plain `sudo /bin/id` authenticates and reports UID/GID 0. Plain `sudo -s` also
opens a root shell and `id` confirms elevation. The earlier startup failure
remains unexplained; this rules out a persistent lookup or installed-mode defect,
without proving which early check failed. No credential policy was changed.

The boot-log and base-confirm one-shot services both exited cleanly with status
0. Stopped state for those completed services is expected. `battery` reports
"Battery telemetry read failed" on the physical laptop.

The same boot still reports no ext4 read failures/anomalies. Its bitmap counters
increased from 102301696 bytes / 4881517942 cycles in the user's capture to
149012480 bytes / 5495098686 cycles after further commands. Checksum counters
also increased from 47778052 to 54187652 bytes. This directly demonstrates that
the counters include post-boot activity; they cannot isolate mount duration.
The original boot wall-clock marker remained `2026-11-03 20:34:09 UTC`.

Only diagnostic reads and authenticated shells were used. No reboot, filesystem
repair, permission changes, account changes or deployment was performed. The
SSH session was closed. Sanitized captures are retained under
`build/gterm-stack-fix/evidence/dell-ssh-*.log`.
