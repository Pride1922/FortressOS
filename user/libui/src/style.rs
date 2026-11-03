//! `SurfaceStyle`: the one rounded-surface primitive behind cards, buttons,
//! badges, rows and pills. A style is a pure function from a logical pixel
//! inside a `w` x `h` box to its colour, so widgets compose it with content
//! (text, icons) and `Painter` handles scaling, clipping and damage.
use crate::gfx::{blend_coverage, rounded_coverage};
use crate::theme;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SurfaceStyle {
    /// Corner radius in logical pixels (clamped to half the box).
    pub radius: usize,
    /// Colour outside the rounded shape (what the corners blend into).
    pub outer_bg: u32,
    /// Interior colour.
    pub fill: u32,
    /// 1 px rim colour.
    pub border: u32,
    /// Optional lit top edge (row 0-1 rim and row 1 interior); 0 = none.
    pub top_highlight: u32,
}

impl SurfaceStyle {
    pub const fn new(radius: usize, outer_bg: u32, fill: u32, border: u32) -> Self {
        Self {
            radius,
            outer_bg,
            fill,
            border,
            top_highlight: 0,
        }
    }

    pub const fn highlight(mut self, top_highlight: u32) -> Self {
        self.top_highlight = top_highlight;
        self
    }

    pub const fn on(mut self, outer_bg: u32) -> Self {
        self.outer_bg = outer_bg;
        self
    }

    /// Raised content card (radius 8); the standard border gets a lit top edge.
    pub const fn card(fill: u32, border: u32) -> Self {
        let top = if border == theme::BORDER { 0xff4c4c52 } else { border };
        Self::new(8, theme::PANEL, fill, border).highlight(top)
    }

    /// Push button look for `fill`, matching the desktop's built-in apps.
    pub const fn button(fill: u32, focused: bool) -> Self {
        let border = if focused {
            theme::ACCENT
        } else if fill == theme::ACCENT {
            0xff38bdf8
        } else if fill == theme::SELECTED {
            0xff3b82f6
        } else {
            theme::BORDER
        };
        let top = if fill == theme::ACCENT {
            0xff5cc8ff
        } else if fill == theme::SELECTED {
            0xff4f94f8
        } else if fill == theme::SURFACE {
            0xff4a4a4a
        } else {
            0
        };
        Self::new(6, theme::PANEL, fill, border).highlight(top)
    }

    /// Small status badge / pill.
    pub const fn badge(fill: u32, border: u32, outer_bg: u32) -> Self {
        Self::new(6, outer_bg, fill, border)
    }

    /// Colour of logical pixel (`lx`, `ly`) of a `w` x `h` surface.
    #[inline]
    pub fn shade(&self, lx: usize, ly: usize, w: usize, h: usize) -> u32 {
        let radius = self.radius;
        let outer_cov = rounded_coverage(lx, ly, w, h, radius);
        if outer_cov == 0 {
            return self.outer_bg;
        }
        let inner_cov = if lx >= 1 && lx + 1 < w && ly >= 1 && ly + 1 < h {
            rounded_coverage(
                lx - 1,
                ly - 1,
                w.saturating_sub(2),
                h.saturating_sub(2),
                radius.saturating_sub(1),
            )
        } else {
            0
        };
        if outer_cov == 16 && inner_cov == 16 {
            return if ly == 1 && self.top_highlight != 0 {
                self.top_highlight
            } else {
                self.fill
            };
        }
        let edge_color = if ly <= 1 && self.top_highlight != 0 {
            self.top_highlight
        } else {
            self.border
        };
        let inner = blend_coverage(self.fill, edge_color, inner_cov);
        blend_coverage(inner, self.outer_bg, outer_cov)
    }
}
