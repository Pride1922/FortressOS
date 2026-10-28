# FortressOS Desktop foundation v0.2.0

The recovery/live binary is `/bin/compositor`; `--version` works without
acquiring the display. Run with existing root display authorization. The app
interface is **built-in app API 1**, not an external process/window IPC ABI.

## Adding a built-in app

Register a stable ID, title and initial size in `user/compositor/src/ui.rs`.
Implement `DesktopApp` for its input state and use `Canvas` labels, buttons and
entries to render. The host routes pointer, keyboard, resize and periodic data
updates. Pixel comparisons produce bounded dirty rectangles; unchanged widgets
do not repaint. Client content is clipped and can scroll when larger than its
window. The current state objects are in `desktop.rs`; device/service adapters
are in `sys.rs`. Do not put application-specific input branches in the event
loop or add kernel syscalls for ordinary app settings.

Settings opens from the system menu or **S** on the desktop. Network opens from
the top-bar network entry or **N**. Date & Time opens from the clock or **C**.
Each has shared page navigation, Tab focus and Enter activation; Escape closes
the page. Wheel or Ctrl+U/Ctrl+D scrolls app content. Drag titlebars to the left,
right or top edge and release to snap; the green control restores the window.
Alt+Tab is not claimed: the current tty input stream does not expose modifier
key state. Files, Notes and Terminal retain their existing interfaces.

## Preferences and services

`/etc/desktop.conf` is strict, bounded version 1 text. Unknown keys, duplicates,
bad values, embedded NUL and oversized files are rejected. Defaults remain
usable with an explicit error. Apply writes an exclusive temporary file and
atomically replaces the old file; failures retain the old configuration and
active preferences. Scale modes are auto/native/large; auto uses native 1080p
and 2x UHD. Wallpaper choices are the original hello-world asset or plain.
Clock format is 12-hour/24-hour. Live `/etc` is RAM-backed: changes survive
compositor restarts, but not a machine reboot. Installed writable `/etc` persists.

The Network page queries the existing SYS_NETCTL default-interface snapshot.
It shows real link, IPv4, gateway and packet counts. Wi-Fi uses SYS_WIFI scan,
status, connect and disconnect, preserving the existing nonblocking connection
state machine and CPU-0 networking ownership. Scan selection retains BSSID;
manual SSID entry permits the existing best-match policy. Passwords are masked,
cleared after use/close, never stored by the desktop and never placed in argv.
Wi-Fi connection is for the current session. Persistent Wi-Fi profiles and
editing Ethernet/DNS settings remain the existing CLI's responsibility.
No unavailable adapter is displayed as connected.

Before its first scan the desktop launches the existing `/bin/wifi init`
firmware preparation as a background child, with null standard descriptors and
no credential arguments. It polls/reaps that child before scanning or joining;
preparation errors are distinct from authentication failures. The shared CLI
preparation retries loading after FAILED/SCANNING status instead of treating
those states as proof of firmware readiness. Selected BSSID channel/timing now
comes from that exact cached AP, even if a newer beacon makes another AP with
the same SSID stronger. See [the GUI connection follow-up](../roadmap/gui-wifi-connect.md).

Wi-Fi opens with an automatic scan and a clickable, paged nearby-network list.
The list groups equal SSID/security pairs and keeps the strongest BSSID, sorted
by signal. Selecting a network focuses its password field; Enter joins. Refresh,
Back/More, signal quality, connected markers, masked password Show/Hide and
connection progress/failure/timeout feedback avoid stepping through individual
access points. Changing the network clears the password and resets visibility.
Join requests are validated locally and repeated requests are suppressed while
connecting; Cancel uses the existing disconnect operation. Passwords are still
session-only. The current kernel connection service requires WPA2 credentials;
open networks are labelled and explain that limitation instead of requesting a
meaningless password. Manual network-name entry retains the existing service's
SSID lookup behavior; this does not add hidden-network discovery.


**CPU-0 Wi-Fi management — [issue #76](https://github.com/Pride1922/FortressOS/issues/76):**
SYS_WIFI now routes all commands through the registered CPU-0 owner using
bounded copied storage; status is a pure snapshot. After actual-code host and
BIOS/UEFI × SMP=1/4/8 BSP/AP verification, the temporary SYS_SYSINFO CPU-count
restriction is removed. Controls require an available adapter on every CPU
count. QEMU mock-radio ownership tests do not establish Intel radio acceptance;
Dell scan/connect/status/disconnect and handover retesting remain pending.
See the [owner audit](../roadmap/wifi-cpu0-owner.md).

Date & Time uses the shared C timezone/TZif engine and system zone database,
not a new hard-coded UTC-offset table. Saving validates an installed IANA name
and atomically updates `/etc/timezone`. The clock follows system timezone
policy; offsets reload at UTC minute boundaries and immediately after a GUI
change. Current calendar conversion and DST rules are supplied by that engine.
This page changes timezone and display format; it does not set the RTC or
replace ntpd. External timezone edits are noticed at the next minute boundary.

Service calls are currently serialized in the desktop event thread; asynchronous
I/O adapters and isolated external application processes are later work. Scan
collection uses a monotonic deadline so wall-clock/timezone corrections cannot
leave a GUI scan pending indefinitely. No heavy package verification or update
operation runs in the renderer.

## Package boundary

The updatable package identity is `fortress-desktop`; its command is **desktop**
(`/bin/desktop` after an admitted package activation). Keep the base-owned
`/bin/compositor` as a recovery fallback: PKG-1 must not overwrite base-owned
commands. Font assets are embedded in the ELF. Existing platform wallpaper and
timezone data are optional/runtime base resources with explicit unavailable
behavior. User preferences and `/etc/timezone` are never package payload members.

Build an **unsigned candidate** with `scripts/build_desktop_package.py`, passing
the approved platform identity, base version range and monotonic build number.
It produces the existing PKG-1 envelope/manifest/gzip-USTAR format, an integrity
checksum and a metadata sidecar. The executable is `bin/desktop`; examples and
license live under `doc/`, matching the existing payload policy. No install
scripts or mutable configuration files are included. Artifact integrity is not
signing, enrollment, catalog approval or root admission.

Production PKG enrollment/key delivery is still separate from the test backend.
This change does not enable it or inject test keys. Once an approved catalog
and enrolled root support the package, stop the desktop to the console, install
the approved package using PKG, then launch `/bin/desktop`; subsequent app API 1
changes can ship in the same versioned ELF. Uninstall retains the base recovery
compositor. A future external app protocol needs explicit surface ownership,
credential/input isolation and version negotiation; this built-in API does not
promise that boundary or runtime library ABI stability.
