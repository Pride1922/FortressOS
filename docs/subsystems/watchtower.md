# Watchtower: operating services and adding a daemon

This is the implementation guide for `/sbin/watchtower` and `/bin/service`.
The [architectural plan](../plans/WATCHTOWER_PLAN.md) describes intent; this guide
distinguishes implemented behavior from remaining work. Acceptance evidence is
in [the lifecycle report](../roadmap/watchtower-lifecycle.md).

## Current implementation limits

- The supervisor explicitly registers **dhcpc, sshd and crond** in
  `user/watchtower/watchtower_main.c`. It does **not** scan directories for
  arbitrary `.service` files. Adding a new service currently requires a source
  registration and rebuilt media, as well as its configuration file.
- Configuration is loaded when Watchtower starts. `service restart` restarts
  the process using the already-loaded configuration; it does not reload files.
  There are no `enable`, `disable`, `reload` or `daemon-reload` commands.
- `User` and `Group` are parsed and displayed, but the current spawn adapter
  does **not** apply them as credential changes. Services inherit supervisor
  credentials. A daemon requiring privilege separation must implement its own
  credential/capability drop; these directives do not establish isolation.
- The config loader rejects invalid files, but the existing three startup
  registration blocks choose built-in defaults on any load failure. An invalid
  file therefore does **not** guarantee the service stays disabled. This is a
  mismatch with the plan's fail-closed startup intent and remains follow-up work.
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

Example **for a daemon you implement and register**, not an already-installed
`metricsd` command:

```ini
Exec=/sbin/metricsd --foreground
Type=simple
Order=40
Enabled=yes
WorkingDirectory=/run
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
| `User`, `Group` | `root`, `root` | Parsed labels only; currently no credential enforcement |
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

For a registered name the loader tries `/etc/watchtower/<name>.service` first,
then `/mnt/.fortress/watchtower/<name>.service` when the first load reports no
readable file (currently includes absence and open/read failures). A parse or
EnvironmentFile error stops fallback within the loader.
The shipped live image already has `/etc` definitions, so dropping another
definition into `/mnt` does **not** override those. See the startup-fallback
limitation above for invalid/missing definitions of the existing three services.

### 3. Register it explicitly

Add a registration in `watchtower_main` beside dhcpc/sshd/crond, before the
initial boot-start loop. Keep the temporary service record static:

```c
static service_t metrics;
if (watchtower_config_load_service(
        "/etc/watchtower/metricsd.service",
        "/mnt/.fortress/watchtower/metricsd.service",
        "metricsd", &metrics, err, sizeof(err)) == 0) {
    if (watchtower_table_add_service(&table, &metrics) < 0)
        wt_log_event("[watchtower] metricsd registration failed\n");
} else {
    wt_log_event("[watchtower] metricsd configuration unavailable or invalid\n");
}
```

This pattern skips a new service on load failure rather than silently starting
defaults. It is an integration example, not a change already made to Watchtower.
Name, executable and definition paths must agree. Review whether its `Order`
and timeout fit dependencies and shutdown budgets; ordering is not a dependency
graph or proof that earlier services are ready.

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
make test-watchtower-boot
make test-watchtower-lifecycle
```

The integration targets build their ISO prerequisite; physical USB image builds
remain a separate requested action. Extend host tests for the new service's
parser/behavior and real-guest tests for its readiness, stop, crash, restart and
output paths. Existing lifecycle coverage primarily exercises crond and sshd;
it is not automatic acceptance of a newly registered daemon.

On the rebuilt system, use `service list`, `service status metricsd` and
`service check metricsd`. Confirm graceful stop, recovery after a crash, retry
exhaustion and shutdown behavior. Configuration edits require a new supervisor
start to take effect; use a controlled reboot after updating deployed files,
not a second concurrent supervisor process.

Documentation validation: the `metricsd.service` example above passes the actual
host configuration loader; local document link targets were checked. This
validates the example syntax, not the hypothetical daemon or its registration.
