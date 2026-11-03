//! `Painter`: an immediate-mode drawing context over a 32-bit pixel buffer
//! (a client's shared window buffer, a compositor window, or a test buffer).
//!
//! Widgets work in *logical* pixels. The painter maps them to device pixels
//! (`scale`), applies vertical scrolling, clips to the buffer and, when
//! damage tracking is on, compares every final pixel so unchanged widgets
//! never invalidate anything. Every coordinate is overflow-checked: hostile
//! or huge rectangles clip instead of panicking.
use crate::gfx::Rect;

pub struct Painter<'a> {
    pub pixels: &'a mut [u32],
    /// Device pixels per row that may be drawn.
    pub width: usize,
    /// Device rows that may be drawn.
    pub height: usize,
    /// Distance between rows in `pixels` (>= `width`).
    pub stride: usize,
    scale: usize,
    scroll_y: usize,
    track: bool,
    dirty: Option<Rect>,
}

impl<'a> Painter<'a> {
    /// None unless `pixels` really holds `height` rows of `stride`.
    /// Starts at scale 1, no scroll, damage tracking off.
    pub fn new(pixels: &'a mut [u32], width: usize, height: usize, stride: usize) -> Option<Self> {
        let needed = stride.checked_mul(height)?;
        (width <= stride && needed <= pixels.len()).then_some(Self {
            pixels,
            width,
            height,
            stride,
            scale: 1,
            scroll_y: 0,
            track: false,
            dirty: None,
        })
    }

    /// Integer scale factor from logical to device pixels (clamped 1..=4).
    pub fn with_scale(mut self, scale: usize) -> Self {
        self.scale = scale.clamp(1, 4);
        self
    }

    /// Logical rows scrolled off the top.
    pub fn with_scroll(mut self, scroll_y: usize) -> Self {
        self.scroll_y = scroll_y;
        self
    }

    /// Compare-before-write with a bounded damage rectangle (see `finish`).
    pub fn with_damage(mut self) -> Self {
        self.track = true;
        self
    }

    pub fn scale(&self) -> usize {
        self.scale
    }

    pub fn scroll_y(&self) -> usize {
        self.scroll_y
    }

    pub fn logical_width(&self) -> usize {
        self.width / self.scale
    }

    pub fn logical_height(&self) -> usize {
        self.height / self.scale
    }

    /// Damage so far, in device pixels relative to the buffer origin.
    pub fn damage(&self) -> Option<Rect> {
        self.dirty
    }

    /// Adds `r` (device pixels) to the damage, e.g. after direct buffer writes.
    pub fn add_damage(&mut self, r: Rect) {
        self.dirty = Some(self.dirty.map(|d| d.union(r)).unwrap_or(r));
    }

    /// Ends drawing; returns the changed area in device pixels (None when
    /// nothing changed or tracking is off).
    pub fn finish(self) -> Option<Rect> {
        self.dirty
    }

    /// Sets one device pixel (clipped; compared when tracking damage).
    #[inline]
    pub fn put(&mut self, x: usize, y: usize, color: u32) {
        if x >= self.width || y >= self.height {
            return;
        }
        let old = &mut self.pixels[y * self.stride + x];
        if !self.track {
            *old = color;
            return;
        }
        if *old == color {
            return;
        }
        *old = color;
        let r = Rect { x, y, w: 1, h: 1 };
        self.dirty = Some(self.dirty.map(|d| d.union(r)).unwrap_or(r));
    }

    /// Core raster loop: paints logical rectangle `r` with `f(lx, ly)`, where
    /// (`lx`, `ly`) are logical coordinates relative to `r`'s origin.
    #[inline]
    pub fn shade(&mut self, r: Rect, mut f: impl FnMut(usize, usize) -> u32) {
        let scale = self.scale;
        let Some(left) = r.x.checked_mul(scale) else { return };
        let Some(top) = r.y.checked_mul(scale) else { return };
        let Some(width) = r.w.checked_mul(scale) else { return };
        let Some(height) = r.h.checked_mul(scale) else { return };
        let draw_width = width.min(self.width.saturating_sub(left));
        let scroll = self.scroll_y.saturating_mul(scale);
        let first_y = scroll.saturating_sub(top).min(height);
        let last_y = scroll
            .saturating_add(self.height)
            .saturating_sub(top)
            .min(height);
        for y in first_y..last_y {
            if top + y < scroll {
                continue;
            }
            let row = top + y - scroll;
            for x in 0..draw_width {
                let p = f(x / scale, y / scale);
                self.put(left + x, row, p);
            }
        }
    }

    /// Whole buffer in one colour (device pixels, ignores scroll).
    pub fn fill(&mut self, color: u32) {
        if self.track {
            for y in 0..self.height {
                for x in 0..self.width {
                    self.put(x, y, color);
                }
            }
            return;
        }
        for y in 0..self.height {
            self.pixels[y * self.stride..y * self.stride + self.width].fill(color);
        }
    }

    /// Solid logical rectangle.
    pub fn fill_rect(&mut self, r: Rect, color: u32) {
        if self.track || self.scale != 1 || self.scroll_y != 0 {
            self.shade(r, |_, _| color);
            return;
        }
        // Fast path for plain client buffers: whole row slices.
        let x1 = r.x.saturating_add(r.w).min(self.width);
        let y1 = r.y.saturating_add(r.h).min(self.height);
        let x0 = r.x.min(x1);
        for row in r.y.min(y1)..y1 {
            self.pixels[row * self.stride + x0..row * self.stride + x1].fill(color);
        }
    }

    /// Solid logical rectangle with (x, y, w, h) coordinates.
    pub fn fill_rect_xy(&mut self, x: usize, y: usize, w: usize, h: usize, color: u32) {
        self.fill_rect(Rect::new(x, y, w, h), color);
    }
}
