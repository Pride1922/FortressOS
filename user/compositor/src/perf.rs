//! Phase 0 frame timing. Each frame is split into two CLOCK_MONOTONIC spans:
//!   render  = wake from poll -> scene ready (event handling + RAM repair)
//!   present = RAM shadow buffer -> VRAM blit of the damaged rectangles
//! `px` is the number of pixels presented, so present throughput (MB/s) shows
//! whether the framebuffer mapping (UC vs WC) or the repaint code dominates.
//! Integer-only formatting: the compositor is built soft-float.
use crate::font::GLYPHS;
use crate::preferences::Text;
use crate::surface::{Rect, Surface};
use crate::sys;
use core::fmt::Write;

pub fn now_ns() -> u64 {
    let mut ts = sys::Timespec::default();
    if sys::clock_gettime(1, &mut ts) != 0 || ts.tv_sec < 0 || ts.tv_nsec < 0 {
        return 0;
    }
    (ts.tv_sec as u64)
        .saturating_mul(1_000_000_000)
        .saturating_add(ts.tv_nsec as u64)
}

#[derive(Clone, Copy, Default)]
pub struct Span {
    pub frames: u64,
    pub render_sum: u64,
    pub render_max: u64,
    pub present_sum: u64,
    pub present_max: u64,
    pub px_sum: u64,
    pub px_max: u64,
}

impl Span {
    pub fn add(&mut self, render: u64, present: u64, px: u64) {
        self.frames += 1;
        self.render_sum += render;
        self.render_max = self.render_max.max(render);
        self.present_sum += present;
        self.present_max = self.present_max.max(present);
        self.px_sum += px;
        self.px_max = self.px_max.max(px);
    }

    /// "12 fr | render 4.1/9.8 ms | present 12.3/20.1 ms | 1.9 MB/fr | 160 MB/s"
    /// (avg/max per frame; MB/s is bytes presented per second of present time).
    pub fn format<const N: usize>(&self, out: &mut Text<N>) {
        let n = self.frames.max(1);
        let _ = write!(out, "{} fr | render ", self.frames);
        ms(out, self.render_sum / n);
        let _ = out.write_str("/");
        ms(out, self.render_max);
        let _ = out.write_str(" ms | present ");
        ms(out, self.present_sum / n);
        let _ = out.write_str("/");
        ms(out, self.present_max);
        let _ = out.write_str(" ms | ");
        let bytes_per_frame = self.px_sum * 4 / n;
        let _ = write!(
            out,
            "{}.{} MB/fr | ",
            bytes_per_frame / 1_000_000,
            bytes_per_frame / 100_000 % 10
        );
        if self.present_sum > 0 {
            // bytes/ns * 1000 = MB/s
            let _ = write!(out, "{} MB/s", self.px_sum * 4 * 1000 / self.present_sum);
        } else {
            let _ = out.write_str("- MB/s");
        }
    }
}

fn ms<const N: usize>(out: &mut Text<N>, ns: u64) {
    let tenths = ns / 100_000;
    let _ = write!(out, "{}.{}", tenths / 10, tenths % 10);
}

pub struct FrameStats {
    /// Draw the live overlay (`compositor --perf`).
    pub hud: bool,
    window: Span,
    window_start: u64,
    last: Text<128>,
    hud_dirty: bool,
    pub session: Span,
}

impl FrameStats {
    pub fn new(hud: bool) -> Self {
        Self {
            hud,
            window: Span::default(),
            window_start: 0,
            last: Text::from_str("perf: measuring..."),
            hud_dirty: true,
            session: Span::default(),
        }
    }

    /// Record one presented frame. Once per second the window is summarised
    /// to stdout (serial) and the HUD text is refreshed. Idle seconds (only
    /// the clock tick) are not logged.
    pub fn record(&mut self, render: u64, present: u64, px: u64, end: u64) {
        self.window.add(render, present, px);
        self.session.add(render, present, px);
        if self.window_start == 0 {
            self.window_start = end;
        }
        if end.saturating_sub(self.window_start) < 1_000_000_000 {
            return;
        }
        self.last.clear();
        let _ = self.last.write_str("perf 1s: ");
        self.window.format(&mut self.last);
        self.hud_dirty = true;
        if self.window.frames >= 2 {
            let mut line = Text::<160>::from_str("[COMPOSITOR] [PERF] ");
            let _ = line.write_str(self.last.as_str());
            let _ = line.write_str("\n");
            sys::write(1, line.as_bytes());
        }
        self.window = Span::default();
        self.window_start = end;
    }

    fn hud_rect(&self, s: &Surface) -> Rect {
        let scale = crate::window::theme::scale(s.height);
        Rect {
            x: 8 * scale,
            y: 28 * scale,
            w: (self.last.len() * 8 + 12).min(s.width.saturating_sub(8 * scale)),
            h: 20,
        }
    }

    /// Draw the overlay after scene repair and before the cursor/present.
    /// Redrawn only when its text changed or repair painted over it, and
    /// damaged as one rect so it cannot overflow the damage list.
    pub fn draw_hud(&mut self, s: &mut Surface) {
        if !self.hud {
            return;
        }
        let r = self.hud_rect(s);
        let mut covered = false;
        let mut i = 0;
        while let Some(d) = s.damage_rect(i) {
            covered |= d.intersection(r).is_some();
            i += 1;
        }
        if !self.hud_dirty && !covered {
            return;
        }
        self.hud_dirty = false;
        let bottom = (r.y + r.h).min(s.height);
        for y in r.y..bottom {
            s.pixels[y * s.width + r.x..y * s.width + r.x + r.w].fill(0xff000000);
        }
        let (mut cx, cy) = (r.x + 6, r.y + 2);
        for c in self.last.as_str().chars() {
            if cx + 8 > r.x + r.w {
                break;
            }
            let glyph = &GLYPHS[if (' '..='~').contains(&c) {
                c as usize - 32
            } else {
                b'?' as usize - 32
            }];
            for (row, bits) in glyph.iter().enumerate() {
                let py = cy + row;
                if py >= bottom {
                    break;
                }
                for col in 0..8 {
                    if bits & (0x80 >> col) != 0 {
                        s.pixels[py * s.width + cx + col] = 0xff7ee787;
                    }
                }
            }
            cx += 8;
        }
        s.damage(r);
    }
}
