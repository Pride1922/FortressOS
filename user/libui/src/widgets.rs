//! Immediate-mode widgets for `Painter`: buttons, labels, text inputs, cards,
//! badges and text rendering.
//!
//! Every widget computes logical pixel colours via `SurfaceStyle` and font
//! utilities, then delegates drawing to `Painter::shade` or pixel loops.
use crate::font;
use crate::gfx::{self, Rect};
use crate::glyphs::GLYPHS;
use crate::painter::Painter;
use crate::style::SurfaceStyle;
use crate::theme;

impl<'a> Painter<'a> {
    /// Renders a single 8x16 bitmap glyph (monospace text).
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
            for c in 0..8 {
                let p = if bits & (0x80 >> c) != 0 { fg } else { bg };
                self.put(x + c, y + r, p);
            }
        }
    }

    /// Anti-aliased proportional UI text in theme font.
    pub fn text(&mut self, x: usize, y: usize, text: &str, fg: u32, bg: u32) {
        let w = font::text_width(text).min(self.width.saturating_sub(x));
        let h = font::LINE_HEIGHT.min(self.height.saturating_sub(y));
        for dy in 0..h {
            for dx in 0..w {
                let p = font::text_pixel(text, dx, dy, fg, bg);
                self.put(x + dx, y + dy, p);
            }
        }
    }

    /// Anti-aliased synthetic bold UI text in theme font.
    pub fn text_bold(&mut self, x: usize, y: usize, text: &str, fg: u32, bg: u32) {
        let w = font::text_width(text).min(self.width.saturating_sub(x));
        let h = font::LINE_HEIGHT.min(self.height.saturating_sub(y));
        for dy in 0..h {
            for dx in 0..w {
                let p = font::text_pixel_bold(text, dx, dy, fg, bg);
                self.put(x + dx, y + dy, p);
            }
        }
    }

    /// Single-line standard label on panel background.
    pub fn label(&mut self, x: usize, y: usize, width: usize, text: &str, color: u32) {
        self.box_text(
            Rect {
                x,
                y,
                w: width,
                h: 22,
            },
            text,
            color,
            theme::PANEL,
            false,
            false,
        );
    }

    /// Single-line standard label on custom background.
    pub fn label_bg(&mut self, x: usize, y: usize, width: usize, text: &str, color: u32, bg: u32) {
        self.box_text(
            Rect {
                x,
                y,
                w: width,
                h: 22,
            },
            text,
            color,
            bg,
            false,
            false,
        );
    }

    /// Label with custom height.
    pub fn label_h(
        &mut self,
        x: usize,
        y: usize,
        width: usize,
        height: usize,
        text: &str,
        color: u32,
        bg: u32,
    ) {
        self.box_text(
            Rect {
                x,
                y,
                w: width,
                h: height,
            },
            text,
            color,
            bg,
            false,
            false,
        );
    }

    /// Bold label with custom height.
    pub fn label_bold(
        &mut self,
        x: usize,
        y: usize,
        width: usize,
        height: usize,
        text: &str,
        color: u32,
        bg: u32,
    ) {
        let r = Rect {
            x,
            y,
            w: width,
            h: height,
        };
        let text_y = r.h.saturating_sub(18) / 2;
        self.shade(r, |lx, ly| {
            if ly >= text_y && ly < text_y + 18 {
                font::text_pixel_bold(text, lx, ly - text_y, color, bg)
            } else {
                bg
            }
        });
    }

    /// Internal left-aligned boxed text with optional border.
    pub fn box_text(
        &mut self,
        r: Rect,
        text: &str,
        color: u32,
        background: u32,
        border: bool,
        focused: bool,
    ) {
        let text_y = r.h.saturating_sub(18) / 2;
        let inset = if border { 8 } else { 0 };
        let right_pad = if border { 4 } else { 0 };
        let border_col = if focused {
            theme::ACCENT
        } else {
            0xff3a4659
        };
        let radius = 6.min(r.h / 2);
        let style = SurfaceStyle {
            radius,
            outer_bg: 0xff282828,
            fill: background,
            border: border_col,
            top_highlight: 0,
        };
        self.shade(r, |lx, ly| {
            let base = if border {
                style.shade(lx, ly, r.w, r.h)
            } else {
                background
            };
            if lx >= inset && lx + right_pad < r.w && ly >= text_y && ly < text_y + 18 {
                font::text_pixel(text, lx - inset, ly - text_y, color, base)
            } else {
                base
            }
        });
    }

    /// Centred boxed text with rounded border and button highlights.
    pub fn box_text_centered(
        &mut self,
        r: Rect,
        text: &str,
        color: u32,
        background: u32,
        border: bool,
        focused: bool,
    ) {
        let text_y = r.h.saturating_sub(18) / 2;
        let tw = font::text_width(text);
        let inset = if tw == 0 {
            8
        } else {
            r.w.saturating_sub(tw) / 2
        }
        .max(4);
        let border_col = if focused {
            theme::ACCENT
        } else if background == theme::ACCENT {
            0xff38bdf8
        } else if background == 0xff244b6e {
            0xff3b82f6
        } else {
            theme::BORDER
        };
        let top_hi = if background == theme::ACCENT {
            0xff5cc8ff
        } else if background == 0xff244b6e {
            0xff4f94f8
        } else if background == theme::SURFACE {
            0xff4a4a4a
        } else {
            0
        };
        let radius = 6.min(r.h / 2);
        let style = SurfaceStyle {
            radius,
            outer_bg: theme::PANEL,
            fill: background,
            border: border_col,
            top_highlight: top_hi,
        };
        self.shade(r, |lx, ly| {
            let base = if border {
                style.shade(lx, ly, r.w, r.h)
            } else {
                background
            };
            if lx >= inset && lx + 3 < r.w && ly >= text_y && ly < text_y + 18 {
                font::text_pixel(text, lx - inset, ly - text_y, color, base)
            } else {
                base
            }
        });
    }

    /// Standard push button.
    pub fn button(&mut self, r: Rect, text: &str, selected: bool, focused: bool) {
        self.button_enabled(r, text, selected, focused, true);
    }

    /// Push button with enabled / disabled state.
    pub fn button_enabled(
        &mut self,
        r: Rect,
        text: &str,
        selected: bool,
        focused: bool,
        enabled: bool,
    ) {
        self.box_text_centered(
            r,
            text,
            if enabled {
                if selected { 0xffffffff } else { theme::TEXT }
            } else {
                theme::MUTED
            },
            if enabled && selected {
                0xff244b6e
            } else if enabled {
                theme::SURFACE
            } else {
                theme::PANEL
            },
            true,
            focused && enabled,
        );
    }

    /// Accent action button (e.g. Save, OK, Apply).
    pub fn button_accent(&mut self, r: Rect, text: &str, focused: bool) {
        self.box_text_centered(r, text, 0xffffffff, theme::ACCENT, true, focused);
    }

    /// Pill button (rounded corners, centred label).
    pub fn pill_button(
        &mut self,
        x: usize,
        y: usize,
        w: usize,
        h: usize,
        label: &str,
        fg: u32,
        fill: u32,
        border: u32,
        outer_bg: u32,
    ) {
        let r = Rect { x, y, w, h };
        let rad = 5.min(w / 2).min(h / 2);
        let tw = font::text_width(label);
        let tx = w.saturating_sub(tw) / 2;
        let ty = h.saturating_sub(18) / 2;
        let style = SurfaceStyle::new(rad, outer_bg, fill, border);
        self.shade(r, |lx, ly| {
            let base = style.shade(lx, ly, w, h);
            if lx >= tx && lx < tx + tw && ly >= ty && ly < ty + 18 {
                font::text_pixel(label, lx - tx, ly - ty, fg, base)
            } else {
                base
            }
        });
    }

    /// Rounded box outline + fill.
    pub fn rounded_box(
        &mut self,
        x: usize,
        y: usize,
        w: usize,
        h: usize,
        radius: usize,
        outer_bg: u32,
        fill: u32,
        border: u32,
    ) {
        let r = Rect { x, y, w, h };
        let rad = radius.min(w / 2).min(h / 2);
        let style = SurfaceStyle::new(rad, outer_bg, fill, border);
        self.shade(r, |lx, ly| style.shade(lx, ly, w, h));
    }

    /// 1 px horizontal divider line.
    pub fn divider_h(&mut self, x: usize, y: usize, w: usize, color: u32) {
        self.fill_rect(Rect::new(x, y, w, 1), color);
    }

    /// Flat card without shadow calculation (fast path, zero shadow damage).
    pub fn card_flat(&mut self, r: Rect, bg: u32, border: u32, outer_bg: u32) {
        let radius = theme::RADIUS_CARD.min(r.h / 2);
        let top_hi = if border == theme::BORDER { theme::TOP_HIGHLIGHT_DEFAULT } else { border };
        let style = SurfaceStyle::new(radius, outer_bg, bg, border).highlight(top_hi);
        self.shade(r, |lx, ly| style.shade(lx, ly, r.w, r.h));
    }

    /// Card with optional elevation shadow (elevation 0 = flat, elevation > 0 = soft drop shadow).
    pub fn card_elevated(
        &mut self,
        r: Rect,
        bg: u32,
        border: u32,
        elevation: usize,
        outer_bg: u32,
    ) {
        if elevation == 0 {
            self.card_flat(r, bg, border, outer_bg);
            return;
        }
        let total_h = r.h.saturating_add(elevation);
        let radius = theme::RADIUS_CARD.min(r.h / 2);
        let top_hi = if border == theme::BORDER { theme::TOP_HIGHLIGHT_DEFAULT } else { border };
        let card_style = SurfaceStyle {
            radius,
            outer_bg,
            fill: bg,
            border,
            top_highlight: top_hi,
        };
        self.shade(
            Rect {
                x: r.x,
                y: r.y,
                w: r.w,
                h: total_h,
            },
            |lx, ly| {
                if ly < r.h {
                    let shadow_bg = if ly >= 2 {
                        let s_cov = gfx::rounded_coverage(lx, ly - 2, r.w, r.h, radius);
                        gfx::blend_coverage(theme::SHADOW_CORE, outer_bg, s_cov * 6 / 16)
                    } else {
                        outer_bg
                    };
                    SurfaceStyle {
                        outer_bg: shadow_bg,
                        ..card_style
                    }
                    .shade(lx, ly, r.w, r.h)
                } else {
                    let dy = ly - r.h;
                    let s_cov = gfx::rounded_coverage(
                        lx,
                        r.h.saturating_sub(radius) + dy,
                        r.w,
                        radius + elevation,
                        radius,
                    );
                    let alpha = match dy {
                        0 => s_cov * 6 / 16,
                        1 => s_cov * 4 / 16,
                        _ => s_cov * 2 / 16,
                    };
                    gfx::blend_coverage(theme::SHADOW_AMBIENT, outer_bg, alpha)
                }
            },
        );
    }

    /// Raised card with 3 px shadow.
    pub fn card(&mut self, r: Rect, bg: u32, border: u32) {
        self.card_elevated(r, bg, border, 3, theme::PANEL);
    }

    /// Push button with custom styling.
    pub fn button_styled(
        &mut self,
        r: Rect,
        text: &str,
        fill: u32,
        border: u32,
        fg: u32,
        focused: bool,
    ) {
        let border_col = if focused { theme::BORDER_FOCUS } else { border };
        let top_hi = if fill == theme::ACCENT {
            theme::TOP_HIGHLIGHT_ACCENT
        } else if fill == theme::SELECTED {
            theme::TOP_HIGHLIGHT_SELECTED
        } else if fill == theme::SURFACE {
            theme::TOP_HIGHLIGHT_SURFACE
        } else {
            0
        };
        let radius = theme::RADIUS_BUTTON.min(r.h / 2);
        let style = SurfaceStyle {
            radius,
            outer_bg: theme::PANEL,
            fill,
            border: border_col,
            top_highlight: top_hi,
        };
        let text_y = r.h.saturating_sub(18) / 2;
        let tw = font::text_width(text);
        let inset = if tw == 0 { 8 } else { r.w.saturating_sub(tw) / 2 }.max(4);
        self.shade(r, |lx, ly| {
            let base = style.shade(lx, ly, r.w, r.h);
            if lx >= inset && lx + 3 < r.w && ly >= text_y && ly < text_y + 18 {
                font::text_pixel(text, lx - inset, ly - text_y, fg, base)
            } else {
                base
            }
        });
    }

    /// Sheet card with halo / glow when active.
    pub fn sheet_card(&mut self, r: Rect, bg: u32, border: u32, active: bool) {
        let pad = 4usize;
        let rx = r.x.saturating_sub(pad);
        let ry = r.y.saturating_sub(2);
        let rw = r.w + pad * 2;
        let rh = r.h + pad + 3;
        let radius = 8.min(r.h / 2);
        let top_hi = if active { 0xff38bdf8 } else { 0xff4c4c52 };
        self.shade(
            Rect {
                x: rx,
                y: ry,
                w: rw,
                h: rh,
            },
            |lx, ly| {
                let shadow_cov = gfx::rounded_coverage(lx, ly, rw, rh, radius + 3);
                let halo_col = if active { 0xff0055cc } else { 0xff080a0f };
                let halo_alpha = shadow_cov * 6 / 16;
                let outer_bg = gfx::blend_coverage(halo_col, theme::PANEL, halo_alpha);
                if lx >= pad && lx < pad + r.w && ly >= 2 && ly < 2 + r.h {
                    let cx = lx - pad;
                    let cy = ly - 2;
                    let row_bg = if cy < 24 && active {
                        0xff1e293b
                    } else if cy == 24 && active {
                        0xff2b3b52
                    } else {
                        bg
                    };
                    SurfaceStyle {
                        radius,
                        outer_bg,
                        fill: row_bg,
                        border,
                        top_highlight: top_hi,
                    }
                    .shade(cx, cy, r.w, r.h)
                } else {
                    outer_bg
                }
            },
        );
    }

    /// Recessed text input field on default 0x282828 background.
    pub fn text_input(&mut self, r: Rect, text: &str, focused: bool) {
        self.text_input_on(r, text, focused, theme::INPUT_OUTER);
    }

    /// Recessed text input field with optional glow on focus.
    pub fn text_input_on(&mut self, r: Rect, text: &str, focused: bool, outer_bg: u32) {
        let glow = 2usize;
        let gx = r.x.saturating_sub(glow);
        let gy = r.y.saturating_sub(glow);
        let gw = r.w + glow * 2;
        let gh = r.h + glow * 2;
        let well_bg = 0xff12151c;
        let radius = 6.min(r.h / 2);
        let text_y = r.h.saturating_sub(18) / 2;
        let inset = 10usize;
        self.shade(
            Rect {
                x: gx,
                y: gy,
                w: gw,
                h: gh,
            },
            |lx, ly| {
                let glow_bg = if focused {
                    let g_cov = gfx::rounded_coverage(lx, ly, gw, gh, radius + glow);
                    gfx::blend_coverage(theme::ACCENT, outer_bg, g_cov * 5 / 16)
                } else {
                    outer_bg
                };
                if lx >= glow && lx < glow + r.w && ly >= glow && ly < glow + r.h {
                    let bx = lx - glow;
                    let by = ly - glow;
                    let outer_cov = gfx::rounded_coverage(bx, by, r.w, r.h, radius);
                    if outer_cov == 0 {
                        glow_bg
                    } else {
                        let inner_cov = if bx >= 1 && bx + 1 < r.w && by >= 1 && by + 1 < r.h {
                            gfx::rounded_coverage(
                                bx - 1,
                                by - 1,
                                r.w.saturating_sub(2),
                                r.h.saturating_sub(2),
                                radius.saturating_sub(1),
                            )
                        } else {
                            0
                        };
                        let rim_col = if focused {
                            theme::ACCENT
                        } else if by == 0 {
                            0xff090b0f
                        } else if by + 1 == r.h {
                            0xff364050
                        } else {
                            0xff1f2530
                        };
                        let interior_col = if !focused && by == 1 {
                            0xff0c0f15
                        } else {
                            well_bg
                        };
                        let inner_blend = gfx::blend_coverage(interior_col, rim_col, inner_cov);
                        let base = gfx::blend_coverage(inner_blend, glow_bg, outer_cov);
                        if bx >= inset && bx + 6 < r.w && by >= text_y && by < text_y + 18 {
                            font::text_pixel(text, bx - inset, by - text_y, 0xffffffff, base)
                        } else {
                            base
                        }
                    }
                } else {
                    glow_bg
                }
            },
        );
    }

    /// Text badge on theme::PANEL background.
    pub fn badge(&mut self, r: Rect, text: &str, fg: u32, bg: u32, border_color: u32) {
        self.badge_on(r, text, fg, bg, border_color, theme::PANEL);
    }

    /// Text badge with custom outer background.
    pub fn badge_on(
        &mut self,
        r: Rect,
        text: &str,
        fg: u32,
        bg: u32,
        border_color: u32,
        outer_bg: u32,
    ) {
        let text_y = r.h.saturating_sub(18) / 2;
        let tw = font::text_width(text);
        let inset = r.w.saturating_sub(tw) / 2;
        let radius = 6.min(r.h / 2);
        let style = SurfaceStyle {
            radius,
            outer_bg,
            fill: bg,
            border: border_color,
            top_highlight: 0,
        };
        self.shade(r, |lx, ly| {
            let base = style.shade(lx, ly, r.w, r.h);
            if lx >= inset
                && lx < inset + tw
                && lx + 1 < r.w
                && ly >= text_y
                && ly < text_y + 18
            {
                font::text_pixel(text, lx - inset, ly - text_y, fg, base)
            } else {
                base
            }
        });
    }

    /// Client file icon (folder = 0, executable = 1, file = 2).
    pub fn file_icon(&mut self, x: usize, y: usize, kind: u8, row_bg: u32) {
        if x + 16 > self.width || y + 14 > self.height {
            return;
        }
        match kind {
            0 => {
                self.fill_rect(Rect::new(x + 1, y + 1, 6, 3), 0xff38bdf8);
                self.rounded_box(x, y + 3, 16, 11, 3, row_bg, 0xff007aff, 0xff38bdf8);
                self.fill_rect(Rect::new(x + 2, y + 5, 12, 1), 0xff7dd3fc);
            }
            1 => {
                self.rounded_box(x, y + 1, 16, 13, 3, row_bg, 0xff133223, 0xff22c55e);
                if x + 12 <= self.width && y + 11 <= self.height {
                    self.fill_rect(Rect::new(x + 4, y + 5, 2, 1), 0xff4ade80);
                    self.fill_rect(Rect::new(x + 5, y + 6, 2, 1), 0xff4ade80);
                    self.fill_rect(Rect::new(x + 6, y + 7, 2, 1), 0xff4ade80);
                    self.fill_rect(Rect::new(x + 5, y + 8, 2, 1), 0xff4ade80);
                    self.fill_rect(Rect::new(x + 4, y + 9, 2, 1), 0xff4ade80);
                    self.fill_rect(Rect::new(x + 9, y + 9, 4, 1), 0xff4ade80);
                }
            }
            _ => {
                self.rounded_box(x + 2, y, 12, 14, 2, row_bg, 0xff242c3a, 0xff64748b);
                self.fill_rect(Rect::new(x + 5, y + 4, 6, 1), 0xff94a3b8);
                self.fill_rect(Rect::new(x + 5, y + 7, 6, 1), 0xff94a3b8);
                self.fill_rect(Rect::new(x + 5, y + 10, 4, 1), 0xff94a3b8);
            }
        }
    }
}
