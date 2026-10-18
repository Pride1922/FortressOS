# Watchtower lifecycle acceptance — bounded QEMU campaign

2026-10-18. Real Ring 3 supervisor, CLI, crond, sshd and shutdown/reboot tools
from the stack-corrected live image; no syscall mocks in this campaign.

Invocation (WSL Ubuntu-24.04):

```sh
python3 scripts/test_watchtower_lifecycle.py
```

Repeatable target: `make test-watchtower-lifecycle`.

| Firmware | CPUs | Service/SSH lifecycle | Final action | Result |
| --- | --- | --- | --- | --- |
| BIOS | 1 | Gates below | Remote shutdown; QEMU powers off with exit 0 | PASS |
| BIOS | 4 | Gates below | Remote reboot; fresh login and shell command | PASS |
| UEFI | 1 | Gates below | Remote shutdown; QEMU powers off with exit 0 | PASS |
| UEFI | 4 | Gates below | Remote reboot; fresh login and shell command | PASS |

Each scenario verifies:

- Real service stop reaches stopped and stays stopped; manual start/restart
  reaches running with a different PID.
- SIGKILL of crond triggers automatic recovery. Further crashes consume the
  configured three-restart budget; the next crash reaches failed and remains
  failed. An explicit restart resets the count to zero and restores running.
- A SIGSTOP-stalled crond is stopped through service control; the supervisor
  escalates to SIGKILL, confirmed by the reported last termination signal 9.
- Temporary live password and RAM host-key setup; authenticated OpenSSH login.
- Active SSH sessions receive RFC 4253 disconnect notifications when the
  supervised daemon is stopped or restarted, observed through OpenSSH's
  `Received disconnect` diagnostic. Clients exit within the bounded wait.
- After stop, `ps` reports no `/sbin/sshd` monitor or worker. New connections
  authenticate after service start and restart, with the monitor PID changed.
- A normal interactive SSH session runs privileged shutdown/reboot, receives
  the wire disconnect, then QEMU powers off or returns through a fresh boot to
  a usable login/shell. Shutdown and reboot are split across SMP=1 and SMP=4
  respectively; every firmware/CPU/action Cartesian combination is not claimed.

The script bounds process waits and kills remaining guest/client processes on
failure. It attaches the existing read-only ISO, local QEMU user networking and
disposable OVMF variables, with no data disks. Passwords and private keys exist
only in disposable guest RAM; askpass reads a synthetic password from the host
test environment. Neither private keys nor secret-bearing disks are retained.
Release IMG/ISO/initramfs hashes are checked unchanged after the campaign.

Tested live IMG SHA-256:
`b7c818279315f5b7187ca73961bed589024c720ae1c9abd8a8526df88b5e8dd5`.
Tested ISO SHA-256:
`8693fb9bb3876182b2f97d7ae2979a2095a7e1fe84ac89a7c962d7b3e1e7b7aa`.
No new OS code or USB image was required for this lifecycle campaign.

## Discovered boundary: interactive prompts in SSH exec mode

An initial shutdown probe used SSH exec requests with interactive sudo and
timed out. Code inspection found that the current exec implementation feeds
the command followed by `exit` into an interactive shell through its PTY.
Prompt-reading commands can consume that queued `exit` as input. A separate
early harness error also used `/bin/shutdown` rather than `/sbin/shutdown`.
Neither attempt establishes a shutdown-coordinator failure.

The accepted shutdown/reboot probes instead open ordinary interactive SSH
sessions, wait for the hidden sudo-password prompt, and then submit input,
matching the Dell MobaXterm/PuTTY workflow. SSH exec requests containing
interactive prompts remain a known limitation requiring a separate correction
and regression test; they are not covered by this acceptance claim.

## Dell physical follow-up and exclusions

The user confirmed bounded Dell startup/control, SSH login and scripting smoke
acceptance. On 2026-10-18 the user also confirmed these physical power checks:

1. SSH `sudo shutdown`: screenshot shows disconnect reason 11 with description
   `System going down`; user confirms the Dell powers off. PASS.
2. Reboot: user reports the same disconnect display and confirms the Dell
   actually reboots. PASS by user report; reboot-command output was not captured.

Live passwords and host identities are intentionally discarded across reboot.
No installed-root persistence or automatic post-reboot SSH login is implied.
Failed-readiness recovery, forking-service
edge cases, concurrent/stale control-mailbox robustness, log rotation durability
and exhaustive worker/resource accounting are not closed by this bounded matrix.

## Disconnect-message staircase investigation

The shutdown/reboot screenshot shows normal FortressOS command output followed
by three stepped diagnostic lines. Investigation found:

- The encrypted worker's disconnect code in `user/sshd/sshd.c` supplies the
  one-line ASCII description `System going down`, with no CR, LF or escape
  bytes. The reason is 11 and the language tag is empty. The surrounding
  `Remote side sent disconnect message` / `type 11 (by application)` strings
  are absent from server output and are generated by the client.
- The kernel PTY slave writer converts lone LF to CRLF (and avoids duplicating
  CR in existing CRLF), so normal command output already has terminal newlines.
- No LNM `ESC [ 20 h/l` mode changes are emitted by the shell/SSH implementation.
  The earlier suggestion of missing restoration of that mode is unsupported.
- PuTTY 0.83 primary source `ssh/transport2.c`, lines 388–394, constructs the
  exact three-line diagnostic using bare LF separators. Its Windows frontend
  normally displays the fatal message in a message box; an embedded client
  rendering it directly into a terminal can show the observed stepped layout.
  This matches the MobaXterm screenshot, but its proprietary rendering path
  was not independently instrumented.

Primary sources inspected:
[PuTTY 0.83 source archive](https://the.earth.li/~sgtatham/putty/0.83/putty-0.83.tar.gz),
`ssh/transport2.c`, `ssh/ssh.c`, `windows/window.c`, `terminal/terminal.c`;
[PuTTY terminal newline documentation](https://the.earth.li/~sgtatham/putty/0.83/htmldoc/Chapter4.html#config-crlf).
The manual describes this exact stepped display for LF without CR and documents
the per-session Terminal option `Implicit CR in every LF` as a workaround.
That setting is verified for PuTTY; the equivalent MobaXterm control is not
established here.

Conclusion: server disconnect content is clean; the evidence points to local
client diagnostic rendering. No FortressOS terminal-reset escape, added control
characters in the disconnect reason, or removal of the disconnect packet is
warranted. No OS code or new image was produced for this investigation. Normal
disconnect/power behavior is accepted; MobaXterm diagnostic alignment remains
a cosmetic client-compatibility issue, without a claimed client-side fix.
