//! Geometry and integer-only coverage maths shared by every widget.

#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct Rect {
    pub x: usize,
    pub y: usize,
    pub w: usize,
    pub h: usize,
}

/// Shorthand for `Rect { x, y, w, h }`.
pub const fn rect(x: usize, y: usize, w: usize, h: usize) -> Rect {
    Rect { x, y, w, h }
}

impl Rect {
    pub const fn new(x: usize, y: usize, w: usize, h: usize) -> Self {
        Self { x, y, w, h }
    }

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

    pub fn area(self) -> usize {
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

    /// Shrinks by `by` on every side (never below zero size).
    pub fn inset(self, by: usize) -> Self {
        Self {
            x: self.x.saturating_add(by),
            y: self.y.saturating_add(by),
            w: self.w.saturating_sub(by * 2),
            h: self.h.saturating_sub(by * 2),
        }
    }
}

/// 4x4 subpixel coverage (0..=16), evaluated in physical pixels rather than
/// enlarged logical pixels. Only the rounded corner squares need supersampling.
pub fn rounded_coverage(x: usize, y: usize, width: usize, height: usize, radius: usize) -> u32 {
    if x >= width || y >= height {
        return 0;
    }
    let radius = radius.min(width / 2).min(height / 2);
    if (x >= radius && x < width - radius) || (y >= radius && y < height - radius) {
        return 16;
    }
    let cx = if x < radius {
        radius * 8
    } else {
        (width - radius) * 8
    } as i64;
    let cy = if y < radius {
        radius * 8
    } else {
        (height - radius) * 8
    } as i64;
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

/// Blends `fg` over `bg` with a 0..=16 coverage; the result is opaque.
pub fn blend_coverage(fg: u32, bg: u32, coverage: u32) -> u32 {
    let mut result = 0xff000000;
    for shift in [0, 8, 16] {
        let f = (fg >> shift) & 255;
        let b = (bg >> shift) & 255;
        result |= ((f * coverage + b * (16 - coverage) + 8) / 16) << shift;
    }
    result
}
