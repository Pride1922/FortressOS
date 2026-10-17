# Authenticated background sudo launch (2026-10-17)

Two behaviors explained the reported SSH startup problem:

- `sudo /sbin/sshd &` runs sudo itself in a background job. A password read
  correctly stops it through existing terminal SIGTTIN enforcement. The same
  launch works on passwordless live media. Direct foreground
  `sudo /sbin/sshd` already starts the server successfully.
- An ordinary `/sbin/sshd &` job inside a root shell is tracked by that shell.
  Exiting the shell sends HUP/CONT and ultimately KILL to remaining jobs. This
  is existing shell-exit behavior, not a daemon service lifetime guarantee.

`sudo -b command [args]` now authenticates in the foreground using the existing
wheel/password/account-generation checks. After the credential transition it
spawns the command with the existing v2 `SPAWN_SETSID` API and stdin from
`/dev/null`. Output/error descriptors remain inherited. Sudo prints the child
PID and returns without waiting; success means spawned, not service readiness.
The separate session keeps the daemon outside shell-exit job cleanup. No kernel
job-control, synchronization or terminal-ownership contracts change. Ordinary
foreground sudo now also retries an interrupted child wait.

Use `sudo -b /sbin/sshd` without an additional ampersand. SSH's configuration,
host-key, account and strong-randomness startup gates remain intact. There is no
new password cache or authentication bypass.

## Verification

`wsl -d Ubuntu-24.04 -- make test-sshd-sudo` runs seven disposable SMP=1 boots:
BIOS foreground authenticated sudo, expected password/background stop,
passwordless background launch, and BIOS/UEFI detached launch both directly
from operator and from a root shell that subsequently exits. Detached operator
cases first reject an incorrect password and assert that no startup message
appears. Successful cases require the listening banner and an authentic
OpenSSH `ssh-keyscan` Ed25519 key exchange; root-shell cases perform the exchange
after returning to the ordinary user's prompt.

Result: **7/7 PASS**, including both firmware modes for detached launches.
The final `make -j4` and image integrity checks pass; ISO and raw IMG rebuilt.

The runner uses copied regular NVMe/ISO fixtures, copied OVMF variables, validates
the QEMU disk arguments and always terminates/reaps QEMU. Serial logs, exact
argv JSON and keyscan outputs remain under `build/sshd-sudo/`; no physical device
paths are attached. It uses the existing SSH acceptance fixture preparation.
This gate does not replace the full SSH session/authentication/teardown matrix
or claim laptop acceptance. `scripts/test_sudo.py` additionally has durable
non-wheel and wrong-password `-b` cases; that full suite was not rerun here.
