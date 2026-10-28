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
    pub fn sidebar_nav(&mut self, r: Rect, text: &str, selected: bool, focused: bool) {
        let bg = if selected { 0xff1e324a } else { 0xff111620 };
        let fg = if selected { 0xffffffff } else { 0xff9fb0c7 };
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
        let text_x = 16;
        for y in first_y..last_y {
            for x in 0..draw_width {
                let lx = x / self.scale;
                let ly = y / self.scale;
                let is_indicator = selected && lx >= 2 && lx <= 4 && ly >= 6 && ly + 6 <= r.h;
                let p = if is_indicator {
                    theme::ACCENT
                } else if focused && (x == 0 || y == 0 || x + 1 == width || y + 1 == height) {
                    theme::ACCENT
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
        height: 540,
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
