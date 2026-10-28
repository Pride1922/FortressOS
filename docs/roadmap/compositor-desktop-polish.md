# Desktop polish — 2026-10-28

Visual correction after Dell feedback: 1920x1080 now uses the native 14 px UI
font and 1x chrome preset, consistent with the existing window decorations and
terminal. The former height>=1080 automatic 2x preset made the panel, brand,
clock and dock oversized and enlarged raster steps. The 2x preset now begins
at height>=2160. The dock silhouette uses integer 4x4 subpixel coverage in
physical pixels and blends its curved edge with the repaired background;
the bright straight top rim is removed. Wallpaper and input/performance fixes
are preserved. Host verification is now 25/25 PASS, including edge coverage
symmetry/intermediate alpha and an explicit native-size 1080p assertion.
BIOS/UEFI guest verification PASS at both 1280x800 and 1920x1080. The native
1080p guest preview is `build/compositor-native-1080p.png`; ISO and raw USB
images are rebuilt. The release frame ceiling remains 968 bytes, without SIMD.

Wallpaper follow-up: at the user's request, normal launches again load the
original `/usr/share/wallpapers/hello-world.bin` and retain it for every redraw,
including exposed areas after dragging/restoring windows. The procedural
background remains the fallback if loading fails; `--test` retains its existing
deterministic background. UI and performance fixes are retained.

The normal compositor starts with the original "hello world" wallpaper, with
the quiet procedural charcoal/blue desktop available as a loading fallback.
The Palette window remains only in the `--test` rendering fixture; its normal
launch paths and unused dock icons are removed. The dismissible password-shaped
lock overlay is removed. This change does not implement login or locking.

Desktop chrome uses a checked-in DejaVu Sans ASCII alpha atlas, proportional
advances and integer alpha blending. The terminal and editor retain their
existing monospace text. Shell chrome (top bar, system menu, dock, tooltips) and
System Information content use 2x scaling at framebuffer heights >=2160, 1x
otherwise. This is not arbitrary fractional scaling or full client UI scaling.
Maximize respects the scaled shell's reserved space.

The four dock apps are Files, Terminal, Notes and System Information. Hover
labels, running dots and a distinct focused dot identify their state. Session
exit lives in the system menu and is explicitly labelled **Exit to Console**;
right-click exits only the legacy `--test` fixture. It does not shut down the OS.

The fixed WLAN and battery strings are removed until actual telemetry and
interactive panels exist. The clock shows HH:MM using the existing clock source
and only damages its rectangle when the displayed minute changes. The normal
event loop uses a 1000 ms poll timeout to update the clock and visible system
information during idle periods; no busy wait is introduced.

System Information uses the existing 72-byte SYS_SYSINFO ABI for initialized CPU
count, managed/free RAM, enumerable processes (including zombies), active
threads, and uptime. Display geometry comes from the framebuffer metadata.
Failed queries display an unavailable message. It does not invent CPU load,
network activity, hardware identity or installed-memory values. The old
screen/pitch/compositor-heap/mouse diagnostics remain in `--test`. The command
line `/bin/sysinfo` is unchanged.

To preserve the 1024-byte per-function stack ceiling, event dispatch is split
into non-inlined handlers and directory enumeration uses a fallibly allocated
1 KiB batch rather than a stack array. PTY reads use 256-byte chunks within the
existing 4096-byte per-event drain budget.

Font atlas regeneration is optional and host-only:
`python scripts/build_compositor_font.py /path/to/DejaVuSans.ttf` (Pillow).
Normal builds use the checked-in atlas without Pillow. The source font's license
is retained in `user/compositor/assets/DejaVu-LICENSE.txt` and copied into the
initramfs at `/usr/share/licenses/compositor/DejaVu-LICENSE.txt`.

## Verification

- `make test-compositor-host`: 20/20 PASS, including proportional alpha glyphs,
  unavailable telemetry, zero timer frequency, minute-only clock damage, scaled
  maximize/restore, and partial/full redraw equivalence at 1280x800/1920x1080.
- `make test-compositor`: retains the independent legacy font/palette oracle and
  adds a normal desktop launch, no fabricated status labels, dock-launched
  System Information with an independent CPU-count glyph oracle, right-click
  survival and explicit menu exit. BIOS/UEFI PASS at 1280x800, SMP=1.
- Release disassembly: largest fixed frame 968 bytes including saved registers;
  no XMM/YMM/ZMM register references. The kernel entry/ABI are unchanged.
- Guest screenshots: `build/compositor-polish-{bios,uefi}.ppm`.

Authenticated sessions, network/power panels, an activity monitor, arbitrary UI
scaling, external application IPC and process isolation remain separate work.
No physical hardware acceptance or tear-free scanout claim is made.

## Fullscreen lag follow-up

The Dell screenshot confirms the 1920x1080 desktop and populated system panel,
but the user reports fullscreen lag specifically while moving the pointer.
This is not physical performance acceptance.
The system panel previously filled and damaged the entire client once per
second. It now initializes the background on creation/resize and only damages
the bounding pixels that actually changed in each text field. Identical samples
produce no client damage. Fixed-width fields erase trailing old digits and clear
stale telemetry on query failures. Contained damage rectangles are deduplicated
before the bounded-list overflow fallback, avoiding repeated fullscreen copies
from focus/resize/refresh in the same event.

Cursor save-under restoration now precedes timer/keyboard scene changes as well
as pointer motion. The host suite adds a maximized 1920x1080 regression: identical
samples produce zero damage and a changed uptime field damages fewer than 6000
pixels (rather than the full client), including shorter/longer counter strings.
The pointer path no longer sleeps unconditionally after every presentation:
SYS_NANOSLEEP rounds the old 1 ms request up to a timer tick, while SYS_POLL
already provides idle waiting. Mouse coalescing consumes at most 16 additional
packets per frame, preserving button-edge packets and leaving the rest queued
so a continuous stream cannot indefinitely delay presentation. A 64-motion
1920x1080 maximized-client host regression limits each pointer-only frame to
512 damaged pixels. Host verification is now 23/23 PASS.
The QEMU suite adds maximize/idle/restore with cursor preservation. Smoothness on
the Dell still requires a repeat test; full resize/restore necessarily repaints
the exposed area, and framebuffer memory attributes/scanout are unchanged.

## Title controls and 1080p allocation follow-up

The user narrowed the remaining pointer lag to approaching the top-left window
controls, including Terminal, and reported that System Information would not
maximize. The hover path invalidated the entire restored window, or the entire
title bar of a maximized window, for every change of hit region. Hover damage
now covers only the old/new control pixels (11x11 for each modern traffic-light
button), or the resize grip. Entering the client/title region adds no hover
damage. A new host regression checks Terminal and System Information in restored
and maximized states, bounds control damage to 242 pixels excluding the cursor,
and compares every repaired frame with a fresh full composition.

The compositor arena is now 32 MiB, within the existing SYS_MMAP cap; kernel
mapping and allocation contracts are unchanged. At 1920x1080, the scene alone
uses 8,294,400 pixel bytes. A 1040x700 system panel plus a newly allocated
1888x872 maximized client requires another 9,180,672 pixel bytes during a moving
reallocation, exceeding the old 16 MiB arena before other allocations. The
smaller initial Terminal buffer can fit the exact-capacity fallback, explaining
the difference. This is a bounded arena, not unlimited window capacity.

The guest suite now creates private ISO copies with a 1920x1080 Limine config,
boots BIOS and UEFI with no data disks and private OVMF variables, and verifies
three actual System Information maximize/restore cycles and normal menu exit.
It asserts the framebuffer resolution so a lower-resolution fallback cannot
pass. Host verification is 24/24 PASS. BIOS and UEFI both PASS the 1280x800
regression and the actual 1920x1080 three-cycle maximize/restore test. The ISO
and 592 MiB raw USB image are rebuilt. With wallpaper restored, the largest fixed frame is 968 bytes,
with no XMM/YMM/ZMM register references in the release disassembly.
The earlier host-only 1920x1080 tests used the host allocator and did not expose
the freestanding arena limit. Physical smoothness remains pending a Dell retest.
