# Desktop foundation 0.2.0 — 2026-10-28

Builds on the compositor polish without changing framebuffer ownership, kernel
syscall numbers, scheduler placement or networking ownership contracts.

## Delivered interfaces

- Built-in app API 1: descriptors, shared clipped/incremental widgets, pointer
  and keyboard routing, Tab/Enter focus, Escape close and scrollable viewports.
  New app dispatch is registry-based; this is not an external process IPC ABI.
- Settings: auto/native/large scale, original/plain wallpaper, 12/24-hour clock,
  strict v1 `/etc/desktop.conf`, explicit Apply and atomic replacement. Clock
  format alone does not resize every window or reload the wallpaper. Scale
  changes reflow system windows and refresh legacy client dimensions.
- Network: real NETCTL default-interface link/IP/gateway/packet snapshots.
  Wi-Fi page has bounded scan parsing, BSSID-aware selection, masked volatile
  password input and session connect/disconnect adapters. No credentials are
  stored or put in argv. Scan result collection uses a monotonic deadline.
- Date & Time: shared existing C calendar/POSIX/TZif/runtime engine, actual IANA
  validation and atomic `/etc/timezone` updates. No duplicated offset table or
  kernel clock/RTC changes. System zone offsets reload at UTC minute boundaries
  and immediately after a GUI save; malformed settings display unavailable.
- Drag-release snap to left/right/top, with green-control restore. No Alt+Tab
  modifier support or external app isolation is claimed.

Shared implementation guide: [desktop subsystem](../subsystems/desktop.md).
Font, wallpaper, bounded damage and native 1080p appearance are retained.
The compositor still requires existing root display authorization.

## Ownership gap and package boundary

Source audit found SYS_WIFI hardware operations on the caller CPU, including
status RX draining, and recorded [#76](https://github.com/Pride1922/FortressOS/issues/76).
The initial desktop disabled Wi-Fi controls on SMP/unknown CPU count. That
boundary is superseded by the verified [CPU-0 owner fix](wifi-cpu0-owner.md): all
five commands use retained copied owner requests, status is a pure snapshot,
and controls now require an available adapter regardless of CPU count. The
protected sole-owner contract and root display authorization are preserved.
QEMU mock-radio ownership verification does not establish Intel acceptance.

`fortress-desktop` packages the versioned ELF as `bin/desktop`, with examples,
developer documentation and license under `doc/`. Base `/bin/compositor`
remains recovery media; mutable `/etc` preferences are excluded from updates.
`make desktop-package` requires approved PLATFORM_ID, DESKTOP_BUILD, BASE_MIN
and BASE_MAX values. Production enrollment/signing/catalog delivery remains
separate; artifact integrity is not authorization. No test keys or test backend
are introduced into release media. On an admitted root, update from the console
and relaunch `/bin/desktop`; the built-in apps travel in the same versioned ELF.

## Verification and qualifications

- `make test-compositor-host`: 31/31 PASS, actual Rust modules, including strict
  preference parsing, edit bounds, scan response validation, app focus/routing,
  idle widget damage, extreme rectangle clipping, snap/restore, viewport growth,
  missing-adapter Wi-Fi fail-closed behavior and all retained pixel/cursor/damage tests.
- `make test-desktop-package`: actual PKG format/manifest/gzip/TAR/ELF verifier
  under ASan/UBSan accepts deterministic desktop candidates and rejects corrupt
  payloads; invalid platform identities are rejected. Uses TEST identities in
  temporary regular files, not an enrolled/installed-root mutation campaign.
- `make test-timezone-host`: shared calendar/POSIX/TZif malformed corpus and
  340692 independent zoneinfo comparisons PASS, with retained date/logger/time
  and clock/DHCP publication regressions. The GUI links this existing engine.
- `scripts/test_desktop_apps.py`: BIOS/UEFI × SMP=1/4 PASS (4 private guests,
  ISO only, no data disks). Exercises unprivileged `--version`, real GUI settings
  save, process restart persistence, original/plain wallpaper selection, actual
  timezone update and owner-safe network status. BIOS SMP=1 also switches to
  large mode, restarts, scrolls the actual GUI and saves native mode again.
- `scripts/test_compositor.py`: BIOS/UEFI at 1280x800 and real 1920x1080 PASS
  (4 private guests); independent font/wallpaper oracles, sparse redraws,
  maximize/restore and lifecycle remain covered.
- Release disassembly: largest fixed frame 968 bytes including saved registers;
  no XMM/YMM/ZMM references. Shared C sources use freestanding/no-SIMD flags and
  `-Wframe-larger-than=1024`. ISO and raw USB image are rebuilt.

Live `/etc` persists across compositor restarts within one boot, not VM reboot.
Installed-root persistence follows the existing writable filesystem contract;
this GUI campaign does not newly establish installed-root reboot durability.
No physical GUI, real-radio, production PKG enrollment/update or new input-latency
acceptance is claimed. Service calls remain serialized in the UI thread.

### Retained development failure

The first BIOS GUI run saved a valid preference file, but process restart read
defaults and failed the 12-hour clock check. The inherited Rust VFS access-mode
constants were 1/2/3 while `src/fs/vfs.h` defines 0/1/2; the new reader was
opening write-only. Corrected the constants, traced Notes read/save callers,
and added ABI value checks. The later matrix verifies actual restart persistence.
The failed-screen capture is `build/desktop-app-debug.png`; per-configuration
guest logs are overwritten by subsequent successful runs, so they are not
represented as retained full failure logs.
