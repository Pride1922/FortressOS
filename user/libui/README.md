# FortressOS App SDK (`libui`)

`libui` is the freestanding, `#![no_std]`, allocation-free GUI toolkit and client runtime for FortressOS desktop applications.

## Architecture & Principles

1. **Integer-Only Rasterization:** No floating-point instructions or SSE/FPU dependencies (`-mno-sse -mno-mmx -msoft-float`).
2. **Deterministic Shading:** All rounded corners and anti-aliased borders use 4x4 subpixel coverage math (`rounded_coverage` / `blend_coverage`).
3. **Compare-Before-Write Damage:** `Painter` tracks modified bounding boxes and avoids repainting identical pixels. Unchanged scenes emit zero damage to the window server.
4. **Zero-Allocation Core:** Primitives, widgets, layouts, and state management require no heap allocator.
5. **Client Runtime (`--features client`):** Connects to the desktop window server via SHM (`SYS_SHM_MAP`) and IPC pipes, providing event loops, argv parsing, and the `app_main!` entry macro.

---

## Writing an App in 50 Lines

```rust
#![no_std]
#![no_main]

use libui::client::{App, Args, Event};
use libui::gfx::rect;
use libui::state::{Grid, hit};
use libui::theme;

libui::app_main!(main, heap = 1 << 20);

fn main(_args: Args) -> i32 {
    let Some(mut app) = App::connect(240, 200, "Hello App") else {
        return 1;
    };
    let grid = Grid::new(12, 60, 2, 100, 36, 12);
    let mut clicks = 0;

    render(&mut app, clicks, &grid);

    loop {
        match app.next_event() {
            Event::Configure => render(&mut app, clicks, &grid),
            Event::Close => return 0,
            Event::Click(x, y) => {
                if hit(grid.cell(0), x, y) {
                    clicks += 1;
                    render(&mut app, clicks, &grid);
                }
            }
            _ => {}
        }
    }
}

fn render(app: &mut App, clicks: usize, grid: &Grid) {
    let damage = {
        let mut p = app.painter().with_damage();
        p.fill(theme::PANEL);
        p.label(12, 16, 200, "Interactive libui App", theme::TEXT);
        p.button(grid.cell(0), "Click Me", false, false);
        p.finish()
    };
    app.present(damage);
}
```

---

## Module Overview

| Module | Contents |
| --- | --- |
| `libui::gfx` | `Rect`, `rect(x, y, w, h)`, `rounded_coverage`, `blend_coverage`. |
| `libui::font` | DejaVu Sans anti-aliased proportional UI font (`text_pixel`, `text_width`), 8x16 monospace `GLYPHS`. |
| `libui::theme` | Canonical palette: `PANEL`, `SURFACE`, `BORDER`, `TEXT`, `MUTED`, `ACCENT`, etc. |
| `libui::style` | `SurfaceStyle`: unified rounded-rectangle geometry with inner borders, top highlights, and shadows. |
| `libui::painter` | `Painter<'a>`: clipping, coordinate scaling, vertical scrolling, and bounded dirty rectangle tracking. |
| `libui::widgets` | Immediate-mode `label`, `button`, `button_accent`, `pill_button`, `card`, `sheet_card`, `text_input`, `badge`, `glyph`. |
| `libui::state` | `TextField<N>` (zero-heap editable text), `FocusRing<N>` (Tab navigation), `Grid` (2D layout), `hit(rect, x, y)`. |
| `libui::client` | `App`, `Event`, `Wait`, `Args`, `raw_sys` (minimal freestanding syscalls), and `app_main!` macro. |
| `libui::proto` | FortressOS desktop IPC wire protocol (`Msg`, `KeyEvent`, `clip_damage`). |

---

## Building and Packaging

Applications depend on `libui`:
```toml
[dependencies]
libui = { path = "../../libui", features = ["client"] }
```

And link with the standard freestanding linker script `user/libui/app.ld`:
```toml
[target.x86_64-unknown-none]
rustflags = ["-C", "link-arg=-T../../libui/app.ld"]
```

See [docs/desktop-apps-guide.md](../../docs/desktop-apps-guide.md) for a comprehensive guide and the reference Calculator app implementation (`user/apps/calc`).
