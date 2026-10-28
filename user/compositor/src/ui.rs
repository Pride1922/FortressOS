//! Shared immediate widgets. Every primitive compares final pixels and reports
//! bounded client damage; idle labels never invalidate an entire application.
use crate::{
    surface::Rect,
    window::{Window, theme},
};

pub struct Canvas<'a> {
    window: &'a mut Window,
    scale: usize,
    dirty: Option<Rect>,
    scroll_y: usize,
}
impl<'a> Canvas<'a> {
    #[allow(dead_code)] // Convenience constructor for new apps and host tests.
    pub fn new(window: &'a mut Window, scale: usize, reset: bool) -> Self {
        Self::scrolled(window, scale, reset, 0)
    }
    pub fn scrolled(window: &'a mut Window, scale: usize, reset: bool, scroll_y: usize) -> Self {
        let scale = scale.clamp(1, 2);
        let mut result = Self {
            window,
            scale,
            dirty: None,
            scroll_y,
        };
        if reset {
            result.window.content.fill(theme::PANEL);
            if registered(result.window.id) {
                let width = result.window.rect.w - 2;
                let sidebar_w = (180 * scale).min(width);
                let height = result.window.rect.h - 26;
                for y in 0..height {
                    for x in 0..sidebar_w {
                        result.window.content[y * width + x] = 0xff111620;
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
    pub fn badge(&mut self, r: Rect, text: &str, fg: u32, bg: u32, border_color: u32) {
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
        let inset = r.w.saturating_sub(text.len() * 8) / 2;
        for y in first_y..last_y {
            for x in 0..draw_width {
                let lx = x / self.scale;
                let ly = y / self.scale;
                let is_border = x == 0 || y == 0 || x + 1 == width || y + 1 == height;
                let p = if is_border {
                    border_color
                } else if lx >= inset && lx < inset + text.len() * 8 && ly >= text_y && ly < text_y + 18 {
                    theme::text_pixel(text, lx - inset, ly - text_y, fg, bg)
                } else {
                    bg
                };
                if top + y >= scroll {
                    self.pixel(left + x, top + y - scroll, p);
                }
            }
        }
    }
    pub fn fill_rect(&mut self, r: Rect, color: u32) {
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
    #[allow(dead_code)]
    pub fn card(&mut self, r: Rect, bg: u32, border: u32) {
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
                let is_border = x == 0 || y == 0 || x + 1 == width || y + 1 == height;
                let color = if is_border { border } else { bg };
                if top + y >= scroll {
                    self.pixel(left + x, top + y - scroll, color);
                }
            }
        }
    }
    pub fn button_accent(&mut self, r: Rect, text: &str, focused: bool) {
        self.box_text(r, text, 0xffffffff, 0xff2563eb, true, focused);
    }
    #[allow(dead_code)]
    pub fn sidebar_nav(&mut self, r: Rect, text: &str, selected: bool, focused: bool) {
        self.sidebar_nav_item(r, 0, text, selected, focused);
    }
    pub fn sidebar_nav_item(
        &mut self,
        r: Rect,
        icon_id: usize,
        text: &str,
        selected: bool,
        focused: bool,
    ) {
        let bg = if selected { 0xff1e2634 } else { 0xff111620 };
        let fg = if selected { 0xffffffff } else { 0xff94a3b8 };
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
        let text_x = 32;
        let icon_x = 10;
        let icon_y = r.h.saturating_sub(12) / 2;

        for y in first_y..last_y {
            for x in 0..draw_width {
                let lx = x / self.scale;
                let ly = y / self.scale;
                let is_indicator = selected && lx <= 2 && ly >= 6 && ly + 6 <= r.h;
                let in_icon = (icon_x..icon_x + 12).contains(&lx)
                    && (icon_y..icon_y + 12).contains(&ly)
                    && category_icon_pixel(icon_id, lx - icon_x, ly - icon_y);

                let p = if is_indicator {
                    0xff38bdf8
                } else if in_icon {
                    if selected { 0xff38bdf8 } else { 0xff94a3b8 }
                } else if focused && (x == 0 || y == 0 || x + 1 == width || y + 1 == height) {
                    0xff38bdf8
                } else if lx >= text_x && lx < r.w && ly >= text_y && ly < text_y + 18 {
                    theme::text_pixel(text, lx - text_x, ly - text_y, fg, bg)
                } else {
                    bg
                };
                if top + y >= scroll {
                    self.pixel(left + x, top + y - scroll, p);
                }
            }
        }
    }
    pub fn toggle_switch(&mut self, r: Rect, on: bool) {
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
        let bg = if on { 0xff0078d4 } else { 0xff20242c };
        let border = if on { 0xff0078d4 } else { 0xff52525b };
        let knob_color = if on { 0xffffffff } else { 0xffa1a1aa };
        let rad = (r.h / 2) as i32;
        let knob_cx = if on { (r.w as i32) - rad } else { rad };
        let knob_cy = rad;
        let knob_rad = rad - 2;

        for y in first_y..last_y {
            for x in 0..draw_width {
                let lx = (x / self.scale) as i32;
                let ly = (y / self.scale) as i32;
                let in_pill = if lx < rad {
                    (lx - rad).pow(2) + (ly - rad).pow(2) <= rad.pow(2)
                } else if lx >= (r.w as i32) - rad {
                    (lx - ((r.w as i32) - rad)).pow(2) + (ly - rad).pow(2) <= rad.pow(2)
                } else {
                    ly >= 0 && ly < r.h as i32
                };
                if !in_pill {
                    continue;
                }
                let in_knob = (lx - knob_cx).pow(2) + (ly - knob_cy).pow(2) <= knob_rad.pow(2);
                let p = if in_knob {
                    knob_color
                } else {
                    let is_border = if !on {
                        if lx < rad {
                            let d = (lx - rad).pow(2) + (ly - rad).pow(2);
                            d >= (rad - 1).pow(2)
                        } else if lx >= (r.w as i32) - rad {
                            let d = (lx - ((r.w as i32) - rad)).pow(2) + (ly - rad).pow(2);
                            d >= (rad - 1).pow(2)
                        } else {
                            ly == 0 || ly == (r.h as i32) - 1
                        }
                    } else {
                        false
                    };
                    if is_border { border } else { bg }
                };
                if top + y >= scroll {
                    self.pixel(left + x, top + y - scroll, p);
                }
            }
        }
    }
    pub fn chevron(&mut self, ox: usize, oy: usize, color: u32) {
        let pattern = [(0, 0), (1, 1), (2, 2), (3, 3), (4, 2), (5, 1), (6, 0)];
        let scroll = self.scroll_y.saturating_mul(self.scale);
        for &(dy, dx) in &pattern {
            for sy in 0..self.scale {
                for sx in 0..self.scale {
                    let px = (ox + dx) * self.scale + sx;
                    let py = (oy + dy) * self.scale + sy;
                    if py >= scroll && py < scroll + self.window.rect.h - 26 {
                        self.pixel(px, py - scroll, color);
                    }
                }
            }
        }
    }
    pub fn search_bar(&mut self, r: Rect, placeholder: &str) {
        self.box_text(r, placeholder, 0xff8b949e, 0xff161b22, true, false);
    }
    pub fn monitor_hero_icon(&mut self, ox: usize, oy: usize, color: u32) {
        let Some(left) = ox.checked_mul(self.scale) else { return; };
        let Some(top) = oy.checked_mul(self.scale) else { return; };
        let scroll = self.scroll_y.saturating_mul(self.scale);

        for iy in 0..26 * self.scale {
            for ix in 0..32 * self.scale {
                let mx = ix / self.scale;
                let my = iy / self.scale;
                let is_monitor = if my == 0 || my == 18 {
                    mx >= 2 && mx <= 29
                } else if my < 18 {
                    (mx == 0 || mx == 31) && my >= 2 && my <= 16
                } else if (19..=22).contains(&my) {
                    mx == 15 || mx == 16
                } else if my == 23 || my == 24 {
                    mx >= 10 && mx <= 21
                } else {
                    false
                };
                if is_monitor {
                    let py = top + iy;
                    if py >= scroll && py < scroll + self.window.rect.h - 26 {
                        self.pixel(left + ix, py - scroll, color);
                    }
                }
            }
        }
    }
    pub fn settings_card(
        &mut self,
        r: Rect,
        icon_id: usize,
        title: &str,
        subtitle: &str,
        status: Option<&str>,
        toggle: Option<bool>,
        has_chevron: bool,
        focused: bool,
    ) {
        let bg = 0xff182230;
        let border = if focused { 0xff38bdf8 } else { 0xff253040 };
        self.card(r, bg, border);

        let icon_x = r.x + 12;
        let icon_y = r.y + (r.h.saturating_sub(12)) / 2;
        let Some(left) = icon_x.checked_mul(self.scale) else { return; };
        let Some(top) = icon_y.checked_mul(self.scale) else { return; };
        let scroll = self.scroll_y.saturating_mul(self.scale);
        for iy in 0..12 * self.scale {
            for ix in 0..12 * self.scale {
                if category_icon_pixel(icon_id, ix / self.scale, iy / self.scale) {
                    let py = top + iy;
                    if py >= scroll && py < scroll + self.window.rect.h - 26 {
                        self.pixel(left + ix, py - scroll, 0xff38bdf8);
                    }
                }
            }
        }

        if subtitle.is_empty() {
            self.label_h(r.x + 34, r.y + (r.h.saturating_sub(18)) / 2, r.w.saturating_sub(140), 18, title, 0xffffffff, bg);
        } else {
            self.label_h(r.x + 34, r.y + 3, r.w.saturating_sub(140), 18, title, 0xffffffff, bg);
            self.label_h(r.x + 34, r.y + 19, r.w.saturating_sub(140), 16, subtitle, theme::MUTED, bg);
        }

        let mut right_cursor = r.x + r.w - 14;
        if has_chevron {
            right_cursor = right_cursor.saturating_sub(10);
            self.chevron(right_cursor, r.y + (r.h.saturating_sub(7)) / 2, 0xff94a3b8);
        }
        if let Some(on) = toggle {
            right_cursor = right_cursor.saturating_sub(38);
            self.toggle_switch(Rect { x: right_cursor, y: r.y + (r.h.saturating_sub(16)) / 2, w: 32, h: 16 }, on);
        }
        if let Some(stat) = status {
            let stat_w = stat.len() * 8;
            right_cursor = right_cursor.saturating_sub(stat_w + 8);
            let stat_color = if stat == "Ligado" || stat == "Connected" { 0xff4ade80 } else { theme::MUTED };
            self.label_h(right_cursor, r.y + (r.h.saturating_sub(18)) / 2, stat_w, 18, stat, stat_color, bg);
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
        self.box_text(
            r,
            text,
            if enabled { theme::TEXT } else { theme::MUTED },
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
        for y in first_y..last_y {
            for x in 0..draw_width {
                let lx = x / self.scale;
                let ly = y / self.scale;
                let is_border = x == 0 || y == 0 || x + 1 == width || y + 1 == height;
                let p = if is_border {
                    border
                } else if lx >= 6 && lx < 28 && ly >= 4 && ly < 24 {
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
                        0xff080a0e
                    }
                } else if selected && lx >= 20 && lx <= 25 && ly >= 6 && ly <= 11 {
                    0xff3b82f6
                } else if lx >= 34 && lx < r.w.saturating_sub(6) && ly >= 5 && ly < 23 {
                    let fg = if selected { 0xffffffff } else { theme::TEXT };
                    theme::text_pixel(title, lx - 34, ly - 5, fg, bg)
                } else {
                    bg
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
            theme::PANEL
        } else if selected {
            0xff1e385b
        } else if focused {
            0xff232e3e
        } else {
            0xff182230
        };
        let border = if !enabled {
            0xff222a36
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
            -55..=127 => 4,
            -67..=-56 => 3,
            -75..=-68 => 2,
            _ => 1,
        };
        for y in first_y..last_y {
            for x in 0..draw_width {
                let lx = x / self.scale;
                let ly = y / self.scale;
                let is_border = x == 0 || y == 0 || x + 1 == width || y + 1 == height;
                let p = if is_border {
                    border
                } else if lx >= 10 && lx < 22 && ly >= 6 && ly < 18 {
                    let bar_idx = (lx - 10) / 3;
                    let bar_x = (lx - 10) % 3;
                    let bar_h = (bar_idx + 1) * 3;
                    let bar_y_start = 18 - bar_h;
                    if bar_x < 2 && ly >= bar_y_start && ly < 18 {
                        if enabled && bar_idx < sig_bars {
                            0xff69b7ff
                        } else {
                            0xff334155
                        }
                    } else {
                        bg
                    }
                } else if security != 0 && lx >= 26 && lx < 34 && ly >= 6 && ly < 18 {
                    let sx = lx - 26;
                    let sy = ly - 6;
                    let is_shackle = (sx == 2 || sx == 5) && (sy >= 2 && sy <= 5)
                        || (sx >= 3 && sx <= 4 && sy == 1);
                    let is_body = sx >= 1 && sx <= 6 && sy >= 5 && sy <= 10;
                    let is_keyhole = sx == 3 && sy >= 7 && sy <= 8;
                    if is_keyhole {
                        bg
                    } else if is_shackle || is_body {
                        if enabled { 0xff9fb0c7 } else { theme::MUTED }
                    } else {
                        bg
                    }
                } else if lx >= 38 && lx < 240 && ly >= 3 && ly < 21 {
                    theme::text_pixel(ssid, lx - 38, ly - 3, fg, bg)
                } else if connected && lx >= 244 && lx < 330 && ly >= 3 && ly < 21 {
                    if lx >= 246 && lx <= 250 && ly >= 9 && ly <= 13 {
                        0xff4ade80
                    } else if lx >= 256 {
                        theme::text_pixel("Connected", lx - 256, ly - 3, 0xff4ade80, bg)
                    } else {
                        bg
                    }
                } else if enabled && lx >= 340 && lx < 400 && ly >= 3 && ly < 21 {
                    let sig_name = match sig_bars {
                        4 => "Excellent",
                        3 => "Good",
                        2 => "Fair",
                        _ => "Weak",
                    };
                    theme::text_pixel(sig_name, lx - 340, ly - 3, theme::MUTED, bg)
                } else if enabled && lx >= 410 && lx < 450 && ly >= 3 && ly < 21 {
                    let sec_name = if security == 0 { "Open" } else { "WPA2" };
                    theme::text_pixel(sec_name, lx - 410, ly - 3, theme::MUTED, bg)
                } else {
                    bg
                };
                if top + y >= scroll {
                    self.pixel(left + x, top + y - scroll, p);
                }
            }
        }
    }
    pub fn entry(&mut self, r: Rect, text: &str, focused: bool) {
        self.box_text(r, text, theme::TEXT, 0xff101722, true, focused);
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
        let inset = if border { 9 } else { 0 };
        for y in first_y..last_y {
            for x in 0..draw_width {
                let lx = x / self.scale;
                let ly = y / self.scale;
                let p = if border && (x == 0 || y == 0 || x + 1 == width || y + 1 == height) {
                    if focused {
                        theme::ACCENT
                    } else {
                        theme::BORDER
                    }
                } else if lx >= inset && lx + inset < r.w && ly >= text_y && ly < text_y + 18 {
                    theme::text_pixel(text, lx - inset, ly - text_y, color, background)
                } else {
                    background
                };
                if top + y >= scroll {
                    self.pixel(left + x, top + y - scroll, p);
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

pub fn category_icon_pixel(icon_id: usize, ix: usize, iy: usize) -> bool {
    if ix >= 12 || iy >= 12 {
        return false;
    }
    match icon_id {
        // 0: House / Home
        0 => match iy {
            0 => (5..=6).contains(&ix),
            1 => (4..=7).contains(&ix),
            2 => (3..=8).contains(&ix),
            3 => (2..=9).contains(&ix),
            4 => (1..=10).contains(&ix),
            5 => true,
            6..=10 => ix == 1 || ix == 10 || (iy == 6 && (4..=7).contains(&ix)),
            11 => (1..=10).contains(&ix),
            _ => false,
        },
        // 1: Laptop / Screen (Sistema)
        1 => match iy {
            1 | 7 => (1..=10).contains(&ix),
            2..=6 => ix == 1 || ix == 10,
            8 => ix == 5 || ix == 6,
            9..=10 => true,
            _ => false,
        },
        // 2: Wi-Fi (Rede e Internet)
        2 => match iy {
            1 => (2..=9).contains(&ix),
            2 => (1..=2).contains(&ix) || (9..=10).contains(&ix),
            3 => ix == 0 || ix == 11,
            4 => (4..=7).contains(&ix),
            5 => (3..=4).contains(&ix) || (7..=8).contains(&ix),
            7..=8 => (5..=6).contains(&ix),
            _ => false,
        },
        // 3: Personalizacao (Palette)
        3 => {
            let dx = ix as i32 - 5;
            let dy = iy as i32 - 5;
            let dist_sq = dx * dx + dy * dy;
            if dist_sq > 25 {
                false
            } else if (ix == 7 || ix == 8) && (iy == 6 || iy == 7) {
                false
            } else if (ix == 3 && iy == 4) || (ix == 5 && iy == 3) || (ix == 7 && iy == 4) {
                false
            } else {
                dist_sq >= 14 || (ix == 3 && iy == 5) || (ix == 5 && iy == 4)
            }
        }
        // 4: Hora e Idioma (Clock)
        4 => {
            let dx = ix as i32 - 5;
            let dy = iy as i32 - 5;
            let dist_sq = dx * dx + dy * dy;
            if dist_sq > 25 || dist_sq < 14 {
                if ix == 5 && (2..=5).contains(&iy) {
                    true
                } else if iy == 5 && (5..=8).contains(&ix) {
                    true
                } else {
                    false
                }
            } else {
                true
            }
        }
        // 5: Contas / User (Person)
        5 => match iy {
            1 | 4 => (4..=7).contains(&ix),
            2..=3 => ix == 3 || ix == 8,
            6 => (3..=8).contains(&ix),
            7..=10 => ix == 1 || ix == 10 || (iy == 10 && (1..=10).contains(&ix)),
            _ => false,
        },
        // 6: Privacidade / Shield
        6 => match iy {
            1 => (1..=10).contains(&ix),
            2..=6 => ix == 1 || ix == 10,
            7 => ix == 2 || ix == 9,
            8 => ix == 3 || ix == 8,
            9 => ix == 4 || ix == 7,
            10 => ix == 5 || ix == 6,
            _ => false,
        },
        // 7: Windows Update (Refresh loop)
        7 => match iy {
            1 | 10 => (3..=8).contains(&ix),
            2..=3 => ix == 2 || ix == 9 || (iy == 1 && ix >= 7),
            8..=9 => ix == 2 || ix == 9 || (iy == 10 && ix <= 4),
            _ => false,
        },
        // 8: Ethernet Port (Ethernet Card)
        8 => match iy {
            0..=1 => (4..=7).contains(&ix),
            2 | 9 => (1..=10).contains(&ix),
            3..=8 => ix == 1 || ix == 10 || (iy == 5 && (3..=8).contains(&ix) && ix % 2 == 1),
            _ => false,
        },
        // 9: VPN / Lock
        9 => match iy {
            1 => (4..=7).contains(&ix),
            2..=4 => ix == 3 || ix == 8,
            5 | 10 => (2..=9).contains(&ix),
            6..=9 => ix == 2 || ix == 9 || (iy == 7 && (5..=6).contains(&ix)),
            _ => false,
        },
        // 10: Hotspot movel (Radio Antenna)
        10 => match iy {
            0 => ix == 1 || ix == 10,
            1 => ix == 2 || ix == 9,
            2..=3 => (4..=7).contains(&ix),
            4..=10 => ix == 5 || ix == 6,
            11 => (3..=8).contains(&ix),
            _ => false,
        },
        // 11: Modo de aviao (Airplane)
        11 => match iy {
            0..=1 => ix == 5 || ix == 6,
            2..=4 => (4..=7).contains(&ix),
            5 => true,
            6 => (2..=9).contains(&ix),
            7..=9 => ix == 5 || ix == 6,
            10 => (3..=8).contains(&ix),
            11 => ix == 5 || ix == 6,
            _ => false,
        },
        // 12: Proxy / Server
        12 => match iy {
            1 | 4 | 6 | 9 => (1..=10).contains(&ix),
            2..=3 => ix == 1 || ix == 10 || (ix == 8 && iy == 2),
            7..=8 => ix == 1 || ix == 10 || (ix == 8 && iy == 7),
            _ => false,
        },
        // 13: Definicoes avancadas (Sliders)
        13 => match iy {
            1..=10 => {
                ix == 3 || ix == 8
                    || ((2..=4).contains(&ix) && (3..=5).contains(&iy))
                    || ((7..=9).contains(&ix) && (6..=8).contains(&iy))
            }
            _ => false,
        },
        _ => false,
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
pub const APPS: [AppDescriptor; 3] = [
    AppDescriptor {
        id: 7,
        name: "Settings",
        width: 680,
        height: 520,
    },
    AppDescriptor {
        id: 8,
        name: "Network & Wi-Fi",
        width: 680,
        height: 720,
    },
    AppDescriptor {
        id: 9,
        name: "Date & Time",
        width: 680,
        height: 520,
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
