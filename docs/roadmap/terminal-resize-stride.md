# Terminal text distortion during resize — 2026-11-03

The user reports terminal letters briefly stretching during window resize and
returning to normal afterward. The compositor's Window::resize previously only
resized the flat pixel Vec before changing the rectangle width. Existing rows
were then interpreted with the new packed stride. The isolated terminal later
consumed EV_CONFIGURE and supplied new pixels, hiding the earlier corruption.
This is pixel-coordinate corruption, not a change in terminal font size.

Resize now preserves the overlapping raster at its original coordinates,
clears exposed columns/rows to the window background, and clips shrinking
content. Widening moves rows bottom-to-top after reserving/resizing; narrowing
moves them top-to-bottom before truncation. Mixed width/height changes are
covered. It uses the existing Vec allocation and overlapping copy operations,
not another full-size frame buffer. Capacity failure leaves the old frame and
geometry intact; reserve headroom arithmetic is saturating.

A successful resize marks built-in app content invalid. AppHost::render and
paint_invalid consume that invalidation even if a shrink/grow round trip ends
at the previous dimensions between render calls. Otherwise clipped content can
be mistaken for a valid old canvas. The flag does not cause continuous repaint
once consumed. Legacy panel invalidation and external configure/redraw routing
remain intact. No desktop IPC ABI, PTY size notification, font scale, shared
buffer stride, display authorization or kernel change is needed.

## Verification

The real terminal glyph raster regression fails on the predecessor at pixel
38,8 immediately after resize and before client repaint. It passes after the
coordinate-preserving correction. Additional regression coverage checks every
retained/exposed pixel through widening, narrowing, mixed aspect ratios and
same-size calls; rejected/overflow/capacity-limit geometry preserves the old
frame; built-in resize round trips repaint correctly and return to idle.

Full compositor/libui host suite PASS: 72 passed, zero failed or ignored.
The normal freestanding compositor/bundled-client release build also succeeds
via `make build/compositor.elf`. Evidence is retained under
`build/terminal-resize-evidence/`, including the original glyph failure, the
initial built-in regression that motivated explicit invalidation, the final
full-suite log and native-build log. These are deterministic pixel/renderer
checks, not a physical desktop visual acceptance claim.

No image was created or replaced for this task. The next requested image build
must include this commit; the currently published DHCP live USB predates it.
