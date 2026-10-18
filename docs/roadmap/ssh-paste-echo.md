# SSH prompt paste and echo correction

2026-10-18: user-reported PowerShell SSH screenshot showed literal `[200~`
paste-marker text, a multiline paste flattened into `whoami id hostname pwd`,
and an extra blank line around command execution after leaving a root shell.

The PTY's default canonical echo and the shell editor were both rendering
input. PTY echo prints the printable portion of escape sequences and echoes
Enter before the editor emits its own newline. The shell now snapshots terminal
attributes before editing, clears only TERM_ECHO, and restores the snapshot on
every return from the editor, including cancellation, EOF and input errors.
Job and password-reader settings are preserved; kernel PTY defaults are unchanged.

Bracketed paste now preserves newlines instead of replacing them with spaces.
CRLF is normalized to one newline, and standalone CR/LF are supported. The
single-line editor displays embedded newlines as literal `\n`, with matching
viewport/cursor column accounting, without executing them during rendering.
Existing paste review remains: the first Enter dismisses review; the second
submits the complete command list. Ctrl+C cancels it.

## Verification

- `python3 scripts/test_shell_host.py` in WSL Ubuntu-24.04: PASS under ASan/UBSan;
  real editor coverage includes retained LF boundaries and mixed CRLF/CR/LF
  paste, review before acceptance, existing editing/history/parser coverage.
- `python3 scripts/test_shell_identity_host.py`: PASS under ASan/UBSan for
  prompt identity, narrow terminals and hostname fallback.
- `make -j4`: PASS; rebuilt ISO/raw image, including GPT/EFI/ext4 image checks.
- `PLINK=/mnt/c/Sources/FortressOS/build/putty-interop/linux/usr/bin/plink
  python3 -u scripts/test_ssh.py`: BIOS/UEFI SMP=1 acceptance. The new real SSH
  PTY gate pastes two CRLF-separated commands, verifies visible `\n` review,
  no leaked `[200~`/`[201~` echo and no premature command execution, then
  requires consecutive exact output lines without a duplicated blank line.
  It repeats after entering and exiting an authenticated sudo root shell.
  Existing OpenSSH/PuTTY authentication, PTY, session-cap and teardown gates
  also run. Output: ignored `build/putty-interop/editor-acceptance.log`.

Generated private keys, disposable password files and secret-bearing disks
remain within the acceptance runner's temporary directory and are deleted.
No physical target was written. PowerShell and MobaXterm GUI physical retests
remain user acceptance; QEMU does not establish those client-specific results.
