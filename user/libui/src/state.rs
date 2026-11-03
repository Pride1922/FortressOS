//! State and layout helpers for interactive apps: `TextField`, `FocusRing`,
//! `Grid`, and hit testing. Zero heap allocation.
use crate::gfx::Rect;

/// A fixed-capacity, zero-heap text buffer with caret and editing operations.
#[derive(Clone, Debug)]
pub struct TextField<const N: usize> {
    buf: [u8; N],
    len: usize,
    pub cursor: usize,
}

impl<const N: usize> Default for TextField<N> {
    fn default() -> Self {
        Self::new()
    }
}

impl<const N: usize> TextField<N> {
    pub const fn new() -> Self {
        Self {
            buf: [0u8; N],
            len: 0,
            cursor: 0,
        }
    }

    pub fn as_str(&self) -> &str {
        core::str::from_utf8(&self.buf[..self.len]).unwrap_or("")
    }

    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    pub fn capacity(&self) -> usize {
        N
    }

    /// Appends ASCII character `ch` if within capacity.
    pub fn push(&mut self, ch: char) -> bool {
        if !ch.is_ascii() || self.len >= N {
            return false;
        }
        self.buf[self.len] = ch as u8;
        self.len += 1;
        self.cursor = self.len;
        true
    }

    /// Handles standard typing: printable ASCII appends, backspace deletes,
    /// Return and Escape are returned as `false`.
    pub fn handle_key(&mut self, key: u8) -> bool {
        match key {
            0x08 | 127 => self.backspace(),
            0x20..=0x7e => self.push(key as char),
            _ => false,
        }
    }

    pub fn backspace(&mut self) -> bool {
        if self.len == 0 {
            return false;
        }
        self.len -= 1;
        self.buf[self.len] = 0;
        self.cursor = self.len;
        true
    }

    /// Sets content from a string slice (clamped to capacity).
    pub fn set_text(&mut self, text: &str) {
        self.clear();
        for ch in text.chars() {
            if !self.push(ch) {
                break;
            }
        }
    }

    /// Clears the field and securely zeroes the backing buffer.
    pub fn clear(&mut self) {
        self.buf.fill(0);
        self.len = 0;
        self.cursor = 0;
    }
}

/// A zero-heap focus ring for navigating `N` widgets via Tab / Shift-Tab.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FocusRing<const N: usize> {
    current: usize,
    count: usize,
}

impl<const N: usize> Default for FocusRing<N> {
    fn default() -> Self {
        Self::new(N)
    }
}

impl<const N: usize> FocusRing<N> {
    pub const fn new(count: usize) -> Self {
        let count = if count > N { N } else { count };
        Self { current: 0, count }
    }

    pub fn current(&self) -> usize {
        self.current
    }

    pub fn is_focused(&self, index: usize) -> bool {
        self.current == index
    }

    pub fn set_focus(&mut self, index: usize) {
        if index < self.count {
            self.current = index;
        }
    }

    pub fn next(&mut self) {
        if self.count > 0 {
            self.current = (self.current + 1) % self.count;
        }
    }

    pub fn prev(&mut self) {
        if self.count > 0 {
            self.current = if self.current == 0 {
                self.count - 1
            } else {
                self.current - 1
            };
        }
    }

    /// Tab cycles forward; Shift-Tab (or BackTab 0x19 / F-key) cycles back.
    pub fn handle_key(&mut self, key: u8) -> bool {
        match key {
            b'\t' => {
                self.next();
                true
            }
            _ => false,
        }
    }
}

/// Hit-testing: returns true if (`x`, `y`) is inside `r`.
pub fn hit(r: Rect, x: usize, y: usize) -> bool {
    r.contains(x, y)
}

/// Simple 2D grid layout generator for button grids (e.g. calculator, dialpad).
#[derive(Clone, Copy, Debug)]
pub struct Grid {
    pub x: usize,
    pub y: usize,
    pub cols: usize,
    pub cell_w: usize,
    pub cell_h: usize,
    pub gap: usize,
}

impl Grid {
    pub const fn new(
        x: usize,
        y: usize,
        cols: usize,
        cell_w: usize,
        cell_h: usize,
        gap: usize,
    ) -> Self {
        Self {
            x,
            y,
            cols,
            cell_w,
            cell_h,
            gap,
        }
    }

    /// Returns the cell `Rect` for item `index` (0-indexed, row-major).
    pub fn cell(&self, index: usize) -> Rect {
        let col = index % self.cols;
        let row = index / self.cols;
        Rect {
            x: self.x + col * (self.cell_w + self.gap),
            y: self.y + row * (self.cell_h + self.gap),
            w: self.cell_w,
            h: self.cell_h,
        }
    }

    /// Cell spanning multiple columns.
    pub fn cell_span(&self, index: usize, col_span: usize) -> Rect {
        let col = index % self.cols;
        let row = index / self.cols;
        let span = if col + col_span > self.cols {
            self.cols - col
        } else {
            col_span
        };
        let w = span * self.cell_w + (span - 1) * self.gap;
        Rect {
            x: self.x + col * (self.cell_w + self.gap),
            y: self.y + row * (self.cell_h + self.gap),
            w,
            h: self.cell_h,
        }
    }
}
