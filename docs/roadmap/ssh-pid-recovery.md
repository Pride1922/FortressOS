# PID-based SSH session recovery

2026-10-18: the user could not clear a hung SSH session without rebooting.
`jobs` already exists in the current shell, but tracks only that shell's owned
jobs; a daemon's worker is not one of them. The prior kill builtin accepted only
job selectors and there was no executable for `sudo kill PID`.

The builtin now supports a positive PID and the existing `%job` syntax. A new
`/bin/kill` executable makes PID signals available through sudo. Shared bounded
parsing accepts `kill TARGET [SIGNAL]`, `kill -SIGNAL TARGET` and
`kill -s SIGNAL TARGET`, defaulting to TERM. Supported symbolic signals and
optional SIG prefixes retain the existing mapping; signal 0 probes permission
and existence. Zero/negative PID selectors and overflow are rejected before
signaling. The executable rejects job selectors because it has no parent-shell
job table. Kernel signal authorization is unchanged.

On the laptop, use `ps -o pid,ppid,name` to identify the worker, then
`sudo kill PID`, escalating to `sudo kill -9 PID` if necessary. SSH workers
appear as sshd processes whose PPID is the monitor's PID. Terminating the monitor
instead would stop the service and its sessions. `jobs` should be run directly;
`sudo jobs` cannot execute a shell builtin. These commands require the updated
image; they cannot repair the already-running older shell binary.

## Verification

- `wsl -d Ubuntu-24.04 -- make test-kill-host`: PASS under ASan/UBSan, including
  actual shell job control and executable code. Covers PID/signal syntax,
  bounds, invalid input with no signal, job group routing, and kernel error
  reporting. The existing jobs fixture's spawn assertion was corrected from
  the mask of all supported v2 flags to actual SETPGROUP|STAGED flags; the full
  jobs/foreground/background suite passes with that stale assertion corrected.
- `make -j4`: PASS, including ISO/raw-image regeneration and image checks.
- `scripts/test_ssh.py` exercises `jobs` on the console, two real silent
  pre-auth SSH workers filling both slots, PID TERM via the builtin, SIGKILL
  via `sudo kill -9`, worker removal while retaining the monitor, and an
  authenticated reconnection. It also verifies that an unprivileged SSH user
  cannot signal the root daemon. PASS on BIOS and UEFI at SMP=1, using
  `PLINK=/mnt/c/Sources/FortressOS/build/putty-interop/linux/usr/bin/plink python3 -u scripts/test_ssh.py`
  in WSL Ubuntu-24.04. Final-source output is recorded in ignored
  `build/putty-interop/pid-kill-acceptance-final.log`.

Disposable test images/keys/password files are deleted. Physical recovery still
needs user acceptance after booting the updated image. No idle timeout,
daemon status/stop CLI, or scheduler/kernel signal ABI changes are introduced.
