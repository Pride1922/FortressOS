//! `compositor --bench-drag`: a scripted, repeatable window drag so every
//! optimisation phase gets a before/after number on QEMU and the Dell.
//! Drives the real WindowManager::mouse() path with synthetic pointer
//! positions, then repairs and presents exactly like the event loop does.
use crate::perf::{self, FrameStats, Span};
use crate::preferences::Text;
use crate::surface::{Rect, Surface};
use crate::sys::{self, FbInfoV1};
use crate::wallpaper::Wallpaper;
use crate::window::{Window, WindowManager};
use core::fmt::Write;

const STEP_X: usize = 8;
const STEP_Y: usize = 2;
const ROUND_TRIPS: usize = 2;

pub fn drag(
    s: &mut Surface,
    windows: &mut WindowManager,
    fb: &FbInfoV1,
    wallpaper: Option<&Wallpaper>,
    perf: &mut FrameStats,
) -> i32 {
    let (sw, sh) = (s.width, s.height);
    let rect = Rect {
        x: sw / 16,
        y: sh / 6,
        w: (sw / 2).max(320).min(sw.saturating_sub(16)),
        h: (sh / 2).max(200).min(sh.saturating_sub(64)),
    };
    let Some(win) = Window::new(4, "Drag benchmark", rect) else {
        sys::write(2, b"[COMPOSITOR] [BENCH] cannot allocate window\n");
        return 1;
    };
    windows.windows.push(win);
    windows.raise(4, s);

    // Grab the title bar away from the buttons; the start must be clear of
    // the snap zones (top bar, left/right screen edge).
    let max_x = sw.saturating_sub(rect.w + sw / 16);
    let travel = max_x.saturating_sub(rect.x) / STEP_X;
    if travel < 8 {
        sys::write(2, b"[COMPOSITOR] [BENCH] screen too small\n");
        return 1;
    }
    let (mut x, mut y) = (rect.x + rect.w / 3, rect.y + 12);
    windows.mouse(x, y, 0, s);
    windows.mouse(x, y, 1, s);
    windows.render_with_wallpaper(s, wallpaper);
    if !unsafe { s.present(fb) } {
        return 1;
    }

    let mut span = Span::default();
    for _ in 0..ROUND_TRIPS {
        for dir in [true, false] {
            for _ in 0..travel {
                if dir {
                    x += STEP_X;
                    y += STEP_Y;
                } else {
                    x -= STEP_X;
                    y -= STEP_Y;
                }
                let t0 = perf::now_ns();
                windows.mouse(x, y, 1, s);
                windows.render_with_wallpaper(s, wallpaper);
                perf.draw_hud(s);
                let px = s.damage_pixels();
                let t1 = perf::now_ns();
                if !unsafe { s.present(fb) } {
                    return 1;
                }
                let t2 = perf::now_ns();
                let (render, present) = (t1.saturating_sub(t0), t2.saturating_sub(t1));
                span.add(render, present, px);
                perf.record(render, present, px, t2);
            }
        }
    }
    windows.mouse(x, y, 0, s);

    let mut line = Text::<256>::default();
    let _ = write!(
        line,
        "[COMPOSITOR] [BENCH] drag {}x{} window on {}x{}: ",
        rect.w, rect.h, sw, sh
    );
    span.format(&mut line);
    let frame = (span.render_sum + span.present_sum) / span.frames.max(1);
    if frame > 0 {
        let _ = write!(line, " | ~{} fps", 1_000_000_000 / frame);
    }
    let _ = line.write_str("\n");
    sys::write(1, line.as_bytes());
    0
}
