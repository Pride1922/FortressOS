# Rust compositor Phase 4 — 2026-10-26

`user/compositor/src/window.rs` adds a single-process window server with a
back-to-front `Vec<Window>`. Windows own a checked, fallibly allocated title
`String` and client `Vec<u32>`, outer `Rect`, stable ID, focus, drag capture and
offset, and minimized state. There is no external client IPC protocol yet.

Each frame has a 1px border and a 24px titlebar (excluding the border), with
active `#2A3A5E` / inactive `#1E2533` fills, clipped 8x16 title text, and `[X]`
and `[-]` controls with separate hover fills. Hit testing walks the stack in
reverse. A new left press raises/focuses the clicked window; title presses
capture dragging until release, including a final motion on the release
packet. Held buttons cannot repeatedly activate controls. Closing drops client
storage and focuses the next visible window. Minimizing retains client storage
and exposes a bottom task button that restores and raises the window.

Frame sizes include decorations. Startup creates System Monitor at (80,120),
320x200, and Palette Canvas at (260,220), 280x180, initially focused. The monitor
shows framebuffer dimensions and pitch, live Talc allocated-layout bytes as KiB
against the 16 MiB heap capacity, and current mouse coordinates. Telemetry uses
a fixed formatting buffer and the mouse path performs no allocations.

Positions remain nonnegative. Drag capture preserves its click offset, and
right/bottom overflow clips to screen bounds. Every event restores the cursor's
RAM save-under before scene edits. Changes enqueue old/new frame bounds and
affected focus/control/monitor/task areas. For each pending clipped rectangle,
composition repairs the desktop and intersects every visible window in z-order;
the cursor is saved/composited last, followed by the existing damage blitter.
There is no intermediate cursor-removed present. Sixteen-rectangle overflow
still falls back to a full redraw.

## Display interference fixed

The initial pixel oracle caught a startup diagnostic overwriting monitor
content after presentation. Startup output now precedes presentation, and the
runner waits for actual frame/cursor pixels after the readiness banner.

The longer drag campaign then caught per-packet USB mouse diagnostics scrolling
the console over undamaged windows. Both `Report popped` and `Event` traces now
use raw UART output in `usb_hid.c`, retaining their COM1 text without drawing to
the framebuffer or appending these high-volume traces to dmesg. Ordinary device
lifecycle logging is unchanged. No IRQ, queue, scheduler, mapping or ownership
behavior changes were needed in this phase.

## Verification

- `make test-compositor-host`: 6/6 PASS, actual Rust modules. Existing Phase 3
  coverage plus hit-test edges, reverse z-order selection, raising/focus,
  captured/final-release motion, hover colors, minimize/restore/close and held
  button edges. Partial repair is compared pixel-for-pixel with a fresh full
  composition across overlapping/clipped drags. Every changed pixel must lie
  in queued damage, and every queued rectangle must be within screen bounds.
- `make test-compositor`: BIOS and UEFI, SMP=1, default 1280x800 PASS. Retains
  C-font byte/pixel oracle and cursor movement/restoration/clamping checks.
  Adds decorated title, screen/pitch/mouse telemetry pixel checks, inactive
  window raising, both drag/occlusion directions, exposed palette byte checks,
  right/bottom clipping and return, release capture, both control hover fills,
  minimize/task restore, focus fallback, both closes, and an empty-desktop pixel
  audit. QMP drives real USB relative packets and button edges. ISO only, no
  data disks, private OVMF vars, bounded waits and guaranteed QEMU reap.
- `make test-hub-host`: USB Hub/HID actual-code ASan/UBSan suite PASS, including
  mouse decoding, edge transitions, bounding, ring demux and FIFO behavior.
- `make test-usb-mouse`: BIOS direct-root and UEFI hub topology PASS, including
  COM1 packet traces, mouse reports and subsequent USB keyboard shell commands.
- `cargo fmt --check` and Python syntax validation PASS. Release disassembly
  has no XMM/YMM/ZMM use; largest stack subtraction is 856 bytes in `rust_main`,
  904 bytes including saved registers, below the 1024-byte frame ceiling.

Evidence: `build/compositor-bios.log`, `build/compositor-uefi.log`,
`build/compositor-windows-{bios,uefi}.ppm`, and
`build/compositor-usb-mouse-regression.log`.

## Boundaries

Run `sudo /bin/compositor`; left-drag titlebars, click task buttons to restore,
and right-click to exit. Client windows are demos within the compositor process;
there is no process-isolated window API. Updates remain event-driven through
blocking SYS_POLL. No vblank/page-flip ABI exists, so scanout tear freedom is
not guaranteed. Other kernel console writers still share the framebuffer;
exclusive display ownership remains future work. No SMP matrix, measured idle
CPU/latency, throughput benchmark or physical-hardware acceptance is claimed.
