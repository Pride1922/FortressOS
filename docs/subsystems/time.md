# Time, timezones and synchronization

The kernel keeps Unix UTC realtime. `CLOCK_MONOTONIC`, sleep deadlines,
Watchtower timers, DHCP lease timers and crond interval scheduling remain
independent of wall-clock corrections. The RTC is interpreted as UTC at boot;
runtime synchronization does not write the RTC.

## Displaying local time

```sh
date -u -I
TZ=Europe/Brussels date -I
TZ=America/New_York date -R
TZ=+05:45 date -I
date +%s
```

`date` chooses, in order:

1. The process's `TZ` environment variable. Empty `TZ` means UTC.
2. One timezone specification in `/etc/timezone`, with an optional trailing newline.
3. `/run/timezone` from DHCP, **only** if `/etc/timezone.dhcp` exists.
4. UTC.

On an installed system, root can put `Europe/Brussels` in `/etc/timezone`.
On readonly live media, use `export TZ=Europe/Brussels` for the shell session.
An explicit invalid/unavailable timezone reports an error. Invalid optional
DHCP discovery falls back to UTC. `-u` always selects UTC, and `+%s` always
prints the original UTC epoch independently of timezone configuration.

Supported specifications:

- IANA names such as `Europe/Brussels`, including historical transitions and
  future daylight-saving rules. An optional leading `:` is accepted for names.
- Signed numeric offsets such as `+02:00`, `-05:30`, or `+05:45`, using the
  familiar ISO sign: positive is east of UTC.
- POSIX TZ strings, such as `UTC0`, `UTC-2`, or
  `EST5EDT,M3.2.0/2,M11.1.0/2`. **POSIX reverses the offset sign**:
  `UTC-2` means UTC+02:00. DST strings require explicit start/end rules;
  `Mmonth.week.weekday`, `Jn` and zero-based day-of-year rules are supported.

The pinned public-domain IANA 2026c source archive is compiled at build time
with `zic` into one `/usr/share/zoneinfo.tzdb` file. This preserves the installer's
existing entry-count bound. No runtime download is needed. Individual TZif records
are bounded to less than 16 KiB, names to 127 bytes and specifications to 255
bytes; the database is capped at 1 MiB and scanned with a 16 KiB buffer. Optional
loose files under `/usr/share/zoneinfo/` are also accepted. The reader accepts
TZif v1–v4 without leap-second tables; Unix seconds
use the existing non-leap-second timescale. Calendar conversion covers years
0000–9999; out-of-range dates fail. ISO output preserves historical offset
seconds, while RFC 2822 rejects offsets that are not whole minutes.

`user/timezone.h` exposes the shared, allocation-free calendar/POSIX/TZif
engine. `user/tools/timezone_io.c` supplies the bounded file/environment adapter.
`date` uses local display; `logger` uses the same calendar conversion in UTC.
Stored audit/timeline records remain UTC (or monotonic uptime when unavailable).
`ls -l` currently has no timestamps in its VFS metadata ABI, so no local-time
file-date field is claimed or fabricated.

## DHCP discovery

`dhcpc` requests RFC 4833 options 100 (POSIX TZ string) and 101 (IANA name).
After an accepted ACK it validates discovery, prefers a locally available IANA
zone, falls back to a valid POSIX string and atomically publishes `/run/timezone`.
Unknown zones, control bytes and strings longer than 127 bytes are ignored;
optional timezone data never makes an otherwise valid lease fail. Missing or
unusable discovery removes stale data. Lease release/expiry also removes it.
Publication I/O failures do not claim a usable discovered timezone.

Discovery never overwrites `/etc/timezone` or `TZ`. To adopt it on an installed
system, remove any explicit `/etc/timezone` setting and create the root-owned
opt-in marker `/etc/timezone.dhcp`. Live media can record discovery without
adopting it; use `TZ` explicitly there.

## Synchronizing UTC

```sh
sudo /sbin/ntpd --once pool.ntp.org
sudo service start ntpd
sudo service status ntpd
sudo cat /run/watchtower/ntpd.log
sudo service stop ntpd
```

`ntpd --once SERVER` makes one bounded SNTP query; `ntpd -f SERVER` remains in
the foreground for Watchtower, resynchronizing hourly and retrying failures
after a minute. SERVER is an IPv4 address or a DNS hostname. The shipped
`ntpd.service` is **disabled by default**; its example server is `pool.ntp.org`.
On installed systems, edit the service's `Exec` server and `Enabled=yes` to
enable it at subsequent boots. Foreground logging is captured by Watchtower.
SIGTERM/SIGINT interrupt its waits for bounded shutdown.

Production requests use UDP 123. Replies must have the expected source IPv4
and port, version 3/4 server mode, a synchronized leap indicator, stratum 1–15,
the request's random 64-bit originate cookie, bounded root dispersion and valid
receive/transmit timestamps. Invalid, unsynchronized, KoD, stale-cookie and
missing replies leave the clock unchanged. A transaction has a five-second
monotonic receive deadline; DNS has its own bounded resolver deadline.

This is a small, single-server SNTP client with clock steps, not a multi-peer
clock-discipline implementation. It has no NTS or authenticated time source.
The supported era window is 1970–2100, resolving the 2036 NTP wrap explicitly.
Use a trusted server/network when enabling automatic corrections.

`SYS_CLOCK_SETTIME=83` accepts `(CLOCK_REALTIME, const timespec_t *)` and requires
CAP_SYS_ADMIN. Other clock IDs, invalid user pointers, negative seconds,
seconds after 2100-12-31 and invalid nanoseconds fail without changing time.
The kernel publishes one atomic correction relative to immutable boot clock
origins, so concurrent readers cannot observe a torn seconds/nanoseconds pair.
It does not alter calibration, monotonic clocks, CPU accounting or sleep state.

## Verification

`make test-timezone-host` runs calendar/TZif/POSIX differential and adversarial
tests, actual kernel correction tests and actual DHCP discovery publication
tests under host sanitizers. Existing date/sleep/RTC/logger tests remain included.

`make test-timezone` also runs a disposable, disk-free BIOS/UEFI × SMP=1/4
acceptance campaign, including a release-excluded Ring 3 clock boundary probe,
real UDP SNTP, display and Watchtower lifecycle checks. The local oracle uses
an unprivileged high UDP port in a separately compiled acceptance client;
production remains UDP 123. Release ISO/image/archive hashes are checked.

See [evidence and limits](../roadmap/time-timezone-ntp.md).
Standards: [TZif](https://www.rfc-editor.org/rfc/rfc9636.html),
[DHCP timezone options](https://www.rfc-editor.org/rfc/rfc4833.html),
[NTP packet protocol](https://www.rfc-editor.org/rfc/rfc5905.html).
