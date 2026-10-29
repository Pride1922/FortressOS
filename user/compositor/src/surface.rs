//! Packed RAM surface with bounded damage; VRAM pitch may include padding.
use crate::{font::GLYPHS, sys::FbInfoV1};
use alloc::vec::Vec;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Rect {
    pub x: usize,
    pub y: usize,
    pub w: usize,
    pub h: usize,
}

impl Rect {
    pub fn intersection(self, other: Self) -> Option<Self> {
        let x = self.x.max(other.x);
        let y = self.y.max(other.y);
        let right = self
            .x
            .saturating_add(self.w)
            .min(other.x.saturating_add(other.w));
        let bottom = self
            .y
            .saturating_add(self.h)
            .min(other.y.saturating_add(other.h));
        (x < right && y < bottom).then(|| Self {
            x,
            y,
            w: right - x,
            h: bottom - y,
        })
    }

    pub fn contains(self, x: usize, y: usize) -> bool {
        x >= self.x && y >= self.y && x - self.x < self.w && y - self.y < self.h
    }

    /// `self` minus `other` as up to four disjoint bands: full-width top and
    /// bottom, then left and right within the overlapping rows.
    pub fn subtract(self, other: Self) -> [Option<Self>; 4] {
        let Some(i) = self.intersection(other) else {
            return [Some(self), None, None, None];
        };
        let (bottom, i_bottom) = (self.y + self.h, i.y + i.h);
        let (right, i_right) = (self.x + self.w, i.x + i.w);
        [
            (i.y > self.y).then(|| Self {
                x: self.x,
                y: self.y,
                w: self.w,
                h: i.y - self.y,
            }),
            (i_bottom < bottom).then(|| Self {
                x: self.x,
                y: i_bottom,
                w: self.w,
                h: bottom - i_bottom,
            }),
            (i.x > self.x).then(|| Self {
                x: self.x,
                y: i.y,
                w: i.x - self.x,
                h: i.h,
            }),
            (i_right < right).then(|| Self {
                x: i_right,
                y: i.y,
                w: right - i_right,
                h: i.h,
            }),
        ]
    }

    fn area(self) -> usize {
        self.w * self.h
    }

    pub fn union(self, other: Self) -> Self {
        let x = self.x.min(other.x);
        let y = self.y.min(other.y);
        let right = (self.x.saturating_add(self.w)).max(other.x.saturating_add(other.w));
        let bottom = (self.y.saturating_add(self.h)).max(other.y.saturating_add(other.h));
        Self {
            x,
            y,
            w: right.saturating_sub(x),
            h: bottom.saturating_sub(y),
        }
    }
}

pub struct Surface {
    pub width: usize,
    pub height: usize,
    pub pixels: Vec<u32>,
    damage: [Rect; 16],
    count: usize,
}

impl Surface {
    pub fn damage_rect(&self, index: usize) -> Option<Rect> {
        (index < self.count).then(|| self.damage[index])
    }
    /// Pixels the next present() will copy (overlapping rects count twice,
    /// exactly as present() copies them).
    pub fn damage_pixels(&self) -> u64 {
        self.damage[..self.count]
            .iter()
            .map(|r| (r.w * r.h) as u64)
            .sum()
    }
    pub fn new(width: usize, height: usize) -> Option<Self> {
        if width == 0 || height == 0 {
            return None;
        }
        let len = width.checked_mul(height)?;
        let mut pixels = Vec::new();
        pixels.try_reserve_exact(len).ok()?;
        pixels.resize(len, 0);
        Some(Self {
            width,
            height,
            pixels,
            damage: [Rect {
                x: 0,
                y: 0,
                w: 0,
                h: 0,
            }; 16],
            count: 0,
        })
    }

    pub fn damage(&mut self, r: Rect) {
        let right = r.x.saturating_add(r.w).min(self.width);
        let bottom = r.y.saturating_add(r.h).min(self.height);
        if r.x >= right || r.y >= bottom {
            return;
        }
        let clipped = Rect {
            x: r.x,
            y: r.y,
            w: right - r.x,
            h: bottom - r.y,
        };
        // Focus, resize, client refresh and cursor repair can enqueue the same
        // area in one event. Present each contained region only once.
        let mut i = 0;
        while i < self.count {
            let old = self.damage[i];
            if old.x <= clipped.x
                && old.y <= clipped.y
                && old.x + old.w >= right
                && old.y + old.h >= bottom
            {
                return;
            }
            if clipped.x <= old.x
                && clipped.y <= old.y
                && right >= old.x + old.w
                && bottom >= old.y + old.h
            {
                self.damage.copy_within(i + 1..self.count, i);
                self.count -= 1;
            } else {
                i += 1;
            }
        }
        if self.count == self.damage.len() {
            // Full: fold the new rect into the entry whose bounding box grows
            // least, instead of repainting and presenting the whole screen.
            // Re-adding the union prunes any entries it now contains.
            let mut best = 0;
            let mut best_growth = usize::MAX;
            for (i, old) in self.damage.iter().enumerate() {
                let growth = old.union(clipped).area() - old.area();
                if growth < best_growth {
                    best = i;
                    best_growth = growth;
                }
            }
            let merged = self.damage[best].union(clipped);
            self.damage.copy_within(best + 1..self.count, best);
            self.count -= 1;
            self.damage(merged);
            return;
        }
        self.damage[self.count] = clipped;
        self.count += 1;
    }

    #[allow(dead_code)]
    pub fn fill(&mut self, color: u32) {
        self.pixels.fill(color);
        self.damage(Rect {
            x: 0,
            y: 0,
            w: self.width,
            h: self.height,
        });
    }

    #[allow(dead_code)] // Public drawing API, also exercised by the host suite.
    pub fn text(&mut self, x: usize, y: usize, text: &str, fg: u32, bg: u32) {
        let (mut cx, mut cy) = (x, y);
        for c in text.chars() {
            if c == '\n' {
                cx = x;
                cy = cy.saturating_add(16);
                continue;
            }
            let glyph = &GLYPHS[if (' '..='~').contains(&c) {
                c as usize - 32
            } else {
                b'?' as usize - 32
            }];
            for (row, bits) in glyph.iter().enumerate() {
                let py = cy.saturating_add(row);
                if py >= self.height {
                    break;
                }
                for col in 0..8 {
                    let px = cx.saturating_add(col);
                    if px < self.width {
                        self.pixels[py * self.width + px] =
                            if bits & (0x80 >> col) != 0 { fg } else { bg };
                    }
                }
            }
            self.damage(Rect {
                x: cx,
                y: cy,
                w: 8,
                h: 16,
            });
            cx = cx.saturating_add(8);
        }
    }

    /// Caller supplies the exclusively owned SYS_FBINFO mapping, valid until exit.
    /// REP MOVSD uses integer state, avoids VRAM reads, and acts as a compiler
    /// memory barrier. SSE2 must wait for kernel SIMD context preservation.
    /// The mapping is write-combining when the kernel's PAT allows it, so the
    /// final SFENCE drains the WC buffers before the frame counts as presented.
    pub unsafe fn present(&mut self, fb: &FbInfoV1) -> bool {
        let pitch = fb.pitch as usize;
        if fb.version != 1
            || fb.bpp != 32
            || fb.width as usize != self.width
            || fb.height as usize != self.height
            || pitch % 4 != 0
            || self.width.checked_mul(4).is_none_or(|n| pitch < n)
            || pitch
                .checked_mul(self.height)
                .is_none_or(|n| n as u64 > fb.fb_size)
            || fb.fb_user_vaddr < 4096
            || fb.fb_user_vaddr % 4 != 0
            || fb
                .fb_user_vaddr
                .checked_add(fb.fb_size)
                .is_none_or(|n| n >= 0x800000000000)
        {
            return false;
        }
        let vram = fb.fb_user_vaddr as *mut u32;
        for r in &self.damage[..self.count] {
            for y in r.y..r.y + r.h {
                unsafe {
                    core::arch::asm!("cld", "rep movsd",
                        inout("rsi") self.pixels.as_ptr().add(y*self.width+r.x) => _,
                        inout("rdi") vram.add(y*(pitch/4)+r.x) => _,
                        inout("rcx") r.w => _, options(nostack));
                }
            }
        }
        unsafe {
            core::arch::asm!("sfence", options(nostack, preserves_flags));
        }
        self.count = 0;
        true
    }
}
