# UTC, timezone and SNTP delivery — 2026-10-18

Implementation and user/developer operations are in the
[time subsystem guide](../subsystems/time.md).

Implemented:

- UTC kernel realtime with capability-gated, pointer-checked atomic corrections.
- Independent monotonic time and existing scheduler/deadline origins.
- Shared bounded Gregorian/POSIX/TZif conversion; IANA 2026c bundled from a
  checksum-pinned public-domain archive, with all 597 zones and aliases in one
  716,166-byte database. Existing installer entry limits remain unchanged.
- `date` environment/configuration precedence, local ISO/RFC/default output,
  explicit UTC and raw epoch overrides. Logger records stay UTC.
- RFC 4833 discovery in DHCP requests/ACKs, IANA validation, POSIX fallback,
  volatile publication, stale removal and explicit adoption opt-in.
- `/sbin/ntpd --once SERVER` and foreground Watchtower operation, disabled by
  default. Production uses UDP 123 and CAP_SYS_ADMIN; no RTC writes.

Verified:

- `make test-timezone-host`: calendar bounds, positive/negative offsets,
  POSIX sign semantics, DST, TZif truncation and 20,000 deterministic mutations,
  output preservation, SNTP cookie/era/length/LI/stratum validation.
- 340,692 independent Python zoneinfo comparisons across every bundled zone, explicit
  transition edges and historical/post-2038/future dates.
- Actual kernel correction code: invalid inputs preserve time, subsecond
  normalization, monotonic isolation, 100,000 concurrent writer/reader iterations
  without torn values, and calibrated-TSC/tick-fallback paths.
- Actual DHCP discovery publisher: valid IANA, unknown-IANA/POSIX fallback,
  traversal rejection, absent/invalid removal, short reads/writes, read/write/
  rename failures, packed/loose data, truncated/invalid database headers,
  unsuccessful interface application and release/expiry flush. Actual
  handshake/renew/rebind/reboot transaction code also carries synthetic ACK
  timezone options into the published lease state.
- Production USTAR packager includes the database with root ownership and
  mode 0644; disposable firmware bootstrap stays within the 8 MiB size limit.
- Existing date/sleep, RTC/time, logger and DHCP v1/v2 host sanitizer suites.
- Disk-free BIOS/UEFI × SMP=1/4: real Ring 3 clock setter argument/pointer/
  capability boundaries, failure preservation, forward/backward corrections
  with monotonic isolation, real UDP SNTP, Brussels/New York/Kathmandu display,
  UTC override, bad-cookie/KoD/no-response deadlines, recovery, and supervised
  NTP start/stop. All four profiles pass.
- Release `bin/fortress.iso`, `bin/fortress.img` and `bin/initramfs.tar` hashes
  unchanged by the disposable acceptance campaign. The clock probe and the
  high-port acceptance NTP executable have no release packaging rule.

Boundaries:

- No Dell physical acceptance or real router option-100/101 packet capture is
  claimed. DHCP wire codec and actual publication logic have host coverage;
  ordinary DHCP acquisition runs in the firmware acceptance guests.
- The SNTP oracle uses UDP 43123 to avoid privileged host-port requirements.
  The separately built production client uses 123; public Internet NTP servers,
  physical packet capture and long-running clock drift are not acceptance claims.
- No NTS, authenticated time, peer selection, frequency discipline, slewing,
  leap-second insertion or hardware RTC update.
- No file-date display in `ls -l`: existing stat ABIs contain no timestamps.
- No new release USB image was created. All fixtures use regular temporary
  files, no data disks, target credentials or retained private keys.
