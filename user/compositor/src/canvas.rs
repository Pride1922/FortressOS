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
}
