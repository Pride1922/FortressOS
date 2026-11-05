//! Text: anti-aliased proportional UI font family (DejaVu Sans at 18px Title,
//! 14px Body, and 12px Caption, see `assets/DejaVu-LICENSE.txt`) and 8x16 bitmap
//! font for monospace grids. Printable ASCII (32..=126); other characters render as `?`.
pub use crate::glyphs::GLYPHS;

/// Typography scale choices for desktop widgets.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum FontSize {
    /// 18 px font size, 22 px line height (page headers, hero labels).
    Title,
    /// 14 px font size, 18 px line height (standard UI body text, buttons).
    #[default]
    Body,
    /// 12 px font size, 16 px line height (helper descriptions, badges, timestamps).
    Caption,
}

/// Line height of the 18 px Title font in pixels.
pub const TITLE_LINE_HEIGHT: usize = 22;
/// Line height of the 14 px Body font in pixels.
pub const BODY_LINE_HEIGHT: usize = 18;
/// Line height of the 12 px Caption font in pixels.
pub const CAPTION_LINE_HEIGHT: usize = 16;
/// Default UI line height (14 px Body).
pub const LINE_HEIGHT: usize = BODY_LINE_HEIGHT;

const ALPHA_BODY: &[u8; 95 * 16 * 18] = include_bytes!("../assets/ui-alpha.bin");
const ADVANCES_BODY: &[u8; 95] = include_bytes!("../assets/ui-advances.bin");

const ALPHA_TITLE: &[u8; 95 * 20 * 22] = include_bytes!("../assets/ui-title-alpha.bin");
const ADVANCES_TITLE: &[u8; 95] = include_bytes!("../assets/ui-title-advances.bin");

const ALPHA_CAPTION: &[u8; 95 * 14 * 16] = include_bytes!("../assets/ui-caption-alpha.bin");
const ADVANCES_CAPTION: &[u8; 95] = include_bytes!("../assets/ui-caption-advances.bin");

#[inline]
fn glyph(c: char) -> usize {
    if (' '..='~').contains(&c) {
        c as usize - 32
    } else {
        b'?' as usize - 32
    }
}

/// Returns the line height in pixels for the given `FontSize`.
#[inline]
pub const fn line_height(size: FontSize) -> usize {
    match size {
        FontSize::Title => TITLE_LINE_HEIGHT,
        FontSize::Body => BODY_LINE_HEIGHT,
        FontSize::Caption => CAPTION_LINE_HEIGHT,
    }
}

/// Width in pixels of `text` in the specified font size.
pub fn text_width_sized(size: FontSize, text: &str) -> usize {
    let advances = match size {
        FontSize::Title => ADVANCES_TITLE,
        FontSize::Body => ADVANCES_BODY,
        FontSize::Caption => ADVANCES_CAPTION,
    };
    text.chars().map(|c| advances[glyph(c)] as usize).sum()
}

/// Width in pixels of `text` in the default Body (14 px) font.
#[inline]
pub fn text_width(text: &str) -> usize {
    text_width_sized(FontSize::Body, text)
}

/// Colour of pixel (`x`, `y`) of `text` drawn in `fg` over `bg` for `size`.
pub fn text_pixel_sized(size: FontSize, text: &str, mut x: usize, y: usize, fg: u32, bg: u32) -> u32 {
    let (alpha, advances, cell_w, cell_h) = match size {
        FontSize::Title => (ALPHA_TITLE.as_slice(), ADVANCES_TITLE, 20usize, 22usize),
        FontSize::Body => (ALPHA_BODY.as_slice(), ADVANCES_BODY, 16usize, 18usize),
        FontSize::Caption => (ALPHA_CAPTION.as_slice(), ADVANCES_CAPTION, 14usize, 16usize),
    };
    if y >= cell_h {
        return bg;
    }
    for c in text.chars() {
        let index = glyph(c);
        let advance = advances[index] as usize;
        if x < advance {
            let a = alpha[index * cell_w * cell_h + y * cell_w + x] as u32;
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

/// Colour of pixel (`x`, `y`) of `text` drawn in `fg` over `bg` (Body 14 px).
#[inline]
pub fn text_pixel(text: &str, x: usize, y: usize, fg: u32, bg: u32) -> u32 {
    text_pixel_sized(FontSize::Body, text, x, y, fg, bg)
}

/// Synthetic bold for specified font size: widened into the right neighbour.
pub fn text_pixel_bold_sized(size: FontSize, text: &str, mut x: usize, y: usize, fg: u32, bg: u32) -> u32 {
    let (alpha, advances, cell_w, cell_h) = match size {
        FontSize::Title => (ALPHA_TITLE.as_slice(), ADVANCES_TITLE, 20usize, 22usize),
        FontSize::Body => (ALPHA_BODY.as_slice(), ADVANCES_BODY, 16usize, 18usize),
        FontSize::Caption => (ALPHA_CAPTION.as_slice(), ADVANCES_CAPTION, 14usize, 16usize),
    };
    if y >= cell_h {
        return bg;
    }
    for c in text.chars() {
        let index = glyph(c);
        let advance = advances[index] as usize;
        if x < advance {
            let base = index * cell_w * cell_h + y * cell_w;
            let a0 = alpha[base + x] as u32;
            let a_prev = if x > 0 { alpha[base + x - 1] as u32 } else { 0 };
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

/// Synthetic bold UI text in default Body (14 px) font.
#[inline]
pub fn text_pixel_bold(text: &str, x: usize, y: usize, fg: u32, bg: u32) -> u32 {
    text_pixel_bold_sized(FontSize::Body, text, x, y, fg, bg)
}

/// Returns the 8-bit glyph alpha (0..=255) for pixel (`x`, `y`) of `text` at `size`.
/// Returns 0 if (`x`, `y`) is outside glyph bounds or on empty background.
pub fn glyph_alpha_sized(size: FontSize, text: &str, mut x: usize, y: usize) -> u8 {
    let (alpha, advances, cell_w, cell_h) = match size {
        FontSize::Title => (ALPHA_TITLE.as_slice(), ADVANCES_TITLE, 20usize, 22usize),
        FontSize::Body => (ALPHA_BODY.as_slice(), ADVANCES_BODY, 16usize, 18usize),
        FontSize::Caption => (ALPHA_CAPTION.as_slice(), ADVANCES_CAPTION, 14usize, 16usize),
    };
    if y >= cell_h {
        return 0;
    }
    for c in text.chars() {
        let index = glyph(c);
        let advance = advances[index] as usize;
        if x < advance {
            return alpha[index * cell_w * cell_h + y * cell_w + x];
        }
        x -= advance;
    }
    0
}

/// Returns the 8-bit glyph alpha (0..=255) for synthetic bold pixel (`x`, `y`) of `text` at `size`.
pub fn glyph_alpha_bold_sized(size: FontSize, text: &str, mut x: usize, y: usize) -> u8 {
    let (alpha, advances, cell_w, cell_h) = match size {
        FontSize::Title => (ALPHA_TITLE.as_slice(), ADVANCES_TITLE, 20usize, 22usize),
        FontSize::Body => (ALPHA_BODY.as_slice(), ADVANCES_BODY, 16usize, 18usize),
        FontSize::Caption => (ALPHA_CAPTION.as_slice(), ADVANCES_CAPTION, 14usize, 16usize),
    };
    if y >= cell_h {
        return 0;
    }
    for c in text.chars() {
        let index = glyph(c);
        let advance = advances[index] as usize;
        if x < advance {
            let base = index * cell_w * cell_h + y * cell_w;
            let a0 = alpha[base + x] as u32;
            let a_prev = if x > 0 { alpha[base + x - 1] as u32 } else { 0 };
            return ((a0 + ((255 - a0) * (a_prev * 5 / 8)) / 255).min(255)) as u8;
        }
        x -= advance;
    }
    0
}

/// Returns the 8-bit glyph alpha for pixel (`x`, `y`) in the default Body font.
#[inline]
pub fn glyph_alpha(text: &str, x: usize, y: usize) -> u8 {
    glyph_alpha_sized(FontSize::Body, text, x, y)
}

/// Returns the 8-bit glyph alpha for synthetic bold pixel (`x`, `y`) in the default Body font.
#[inline]
pub fn glyph_alpha_bold(text: &str, x: usize, y: usize) -> u8 {
    glyph_alpha_bold_sized(FontSize::Body, text, x, y)
}
