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
            result.dirty = Some(Rect {
                x: 0,
                y: 0,
                w: result.window.rect.w - 2,
                h: result.window.rect.h - 26,
            });
        }
        result
    }
    pub fn width(&self) -> usize {
        (self.window.rect.w - 2) / self.scale
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
        width: 620,
        height: 450,
    },
    AppDescriptor {
        id: 8,
        name: "Network & Wi-Fi",
        width: 680,
        height: 580,
    },
    AppDescriptor {
        id: 9,
        name: "Date & Time",
        width: 620,
        height: 450,
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
