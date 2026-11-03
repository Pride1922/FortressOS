# FortressOS Desktop foundation v0.2.0

The recovery/live binary is `/bin/compositor`; `--version` works without
acquiring the display. Run with existing root display authorization. The app
interface includes **built-in app API 1** and isolated external clients using
`src/include/desktop_proto.h` for SHM surfaces and input events.

During a live resize, the last client frame stays at its original pixel scale:
existing rows retain their coordinates, shrinking clips them, and newly exposed
space uses the window background until EV_CONFIGURE/DAMAGE supplies a fresh
frame. Text is not stretched while waiting for a client repaint. Built-in apps
also invalidate their canvas after a geometry change, even when several resizes
return to the old size before the next render. [Cause and regression evidence](../roadmap/terminal-resize-stride.md).

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
Power & Battery opens from the top-bar battery icon, tray power button or Settings sidebar.
Battery telemetry is currently unsupported on production hardware; the battery icon is hidden instead of displaying guessed readings. The rendering/UI plumbing remains implemented: with a valid status provider, the top bar features a dynamic battery icon with charging bolt and level coloring, opening a glassmorphic power menu with energy profile toggling (Balanced vs High Performance). On desktop PCs without a battery, the tray automatically compacts and omits the battery icon, while retaining full power profile configuration in Settings.
Each has shared page navigation, Tab focus and Enter activation; Escape closes
the page. Wheel or Ctrl+U/Ctrl+D scrolls app content. Drag titlebars to the left,
right or top edge and release to snap; the green control restores the window.
The compositor reads 8-byte `key_event_t` packets directly from
`/dev/input/keyboard` (`/dev/kbd0`), receiving Linux keycodes, active modifier
bitmasks (`MOD_SHIFT`, `MOD_CTRL`, `MOD_ALT`, `MOD_SUPER`, `MOD_CAPS`,
`MOD_NUMLOCK`), press/release/repeat state, and translated ASCII while muting
background console TTY `ingress()`. Global `Alt+Tab` cycles windows, `Super`
toggles the launcher menu, and `Ctrl+Alt+Esc` acts as the reserved Secure
Attention Key (SAK), immediately unfocusing/releasing capture on any active
client window and raising the System Monitor without forwarding the key event
to clients. External SHM client processes (`/bin/desktop-terminal` and
`/bin/desktop-files`) communicate over the desktop IPC protocol (`EV_KEY`,
`EV_MOUSE`, `EV_OPEN_FILE`, `MSG_DAMAGE`).

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
Dell scan/connect, wrong-password retry, disconnect and Ethernet/Wi-Fi handover
retesting was user-confirmed complete on 2026-10-28. This is manual user-supplied
radio evidence, separate from the QEMU ownership tests and PKG acceptance.
See the [owner audit](../roadmap/wifi-cpu0-owner.md).

Date & Time uses the shared C timezone/TZif engine and system zone database,
not a new hard-coded UTC-offset table. Saving validates an installed IANA name
and atomically updates `/etc/timezone`. The clock follows system timezone
policy; offsets reload at UTC minute boundaries and immediately after a GUI
change. Current calendar conversion and DST rules are supplied by that engine.
This page changes timezone and display format; it does not set the RTC or
replace ntpd. External timezone edits are noticed at the next minute boundary.

Service calls are currently serialized in the desktop event thread; Terminal,
Files and Notepad run as isolated external application processes. Scan
collection uses a monotonic deadline so wall-clock/timezone corrections cannot
leave a GUI scan pending indefinitely. No heavy package verification or update
operation runs in the renderer.

## Package boundary

The installed image provisions four independent ordinary packages:

| Package | Command |
| --- | --- |
| `fortress-desktop` | `/usr/bin/desktop` |
| `fortress-files` | `/usr/bin/files` |
| `fortress-notepad` | `/usr/bin/notepad` |
| `fortress-terminal` | `/usr/bin/terminal` |

Notes launches Notepad with `/tmp/notes.txt`; it has no separate executable or
package. That default file is temporary. The application provides an integrated elevated modal
"Open / Save" file dialog (invoked via toolbar buttons or shortcuts) featuring folder navigation,
parent traversal, quick directory shortcuts (`/`, `/tmp`, `/home`), existing file listing, and
filename editing to save or open documents anywhere on persistent user storage.

The base retains `/bin/compositor` as the recovery UI and privileged settings
broker. Wi-Fi drivers, firmware, networking and SSH server remain in
`fortress-base`. Installed base bootstraps exclude `/bin/files`, `/bin/notepad`
and `/bin/terminal`, leaving those command names to ordinary packages. Live media
retains bundled clients; the launcher prefers installed `/usr/bin` commands and
uses `/bin` fallbacks on live media. Root display authorization remains required.

Factory provisioning uses the trusted installer image to create closed ordinary
package records and retain exact original FPK sources. It does not invent catalog
signatures or enroll test keys. Subsequent upgrades require the signed repository
catalog and the backend's predecessor-source hash checks. The retained sources
live under `/var/lib/pkg/sources/<8 hash characters>/<remaining hash>.fpk`.

Stop the desktop to the console before updating its packages. Launch the installed
UI with `sudo desktop`; use `/bin/compositor` for recovery. App packages contain
one executable and documentation, with no install scripts or mutable settings.
`/etc/desktop.conf`, `/etc/timezone`, credentials and user documents remain outside
the payload. Ordinary package removal is not implemented.

See [desktop package workflow](../desktop-packages.md) and
[host acceptance evidence](../roadmap/desktop-packages.md). External clients use
the existing protocol; package compatibility must respect its version and the
base ABI. Built-in API 1 alone does not promise an external runtime library ABI.

## Desktop command and process restart

The updated sudo resolver searches fixed `/sbin`, `/bin`, `/usr/bin` directories,
so `sudo desktop` resolves to the packaged `/usr/bin/desktop`. Caller PATH and
relative paths cannot redirect privileged command lookup. Existing authentication,
invoking-user identity and package admission remain in place.

The foreground `sudo desktop` invocation also waits as a session launcher. The
Activities menu's **Restart (save/close apps)** action requires all external app
windows and pending child teardown to be closed first. It then exits the old
compositor through normal input/framebuffer cleanup with a dedicated restart
status. The sudo parent launches the new executable, retaining SUDO_UID/GID/groups;
the new desktop performs the existing privilege drop. Normal exit and crashes
return to the console. Repeated restart requests are bounded at three per launcher
invocation; launch again from the console if that limit is reached.

`pkg upgrade` now explains that a desktop-process restart is required after a
changed `fortress-desktop` activation. It does not force a restart or discard open
documents. A laptop reboot is unnecessary for application-only changes.

Do not register this interactive desktop as a Watchtower service. Detached root
service startup lacks the authenticated login identity and console ownership used
here. A future graphical login/session manager is separate work.

Delivery: these launcher/restart changes are locally implemented and host-checked,
not yet published. They require both an updated base-owned `/bin/sudo` and a newer
`fortress-desktop` package. Existing installations can exit to the console and run
`sudo /usr/bin/desktop` immediately to load a newly installed desktop without reboot.
The base update installing the new launcher requires its normal OS reboot once;
subsequent application-only updates use desktop-process restart.

## The App SDK (`libui`)

External desktop applications (and compositor widgets) share the freestanding
`libui` crate (`user/libui`). It extracts:
- Core 4x4 subpixel coverage and integer-only blending (`rounded_coverage`, `blend_coverage`).
- The `SurfaceStyle` unified rounded-box primitive (`radius`, `fill`, `border`, `top_highlight`, `outer_bg`).
- The immediate-mode `Painter` abstraction supporting scale, vertical scroll, clipping, and compare-before-write dirty rect tracking.
- Common widgets: `label`, `button`, `button_accent`, `card`, `sheet_card`, `text_input`, `badge`, `pill_button`, `rounded_box`.
- Interactive state helpers: `TextField<N>`, `FocusRing<N>`, `Grid`, and `hit(rect, x, y)`.
- Client runtime (`--features client`): `App::connect`, `next_event`, `painter`, `present`, `app_main!` entry macro, panic handler, and minimal freestanding syscalls.

Writing a new desktop application (such as the proof-of-concept Calculator at
`user/apps/calc` in `/bin/calc`) requires ~50-80 lines of Rust code. Apps link
statically using `user/libui/app.ld`. See [Building Desktop Applications with libui](../desktop-apps-guide.md)
for a comprehensive tutorial, API reference, and packaging walkthrough. `libui`
is a source-level SDK; binary compatibility with the compositor remains strictly
governed by `desktop_proto.h` and base syscall contracts.
