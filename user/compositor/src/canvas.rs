//! A pixel area apps draw into: their shared window buffer (or a test
//! buffer). Backed by libui::Painter. Every operation clips to width x height;
//! `stride` may exceed `width` (app buffers use the screen width as stride).
use crate::theme;

pub struct Canvas<'a> {
    pub pixels: &'a mut [u32],
    pub width: usize,
    pub height: usize,
    pub stride: usize,
}

impl<'a> Canvas<'a> {
    /// None unless `pixels` really holds `height` rows of `stride`.
    pub fn new(pixels: &'a mut [u32], width: usize, height: usize, stride: usize) -> Option<Self> {
        let needed = stride.checked_mul(height)?;
        (width <= stride && needed <= pixels.len()).then_some(Self {
            pixels,
            width,
            height,
            stride,
        })
    }

    pub fn painter(&mut self) -> libui::Painter<'_> {
        libui::Painter::new(self.pixels, self.width, self.height, self.stride)
            .expect("validated in Canvas::new")
    }

    pub fn fill(&mut self, color: u32) {
        self.painter().fill(color);
    }

    pub fn fill_rect(&mut self, x: usize, y: usize, w: usize, h: usize, color: u32) {
        self.painter().fill_rect_xy(x, y, w, h, color);
    }

    /// One 8x16 bitmap glyph (monospace grids such as an editor).
    pub fn glyph(&mut self, x: usize, y: usize, ch: u8, fg: u32, bg: u32) {
        self.painter().glyph(x, y, ch, fg, bg);
    }

    /// Proportional anti-aliased UI text (the desktop's theme font).
    pub fn text(&mut self, x: usize, y: usize, text: &str, fg: u32, bg: u32) {
        self.painter().text(x, y, text, fg, bg);
    }

    #[allow(dead_code)]
    pub fn text_bold(&mut self, x: usize, y: usize, text: &str, fg: u32, bg: u32) {
        self.painter().text_bold(x, y, text, fg, bg);
    }

    #[allow(dead_code)]
    pub fn rounded_box(
        &mut self,
        x: usize,
        y: usize,
        w: usize,
        h: usize,
        radius: usize,
        outer_bg: u32,
        fill: u32,
        border: u32,
    ) {
        self.painter()
            .rounded_box(x, y, w, h, radius, outer_bg, fill, border);
    }

    #[allow(dead_code)]
    pub fn pill_button(
        &mut self,
        x: usize,
        y: usize,
        w: usize,
        h: usize,
        label: &str,
        fg: u32,
        fill: u32,
        border: u32,
        outer_bg: u32,
    ) {
        self.painter()
            .pill_button(x, y, w, h, label, fg, fill, border, outer_bg);
    }

    #[allow(dead_code)]
    pub fn file_icon(&mut self, x: usize, y: usize, kind: u8, row_bg: u32) {
        self.painter().file_icon(x, y, kind, row_bg);
    }

    pub fn battery_icon(&mut self, x: usize, y: usize, status: &crate::sys::BatteryStatus) {
        if x >= self.width || y >= self.height {
            return;
        }
        let draw_w = 22.min(self.width.saturating_sub(x));
        let draw_h = 10.min(self.height.saturating_sub(y));
        for dy in 0..draw_h {
            for dx in 0..draw_w {
                if let Some(col) = theme::render_battery_pixel(dx, dy, status, theme::PANEL) {
                    self.pixels[(y + dy) * self.stride + x + dx] = col;
                }
            }
        }
    }
}
