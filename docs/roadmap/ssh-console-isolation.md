# SSH prompt isolation from the physical console

2026-10-18: typing on the laptop caused an open PowerShell SSH prompt to
insert a newline. SYS_TERMCTL returned global physical-console generation and
input-drop counters even for processes using a PTY. The shell interpreted the
local display change as output interrupting its own prompt and redrew it on a
new line. This was a display notification leak, not an injected Enter key.

SYS_TERMCTL now selects the process's PTY using the same descriptor order as
the existing `/dev/tty` lookup. It queries that PTY's window dimensions and
returns zero for the unsupported PTY display-generation counter and the
inapplicable physical-input drop counter. PTY input uses backpressure.
Physical-terminal queries retain their existing counters and redraw behavior.
No syscall layout, pointer validation, PTY locking or scheduling policy changes.
PTY background-output generation tracking is not implemented by this fix.

## Acceptance

The actual SSH test now runs commands through the independent UART console
while the SSH prompt is idle and while it holds an unsubmitted command.
It requires zero SSH output after local activity, then submits the SSH draft
and verifies its exact output. This exercises the real kernel syscall and
shell polling loop rather than a mocked generation value.

- Before the fix, BIOS acceptance failed with the expected captured SSH
  `\r\n` and prompt redraw. Log:
  `build/putty-interop/console-isolation-before.log` (ignored).
- `wsl -d Ubuntu-24.04 -- make -j4`: PASS; kernel, ISO and raw image rebuilt;
  raw-image GPT/EFI/ext4 checks pass.
- In WSL Ubuntu-24.04, run
  `PLINK=/mnt/c/Sources/FortressOS/build/putty-interop/linux/usr/bin/plink python3 -u scripts/test_ssh.py`.
  PASS on BIOS and UEFI at SMP=1. Results are recorded in ignored
  `build/putty-interop/console-isolation-after.log`, including the existing
  paste/echo, nested sudo, OpenSSH/PuTTY, concurrency and teardown gates.

Fixtures use temporary NVMe images, ISO copies, keys and passwords; the runner
deletes them and terminates QEMU. No physical device was written. Laptop and
PowerShell physical retesting remains user acceptance.
