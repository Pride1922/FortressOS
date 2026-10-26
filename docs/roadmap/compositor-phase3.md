# Rust compositor Phase 3 — 2026-10-26

The freestanding `/bin/compositor` now maintains a packed `Vec<u32>` RAM
surface matching the SYS_FBINFO framebuffer. Sixteen bounded damage rectangles
clip to the surface; overflow falls back to full-surface damage. Present copies
only damaged rows, respecting VRAM pitch and validating v1/32-bpp metadata and
mapping bounds. The blitter uses integer `REP MOVSD`, with no VRAM reads.

`user/compositor/src/font.rs` ports all 95 printable ASCII 8x16 glyphs from
`src/drivers/font.h` byte for byte. Text supports foreground/background colors,
newlines, clipping, and `?` for unsupported characters.

The 16x16 arrow has opaque fill/outline and half-alpha edge pixels. Save-under
storage lives on the heap. Movement restores the old RAM rectangle, captures
the new background, composites the sprite, then presents both damaged areas.
Scene edits beneath a visible cursor must be bracketed by `hide`/`show`.

Input opens `/dev/input/mouse`, falling back to `/dev/mouse0`, and blocks in
`SYS_POLL(..., -1)` before reading four-byte `[buttons, dx, dy, dz]` reports.
Signed X/Y motion clamps to the last pixel, with positive Y down for the USB
boot mouse device. Wheel bytes are consumed but have no UI action. Right click
exits and closes the descriptor. Run `sudo /bin/compositor`; the existing mouse
device permission policy remains root-owned 0660.

Prerequisite fixes: the mouse FIFO now uses a named Rank-1 lock rather than an
uninitialized zero lock. The bounded compositor heap is 16 MiB; the ELF image
admission ceiling increases globally from 4 MiB to 20 MiB (stack/restorer remain
additional). Mapping, rollback, permissions, and ownership paths are unchanged.
Each compositor launch reserves its whole static arena; modes requiring more
than the available heap fail cleanly. This is a bounded interim allocator until
a userspace dynamic-memory mapping API exists.

## Verification

- `make test-compositor-host`: 3/3 actual Rust module tests PASS. Covers glyph
  foreground/background pixels, clipping/overflow, tiny surfaces, save-under
  restoration over text, overlapping movement and edges, damage-only writes,
  VRAM pitch/padding, invalid metadata, damage overflow, and no-damage presents.
- `make test-elf-page-host`: ASan/UBSan page initialization (16,388 cases) and
  actual loader tests PASS, including existing rollback/permissions checks and
  new 16 MiB admission / aggregate-limit rejection before space allocation.
- `python3 scripts/test_compositor.py`: BIOS and UEFI, SMP=1, 1280x800 PASS.
  ISO only, no data disks, private OVMF variables, bounded waits, QEMU reaped.
  QMP screenshots compare all title pixels to the independent C font table,
  the Rust table to all C glyph bytes, translated cursor sprite pixels,
  exact restored backgrounds, round-trip alpha edges, and both screen clamps.
  Actual QMP mouse reports wake the compositor; right-click returns to shell.
  Serial evidence: `build/compositor-bios.log`, `build/compositor-uefi.log`.
- Release disassembly contains no XMM/YMM/ZMM register use; largest observed
  stack subtraction is 792 bytes in `rust_main` (840 including saved registers).

Retained initial failures: the original 4 MiB ELF ceiling prevented allocation
at QEMU's default 1280x800 mode; after increasing it, mouse polling exposed the
zero-initialized FIFO lock. Both were fixed before the successful matrix.

## Evidence boundaries

SSE2 remains deferred: AGENTS.md section 4 says SIMD state management is not
implemented. Enabling SSE2 blits requires a separate scheduler/SIMD context
preservation change. No SIMD performance claim is made.

Save-under removes trails and avoids an intermediate cursor-removed present.
There is no vblank/page-flip ABI, so scanout-level tear/flicker freedom is not
guaranteed. SYS_POLL blocks rather than busy waiting in userspace; neither total
machine idle CPU nor the existing USB polling worker's CPU cost was measured.
No SMP compositor matrix or physical hardware acceptance is claimed. The kernel
console still shares the framebuffer; exclusive display ownership is future work.
