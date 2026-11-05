//! Shared immediate widgets. Every primitive compares final pixels and reports
//! bounded client damage; idle labels never invalidate an entire application.
use crate::{
    surface::Rect,
    window::{Window, theme},
};

pub const fn content_offset(logical_w: usize) -> usize {
    if logical_w > 800 {
        logical_w.saturating_sub(180).saturating_sub(572) / 2
    } else {
        0
    }
}

pub struct Canvas<'a> {
    window: &'a mut Window,
    scale: usize,
    dirty: Option<Rect>,
    scroll_y: usize,
    x_offset: usize,
}
impl<'a> Canvas<'a> {
    #[allow(dead_code)] // Convenience constructor for new apps and host tests.
    pub fn new(window: &'a mut Window, scale: usize, reset: bool) -> Self {
        Self::scrolled(window, scale, reset, 0)
    }
    pub fn scrolled(window: &'a mut Window, scale: usize, reset: bool, scroll_y: usize) -> Self {
        let scale = scale.clamp(1, 2);
        let logical_w = (window.rect.w - 2) / scale;
        let x_offset = if registered(window.id) {
            content_offset(logical_w)
        } else {
            0
        };
        let mut result = Self {
            window,
            scale,
            dirty: None,
            scroll_y,
            x_offset,
        };
        if reset {
            result.window.content.fill(theme::PANEL);
            if registered(result.window.id) {
                let width = result.window.rect.w - 2;
                let sidebar_w = (theme::WIDTH_SIDEBAR * scale).min(width);
                let height = result.window.rect.h - 26;
                for y in 0..height {
                    for x in 0..sidebar_w {
                        result.window.content[y * width + x] = theme::BG_SIDEBAR;
                    }
                }
            }
            result.dirty = Some(Rect {
                x: 0,
                y: 0,
                w: result.window.rect.w - 2,
                h: result.window.rect.h - 26,
            });
        }
        result
    }
    #[allow(dead_code)]
    pub fn width(&self) -> usize {
        (self.window.rect.w - 2) / self.scale
    }
    pub fn height(&self) -> usize {
        (self.window.rect.h - 26) / self.scale
    }
    fn pixel(&mut self, x: usize, y: usize, color: u32) {
        let width = self.window.rect.w - 2;
        if x >= width || y >= self.window.rect.h - 26 {
            return;
        }
        let old = &mut self.window.content[y * width + x];
        if *old == color {
            return;
        }
        *old = color;
        let r = Rect { x, y, w: 1, h: 1 };
        self.dirty = Some(self.dirty.map(|d| d.union(r)).unwrap_or(r));
    }
    #[allow(dead_code)]
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
    #[allow(dead_code)]
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
    pub fn label_h(&mut self, x: usize, y: usize, width: usize, height: usize, text: &str, color: u32, bg: u32) {
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
    #[inline]
    fn adj(&self, r: Rect) -> Rect {
        if r.x > 180 {
            Rect {
                x: r.x.saturating_add(self.x_offset),
                ..r
            }
        } else {
            r
        }
    }
    pub fn label_bold(&mut self, x: usize, y: usize, width: usize, height: usize, text: &str, color: u32, bg: u32) {
        let r = self.adj(Rect {
            x,
            y,
            w: width,
            h: height,
        });
        let Some(left) = r.x.checked_mul(self.scale) else { return; };
        let Some(top) = r.y.checked_mul(self.scale) else { return; };
        let Some(w) = r.w.checked_mul(self.scale) else { return; };
        let Some(h) = r.h.checked_mul(self.scale) else { return; };
        let draw_width = w.min((self.window.rect.w - 2).saturating_sub(left));
        let scroll = self.scroll_y.saturating_mul(self.scale);
        let first_y = scroll.saturating_sub(top).min(h);
        let last_y = scroll
            .saturating_add(self.window.rect.h - 26)
            .saturating_sub(top)
            .min(h);
        let text_y = r.h.saturating_sub(18) / 2;
        for py in first_y..last_y {
            for px in 0..draw_width {
                let lx = px / self.scale;
                let ly = py / self.scale;
                let p = if ly >= text_y && ly < text_y + 18 {
                    theme::text_pixel_bold(text, lx, ly - text_y, color, bg)
                } else {
                    bg
                };
                if top + py >= scroll {
                    self.pixel(left + px, top + py - scroll, p);
                }
            }
        }
    }
    #[allow(dead_code)]
    pub fn title(&mut self, x: usize, y: usize, text: &str, color: u32, bg: u32) {
        let tw = crate::font::text_width_sized(crate::font::FontSize::Title, text);
        let r = self.adj(Rect { x, y, w: tw, h: crate::font::TITLE_LINE_HEIGHT });
        let Some(left) = r.x.checked_mul(self.scale) else { return; };
        let Some(top) = r.y.checked_mul(self.scale) else { return; };
        let Some(w) = r.w.checked_mul(self.scale) else { return; };
        let Some(h) = r.h.checked_mul(self.scale) else { return; };
        let draw_width = w.min((self.window.rect.w - 2).saturating_sub(left));
        let scroll = self.scroll_y.saturating_mul(self.scale);
        let first_y = scroll.saturating_sub(top).min(h);
        let last_y = scroll
            .saturating_add(self.window.rect.h - 26)
            .saturating_sub(top)
            .min(h);
        for py in first_y..last_y {
            for px in 0..draw_width {
                let lx = px / self.scale;
                let ly = py / self.scale;
                let p = crate::font::text_pixel_sized(crate::font::FontSize::Title, text, lx, ly, color, bg);
                if top + py >= scroll {
                    self.pixel(left + px, top + py - scroll, p);
                }
            }
        }
    }
    #[allow(dead_code)]
    pub fn caption(&mut self, x: usize, y: usize, text: &str, color: u32, bg: u32) {
        let tw = crate::font::text_width_sized(crate::font::FontSize::Caption, text);
        let r = self.adj(Rect { x, y, w: tw, h: crate::font::CAPTION_LINE_HEIGHT });
        let Some(left) = r.x.checked_mul(self.scale) else { return; };
        let Some(top) = r.y.checked_mul(self.scale) else { return; };
        let Some(w) = r.w.checked_mul(self.scale) else { return; };
        let Some(h) = r.h.checked_mul(self.scale) else { return; };
        let draw_width = w.min((self.window.rect.w - 2).saturating_sub(left));
        let scroll = self.scroll_y.saturating_mul(self.scale);
        let first_y = scroll.saturating_sub(top).min(h);
        let last_y = scroll
            .saturating_add(self.window.rect.h - 26)
            .saturating_sub(top)
            .min(h);
        for py in first_y..last_y {
            for px in 0..draw_width {
                let lx = px / self.scale;
                let ly = py / self.scale;
                let p = crate::font::text_pixel_sized(crate::font::FontSize::Caption, text, lx, ly, color, bg);
                if top + py >= scroll {
                    self.pixel(left + px, top + py - scroll, p);
                }
            }
        }
    }
    #[inline]
    fn rounded_frame_pixel(
        lx: usize,
        ly: usize,
        w: usize,
        h: usize,
        radius: usize,
        outer_bg: u32,
        fill: u32,
        border: u32,
        top_highlight: u32,
    ) -> u32 {
        let outer_cov = theme::rounded_coverage(lx, ly, w, h, radius);
        if outer_cov == 0 {
            return outer_bg;
        }
        let inner_cov = if lx >= 1 && lx + 1 < w && ly >= 1 && ly + 1 < h {
            theme::rounded_coverage(
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
            return if ly == 1 && top_highlight != 0 {
                top_highlight
            } else {
                fill
            };
        }
        let edge_color = if ly <= 1 && top_highlight != 0 {
            top_highlight
        } else {
            border
        };
        let inner = theme::blend_coverage(fill, edge_color, inner_cov);
        theme::blend_coverage(inner, outer_bg, outer_cov)
    }
    #[allow(dead_code)]
    pub fn badge(&mut self, r: Rect, text: &str, fg: u32, bg: u32, border_color: u32) {
        self.badge_on(r, text, fg, bg, border_color, theme::PANEL);
    }
    pub fn icon_badge(&mut self, r: Rect, kind: u8, fg: u32, bg: u32, border_color: u32, outer_bg: u32) {
        let r = self.adj(r);
        let Some(left) = r.x.checked_mul(self.scale) else { return; };
        let Some(top) = r.y.checked_mul(self.scale) else { return; };
        let Some(width) = r.w.checked_mul(self.scale) else { return; };
        let Some(height) = r.h.checked_mul(self.scale) else { return; };
        let draw_width = width.min((self.window.rect.w - 2).saturating_sub(left));
        let scroll = self.scroll_y.saturating_mul(self.scale);
        let first_y = scroll.saturating_sub(top).min(height);
        let last_y = scroll
            .saturating_add(self.window.rect.h - 26)
            .saturating_sub(top)
            .min(height);
        let radius = 7.min(r.h / 2);
        let cx = (r.w / 2) as isize;
        let cy = (r.h / 2) as isize;
        for y in first_y..last_y {
            for x in 0..draw_width {
                let lx = x / self.scale;
                let ly = y / self.scale;
                let base = Self::rounded_frame_pixel(
                    lx,
                    ly,
                    r.w,
                    r.h,
                    radius,
                    outer_bg,
                    bg,
                    border_color,
                    0,
                );
                let ix = lx as isize - cx;
                let iy = ly as isize - cy;
                let on_icon = match kind {
                    0 => {
                        // Ethernet LAN topology glyph
                        let top_box = ix >= -3 && ix <= 2 && iy >= -8 && iy <= -4
                            && !(ix >= -1 && ix <= 0 && iy >= -6 && iy <= -5);
                        let bus = ix >= -7 && ix <= 6 && iy == 0;
                        let stem_mid = (ix == -1 || ix == 0) && iy >= -3 && iy <= 0;
                        let stem_l = (ix == -5 || ix == -4) && iy >= 1 && iy <= 3;
                        let stem_r = (ix == 3 || ix == 4) && iy >= 1 && iy <= 3;
                        let bot_l = ix >= -7 && ix <= -2 && iy >= 4 && iy <= 7
                            && !(ix >= -5 && ix <= -4 && iy >= 5 && iy <= 6);
                        let bot_r = ix >= 1 && ix <= 6 && iy >= 4 && iy <= 7
                            && !(ix >= 3 && ix <= 4 && iy >= 5 && iy <= 6);
                        top_box || bus || stem_mid || stem_l || stem_r || bot_l || bot_r
                    }
                    1 => {
                        // Wi-Fi concentric signal waves glyph
                        let wy = ly as isize - (cy + 5);
                        let d2 = ix * ix + wy * wy;
                        let in_cone = wy <= 0 && ix.abs() <= (-wy + 1);
                        let dot = d2 <= 2;
                        let inner_arc = in_cone && (20..=35).contains(&d2);
                        let outer_arc = in_cone && (68..=94).contains(&d2);
                        dot || inner_arc || outer_arc
                    }
                    _ => {
                        // Clock face glyph
                        let d2 = ix * ix + iy * iy;
                        let ring = (40..=64).contains(&d2);
                        let hands = (ix == 0 && (-5..=0).contains(&iy))
                            || (iy == 0 && (0..=4).contains(&ix));
                        ring || hands
                    }
                };
                let p = if on_icon { fg } else { base };
                if top + y >= scroll {
                    self.pixel(left + x, top + y - scroll, p);
                }
            }
        }
    }
    pub fn badge_on(
        &mut self,
        r: Rect,
        text: &str,
        fg: u32,
        bg: u32,
        border_color: u32,
        outer_bg: u32,
    ) {
        let r = self.adj(r);
        let Some(left) = r.x.checked_mul(self.scale) else { return; };
        let Some(top) = r.y.checked_mul(self.scale) else { return; };
        let Some(width) = r.w.checked_mul(self.scale) else { return; };
        let Some(height) = r.h.checked_mul(self.scale) else { return; };
        let draw_width = width.min((self.window.rect.w - 2).saturating_sub(left));
        let scroll = self.scroll_y.saturating_mul(self.scale);
        let first_y = scroll.saturating_sub(top).min(height);
        let last_y = scroll
            .saturating_add(self.window.rect.h - 26)
            .saturating_sub(top)
            .min(height);
        let text_y = r.h.saturating_sub(18) / 2;
        let tw = theme::text_width(text);
        let inset = r.w.saturating_sub(tw) / 2;
        let radius = 6.min(r.h / 2);
        for y in first_y..last_y {
            for x in 0..draw_width {
                let lx = x / self.scale;
                let ly = y / self.scale;
                let base = Self::rounded_frame_pixel(
                    lx,
                    ly,
                    r.w,
                    r.h,
                    radius,
                    outer_bg,
                    bg,
                    border_color,
                    0,
                );
                let p = if lx >= inset
                    && lx < inset + tw
                    && lx + 1 < r.w
                    && ly >= text_y
                    && ly < text_y + 18
                {
                    theme::text_pixel(text, lx - inset, ly - text_y, fg, base)
                } else {
                    base
                };
                if top + y >= scroll {
                    self.pixel(left + x, top + y - scroll, p);
                }
            }
        }
    }
    pub fn fill_rect(&mut self, r: Rect, color: u32) {
        let r = self.adj(r);
        let Some(left) = r.x.checked_mul(self.scale) else { return; };
        let Some(top) = r.y.checked_mul(self.scale) else { return; };
        let Some(width) = r.w.checked_mul(self.scale) else { return; };
        let Some(height) = r.h.checked_mul(self.scale) else { return; };
        let draw_width = width.min((self.window.rect.w - 2).saturating_sub(left));
        let scroll = self.scroll_y.saturating_mul(self.scale);
        let first_y = scroll.saturating_sub(top).min(height);
        let last_y = scroll
            .saturating_add(self.window.rect.h - 26)
            .saturating_sub(top)
            .min(height);
        for y in first_y..last_y {
            for x in 0..draw_width {
                if top + y >= scroll {
                    self.pixel(left + x, top + y - scroll, color);
                }
            }
        }
    }
    /// 1 px horizontal divider line.
    #[allow(dead_code)]
    pub fn divider_h(&mut self, x: usize, y: usize, w: usize, color: u32) {
        self.fill_rect(Rect { x, y, w, h: 1 }, color);
    }

    /// Flat card without shadow calculation (fast path, zero shadow damage).
    pub fn card_flat(&mut self, r: Rect, bg: u32, border: u32, outer_bg: u32) {
        let r = self.adj(r);
        let Some(left) = r.x.checked_mul(self.scale) else { return; };
        let Some(top) = r.y.checked_mul(self.scale) else { return; };
        let Some(width) = r.w.checked_mul(self.scale) else { return; };
        let Some(height) = r.h.checked_mul(self.scale) else { return; };
        let draw_width = width.min((self.window.rect.w - 2).saturating_sub(left));
        let scroll = self.scroll_y.saturating_mul(self.scale);
        let first_y = scroll.saturating_sub(top).min(height);
        let last_y = scroll
            .saturating_add(self.window.rect.h - 26)
            .saturating_sub(top)
            .min(height);
        let radius = theme::RADIUS_CARD.min(r.h / 2);
        let top_hi = if border == theme::BORDER { theme::TOP_HIGHLIGHT_DEFAULT } else { border };
        for y in first_y..last_y {
            for x in 0..draw_width {
                let lx = x / self.scale;
                let ly = y / self.scale;
                let color = Self::rounded_frame_pixel(
                    lx,
                    ly,
                    r.w,
                    r.h,
                    radius,
                    outer_bg,
                    bg,
                    border,
                    top_hi,
                );
                if top + y >= scroll {
                    self.pixel(left + x, top + y - scroll, color);
                }
            }
        }
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
        let r = self.adj(r);
        let total_h = r.h.saturating_add(elevation);
        let Some(left) = r.x.checked_mul(self.scale) else { return; };
        let Some(top) = r.y.checked_mul(self.scale) else { return; };
        let Some(width) = r.w.checked_mul(self.scale) else { return; };
        let Some(height) = total_h.checked_mul(self.scale) else { return; };
        let draw_width = width.min((self.window.rect.w - 2).saturating_sub(left));
        let scroll = self.scroll_y.saturating_mul(self.scale);
        let first_y = scroll.saturating_sub(top).min(height);
        let last_y = scroll
            .saturating_add(self.window.rect.h - 26)
            .saturating_sub(top)
            .min(height);
        let radius = theme::RADIUS_CARD.min(r.h / 2);
        let top_hi = if border == theme::BORDER { theme::TOP_HIGHLIGHT_DEFAULT } else { border };
        for y in first_y..last_y {
            for x in 0..draw_width {
                let lx = x / self.scale;
                let ly = y / self.scale;
                let color = if ly < r.h {
                    let shadow_bg = if ly >= 2 {
                        let s_cov = theme::rounded_coverage(lx, ly - 2, r.w, r.h, radius);
                        theme::blend_coverage(theme::SHADOW_CORE, outer_bg, s_cov * 6 / 16)
                    } else {
                        outer_bg
                    };
                    Self::rounded_frame_pixel(
                        lx,
                        ly,
                        r.w,
                        r.h,
                        radius,
                        shadow_bg,
                        bg,
                        border,
                        top_hi,
                    )
                } else {
                    let dy = ly - r.h;
                    let s_cov = theme::rounded_coverage(
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
                    theme::blend_coverage(theme::SHADOW_AMBIENT, outer_bg, alpha)
                };
                if top + y >= scroll {
                    self.pixel(left + x, top + y - scroll, color);
                }
            }
        }
    }

    #[allow(dead_code)]
    pub fn card(&mut self, r: Rect, bg: u32, border: u32) {
        self.card_elevated(r, bg, border, 3, theme::PANEL);
    }
    pub fn sheet_card(&mut self, r: Rect, bg: u32, border: u32, active: bool) {
        let r = self.adj(r);
        let pad = 4usize;
        let rx = r.x.saturating_sub(pad);
        let ry = r.y.saturating_sub(2);
        let rw = r.w + pad * 2;
        let rh = r.h + pad + 3;
        let Some(left) = rx.checked_mul(self.scale) else { return; };
        let Some(top) = ry.checked_mul(self.scale) else { return; };
        let Some(width) = rw.checked_mul(self.scale) else { return; };
        let Some(height) = rh.checked_mul(self.scale) else { return; };
        let draw_width = width.min((self.window.rect.w - 2).saturating_sub(left));
        let scroll = self.scroll_y.saturating_mul(self.scale);
        let first_y = scroll.saturating_sub(top).min(height);
        let last_y = scroll
            .saturating_add(self.window.rect.h - 26)
            .saturating_sub(top)
            .min(height);
        let radius = 8.min(r.h / 2);
        let top_hi = if active { 0xff38bdf8 } else { 0xff4c4c52 };
        for y in first_y..last_y {
            for x in 0..draw_width {
                let lx = x / self.scale;
                let ly = y / self.scale;
                let shadow_cov = theme::rounded_coverage(lx, ly, rw, rh, radius + 3);
                let halo_col = if active { 0xff0055cc } else { 0xff080a0f };
                let halo_alpha = if active { shadow_cov * 6 / 16 } else { shadow_cov * 6 / 16 };
                let outer_bg = theme::blend_coverage(halo_col, theme::PANEL, halo_alpha);
                let color = if lx >= pad && lx < pad + r.w && ly >= 2 && ly < 2 + r.h {
                    let cx = lx - pad;
                    let cy = ly - 2;
                    let row_bg = if cy < 24 && active {
                        0xff1e293b
                    } else if cy == 24 && active {
                        0xff2b3b52
                    } else {
                        bg
                    };
                    Self::rounded_frame_pixel(
                        cx,
                        cy,
                        r.w,
                        r.h,
                        radius,
                        outer_bg,
                        row_bg,
                        border,
                        top_hi,
                    )
                } else {
                    outer_bg
                };
                if top + y >= scroll {
                    self.pixel(left + x, top + y - scroll, color);
                }
            }
        }
    }
    pub fn button_accent(&mut self, r: Rect, text: &str, focused: bool) {
        self.box_text_centered(r, text, theme::TEXT_ON_ACCENT, theme::ACCENT, true, focused);
    }
    pub fn sidebar_nav(&mut self, r: Rect, text: &str, selected: bool, focused: bool) {
        let outer_bg = theme::BG_SIDEBAR;
        let bg = if selected { theme::BG_NAV_ROW } else { outer_bg };
        let border = if focused {
            theme::ACCENT
        } else if selected {
            theme::BG_NAV_SELECTED
        } else {
            outer_bg
        };
        let fg = if selected { theme::TEXT_PRIMARY } else { theme::TEXT_NAV_UNSELECTED };
        let icon_col = if selected { theme::BORDER_ACCENT_LIGHT } else { theme::TEXT_SLATE };
        let nav_kind = if text.starts_with("Personal") {
            0u8
        } else if text.starts_with("Network") {
            1u8
        } else if text.starts_with("Power") {
            3u8
        } else {
            2u8
        };
        let Some(left) = r.x.checked_mul(self.scale) else { return; };
        let Some(top) = r.y.checked_mul(self.scale) else { return; };
        let Some(width) = r.w.checked_mul(self.scale) else { return; };
        let Some(height) = r.h.checked_mul(self.scale) else { return; };
        let draw_width = width.min((self.window.rect.w - 2).saturating_sub(left));
        let scroll = self.scroll_y.saturating_mul(self.scale);
        let first_y = scroll.saturating_sub(top).min(height);
        let last_y = scroll
            .saturating_add(self.window.rect.h - 26)
            .saturating_sub(top)
            .min(height);
        let text_y = r.h.saturating_sub(18) / 2;
        let text_x = 22;
        let ic_x = 12isize;
        let ic_y = (r.h / 2) as isize;
        let top_hi = if focused {
            theme::TOP_HIGHLIGHT_NAV_FOCUSED
        } else if selected {
            theme::TOP_HIGHLIGHT_NAV
        } else {
            0
        };
        for y in first_y..last_y {
            for x in 0..draw_width {
                let lx = x / self.scale;
                let ly = y / self.scale;
                let base = if selected || focused {
                    Self::rounded_frame_pixel(lx, ly, r.w, r.h, 6, outer_bg, bg, border, top_hi)
                } else {
                    outer_bg
                };
                let is_indicator = selected && lx >= 4 && lx <= 6 && ly >= 8 && ly + 8 < r.h;
                let ix = lx as isize - ic_x;
                let iy = ly as isize - ic_y;
                let on_icon = if (-6..=6).contains(&ix) && (-6..=6).contains(&iy) {
                    match nav_kind {
                        0 => {
                            // Sliders icon
                            let line1 = iy == -3 && (-5..=5).contains(&ix);
                            let knob1 = (-4..=-2).contains(&iy) && (1..=3).contains(&ix);
                            let line2 = iy == 2 && (-5..=5).contains(&ix);
                            let knob2 = (1..=3).contains(&iy) && (-3..=-1).contains(&ix);
                            line1 || knob1 || line2 || knob2
                        }
                        1 => {
                            // Mini Wi-Fi waves icon
                            let wy = iy - 4;
                            let d2 = ix * ix + wy * wy;
                            let cone = wy <= 0 && ix.abs() <= (-wy + 1);
                            d2 <= 1 || (cone && ((12..=22).contains(&d2) || (42..=58).contains(&d2)))
                        }
                        2 => {
                            // Mini clock icon
                            let d2 = ix * ix + iy * iy;
                            (20..=34).contains(&d2)
                                || (ix == 0 && (-3..=0).contains(&iy))
                                || (iy == 0 && (0..=3).contains(&ix))
                        }
                        _ => {
                            // Mini battery icon: 10x6 body with 1x2 nub
                            let bx = ix;
                            let by = iy;
                            let is_nub = bx == 5 && (-1..=1).contains(&by);
                            let is_body = (-5..=4).contains(&bx) && (-3..=3).contains(&by);
                            let is_border = is_body && (bx == -5 || bx == 4 || by == -3 || by == 3);
                            let is_fill = (-4..=1).contains(&bx) && (-2..=2).contains(&by);
                            is_nub || is_border || is_fill
                        }
                    }
                } else {
                    false
                };
                let p = if is_indicator {
                    theme::ACCENT
                } else if on_icon {
                    icon_col
                } else if lx >= text_x && lx + 2 < r.w && ly >= text_y && ly < text_y + 18 {
                    if selected {
                        theme::text_pixel_bold(text, lx - text_x, ly - text_y, fg, base)
                    } else {
                        theme::text_pixel(text, lx - text_x, ly - text_y, fg, base)
                    }
                } else {
                    base
                };
                if top + y >= scroll {
                    self.pixel(left + x, top + y - scroll, p);
                }
            }
        }
    }
    pub fn button(&mut self, r: Rect, text: &str, selected: bool, focused: bool) {
        self.button_enabled(r, text, selected, focused, true);
    }
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
    pub fn wallpaper_card(
        &mut self,
        r: Rect,
        title: &str,
        is_wallpaper: bool,
        selected: bool,
        focused: bool,
    ) {
        let r = self.adj(r);
        let Some(left) = r.x.checked_mul(self.scale) else { return; };
        let Some(top) = r.y.checked_mul(self.scale) else { return; };
        let Some(width) = r.w.checked_mul(self.scale) else { return; };
        let Some(height) = r.h.checked_mul(self.scale) else { return; };
        let draw_width = width.min((self.window.rect.w - 2).saturating_sub(left));
        let scroll = self.scroll_y.saturating_mul(self.scale);
        let first_y = scroll.saturating_sub(top).min(height);
        let last_y = scroll
            .saturating_add(self.window.rect.h - 26)
            .saturating_sub(top)
            .min(height);
        let bg = if selected { 0xff1c2e44 } else { 0xff161e2a };
        let border = if selected {
            0xff3b82f6
        } else if focused {
            0xff69b7ff
        } else {
            0xff2a3648
        };
        let top_hi = if selected {
            0xff60a5fa
        } else if focused {
            0xff7dd3fc
        } else {
            0xff3a4a63
        };
        for y in first_y..last_y {
            for x in 0..draw_width {
                let lx = x / self.scale;
                let ly = y / self.scale;
                let base = Self::rounded_frame_pixel(
                    lx,
                    ly,
                    r.w,
                    r.h,
                    6,
                    theme::PANEL,
                    bg,
                    border,
                    top_hi,
                );
                let p = if lx >= 6 && lx < 28 && ly >= 4 && ly < 24 {
                    let sx = lx - 6;
                    let sy = ly - 4;
                    let is_swatch_border = sx == 0 || sy == 0 || sx == 21 || sy == 19;
                    if is_swatch_border {
                        if selected { 0xff3b82f6 } else { 0xff3a485c }
                    } else if is_wallpaper {
                        if sy < 10 {
                            if (sx == 4 && sy == 3) || (sx == 16 && sy == 5) {
                                0xffffffff
                            } else {
                                0xff0f1b2b
                            }
                        } else if sy == 10 {
                            0xff38bdf8
                        } else {
                            0xff09111c
                        }
                    } else {
                        let dx = sx as i32 - 11;
                        let dy = sy as i32 - 9;
                        let d2 = dx * dx + dy * dy;
                        if d2 <= 18 {
                            0xff254370
                        } else if d2 <= 48 {
                            0xff182b49
                        } else {
                            0xff0d1524
                        }
                    }
                } else if selected && lx >= 20 && lx <= 25 && ly >= 6 && ly <= 11 {
                    0xff3b82f6
                } else if lx >= 34 && lx < r.w.saturating_sub(6) && ly >= 5 && ly < 23 {
                    let fg = if selected { 0xffffffff } else { theme::TEXT };
                    theme::text_pixel(title, lx - 34, ly - 5, fg, base)
                } else {
                    base
                };
                if top + y >= scroll {
                    self.pixel(left + x, top + y - scroll, p);
                }
            }
        }
    }
    pub fn wifi_row(
        &mut self,
        r: Rect,
        ssid: &str,
        rssi: i8,
        security: u8,
        connected: bool,
        selected: bool,
        focused: bool,
        enabled: bool,
    ) {
        let r = self.adj(r);
        let Some(left) = r.x.checked_mul(self.scale) else { return; };
        let Some(top) = r.y.checked_mul(self.scale) else { return; };
        let Some(width) = r.w.checked_mul(self.scale) else { return; };
        let Some(height) = r.h.checked_mul(self.scale) else { return; };
        let draw_width = width.min((self.window.rect.w - 2).saturating_sub(left));
        let scroll = self.scroll_y.saturating_mul(self.scale);
        let first_y = scroll.saturating_sub(top).min(height);
        let last_y = scroll
            .saturating_add(self.window.rect.h - 26)
            .saturating_sub(top)
            .min(height);
        let bg = if !enabled {
            0xff1b2028
        } else if selected {
            0xff1e385b
        } else if focused {
            0xff232e3e
        } else {
            0xff182230
        };
        let border = if !enabled {
            0xff262f3d
        } else if selected {
            0xff3b82f6
        } else if focused {
            0xff69b7ff
        } else {
            0xff2a3648
        };
        let fg = if !enabled {
            theme::MUTED
        } else if selected {
            0xffffffff
        } else {
            theme::TEXT
        };
        let sig_bars = match rssi {
            -50..=127 => 4,
            -65..=-51 => 3,
            -75..=-66 => 2,
            _ => 1,
        };
        let sec_name = match security {
            0 => "Open",
            2 => "WPA3",
            3 => "WPA2/3",
            _ => "WPA2",
        };
        let sec_tw = theme::text_width(sec_name);
        let sec_pill_w = sec_tw + 14;
        let sec_pill_x = r.w.saturating_sub(sec_pill_w + 8);
        let sec_text_x = sec_pill_x + 7;
        let top_hi = if !enabled {
            0
        } else if selected {
            0xff60a5fa
        } else if focused {
            0xff7dd3fc
        } else {
            0xff3b4c66
        };
        for y in first_y..last_y {
            for x in 0..draw_width {
                let lx = x / self.scale;
                let ly = y / self.scale;
                let base = Self::rounded_frame_pixel(
                    lx,
                    ly,
                    r.w,
                    r.h,
                    6,
                    theme::PANEL,
                    bg,
                    border,
                    top_hi,
                );
                let p = if lx >= 10 && lx < 22 && ly >= 6 && ly < 18 {
                    let bar_idx = (lx - 10) / 3;
                    let bar_x = (lx - 10) % 3;
                    let bar_h = (bar_idx + 1) * 3;
                    let bar_y_start = 18 - bar_h;
                    if bar_x < 2 && ly >= bar_y_start && ly < 18 {
                        if enabled && bar_idx < sig_bars {
                            0xff38bdf8
                        } else {
                            0xff334155
                        }
                    } else {
                        base
                    }
                } else if security != 0 && lx >= 26 && lx < 34 && ly >= 6 && ly < 18 {
                    let sx = lx - 26;
                    let sy = ly - 6;
                    let is_shackle = (sx == 2 || sx == 5) && (sy >= 2 && sy <= 5)
                        || (sx >= 3 && sx <= 4 && sy == 1);
                    let is_body = sx >= 1 && sx <= 6 && sy >= 5 && sy <= 10;
                    let is_keyhole = sx == 3 && sy >= 7 && sy <= 8;
                    if is_keyhole {
                        base
                    } else if is_shackle || is_body {
                        if enabled { 0xff9fb0c7 } else { theme::MUTED }
                    } else {
                        base
                    }
                } else if lx >= 38 && lx < 250 && ly >= 3 && ly < 21 {
                    theme::text_pixel(ssid, lx - 38, ly - 3, fg, base)
                } else if connected && lx >= 252 && lx < 368 && ly >= 3 && ly < 21 {
                    let pill_x = lx - 252;
                    let pill_y = ly - 3;
                    let pill_bg = Self::rounded_frame_pixel(
                        pill_x,
                        pill_y,
                        116,
                        18,
                        5,
                        base,
                        0xff133223,
                        0xff22c55e,
                        0,
                    );
                    if pill_x >= 8 && pill_x <= 12 && pill_y >= 7 && pill_y <= 11 {
                        0xff4ade80
                    } else if pill_x >= 18 && pill_x < 110 {
                        theme::text_pixel("Connected", pill_x - 18, pill_y, 0xff4ade80, pill_bg)
                    } else {
                        pill_bg
                    }
                } else if enabled && lx >= 382 && lx < 458 && ly >= 3 && ly < 21 {
                    let sig_name = match sig_bars {
                        4 => "Excellent",
                        3 => "Good",
                        2 => "Fair",
                        _ => "Weak",
                    };
                    theme::text_pixel(sig_name, lx - 382, ly - 3, theme::MUTED, base)
                } else if enabled && lx >= sec_pill_x && lx < sec_pill_x + sec_pill_w && ly >= 3 && ly < 21 {
                    let pill_x = lx - sec_pill_x;
                    let pill_y = ly - 3;
                    let pill_bg = Self::rounded_frame_pixel(
                        pill_x,
                        pill_y,
                        sec_pill_w,
                        18,
                        5,
                        base,
                        0xff222f42,
                        0xff33465e,
                        0,
                    );
                    if lx >= sec_text_x && lx < sec_text_x + sec_tw {
                        theme::text_pixel(sec_name, lx - sec_text_x, pill_y, 0xffb8c7dc, pill_bg)
                    } else {
                        pill_bg
                    }
                } else {
                    base
                };
                if top + y >= scroll {
                    self.pixel(left + x, top + y - scroll, p);
                }
            }
        }
    }
    pub fn entry(&mut self, r: Rect, text: &str, focused: bool) {
        self.entry_on(r, text, focused, 0xff282828);
    }
    pub fn entry_on(&mut self, r: Rect, text: &str, focused: bool, outer_bg: u32) {
        let r = self.adj(r);
        let glow = 2usize;
        let gx = r.x.saturating_sub(glow);
        let gy = r.y.saturating_sub(glow);
        let gw = r.w + glow * 2;
        let gh = r.h + glow * 2;
        let Some(left) = gx.checked_mul(self.scale) else { return; };
        let Some(top) = gy.checked_mul(self.scale) else { return; };
        let Some(width) = gw.checked_mul(self.scale) else { return; };
        let Some(height) = gh.checked_mul(self.scale) else { return; };
        let draw_width = width.min((self.window.rect.w - 2).saturating_sub(left));
        let scroll = self.scroll_y.saturating_mul(self.scale);
        let first_y = scroll.saturating_sub(top).min(height);
        let last_y = scroll
            .saturating_add(self.window.rect.h - 26)
            .saturating_sub(top)
            .min(height);
        let well_bg = 0xff12151c;
        let radius = 6.min(r.h / 2);
        let text_y = r.h.saturating_sub(18) / 2;
        let inset = 10usize;
        for y in first_y..last_y {
            for x in 0..draw_width {
                let lx = x / self.scale;
                let ly = y / self.scale;
                let glow_bg = if focused {
                    let g_cov = theme::rounded_coverage(lx, ly, gw, gh, radius + glow);
                    theme::blend_coverage(theme::ACCENT, outer_bg, g_cov * 5 / 16)
                } else {
                    outer_bg
                };
                let p = if lx >= glow && lx < glow + r.w && ly >= glow && ly < glow + r.h {
                    let bx = lx - glow;
                    let by = ly - glow;
                    let outer_cov = theme::rounded_coverage(bx, by, r.w, r.h, radius);
                    if outer_cov == 0 {
                        glow_bg
                    } else {
                        let inner_cov = if bx >= 1 && bx + 1 < r.w && by >= 1 && by + 1 < r.h {
                            theme::rounded_coverage(
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
                            0xff090b0f // Dark top inner shadow (recessed cutout)
                        } else if by + 1 == r.h {
                            0xff364050 // Subtle bottom ambient lip
                        } else {
                            0xff1f2530
                        };
                        let interior_col = if !focused && by == 1 {
                            0xff0c0f15 // 1px subtle top inner shadow inside the well
                        } else {
                            well_bg
                        };
                        let inner_blend = theme::blend_coverage(interior_col, rim_col, inner_cov);
                        let base = theme::blend_coverage(inner_blend, glow_bg, outer_cov);
                        if bx >= inset && bx + 6 < r.w && by >= text_y && by < text_y + 18 {
                            theme::text_pixel(text, bx - inset, by - text_y, 0xffffffff, base)
                        } else {
                            base
                        }
                    }
                } else {
                    glow_bg
                };
                if top + y >= scroll {
                    self.pixel(left + x, top + y - scroll, p);
                }
            }
        }
    }
    fn box_text_centered(
        &mut self,
        r: Rect,
        text: &str,
        color: u32,
        background: u32,
        border: bool,
        focused: bool,
    ) {
        let r = self.adj(r);
        let Some(left) = r.x.checked_mul(self.scale) else {
            return;
        };
        let Some(top) = r.y.checked_mul(self.scale) else {
            return;
        };
        let Some(width) = r.w.checked_mul(self.scale) else {
            return;
        };
        let Some(height) = r.h.checked_mul(self.scale) else {
            return;
        };
        let draw_width = width.min((self.window.rect.w - 2).saturating_sub(left));
        let scroll = self.scroll_y.saturating_mul(self.scale);
        let first_y = scroll.saturating_sub(top).min(height);
        let last_y = scroll
            .saturating_add(self.window.rect.h - 26)
            .saturating_sub(top)
            .min(height);
        let text_y = r.h.saturating_sub(18) / 2;
        let tw = theme::text_width(text);
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
        for y in first_y..last_y {
            for x in 0..draw_width {
                let lx = x / self.scale;
                let ly = y / self.scale;
                let base = if border {
                    Self::rounded_frame_pixel(
                        lx,
                        ly,
                        r.w,
                        r.h,
                        radius,
                        theme::PANEL,
                        background,
                        border_col,
                        top_hi,
                    )
                } else {
                    background
                };
                let p = if lx >= inset && lx + 3 < r.w && ly >= text_y && ly < text_y + 18 {
                    theme::text_pixel(text, lx - inset, ly - text_y, color, base)
                } else {
                    base
                };
                if top + y >= scroll {
                    self.pixel(left + x, top + y - scroll, p);
                }
            }
        }
    }
    fn box_text(
        &mut self,
        r: Rect,
        text: &str,
        color: u32,
        background: u32,
        border: bool,
        focused: bool,
    ) {
        let r = self.adj(r);
        let Some(left) = r.x.checked_mul(self.scale) else {
            return;
        };
        let Some(top) = r.y.checked_mul(self.scale) else {
            return;
        };
        let Some(width) = r.w.checked_mul(self.scale) else {
            return;
        };
        let Some(height) = r.h.checked_mul(self.scale) else {
            return;
        };
        let draw_width = width.min((self.window.rect.w - 2).saturating_sub(left));
        let scroll = self.scroll_y.saturating_mul(self.scale);
        let first_y = scroll.saturating_sub(top).min(height);
        let last_y = scroll
            .saturating_add(self.window.rect.h - 26)
            .saturating_sub(top)
            .min(height);
        let text_y = r.h.saturating_sub(18) / 2;
        let inset = if border { 8 } else { 0 };
        let right_pad = if border { 4 } else { 0 };
        let border_col = if focused {
            theme::ACCENT
        } else {
            0xff3a4659
        };
        let radius = 6.min(r.h / 2);
        for y in first_y..last_y {
            for x in 0..draw_width {
                let lx = x / self.scale;
                let ly = y / self.scale;
                let base = if border {
                    Self::rounded_frame_pixel(
                        lx,
                        ly,
                        r.w,
                        r.h,
                        radius,
                        0xff282828,
                        background,
                        border_col,
                        0,
                    )
                } else {
                    background
                };
                let p = if lx >= inset && lx + right_pad < r.w && ly >= text_y && ly < text_y + 18 {
                    theme::text_pixel(text, lx - inset, ly - text_y, color, base)
                } else {
                    base
                };
                if top + y >= scroll {
                    self.pixel(left + x, top + y - scroll, p);
                }
            }
        }
    }
    pub fn battery_icon(&mut self, x: usize, y: usize, status: &crate::sys::BatteryStatus) {
        let r = self.adj(Rect { x, y, w: 22, h: 10 });
        let Some(left) = r.x.checked_mul(self.scale) else { return; };
        let Some(top) = r.y.checked_mul(self.scale) else { return; };
        let draw_width = (22 * self.scale).min((self.window.rect.w - 2).saturating_sub(left));
        let draw_height = (10 * self.scale).min((self.window.rect.h - 26).saturating_sub(top));
        for dy in 0..draw_height {
            for dx in 0..draw_width {
                let lx = dx / self.scale;
                let ly = dy / self.scale;
                if let Some(col) = render_battery_pixel(lx, ly, status, theme::PANEL) {
                    self.pixel(left + dx, top + dy, col);
                }
            }
        }
    }
    pub fn finish(self) -> Option<Rect> {
        self.dirty.map(|r| Rect {
            x: self.window.rect.x + 1 + r.x,
            y: self.window.rect.y + 25 + r.y,
            ..r
        })
    }
}

/// Data-driven registration: stable IDs are part of the built-in app interface.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AppDescriptor {
    pub id: u32,
    pub name: &'static str,
    pub width: usize,
    pub height: usize,
}
pub const APP_API_VERSION: u32 = 1;
pub const APPS: [AppDescriptor; 4] = [
    AppDescriptor {
        id: 7,
        name: "Settings",
        width: 760,
        height: 540,
    },
    AppDescriptor {
        id: 8,
        name: "Network & Wi-Fi",
        width: 760,
        height: 540,
    },
    AppDescriptor {
        id: 9,
        name: "Date & Time",
        width: 760,
        height: 540,
    },
    AppDescriptor {
        id: 10,
        name: "Power & Battery",
        width: 760,
        height: 540,
    },
];
pub fn registered(id: u32) -> bool {
    APPS.iter().any(|app| app.id == id)
}

pub trait DesktopApp {
    fn descriptor(&self) -> AppDescriptor;
    fn pointer(&mut self, x: usize, y: usize) -> Option<u16>;
    fn key(&mut self, byte: u8) -> Option<u16>;
}

// Shared with isolated apps; keep the widget API available here.
pub use crate::window::theme::render_battery_pixel;
