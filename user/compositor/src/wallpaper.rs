//! Wallpaper loader and renderer for FortressOS Compositor.

use alloc::vec::Vec;
use crate::sys;

pub struct Wallpaper {
    #[allow(dead_code)]
    pub orig_w: u32,
    #[allow(dead_code)]
    pub orig_h: u32,
    pub bg_color: u32,
    pub crop_w: u32,
    pub crop_h: u32,
    pub pixels: Vec<u32>,
}

impl Wallpaper {
    pub fn load(path: &str) -> Option<Self> {
        let fd = sys::open(path, 0);
        if fd < 0 {
            return None;
        }

        let mut header = [0u8; 24];
        let mut total = 0;
        while total < 24 {
            let n = sys::read(fd as i32, &mut header[total..]);
            if n <= 0 {
                sys::close(fd as i32);
                return None;
            }
            total += n as usize;
        }

        if &header[0..4] != b"FWP1" {
            sys::close(fd as i32);
            return None;
        }

        let orig_w = u32::from_le_bytes(header[4..8].try_into().ok()?);
        let orig_h = u32::from_le_bytes(header[8..12].try_into().ok()?);
        let bg_color = u32::from_le_bytes(header[12..16].try_into().ok()?);
        let crop_w = u32::from_le_bytes(header[16..20].try_into().ok()?);
        let crop_h = u32::from_le_bytes(header[20..24].try_into().ok()?);

        let pixel_count = (crop_w as usize).checked_mul(crop_h as usize)?;
        let byte_count = pixel_count.checked_mul(4)?;
        let mut pixels = Vec::new();
        pixels.try_reserve_exact(pixel_count).ok()?;
        pixels.resize(pixel_count, 0);

        let byte_slice = unsafe {
            core::slice::from_raw_parts_mut(pixels.as_mut_ptr() as *mut u8, byte_count)
        };

        let mut read_bytes = 0;
        while read_bytes < byte_count {
            let n = sys::read(fd as i32, &mut byte_slice[read_bytes..]);
            if n <= 0 {
                break;
            }
            read_bytes += n as usize;
        }
        sys::close(fd as i32);

        if read_bytes != byte_count {
            return None;
        }

        Some(Self {
            orig_w,
            orig_h,
            bg_color,
            crop_w,
            crop_h,
            pixels,
        })
    }

    /// Row form of `pixel()` for `out.len()` pixels starting at (x0, y): one
    /// slice copy for the image part, a fill for the background around it.
    pub fn fill_row(&self, x0: usize, y: usize, screen_w: usize, screen_h: usize, out: &mut [u32]) {
        let (cw, ch) = (self.crop_w as usize, self.crop_h as usize);
        let (sx, sy) = if screen_w >= cw && screen_h >= ch {
            ((screen_w - cw) / 2, (screen_h - ch) / 2)
        } else {
            (0, 0)
        };
        let x1 = x0 + out.len();
        let (a, b) = (x0.max(sx), x1.min(sx + cw));
        if y < sy || y >= sy + ch || a >= b {
            out.fill(self.bg_color);
            return;
        }
        out[..a - x0].fill(self.bg_color);
        let src = (y - sy) * cw + (a - sx);
        out[a - x0..b - x0].copy_from_slice(&self.pixels[src..src + (b - a)]);
        out[b - x0..].fill(self.bg_color);
    }

    /// Per-pixel reference for `fill_row()`, kept for the host tests.
    #[allow(dead_code)]
    #[inline]
    pub fn pixel(&self, x: usize, y: usize, screen_w: usize, screen_h: usize) -> u32 {
        let cw = self.crop_w as usize;
        let ch = self.crop_h as usize;
        if screen_w >= cw && screen_h >= ch {
            let start_x = (screen_w - cw) / 2;
            let start_y = (screen_h - ch) / 2;
            if x >= start_x && x < start_x + cw && y >= start_y && y < start_y + ch {
                return self.pixels[(y - start_y) * cw + (x - start_x)];
            }
        } else if x < cw && y < ch {
            return self.pixels[y * cw + x];
        }
        self.bg_color
    }
}
