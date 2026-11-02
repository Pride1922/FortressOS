//! A pixel area apps draw into: their shared window buffer (or a test
//! buffer). Every operation clips to width x height; `stride` may exceed
//! `width` (app buffers use the screen width as stride).
use crate::font::GLYPHS;
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

    pub fn fill(&mut self, color: u32) {
        self.fill_rect(0, 0, self.width, self.height, color);
    }

    pub fn fill_rect(&mut self, x: usize, y: usize, w: usize, h: usize, color: u32) {
        let x1 = x.saturating_add(w).min(self.width);
        let y1 = y.saturating_add(h).min(self.height);
        for row in y.min(y1)..y1 {
            self.pixels[row * self.stride + x.min(x1)..row * self.stride + x1].fill(color);
        }
    }

    /// One 8x16 bitmap glyph (monospace grids such as an editor).
    pub fn glyph(&mut self, x: usize, y: usize, ch: u8, fg: u32, bg: u32) {
        if x + 8 > self.width || y + 16 > self.height {
            return;
        }
        let glyph = &GLYPHS[if (32..=126).contains(&ch) {
            ch as usize - 32
        } else {
            0
        }];
        for (r, bits) in glyph.iter().enumerate() {
            let row = &mut self.pixels[(y + r) * self.stride + x..(y + r) * self.stride + x + 8];
            for (c, p) in row.iter_mut().enumerate() {
                *p = if bits & (0x80 >> c) != 0 { fg } else { bg };
            }
        }
    }

    /// Proportional anti-aliased UI text (the desktop's theme font).
    pub fn text(&mut self, x: usize, y: usize, text: &str, fg: u32, bg: u32) {
        let w = theme::text_width(text).min(self.width.saturating_sub(x));
        let h = 18.min(self.height.saturating_sub(y));
        for dy in 0..h {
            for dx in 0..w {
                self.pixels[(y + dy) * self.stride + x + dx] =
                    theme::text_pixel(text, dx, dy, fg, bg);
            }
        }
    }

    #[allow(dead_code)]
    pub fn text_bold(&mut self, x: usize, y: usize, text: &str, fg: u32, bg: u32) {
        let w = theme::text_width(text).min(self.width.saturating_sub(x));
        let h = 18.min(self.height.saturating_sub(y));
        for dy in 0..h {
            for dx in 0..w {
                self.pixels[(y + dy) * self.stride + x + dx] =
                    theme::text_pixel_bold(text, dx, dy, fg, bg);
            }
        }
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
        if x >= self.width || y >= self.height || w == 0 || h == 0 {
            return;
        }
        let draw_w = w.min(self.width.saturating_sub(x));
        let draw_h = h.min(self.height.saturating_sub(y));
        let rad = radius.min(w / 2).min(h / 2);
        for dy in 0..draw_h {
            let row = &mut self.pixels[(y + dy) * self.stride + x..(y + dy) * self.stride + x + draw_w];
            for (dx, p) in row.iter_mut().enumerate() {
                let outer_cov = theme::rounded_coverage(dx, dy, w, h, rad);
                if outer_cov == 0 {
                    *p = outer_bg;
                } else {
                    let inner_cov = if dx >= 1 && dx + 1 < w && dy >= 1 && dy + 1 < h {
                        theme::rounded_coverage(
                            dx - 1,
                            dy - 1,
                            w.saturating_sub(2),
                            h.saturating_sub(2),
                            rad.saturating_sub(1),
                        )
                    } else {
                        0
                    };
                    let inner = theme::blend_coverage(fill, border, inner_cov);
                    *p = theme::blend_coverage(inner, outer_bg, outer_cov);
                }
            }
        }
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
        if x >= self.width || y >= self.height || w == 0 || h == 0 {
            return;
        }
        let draw_w = w.min(self.width.saturating_sub(x));
        let draw_h = h.min(self.height.saturating_sub(y));
        let rad = 5.min(w / 2).min(h / 2);
        let tw = theme::text_width(label);
        let tx = w.saturating_sub(tw) / 2;
        let ty = h.saturating_sub(18) / 2;
        for dy in 0..draw_h {
            let row = &mut self.pixels[(y + dy) * self.stride + x..(y + dy) * self.stride + x + draw_w];
            for (dx, p) in row.iter_mut().enumerate() {
                let outer_cov = theme::rounded_coverage(dx, dy, w, h, rad);
                let base = if outer_cov == 0 {
                    outer_bg
                } else {
                    let inner_cov = if dx >= 1 && dx + 1 < w && dy >= 1 && dy + 1 < h {
                        theme::rounded_coverage(
                            dx - 1,
                            dy - 1,
                            w.saturating_sub(2),
                            h.saturating_sub(2),
                            rad.saturating_sub(1),
                        )
                    } else {
                        0
                    };
                    let inner = theme::blend_coverage(fill, border, inner_cov);
                    theme::blend_coverage(inner, outer_bg, outer_cov)
                };
                *p = if dx >= tx && dx < tx + tw && dy >= ty && dy < ty + 18 {
                    theme::text_pixel(label, dx - tx, dy - ty, fg, base)
                } else {
                    base
                };
            }
        }
    }

    #[allow(dead_code)]
    pub fn file_icon(&mut self, x: usize, y: usize, kind: u8, row_bg: u32) {
        if x + 16 > self.width || y + 14 > self.height {
            return;
        }
        match kind {
            0 => {
                // Folder icon: top-left tab + rounded folder body
                self.fill_rect(x + 1, y + 1, 6, 3, 0xff38bdf8);
                self.rounded_box(x, y + 3, 16, 11, 3, row_bg, 0xff007aff, 0xff38bdf8);
                self.fill_rect(x + 2, y + 5, 12, 1, 0xff7dd3fc);
            }
            1 => {
                // Executable binary badge
                self.rounded_box(x, y + 1, 16, 13, 3, row_bg, 0xff133223, 0xff22c55e);
                // Small '>' chevron + '_' inside badge
                if x + 12 <= self.width && y + 11 <= self.height {
                    self.fill_rect(x + 4, y + 5, 2, 1, 0xff4ade80);
                    self.fill_rect(x + 5, y + 6, 2, 1, 0xff4ade80);
                    self.fill_rect(x + 6, y + 7, 2, 1, 0xff4ade80);
                    self.fill_rect(x + 5, y + 8, 2, 1, 0xff4ade80);
                    self.fill_rect(x + 4, y + 9, 2, 1, 0xff4ade80);
                    self.fill_rect(x + 9, y + 9, 4, 1, 0xff4ade80);
                }
            }
            _ => {
                // Document icon with dog-ear corner
                self.rounded_box(x + 2, y, 12, 14, 2, row_bg, 0xff242c3a, 0xff64748b);
                self.fill_rect(x + 5, y + 4, 6, 1, 0xff94a3b8);
                self.fill_rect(x + 5, y + 7, 6, 1, 0xff94a3b8);
                self.fill_rect(x + 5, y + 10, 4, 1, 0xff94a3b8);
            }
        }
    }

    pub fn battery_icon(&mut self, x: usize, y: usize, status: &crate::sys::BatteryStatus) {
        if x >= self.width || y >= self.height {
            return;
        }
        let draw_w = 22.min(self.width.saturating_sub(x));
        let draw_h = 10.min(self.height.saturating_sub(y));
        for dy in 0..draw_h {
            for dx in 0..draw_w {
                if let Some(col) = crate::ui::render_battery_pixel(dx, dy, status, theme::PANEL) {
                    self.pixels[(y + dy) * self.stride + x + dx] = col;
                }
            }
        }
    }
}
