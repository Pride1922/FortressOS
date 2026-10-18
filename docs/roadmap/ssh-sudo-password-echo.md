# Hidden sudo passwords on SSH PTYs

2026-10-18: user confirmed that the `sudo password:` prompt inside a
PowerShell SSH session displayed the actual password. The sudo reader did not
write password bytes itself, but left the PTY's TERM_ECHO flag enabled.

Sudo now snapshots the controlling terminal attributes, clears only TERM_ECHO,
and completes that change before printing the password prompt. It restores the
snapshot after reading, including interrupted/failed reads and rejected
authentication. GET/SET failures before the prompt fail authentication without
requesting or consuming a password; restoration failure also rejects elevation.
Existing password-buffer wiping, credential checks and passwordless policy stay
in place. No global PTY defaults or kernel ABI changes are needed.

## Verification

- WSL Ubuntu-24.04: `python3 scripts/test_perm_spawn_host.py` PASS under
  ASan/UBSan. Actual sudo adapters verify disabled echo at prompt publication,
  attribute restoration, interrupted reads, and fail-closed TERM_GET/TERM_SET
  errors with no password prompt or read. Existing credential tests pass.
- `make -j4`: PASS; rebuilt ISO/raw image and passed GPT/EFI/ext4 image checks.
- `PLINK=/mnt/c/Sources/FortressOS/build/putty-interop/linux/usr/bin/plink python3 -u scripts/test_ssh.py`
  exercises rejected and successful sudo authentication over a real SSH PTY
  and checks that neither disposable password appears in the returned bytes.
  The same connection exits its root shell and repeats paste/echo checks.
  PASS on BIOS and UEFI at SMP=1. Full results are in ignored
  `build/putty-interop/sudo-noecho-acceptance.log`.

The test uses disposable fixture credentials, deletes private keys and disks,
and writes no physical target. The updated image still needs user acceptance
on the laptop using PowerShell.
