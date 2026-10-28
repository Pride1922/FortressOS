//! Shared desktop styling. Integer-only, allocation-free raster operations.
pub const PANEL: u32 = 0xff161d29;
pub const SURFACE: u32 = 0xff1d2736;
pub const BORDER: u32 = 0xff354358;
pub const TEXT: u32 = 0xffe7edf6;
pub const MUTED: u32 = 0xffa1aec2;
pub const ACCENT: u32 = 0xff69b7ff;
const ALPHA: &[u8; 95 * 16 * 18] = include_bytes!("../assets/ui-alpha.bin");
const ADVANCES: &[u8; 95] = include_bytes!("../assets/ui-advances.bin");

// 1080p uses native raster sizes. Only UHD needs the larger chrome preset.
pub fn scale(height: usize) -> usize {
    if height >= 2160 { 2 } else { 1 }
}

/// 4x4 subpixel coverage, evaluated in physical pixels rather than enlarged
/// logical pixels. Only the rounded corner squares need supersampling.
pub fn rounded_coverage(x: usize, y: usize, width: usize, height: usize, radius: usize) -> u32 {
    if x >= width || y >= height { return 0; }
    let radius = radius.min(width / 2).min(height / 2);
    if (x >= radius && x < width - radius) || (y >= radius && y < height - radius) {
        return 16;
    }
    let cx = if x < radius { radius * 8 } else { (width - radius) * 8 } as i64;
    let cy = if y < radius { radius * 8 } else { (height - radius) * 8 } as i64;
    let r2 = (radius * 8) as i64 * (radius * 8) as i64;
    let mut coverage = 0;
    for sy in [1, 3, 5, 7] {
        for sx in [1, 3, 5, 7] {
            let dx = (x * 8 + sx) as i64 - cx;
            let dy = (y * 8 + sy) as i64 - cy;
            coverage += (dx * dx + dy * dy <= r2) as u32;
        }
    }
    coverage
}

pub fn blend_coverage(fg: u32, bg: u32, coverage: u32) -> u32 {
    let mut result = 0xff000000;
    for shift in [0, 8, 16] {
        let f = (fg >> shift) & 255;
        let b = (bg >> shift) & 255;
        result |= ((f * coverage + b * (16 - coverage) + 8) / 16) << shift;
    }
    result
}

fn glyph(c: char) -> usize {
    if (' '..='~').contains(&c) {
        c as usize - 32
    } else {
        b'?' as usize - 32
    }
}

pub fn text_width(text: &str) -> usize {
    text.chars().map(|c| ADVANCES[glyph(c)] as usize).sum()
}

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

pub fn desktop_pixel(x: usize, y: usize, width: usize, height: usize) -> u32 {
    let nx = (x * 1024 / width.max(1)) as i32;
    let ny = (y * 1024 / height.max(1)) as i32;
    let distance = (nx - 760).abs() + (ny - 640).abs();
    let glow = (900 - distance).max(0) as u32;
    let r = 12 + glow * 9 / 900;
    let g = 18 + glow * 24 / 900;
    let b = 30 + glow * 47 / 900;
    0xff000000 | r << 16 | g << 8 | b
}
