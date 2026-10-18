# Watchtower: operating services and adding a daemon

This is the implementation guide for `/sbin/watchtower` and `/bin/service`.
The [architectural plan](../plans/WATCHTOWER_PLAN.md) describes intent; this guide
distinguishes implemented behavior from remaining work. Acceptance evidence is
in [the lifecycle report](../roadmap/watchtower-lifecycle.md).

## Current implementation limits

- The supervisor discovers up to 16 `.service` files from `/etc/watchtower`.
  When that directory has no service definitions, it scans
  `/mnt/.fortress/watchtower`. Invalid definitions are skipped; they do not
  acquire built-in defaults. No C registration is needed for a new service.
- Configuration is loaded when Watchtower starts. `service restart` restarts
  the process using the already-loaded configuration; it does not reload files.
  There are no `enable`, `disable`, `reload` or `daemon-reload` commands.
- `User` and `Group` are enforced on every launch, including restarts. Names
  and decimal IDs must resolve to existing account/group records. Lookup,
  generation-check, credential, pipe or spawn failure leaves the service failed
  without a child or readiness claim. See [service identities](#service-identities).
- Control-mailbox concurrency/stale-lock robustness and forking-service edge
  cases remain outside the completed lifecycle acceptance. Operate one service
  command at a time. Do not start a second Watchtower instance.

## User commands

Use names such as `sshd`, not the executable path or `sshd.service`.

| Command | Purpose |
| --- | --- |
| `service --help` | Show supported command syntax |
| `service list` | List registered services, state, PID, boot order and policy |
| `service status sshd` | Show state, readiness, executable, restart count, last exit and dropped output |
| `service check sshd` | Query readiness; this does not start or repair the service |
| `sudo service start sshd` | Start a stopped/failed service |
| `sudo service stop sshd` | Request graceful termination, with bounded escalation |
| `sudo service restart sshd` | Reset restart counters and stop/start using loaded configuration |

The control protocol permits read queries without mutation privileges. Mutating
requests require root or the primary request-file group ID 10; supplementary
wheel membership is not separately resolved by this protocol. Using `sudo` is
the supported reliable path for administration. Filesystem permissions can also
restrict access to the control files.

CLI exit codes: **0** for a successful response; **1** for syntax, service,
permission or readiness errors/warnings; **2** for an unavailable supervisor,
busy control channel or response timeout. `check` reports success only when the
service is both running and ready. A successful `start`/`restart` response means
the request was accepted, not that asynchronous readiness has completed.

```sh
sudo service restart crond
sleep 3
service status crond
```

States are `stopped`, `starting`, `running`, `stopping` and `failed`.
`starting` can be waiting for a readiness file or a forking daemon PID.
`failed` can mean spawn failure, exhausted retries or a readiness failure.
Use the last-exit information and service log to distinguish them. Deliberate
stop suppresses automatic recovery. Unexpected failures follow the configured
restart policy, delay and bounded retry window. An explicit restart resets the
failure counter; `start` alone should not be used as a counter-reset operation.

### Starting the supervisor

The kernel starts Watchtower when boot admission is ready, this is not an
installer boot, and `/sbin/watchtower` exists. If `service list` says the
supervisor is not running, start it once from the local console:

```sh
sudo -b /sbin/watchtower
```

Then query `service list` again. Do not run bare `watchtower` as an ordinary
user, or launch another copy while the existing one is running. Current startup
does not implement a verified automatic supervisor-crash restart policy.

### SSH on a fresh live USB

Live credentials and host keys are intentionally temporary. SSH can exhaust its
startup retries before those prerequisites exist. On a live boot only:

```sh
sudo /bin/passwd --live operator
sudo /sbin/ssh-keygen --live
sudo service restart sshd
sudo service status sshd
```

Choose at least eight password characters. After setup, sudo uses that password.
Wait for `running` and `Readiness: ready`, then connect normally. Do not start
another `/sbin/sshd` manually while Watchtower owns it. Installed systems use
their provisioned accounts and host identity; `--live` is not their setup path.

### Logs and troubleshooting

Supervisor diagnostics and each service's stdout/stderr are captured separately:

Watchtower startup, shutdown and warning messages go only to its log; they are
not printed over the local login or shell prompt.

| Boot/storage mode | Supervisor log | Example service log |
| --- | --- | --- |
| Writable installed logging directory | `/var/log/watchtower/watchtower.log` | `/var/log/watchtower/sshd.log` |
| Live/read-only fallback | `/run/watchtower/watchtower.log` | `/run/watchtower/sshd.log` |

```sh
sudo tail -n 30 /run/watchtower/sshd.log
sudo tail -n 30 /run/watchtower/watchtower.log
```

Use the `/var/log/watchtower` paths when that directory is selected instead.
The `/run` logs, readiness files and PIDs disappear on reboot. Logging/rotation
work is evolving separately; consult the [logging plan](../plans/LOGGING_FOUNDATION_PLAN.md)
for its current delivery scope. `Dropped Output` is an output-loss diagnostic,
not a guarantee that every service message is durable.

For `control channel busy` or a response timeout, avoid repeated/concurrent
commands. Diagnose the supervisor from the local console. Do not blindly delete
control files while an operation may still be active.

### Shutdown and reboot

From a normal interactive SSH session or the local console:

```sh
sudo shutdown
```

or:

```sh
sudo reboot
```

The privileged coordinator requests supervisor teardown, waits with a bounded
deadline, and then requests kernel power-off/reset. Watchtower signals services
in reverse boot order; completion can overlap. SSH sends a disconnect before
hardware power transition. Dell power-off and reboot are user-confirmed.
Run these interactively: SSH exec requests with prompt-reading commands such as
sudo have a [documented limitation](../roadmap/watchtower-lifecycle.md#discovered-boundary-interactive-prompts-in-ssh-exec-mode).

## Developer: add a service

### 1. Implement a foreground daemon

Prefer `Type=simple` with a foreground option. Watchtower must track the actual
long-lived child. Do not background it with `&` in `Exec`. Use absolute program
paths; the service manager constructs argv and does not invoke a shell.
Shell variables, pipes, redirections and substitutions are not expanded.

Handle SIGTERM by setting a flag, then perform bounded cleanup in the normal
event loop. Close descriptors and remove owned readiness/PID files on exit.
Publish readiness only after actual initialization; remove stale readiness
before a new start. `Readiness=file:` currently checks existence, not the file's
contents or generation. Use `Readiness=none` only when immediate process-start
readiness is acceptable.

Keep large records/buffers in bounded owned storage rather than automatic stack
arrays. The current ELF loader maps **one 4 KiB user stack page**. Review nested
calls and signal delivery as well as individual compiler frame sizes. See
[the Dell stack correction](../plans/WATCHTOWER_PLAN.md#dell-boot-stack-correction--2026-10-18).

`Type=forking` is parsed and supported by PID-file tracking, but has narrower
acceptance. Specify an absolute `PIDFile`, arrange for the launcher to exit
successfully and the daemon to publish its actual PID. Prefer foreground mode
for new daemons until the forking edge cases are covered.

### 2. Supply configuration

Example **for a daemon you implement and package**, not an already-installed
`metricsd` command:

```ini
Exec=/sbin/metricsd --foreground
Type=simple
Order=40
Enabled=yes
WorkingDirectory=/run
User=operator
Group=operator
Environment=PATH=/bin:/sbin
Restart=on-failure
RestartLimit=3
RestartDelay=5
TimeoutStart=5
TimeoutStop=5
Readiness=file:/run/metricsd.ready
```

One `Key=value` per line; blank lines and whole-line `#` comments are accepted.
Use simple argv tokens or whole quoted arguments for spaces; this is not shell
quoting/escape syntax. Unknown directives and duplicate nonrepeatable keys are
rejected. Do not add systemd directives such as `ExecStart` or section headers.

| Directive | Default | Accepted scope |
| --- | --- | --- |
| `Exec` | Required | Executable and up to 16 additional arguments; use absolute executable paths |
| `Type` | `simple` | `simple`, `forking` |
| `Order` | `100` | 1..999; lower starts first |
| `Enabled` | `yes` | `yes`/`no`; controls boot start, not availability for manual start |
| `WorkingDirectory` | Unspecified | Absolute path when supplied |
| `PIDFile` | Unspecified | Absolute path; needed for practical forking tracking |
| `Environment` | None | Repeatable `NAME=value`, up to 16 total entries including EnvironmentFile |
| `EnvironmentFile` | None | Absolute path; loaded once at startup; missing/unreadable file is a loader error |
| `User`, `Group` | `root`, `root` | Existing account/group names or decimal IDs; applied before the child can run |
| `Restart` | `on-failure` | `never`, `on-failure`, `always` |
| `RestartLimit` | `3` | 1..100 retries within the 60-second counter window |
| `RestartDelay` | `5` | Parser accepts 0..3600 seconds; registration currently replaces 0 with 5 |
| `TimeoutStart` | `5` | 1..3600 seconds |
| `TimeoutStop` | `5`; sshd `10` | 1..3600 seconds; leave SSH time to finish its internal teardown |
| `Readiness` | `none` | `none` or `file:/absolute/path` (use absolute paths by convention) |

Configuration file buffer is 4096 bytes; lines use 256-byte buffers. Service
names use 32-byte storage, paths and individual arguments 128-byte storage,
and environment entries 128-byte storage (all include their NUL terminator).
The service table has 16 entries. Keep values within those limits; maximum
file/line sizes also need terminator space. EnvironmentFile entries append to
inline entries; avoid duplicate names rather than relying on an override order.
Spawn ABI budgets still apply independently: argv strings together must fit
1024 bytes, and environment strings together must fit another 1024 bytes,
including NUL terminators. A definition can parse yet exceed a spawn budget.

Discovery treats a directory containing `.service` files as authoritative even
if every definition is invalid. Per-name loading tries `/etc/watchtower` first;
a parse or EnvironmentFile error rejects that definition without fallback.
An absent/unreadable file permits the corresponding live-media path. The shipped
live image includes `/etc` definitions, so `/mnt` definitions do not override it.

### Service identities

Set both directives for an unprivileged service, for example `User=operator`
and `Group=operator`. The existing defaults remain `root:root`: omitting `Group`
does not select the account's primary group. Prefer a dedicated service account
when deployed; `operator` retains its account database supplementary memberships.
Numeric IDs must also exist in the database; anonymous numeric identities are
rejected. Password hashes and login authentication are not used for service
launches, so a locked account can still be a service identity.

Every start takes a fresh public account snapshot and checks its generation.
The configured group replaces the account's primary group; supplementary groups
come from that account's database memberships, with duplicates removed. No
supervisor supplementary groups or saved root IDs survive a non-root launch.
All three UID fields and all three GID fields are set before the child is
published, with zero capabilities for nonzero UID. UID zero retains only the
supervisor's current capabilities; this path never replenishes them. Set-id
bits on the initial executable do not override the requested identity.

The kernel uses the target identity for executable, cwd and OPEN-action access.
Inherited descriptors above stderr survive only when explicitly mapped by fd
actions; stdout/stderr go to the
service pipe and stdin is `/dev/null`. The default cwd is `/`. An explicit
WorkingDirectory must be searchable by the service identity. These are process
credential boundaries, not a sandbox: ordinary later set-id program execution
and explicitly granted account memberships retain their existing OS semantics.

Account generation is rechecked immediately before the spawn syscall using the
same snapshot/check model as other account consumers. This is not an atomic
account-update/spawn transaction, nor does editing accounts revoke credentials
of an already-running service. Configuration files are still loaded once;
restarting refreshes account resolution, not service definitions.

### 3. Discover the definition

Place `<name>.service` in the selected configuration directory and include it
in the installed/live payload. Watchtower discovers it at supervisor startup;
there is no source registration list or automatic live directory reload.
Review its `Order` and teardown budgets: ordering is not a dependency graph or
proof that earlier services are ready. Invalid definitions stay absent; an
unresolvable identity leaves a discovered service in `failed` state on start.

### 4. Package it and verify

Follow [AGENTS.md §7.6](../../AGENTS.md#76-a-user-program-in-initramfs) for the
freestanding ELF build. Add the executable to initramfs dependencies and stage
it under `/sbin`. Add its definition to the Makefile's `/etc/watchtower` staging
section, using existing services as examples. Both artifacts must be part of
the rebuilt media/install payload; copying a file into a developer build
directory alone does not register or deploy the daemon.

After changes, verify:

```sh
make test-watchtower-host
make test-watchtower-identity
make test-watchtower-boot
make test-watchtower-lifecycle
```

The integration targets build their ISO prerequisite; physical USB image builds
remain a separate requested action. Extend host tests for the new service's
parser/behavior and real-guest tests for its readiness, stop, crash, restart and
output paths. Existing lifecycle coverage primarily exercises crond and sshd;
it is not automatic acceptance of a newly added daemon. The identity target
uses the existing `bin/initramfs.tar` as a read-only base and creates a temporary
ISO; it never rebuilds the USB image or includes its probe in release media.

On the rebuilt system, use `service list`, `service status metricsd` and
`service check metricsd`. Confirm graceful stop, recovery after a crash, retry
exhaustion and shutdown behavior. Configuration edits require a new supervisor
start to take effect; use a controlled reboot after updating deployed files,
not a second concurrent supervisor process.

Documentation validation: the `metricsd.service` example above passes the actual
host configuration loader; local document link targets were checked. This
validates the example syntax, not the hypothetical daemon or its registration.
