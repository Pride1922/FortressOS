# SSH large-output progress fixes

2026-10-18: a user reported that root `dmesg` stalled a PowerShell SSH session.
Two independent output-path defects were reproduced and corrected.

1. The daemon's channel-reply wire buffer allowed only 4096 bytes, while the
   PTY bridge can supply 4096 data bytes before channel fields, padding and
   authentication overhead. Framing errors were ignored, as were short socket
   sends. The buffer now accommodates the bounded maximum SSH frame; framing
   errors terminate the session, sends drain the complete frame, and zero/error
   sends take the existing session cleanup path instead of discarding bytes.
2. PTY newline expansion requires two output-ring slots for CRLF. With one
   slot free, the write loop could not progress but its wait predicate returned
   ready immediately. At masked-IRQ syscall entry this can spin without letting
   the master run. The predicate now waits for the actual one- or two-slot
   requirement, or master closure. It still only acquire-loads published state
   under scheduler exclusion; the PTY lock is released before waiting. No lock
   rank, scheduler API, IRQ transition or ring-ownership contract changes.

## Evidence

- Before the first fix, the actual-worker host adapter failed to decode its
  4096-byte encrypted channel reply. Ignored log:
  `build/putty-interop/large-output-before.log`.
- With framing corrected, both `dmesg` forms completed but a 104 KiB stream
  stalled. Retained ignored logs `large-output-after.log` and
  `large-output-debug.log` in the same directory show the intermediate failure.
- `python3 scripts/test_ssh_transport_host.py` in WSL Ubuntu-24.04: PASS under
  ASan/UBSan. Actual worker framing/encryption handles all 4096 data bytes,
  simulated 137-byte short sends, zero progress and socket failure.
- `python3 scripts/test_ssh_pty_host.py`: PASS under ASan/UBSan. Additional
  actual-kernel PTY coverage checks the one-slot CRLF wait, ring wrap, a
  nonblocking retry, a single-byte write, existing CR prefix and master closure.
  Host locks/scheduler are adapters; this alone makes no IRQ or SMP claim.
- `make -j4`: PASS; kernel, daemon, ISO and raw image rebuilt, image checks pass.
- `PLINK=/mnt/c/Sources/FortressOS/build/putty-interop/linux/usr/bin/plink python3 -u scripts/test_ssh.py`:
  PASS on BIOS and UEFI at SMP=1. Root builtin `dmesg` and `/bin/dmesg` return
  kernel network logs and completion markers; a 104 KiB ASCII fixture crosses
  multiple ring/packet boundaries and matches the entire CRLF-transformed
  byte stream. The connection then exits root and repeats paste checks.
  Existing authentication, local-console isolation, hidden sudo password,
  session-cap and teardown gates also pass. Ignored log:
  `build/putty-interop/large-output-final.log`.

Only disposable fixture credentials and disks are used and removed by the
runner. No physical writes or new idle-timeout behavior. Physical PowerShell
retesting remains user acceptance.
