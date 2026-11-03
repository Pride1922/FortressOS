//! Text: the anti-aliased proportional UI font (DejaVu Sans 14 px atlas,
//! 18 px line, see `assets/DejaVu-LICENSE.txt`) and the 8x16 bitmap font
//! for monospace grids. Printable ASCII; anything else renders as `?`.
pub use crate::glyphs::GLYPHS;

/// Line height of the proportional UI font in pixels.
pub const LINE_HEIGHT: usize = 18;

const ALPHA: &[u8; 95 * 16 * 18] = include_bytes!("../assets/ui-alpha.bin");
const ADVANCES: &[u8; 95] = include_bytes!("../assets/ui-advances.bin");

fn glyph(c: char) -> usize {
    if (' '..='~').contains(&c) {
        c as usize - 32
    } else {
        b'?' as usize - 32
    }
}

/// Width in pixels of `text` in the proportional UI font.
pub fn text_width(text: &str) -> usize {
    text.chars().map(|c| ADVANCES[glyph(c)] as usize).sum()
}

/// Colour of pixel (`x`, `y`) of `text` drawn in `fg` over `bg`.
pub fn text_pixel(text: &str, mut x: usize, y: usize, fg: u32, bg: u32) -> u32 {
    if y >= 18 {
        return bg;
    }
    for c in text.chars() {
        let index = glyph(c);
        let advance = ADVANCES[index] as usize;
        if x < advance {
            let a = ALPHA[index * 16 * 18 + y * 16 + x] as u32;
            let mut color = 0xff000000;
            for shift in [0, 8, 16] {
                let f = (fg >> shift) & 255;
                let b = (bg >> shift) & 255;
                color |= ((f * a + b * (255 - a) + 127) / 255) << shift;
            }
            return color;
        }
        x -= advance;
    }
    bg
}

/// Synthetic bold: each glyph column is widened into its right neighbour.
pub fn text_pixel_bold(text: &str, mut x: usize, y: usize, fg: u32, bg: u32) -> u32 {
    if y >= 18 {
        return bg;
    }
    for c in text.chars() {
        let index = glyph(c);
        let advance = ADVANCES[index] as usize;
        if x < advance {
            let base = index * 16 * 18 + y * 16;
            let a0 = ALPHA[base + x] as u32;
            let a_prev = if x > 0 { ALPHA[base + x - 1] as u32 } else { 0 };
            let a = (a0 + ((255 - a0) * (a_prev * 5 / 8)) / 255).min(255);
            let mut color = 0xff000000;
            for shift in [0, 8, 16] {
                let f = (fg >> shift) & 255;
                let b = (bg >> shift) & 255;
                color |= ((f * a + b * (255 - a) + 127) / 255) << shift;
            }
            return color;
        }
        x -= advance;
    }
    bg
}
