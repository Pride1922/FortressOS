//! Single-process window server. Stack order is back to front; no client IPC yet.
#[path = "theme.rs"]
pub mod theme;
#[path = "app_registry.rs"]
pub mod app_registry;
pub use app_registry::AppId;

use crate::{
    font::GLYPHS,
    surface::{Rect, Surface},
};
use alloc::{string::String, vec::Vec};
use core::fmt::{self, Write};
use libui::state::TextField;

pub const DESKTOP: u32 = 0xff141923;
pub const ACTIVE: u32 = 0xff2a3a5e;
pub const INACTIVE: u32 = 0xff1e2533;
pub const BORDER: u32 = 0xff607089;
pub const CLIENT: u32 = 0xff202b3b;
pub const HOVER_CLOSE: u32 = 0xffa63c50;
pub const HOVER_MIN: u32 = 0xff486080;
const TEXT: u32 = 0xffe0e8ff;
const TITLE_HEIGHT: usize = 24;

/// Opaque windows stacked above whatever is being repaired, clipped to one
/// damage rect. Repair skips pixels a later pass overwrites unconditionally,
/// so the result is pixel-identical to plain painter's order.
struct Occluders {
    rects: [Rect; 16],
    n: usize,
}

impl Occluders {
    /// Rows the window pass always writes, at full width (bevels included):
    /// below the title bar and above the rounded bottom edge. Title rows and
    /// the bottom 8 rows can be transparent (corner mask) and stay excluded.
    fn core(w: &Window) -> Option<Rect> {
        if w.is_minimized {
            return None;
        }
        let h = w.rect.h.checked_sub(TITLE_HEIGHT + 1 + 8)?;
        (h > 0).then_some(Rect {
            x: w.rect.x,
            y: w.rect.y + TITLE_HEIGHT + 1,
            w: w.rect.w,
            h,
        })
    }

    /// Cores of `windows` intersecting `r`. Past 16 the rest are ignored,
    /// which only means painting pixels that get overwritten anyway.
    fn collect(windows: &[Window], r: Rect) -> Self {
        let mut o = Self {
            rects: [Rect {
                x: 0,
                y: 0,
                w: 0,
                h: 0,
            }; 16],
            n: 0,
        };
        for w in windows {
            if o.n == o.rects.len() {
                break;
            }
            if let Some(c) = Self::core(w).and_then(|c| c.intersection(r)) {
                o.rects[o.n] = c;
                o.n += 1;
            }
        }
        o
    }

    /// Calls `f(start, end)` for each uncovered span of row `y` in `x0..x1`.
    fn spans(&self, y: usize, x0: usize, x1: usize, mut f: impl FnMut(usize, usize)) {
        let mut cover = [(0usize, 0usize); 16];
        let mut n = 0;
        for c in &self.rects[..self.n] {
            if y >= c.y && y < c.y + c.h {
                // Insertion sort by start: n <= 16.
                let item = (c.x, c.x + c.w);
                let mut i = n;
                while i > 0 && cover[i - 1].0 > item.0 {
                    cover[i] = cover[i - 1];
                    i -= 1;
                }
                cover[i] = item;
                n += 1;
            }
        }
        let mut x = x0;
        for &(a, b) in &cover[..n] {
            if a > x {
                f(x, a.min(x1));
            }
            x = x.max(b);
            if x >= x1 {
                return;
            }
        }
        if x < x1 {
            f(x, x1);
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Hit {
    Client,
    Title,
    Close,
    Minimize,
    Maximize,
    Border,
    Resize,
}

pub struct Window {
    pub id: u32,
    pub app_id: Option<AppId>,
    pub title: String,
    /// Outer bounds. Content starts at (1,25), excluding the titlebar and border.
    pub rect: Rect,
    pub content: Vec<u32>,
    /// Built-in apps must reset their canvas after a geometry change, even
    /// when several resizes return to the previous size before they repaint.
    pub needs_app_repaint: bool,
    pub is_focused: bool,
    pub is_dragging: bool,
    pub is_resizing: bool,
    pub drag_offset: (usize, usize),
    pub is_minimized: bool,
    pub bg_color: u32,
    pub saved_rect: Option<Rect>,
    panel_ready: bool,
    /// Rendered title bar rows (0..=TITLE_HEIGHT, full width, 0 = transparent).
    title_cache: core::cell::RefCell<TitleCache>,
}

/// Everything the title bar rows depend on. Content never reaches them.
#[derive(Clone, Copy, PartialEq, Eq)]
struct TitleKey {
    w: usize,
    h: usize,
    focused: bool,
    maximized: bool,
    hover: Option<Hit>,
    title_hash: u64,
}

#[derive(Default)]
struct TitleCache {
    key: Option<TitleKey>,
    pixels: Vec<u32>,
}

fn fnv1a(text: &str) -> u64 {
    text.bytes().fold(0xcbf29ce484222325, |h, b| {
        (h ^ b as u64).wrapping_mul(0x100000001b3)
    })
}

impl Window {
    #[inline(never)]
    pub fn new(id: u32, title: &str, rect: Rect) -> Option<Self> {
        if rect.w < 96 || rect.h < TITLE_HEIGHT + 3 {
            return None;
        }
        let bg_color = match id {
            3 => 0xff0d1117,
            4 => 0xff161b22,
            5 => 0xff0d1117,
            _ => CLIENT,
        };
        let mut content = Vec::new();
        let len = (rect.w - 2).checked_mul(rect.h - TITLE_HEIGHT - 2)?;
        content.try_reserve_exact(len).ok()?;
        content.resize(len, bg_color);
        let mut name = String::new();
        name.try_reserve_exact(title.len()).ok()?;
        name.push_str(title);
        let app_id = match id {
            3 => Some(AppId::Terminal),
            4 => Some(AppId::Notepad),
            5 => Some(AppId::Files),
            1 | 6 => Some(AppId::SysInfo),
            7 => Some(AppId::Settings),
            8 => Some(AppId::Network),
            9 => Some(AppId::DateTime),
            10 => Some(AppId::Power),
            _ => None,
        };
        Some(Self {
            id,
            app_id,
            title: name,
            rect,
            content,
            needs_app_repaint: true,
            is_focused: false,
            is_dragging: false,
            is_resizing: false,
            drag_offset: (0, 0),
            is_minimized: false,
            bg_color,
            saved_rect: None,
            panel_ready: false,
            title_cache: core::cell::RefCell::new(TitleCache::default()),
        })
    }

    #[allow(dead_code)]
    pub fn with_app_id(mut self, app_id: AppId) -> Self {
        self.app_id = Some(app_id);
        self
    }

    /// Runs `f` with the title bar rows, re-rendering them through `pixel()`
    /// only when focus, hover, size, maximize state or title changed. A drag
    /// changes none of these, so it never re-rasterizes the title text.
    /// None if the cache cannot be allocated (callers fall back to `pixel()`).
    fn with_title_rows<R>(&self, hover: Option<Hit>, f: impl FnOnce(&[u32]) -> R) -> Option<R> {
        let key = TitleKey {
            w: self.rect.w,
            h: self.rect.h,
            focused: self.is_focused,
            maximized: self.saved_rect.is_some(),
            hover,
            title_hash: fnv1a(&self.title),
        };
        let mut cache = self.title_cache.try_borrow_mut().ok()?;
        if cache.key != Some(key) {
            let len = self.rect.w.checked_mul(TITLE_HEIGHT + 1)?;
            cache.key = None;
            cache.pixels.clear();
            cache.pixels.try_reserve_exact(len).ok()?;
            for y in 0..=TITLE_HEIGHT.min(self.rect.h - 1) {
                for x in 0..self.rect.w {
                    let p = self.pixel(x, y, hover);
                    cache.pixels.push(p);
                }
            }
            cache.pixels.resize(len, 0);
            cache.key = Some(key);
        }
        Some(f(&cache.pixels))
    }

    pub fn damage_rect(&self) -> Rect {
        if self.id >= 3 {
            Rect {
                x: self.rect.x.saturating_sub(8),
                y: self.rect.y.saturating_sub(5),
                w: self.rect.w + 16,
                h: self.rect.h + 16,
            }
        } else {
            self.rect
        }
    }

    pub fn resize(&mut self, new_w: usize, new_h: usize) -> bool {
        if new_w < 120 || new_h < TITLE_HEIGHT + 20 {
            return false;
        }
        let new_content_w = new_w - 2;
        let new_content_h = new_h - TITLE_HEIGHT - 2;
        let len = match new_content_w.checked_mul(new_content_h) {
            Some(l) => l,
            None => return false,
        };
        if self.content.capacity() < len {
            let reserve_target = len.saturating_add(len / 4);
            if self
                .content
                .try_reserve(reserve_target.saturating_sub(self.content.len()))
                .is_err()
            {
                if self
                    .content
                    .try_reserve_exact(len.saturating_sub(self.content.len()))
                    .is_err()
                {
                    return false;
                }
            }
        }
        // Keep the last client frame at its original pixel coordinates until
        // EV_CONFIGURE is consumed and the client supplies a new frame. A
        // plain Vec::resize would reinterpret packed rows with a new stride,
        // visibly distorting text during every asynchronous resize.
        let old_content_w = self.rect.w - 2;
        let old_content_h = self.content.len() / old_content_w;
        let kept_w = old_content_w.min(new_content_w);
        let kept_h = old_content_h.min(new_content_h);
        if new_content_w > old_content_w {
            // Widen from the bottom so a destination never destroys a source
            // row that has not moved yet. Resizing first also covers expansion.
            self.content.resize(len, self.bg_color);
            for row in (0..kept_h).rev() {
                let src = row * old_content_w;
                let dst = row * new_content_w;
                self.content.copy_within(src..src + kept_w, dst);
                self.content[dst + kept_w..dst + new_content_w].fill(self.bg_color);
            }
        } else {
            // Narrow while the old source rows still exist; truncate only
            // after compaction, including mixed width/height changes.
            for row in 0..kept_h {
                let src = row * old_content_w;
                self.content.copy_within(src..src + kept_w, row * new_content_w);
            }
            self.content.resize(len, self.bg_color);
        }
        self.content[kept_h * new_content_w..].fill(self.bg_color);
        self.rect.w = new_w;
        self.rect.h = new_h;
        self.needs_app_repaint = true;
        self.panel_ready = false;
        true
    }

    pub fn hit(&self, x: usize, y: usize) -> Option<Hit> {
        if self.is_minimized || !self.rect.contains(x, y) {
            return None;
        }
        let (x, y) = (x - self.rect.x, y - self.rect.y);
        if self.id >= 3 {
            let is_corner = (x < 8 && y < 8 && (7 - x as i32).pow(2) + (7 - y as i32).pow(2) > 49)
                || (x >= self.rect.w - 8
                    && y < 8
                    && (x as i32 - (self.rect.w as i32 - 8)).pow(2) + (7 - y as i32).pow(2) > 49)
                || (x < 8
                    && y >= self.rect.h - 8
                    && (7 - x as i32).pow(2) + (y as i32 - (self.rect.h as i32 - 8)).pow(2) > 49)
                || (x >= self.rect.w - 8
                    && y >= self.rect.h - 8
                    && (x as i32 - (self.rect.w as i32 - 8)).pow(2)
                        + (y as i32 - (self.rect.h as i32 - 8)).pow(2)
                        > 49);
            if is_corner {
                return None;
            }
        }
        Some(
            if x >= self.rect.w.saturating_sub(12) && y >= self.rect.h.saturating_sub(12) {
                Hit::Resize
            } else if x == 0 || y == 0 || x == self.rect.w - 1 || y == self.rect.h - 1 {
                Hit::Border
            } else if y <= TITLE_HEIGHT {
                if self.id <= 2 && x >= self.rect.w - 25 {
                    Hit::Close
                } else if self.id <= 2 && x >= self.rect.w - 49 {
                    Hit::Minimize
                } else if self.id >= 3 && x >= self.rect.w.saturating_sub(36) {
                    Hit::Close
                } else if self.id >= 3 && x >= self.rect.w.saturating_sub(72) {
                    Hit::Maximize
                } else if self.id >= 3 && x >= self.rect.w.saturating_sub(108) {
                    Hit::Minimize
                } else {
                    Hit::Title
                }
            } else {
                Hit::Client
            },
        )
    }

    /// Only these pixels depend on hover. Moving between client/title/background
    /// must not repaint an entire client or a full-width titlebar.
    fn hover_damage(&self, hit: Hit) -> Option<Rect> {
        let (x, y, w, h) = match hit {
            Hit::Close if self.id <= 2 => (self.rect.w - 25, 1, 24, TITLE_HEIGHT),
            Hit::Minimize if self.id <= 2 => (self.rect.w - 49, 1, 24, TITLE_HEIGHT),
            Hit::Close => (self.rect.w.saturating_sub(36), 1, 35, TITLE_HEIGHT),
            Hit::Maximize => (self.rect.w.saturating_sub(72), 1, 36, TITLE_HEIGHT),
            Hit::Minimize => (self.rect.w.saturating_sub(108), 1, 36, TITLE_HEIGHT),
            Hit::Resize => (self.rect.w - 12, self.rect.h - 12, 12, 12),
            _ => return None,
        };
        Some(Rect {
            x: self.rect.x + x,
            y: self.rect.y + y,
            w,
            h,
        })
    }

    fn pixel(&self, x: usize, y: usize, hover: Option<Hit>) -> u32 {
        if self.id >= 3 {
            let is_maximized = self.saved_rect.is_some();

            // Interior client area fast path (covers 99% of window pixels)
            if y > TITLE_HEIGHT && (is_maximized || y < self.rect.h.saturating_sub(8)) {
                if x == 0 {
                    return 0xff484848; // 1px left highlight bevel
                }
                if x == self.rect.w - 1 {
                    return 0xff181818; // 1px right dark shadow bevel
                }
                if is_maximized && y == self.rect.h - 1 {
                    return 0xff141414; // 1px bottom dark shadow bevel
                }
                return self.content[(y - 25) * (self.rect.w - 2) + x - 1];
            }

            if !is_maximized {
                // Check 8px rounded corners
                let is_corner = (x < 8
                    && y < 8
                    && (7 - x as i32).pow(2) + (7 - y as i32).pow(2) > 49)
                    || (x >= self.rect.w - 8
                        && y < 8
                        && (x as i32 - (self.rect.w as i32 - 8)).pow(2) + (7 - y as i32).pow(2)
                            > 49)
                    || (x < 8
                        && y >= self.rect.h - 8
                        && (7 - x as i32).pow(2) + (y as i32 - (self.rect.h as i32 - 8)).pow(2)
                            > 49)
                    || (x >= self.rect.w - 8
                        && y >= self.rect.h - 8
                        && (x as i32 - (self.rect.w as i32 - 8)).pow(2)
                            + (y as i32 - (self.rect.h as i32 - 8)).pow(2)
                            > 49);
                if is_corner {
                    return 0; // Transparent mask for rounded corners
                }

                // Sleek macOS dark window border with 3D 1-pixel bevel highlight and shadow
                let is_border = x == 0
                    || y == 0
                    || x == self.rect.w - 1
                    || y == self.rect.h - 1
                    || (x < 8 && y < 8 && (7 - x as i32).pow(2) + (7 - y as i32).pow(2) >= 36)
                    || (x >= self.rect.w - 8
                        && y < 8
                        && (x as i32 - (self.rect.w as i32 - 8)).pow(2) + (7 - y as i32).pow(2)
                            >= 36)
                    || (x < 8
                        && y >= self.rect.h - 8
                        && (7 - x as i32).pow(2) + (y as i32 - (self.rect.h as i32 - 8)).pow(2)
                            >= 36)
                    || (x >= self.rect.w - 8
                        && y >= self.rect.h - 8
                        && (x as i32 - (self.rect.w as i32 - 8)).pow(2)
                            + (y as i32 - (self.rect.h as i32 - 8)).pow(2)
                            >= 36);
                if is_border {
                    if self.is_focused {
                        if y == 0 || y < 8 {
                            return 0xff38bdf8; // 1px bright sky-blue active top rim
                        } else if x == 0 || x == self.rect.w - 1 {
                            return 0xff2563eb; // crisp cobalt active side border
                        } else {
                            return 0xff1d4ed8; // active bottom rim
                        }
                    } else if y == 0 || y < 8 {
                        return 0xff3b404c; // muted inactive top rim
                    } else if x == 0 {
                        return 0xff2b303b;
                    } else if y == self.rect.h - 1 {
                        return 0xff14171d;
                    } else if x == self.rect.w - 1 {
                        return 0xff232730;
                    } else {
                        return 0xff181b22;
                    }
                }

                // Bottom-right resize grip
                let rx = self.rect.w.saturating_sub(x);
                let ry = self.rect.h.saturating_sub(y);
                if rx <= 10 && ry <= 10 && (rx + ry == 6 || rx + ry == 9 || rx + ry == 12) {
                    return if hover == Some(Hit::Resize) {
                        theme::ACCENT
                    } else if self.is_focused {
                        0xff38bdf8
                    } else {
                        0xff484848
                    };
                }
            } else {
                if self.is_focused {
                    if y == 0 {
                        return 0xff38bdf8;
                    } else if x == 0 || x == self.rect.w - 1 {
                        return 0xff2563eb;
                    } else if y == self.rect.h - 1 {
                        return 0xff1d4ed8;
                    }
                } else if y == 0 {
                    return 0xff3b404c;
                } else if x == 0 {
                    return 0xff2b303b;
                } else if y == self.rect.h - 1 {
                    return 0xff14171d;
                } else if x == self.rect.w - 1 {
                    return 0xff232730;
                }
            }

            if y > TITLE_HEIGHT {
                return self.content[(y - 25) * (self.rect.w - 2) + x - 1];
            }

            let title_bg = if self.is_focused {
                0xff222b3a // rich sapphire-tinted active titlebar
            } else {
                0xff1c1f26 // muted inactive titlebar
            };

            // Modern top-right traffic-light window controls (Close, Maximize/Restore, Minimize)
            if x >= self.rect.w.saturating_sub(108) {
                let is_hover = |btn: Hit| hover == Some(btn);
                let base_bg = if y == 1 { 0xff404040 } else { title_bg };
                let (btn, bx, cx, active_fill, hover_fill, rim_col, glyph_col) =
                    if x >= self.rect.w.saturating_sub(36) {
                        (
                            Hit::Close,
                            x - self.rect.w.saturating_sub(36),
                            17usize,
                            0xffff5f57u32,
                            0xffff7b73u32,
                            0xffe0443eu32,
                            0xff4a0f0cu32,
                        )
                    } else if x >= self.rect.w.saturating_sub(72) {
                        (
                            Hit::Maximize,
                            x - self.rect.w.saturating_sub(72),
                            18usize,
                            0xff28c840u32,
                            0xff44dc5cu32,
                            0xff1ca632u32,
                            0xff0b3d14u32,
                        )
                    } else {
                        (
                            Hit::Minimize,
                            x - self.rect.w.saturating_sub(108),
                            18usize,
                            0xfffebc2eu32,
                            0xffffce54u32,
                            0xffd69816u32,
                            0xff4a3000u32,
                        )
                    };
                let hovered = is_hover(btn);
                let left_x = cx - 7;
                if (left_x..left_x + 14).contains(&bx) && (5..19).contains(&y) {
                    let lx = bx - left_x;
                    let ly = y - 5;
                    let outer_cov = theme::rounded_coverage(lx, ly, 14, 14, 7);
                    if outer_cov > 0 {
                        let inner_cov = if (1..13).contains(&lx) && (1..13).contains(&ly) {
                            theme::rounded_coverage(lx - 1, ly - 1, 12, 12, 6)
                        } else {
                            0
                        };
                        let (fill, rim) = if hovered {
                            (hover_fill, rim_col)
                        } else if self.is_focused {
                            (active_fill, rim_col)
                        } else {
                            (0xff424652, 0xff525866)
                        };
                        let dot_col = theme::blend_coverage(
                            theme::blend_coverage(fill, rim, inner_cov),
                            base_bg,
                            outer_cov,
                        );
                        if hovered {
                            let dx = (bx as i32 - cx as i32).abs();
                            let dy = (y as i32 - 12).abs();
                            let on_glyph = match btn {
                                Hit::Close => dx == dy && dx <= 3,
                                Hit::Minimize => dy == 0 && dx <= 3,
                                _ => {
                                    if is_maximized {
                                        (dx <= 2 && dy == 2) || (dy <= 2 && dx == 2)
                                    } else {
                                        (dx == 0 && dy <= 3) || (dy == 0 && dx <= 3)
                                    }
                                }
                            };
                            if on_glyph {
                                return glyph_col;
                            }
                        }
                        return dot_col;
                    }
                }
                return base_bg;
            }

            // Titlebar top 1px subtle glass highlight for non-button title area
            if y == 1 {
                return if self.is_focused { 0xff33435c } else { 0xff282c36 };
            }

            // Left-aligned clean title with Windows styling
            let title_len = theme::text_width(&self.title);
            let title_max_x = self.rect.w.saturating_sub(116);
            if x >= 12 && x < (12 + title_len).min(title_max_x) && (3..21).contains(&y) {
                let title_fg = if self.is_focused { 0xffffffff } else { 0xff8b949e };
                return theme::text_pixel(&self.title, x - 12, y - 3, title_fg, title_bg);
            }

            return title_bg;
        }

        // Window id <= 2 (matching test_compositor.py baseline)
        if x == 0 || y == 0 || x == self.rect.w - 1 || y == self.rect.h - 1 {
            return BORDER;
        }
        let rx = self.rect.w.saturating_sub(x);
        let ry = self.rect.h.saturating_sub(y);
        if rx <= 10 && ry <= 10 && (rx + ry == 6 || rx + ry == 9 || rx + ry == 12) {
            return if hover == Some(Hit::Resize) {
                HOVER_MIN
            } else {
                BORDER
            };
        }
        if y > TITLE_HEIGHT {
            return self.content[(y - 25) * (self.rect.w - 2) + x - 1];
        }
        let title_bg = if self.is_focused { ACTIVE } else { INACTIVE };

        if x >= self.rect.w - 25 {
            let lx = x - (self.rect.w - 25);
            let bg = if hover == Some(Hit::Close) {
                HOVER_CLOSE
            } else {
                title_bg
            };
            return if (5..21).contains(&y) {
                label_pixel("[X]", lx, y - 5, TEXT, bg)
            } else {
                bg
            };
        } else if x >= self.rect.w - 49 {
            let lx = x - (self.rect.w - 49);
            let bg = if hover == Some(Hit::Minimize) {
                HOVER_MIN
            } else {
                title_bg
            };
            return if (5..21).contains(&y) {
                label_pixel("[-]", lx, y - 5, TEXT, bg)
            } else {
                bg
            };
        }

        if x >= 8 && (5..21).contains(&y) {
            return label_pixel(self.title.as_str(), x - 8, y - 5, TEXT, title_bg);
        }

        title_bg
    }
}

#[inline]
pub fn blend_darken(color: u32, factor: u32) -> u32 {
    let r = (((color >> 16) & 0xff) * (256 - factor)) >> 8;
    let g = (((color >> 8) & 0xff) * (256 - factor)) >> 8;
    let b = ((color & 0xff) * (256 - factor)) >> 8;
    0xff000000 | (r << 16) | (g << 8) | b
}

#[inline]
pub fn blend_color(fg: u32, bg: u32, alpha: u32) -> u32 {
    let r = (((fg >> 16) & 0xff) * alpha + ((bg >> 16) & 0xff) * (256 - alpha)) >> 8;
    let g = (((fg >> 8) & 0xff) * alpha + ((bg >> 8) & 0xff) * (256 - alpha)) >> 8;
    let b = ((fg & 0xff) * alpha + (bg & 0xff) * (256 - alpha)) >> 8;
    0xff000000 | (r << 16) | (g << 8) | b
}

fn panel_box(
    w: &mut Window,
    x: usize,
    y: usize,
    bw: usize,
    bh: usize,
    radius: usize,
    outer_bg: u32,
    fill: u32,
    border: u32,
    scale: usize,
) {
    let width = w.rect.w - 2;
    let height = w.rect.h - TITLE_HEIGHT - 2;
    let left = x * scale;
    let top = y * scale;
    let pw = (bw * scale).min(width.saturating_sub(left));
    let ph = (bh * scale).min(height.saturating_sub(top));
    for py in 0..ph {
        for px in 0..pw {
            let lx = px / scale;
            let ly = py / scale;
            let outer_cov = theme::rounded_coverage(lx, ly, bw, bh, radius);
            let pixel = if outer_cov == 0 {
                outer_bg
            } else {
                let inner_cov = if lx >= 1 && lx + 1 < bw && ly >= 1 && ly + 1 < bh {
                    theme::rounded_coverage(
                        lx - 1,
                        ly - 1,
                        bw.saturating_sub(2),
                        bh.saturating_sub(2),
                        radius.saturating_sub(1),
                    )
                } else {
                    0
                };
                let edge_col = if ly == 0 { 0xff4c566a } else { border };
                let inner = theme::blend_coverage(fill, edge_col, inner_cov);
                theme::blend_coverage(inner, outer_bg, outer_cov)
            };
            w.content[(top + py) * width + left + px] = pixel;
        }
    }
}

fn panel_text(
    w: &mut Window,
    s: &mut Surface,
    x: usize,
    y: usize,
    field_width: usize,
    text: &str,
    color: u32,
    bg: u32,
    bold: bool,
    scale: usize,
) {
    let width = w.rect.w - 2;
    let height = w.rect.h - TITLE_HEIGHT - 2;
    let left = x * scale;
    let top = y * scale;
    let text_end = left + theme::text_width(text) * scale;
    let (mut min_x, mut min_y, mut max_x, mut max_y) = (width, height, 0, 0);
    for py in top..(top + 18 * scale).min(height) {
        for px in left..(left + field_width * scale).min(width) {
            let pixel = if px < text_end {
                if bold {
                    theme::text_pixel_bold(
                        text,
                        (px - left) / scale,
                        (py - top) / scale,
                        color,
                        bg,
                    )
                } else {
                    theme::text_pixel(
                        text,
                        (px - left) / scale,
                        (py - top) / scale,
                        color,
                        bg,
                    )
                }
            } else {
                bg
            };
            let old = &mut w.content[py * width + px];
            if *old != pixel {
                *old = pixel;
                min_x = min_x.min(px);
                min_y = min_y.min(py);
                max_x = max_x.max(px + 1);
                max_y = max_y.max(py + 1);
            }
        }
    }
    if min_x < max_x && w.panel_ready {
        s.damage(Rect {
            x: w.rect.x + 1 + min_x,
            y: w.rect.y + 25 + min_y,
            w: max_x - min_x,
            h: max_y - min_y,
        });
    }
}

fn label_pixel(label: &str, x: usize, y: usize, fg: u32, bg: u32) -> u32 {
    let Some(c) = label.chars().nth(x / 8) else {
        return bg;
    };
    let c = if (' '..='~').contains(&c) {
        c as usize
    } else {
        b'?' as usize
    };
    if GLYPHS[c - 32][y] & (128 >> (x % 8)) != 0 {
        fg
    } else {
        bg
    }
}

fn fortress_tower_pixel(tx: usize, ty: usize) -> bool {
    if tx > 8 || ty > 11 {
        return false;
    }
    match ty {
        0..=1 => tx <= 1 || (3..=5).contains(&tx) || tx >= 7,
        2..=3 => true,
        4..=5 => (1..=7).contains(&tx),
        6..=7 => (1..=7).contains(&tx) && tx != 4,
        8 => (1..=7).contains(&tx) && !(3..=5).contains(&tx),
        9..=10 => (1..=7).contains(&tx) && !(2..=6).contains(&tx),
        11 => true,
        _ => false,
    }
}

fn wifi_tray_pixel(x: usize, y: usize) -> bool {
    match y {
        0 => (3..=8).contains(&x),
        1 => (1..=2).contains(&x) || (9..=10).contains(&x),
        2 => x == 0 || x == 11,
        3 => (4..=7).contains(&x),
        4 => (2..=3).contains(&x) || (8..=9).contains(&x),
        6 => (5..=6).contains(&x),
        7 => (4..=7).contains(&x),
        8 => (5..=6).contains(&x),
        _ => false,
    }
}

fn volume_tray_pixel(x: usize, y: usize) -> bool {
    if x <= 2 {
        (x == 2 && y <= 8) || (x == 1 && (1..=7).contains(&y)) || (x == 0 && (2..=6).contains(&y))
    } else if x == 3 && (2..=6).contains(&y) {
        true
    } else if (y == 2 && x == 5) || ((3..=5).contains(&y) && x == 6) || (y == 6 && x == 5) {
        true
    } else if (y == 1 && x == 8) || (y == 2 && x == 9) || ((3..=5).contains(&y) && x == 10) || (y == 6 && x == 9) || (y == 7 && x == 8) {
        true
    } else {
        false
    }
}

#[allow(dead_code)]
fn battery_tray_pixel(x: usize, y: usize) -> (bool, bool) {
    let is_terminal = (13..=14).contains(&x) && (2..=5).contains(&y);
    let is_border = ((x == 0 || x == 12) && y <= 7) || ((y == 0 || y == 7) && x <= 12);
    if is_terminal || is_border {
        (true, false)
    } else if (2..=9).contains(&x) && (2..=5).contains(&y) {
        (true, true)
    } else {
        (false, false)
    }
}

fn power_tray_pixel(x: usize, y: usize) -> bool {
    if x == 4 && y <= 4 {
        true
    } else if y == 2 && (x == 1 || x == 7) {
        true
    } else if (3..=5).contains(&y) && (x == 0 || x == 8) {
        true
    } else if y == 6 && (x == 1 || x == 7) {
        true
    } else if y == 7 && (2..=6).contains(&x) {
        true
    } else {
        false
    }
}

fn civil_from_days(days: i64) -> (i32, u32, u32) {
    let z = days + 719468;
    let era = if z >= 0 { z } else { z - 146096 } / 146097;
    let doe = (z - era * 146097) as u32;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let y = (yoe as i64 + era * 400) as i32;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = if m <= 2 { y + 1 } else { y };
    (y, m, d)
}

fn format_clock(sec: i64, is_24h: bool, buf: &mut [u8; 32]) -> &str {
    if sec < 0 {
        return "--:--";
    }
    let seconds_in_day = sec.rem_euclid(86400) as u32;
    let days = sec.div_euclid(86400);
    let (_y, m, d) = civil_from_days(days);
    let hour = seconds_in_day / 3600;
    let minute = (seconds_in_day / 60) % 60;

    let display_hour = if is_24h {
        hour
    } else {
        let h = hour % 12;
        if h == 0 { 12 } else { h }
    };

    const MONTHS: [&[u8]; 12] = [
        b"Jan", b"Feb", b"Mar", b"Apr", b"May", b"Jun",
        b"Jul", b"Aug", b"Sep", b"Oct", b"Nov", b"Dec",
    ];
    let month_idx = (m.saturating_sub(1) as usize).min(11);
    let month_str = MONTHS[month_idx];

    let mut len = 0;
    if d >= 10 {
        buf[len] = b'0' + (d / 10) as u8;
        len += 1;
    }
    buf[len] = b'0' + (d % 10) as u8;
    len += 1;

    buf[len] = b' ';
    len += 1;

    buf[len..len + 3].copy_from_slice(month_str);
    len += 3;

    buf[len] = b' ';
    len += 1;

    buf[len] = b'0' + (display_hour / 10) as u8;
    buf[len + 1] = b'0' + (display_hour % 10) as u8;
    buf[len + 2] = b':';
    buf[len + 3] = b'0' + (minute / 10) as u8;
    buf[len + 4] = b'0' + (minute % 10) as u8;
    len += 5;

    if !is_24h {
        buf[len] = b' ';
        buf[len + 1] = if hour < 12 { b'A' } else { b'P' };
        buf[len + 2] = b'M';
        len += 3;
    }

    core::str::from_utf8(&buf[..len]).unwrap_or("--:--")
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Panel {
    pub rect: Rect,
}

impl Panel {
    pub fn new(width: usize, height: usize) -> Self {
        Self {
            rect: WindowManager::top_bar_rect(width, height),
        }
    }

    pub fn render(&self, wm: &WindowManager, s: &mut Surface, r: Rect) {
        let Some(clip) = self.rect.intersection(r) else {
            return;
        };
        let scale = theme::scale(s.height);
        let logical_width = s.width / scale;

        let mut clock_buf = [0u8; 32];
        let clock_str = format_clock(wm.clock_sec, wm.clock_24h, &mut clock_buf);
        let clock_tw = theme::text_width(clock_str);
        let clock_x = logical_width.saturating_sub(clock_tw) / 2;
        let clock_pill_w = clock_tw + 20;
        let clock_pill_x = logical_width.saturating_sub(clock_pill_w) / 2;

        let has_bat = wm.battery_status.is_some();
        let tray_w = if has_bat { 114 } else { 86 };
        let tray_x = logical_width.saturating_sub(tray_w + 8);

        for y in clip.y..clip.y + clip.h {
            for x in clip.x..clip.x + clip.w {
                let lx = x / scale;
                let ly = y / scale;

                let under = s.pixels[y * s.width + x];
                let bg = if ly == 27 {
                    blend_color(0x384152, under, 160)
                } else {
                    blend_color(0x101318, under, 204)
                };

                // Activities Pill (Left)
                let act_cov = if (8..102).contains(&lx) && (3..25).contains(&ly) {
                    theme::rounded_coverage(lx - 8, ly - 3, 94, 22, 10)
                } else {
                    0
                };
                let act_bg = if act_cov > 0 {
                    if wm.start_open {
                        blend_color(0x2d3748, bg, (act_cov * 255) / 16)
                    } else {
                        blend_color(0xffffff, bg, (act_cov * 22) / 16)
                    }
                } else {
                    bg
                };

                // Date & Time Pill (Center)
                let clk_cov = if (clock_pill_x..clock_pill_x + clock_pill_w).contains(&lx) && (3..25).contains(&ly) {
                    theme::rounded_coverage(lx - clock_pill_x, ly - 3, clock_pill_w, 22, 10)
                } else {
                    0
                };
                let clk_bg = if clk_cov > 0 {
                    blend_color(0xffffff, bg, (clk_cov * 20) / 16)
                } else {
                    bg
                };

                // System Tray Pill (Right)
                let tray_cov = if (tray_x..tray_x + tray_w).contains(&lx) && (3..25).contains(&ly) {
                    theme::rounded_coverage(lx - tray_x, ly - 3, tray_w, 22, 10)
                } else {
                    0
                };
                let tray_bg = if tray_cov > 0 {
                    blend_color(0xffffff, bg, (tray_cov * 22) / 16)
                } else {
                    bg
                };

                let p = if (15..=23).contains(&lx) && (8..=19).contains(&ly) && fortress_tower_pixel(lx - 15, ly - 8) {
                    0xff38bdf8
                } else if (5..23).contains(&ly) && (28..96).contains(&lx) {
                    theme::text_pixel("Activities", lx - 28, ly - 5, 0xfff8fafc, act_bg)
                } else if act_cov > 0 {
                    act_bg
                } else if (5..23).contains(&ly) && (clock_x..clock_x + clock_tw).contains(&lx) {
                    theme::text_pixel(clock_str, lx - clock_x, ly - 5, 0xfff8fafc, clk_bg)
                } else if clk_cov > 0 {
                    clk_bg
                } else if (9..18).contains(&ly) && (tray_x + 12..tray_x + 24).contains(&lx) {
                    if wifi_tray_pixel(lx - (tray_x + 12), ly - 9) {
                        match wm.network_state {
                            2 => 0xff38bdf8,
                            1 => 0xff64748b,
                            _ => 0xff94a3b8,
                        }
                    } else {
                        tray_bg
                    }
                } else if has_bat && (9..19).contains(&ly) && (tray_x + 28..tray_x + 50).contains(&lx) {
                    if let Some(col) = crate::ui::render_battery_pixel(lx - (tray_x + 28), ly - 9, wm.battery_status.as_ref().unwrap(), tray_bg) {
                        col
                    } else {
                        tray_bg
                    }
                } else if has_bat && (9..18).contains(&ly) && (tray_x + 54..tray_x + 65).contains(&lx) {
                    if volume_tray_pixel(lx - (tray_x + 54), ly - 9) {
                        0xffcbd5e1
                    } else {
                        tray_bg
                    }
                } else if has_bat && (9..18).contains(&ly) && (tray_x + 72..tray_x + 81).contains(&lx) {
                    if power_tray_pixel(lx - (tray_x + 72), ly - 9) {
                        0xffe2e8f0
                    } else {
                        tray_bg
                    }
                } else if !has_bat && (9..18).contains(&ly) && (tray_x + 36..tray_x + 47).contains(&lx) {
                    if volume_tray_pixel(lx - (tray_x + 36), ly - 9) {
                        0xffcbd5e1
                    } else {
                        tray_bg
                    }
                } else if !has_bat && (9..18).contains(&ly) && (tray_x + 56..tray_x + 65).contains(&lx) {
                    if power_tray_pixel(lx - (tray_x + 56), ly - 9) {
                        0xffe2e8f0
                    } else {
                        tray_bg
                    }
                } else if tray_cov > 0 {
                    tray_bg
                } else {
                    bg
                };

                s.pixels[y * s.width + x] = p;
            }
        }
    }
}

fn render_dock_icon(item: usize, ix: usize, iy: usize, dock_bg: u32) -> u32 {
    let is_corner = (ix < 5 && iy < 5 && (4 - ix as i32).pow(2) + (4 - iy as i32).pow(2) > 25)
        || (ix >= 23 && iy < 5 && (ix as i32 - 23).pow(2) + (4 - iy as i32).pow(2) > 25)
        || (ix < 5 && iy >= 23 && (4 - ix as i32).pow(2) + (iy as i32 - 23).pow(2) > 25)
        || (ix >= 23 && iy >= 23 && (ix as i32 - 23).pow(2) + (iy as i32 - 23).pow(2) > 25);
    if is_corner {
        return dock_bg;
    }

    match item {
        0 => {
            // Windows 11 File Explorer: Golden folder with cyan document peeking out
            if iy == 0 {
                return 0xffd29922;
            }
            // Tab at top-left
            if (3..=5).contains(&iy) && (3..=9).contains(&ix) {
                return 0xffd29922;
            }
            // Document inside
            if (6..=11).contains(&iy) && (6..=21).contains(&ix) {
                return 0xff38bdf8;
            }
            // Back folder
            if (5..=11).contains(&iy) && (3..=24).contains(&ix) {
                return 0xffe3b341;
            }
            // Front folder flap with depth
            if iy == 12 && (2..=25).contains(&ix) {
                return 0xfffcd34d; // top highlight of front flap
            }
            if (13..=23).contains(&iy) && (2..=25).contains(&ix) {
                return 0xfff59e0b;
            }
            0xffd29922
        }
        1 => {
            // Windows 11 Windows Terminal: Sleek dark graphite tile with green prompt
            if iy == 0 {
                return 0xff30363d;
            }
            // Top tab strip
            if (2..=5).contains(&iy) && (2..=25).contains(&ix) {
                if (4..=14).contains(&ix) {
                    return 0xff21262d;
                }
                return 0xff161b22;
            }
            // Prompt > in bright console green
            if (ix == 6 && (iy == 10 || iy == 14))
                || (ix == 7 && (iy == 11 || iy == 13))
                || (ix == 8 && iy == 12)
            {
                return 0xff4ade80;
            }
            // Cursor _ in cyan
            if (11..=16).contains(&ix) && (iy == 14 || iy == 15) {
                return 0xff58a6ff;
            }
            0xff0d1117
        }
        2 => {
            // Windows 11 Notepad: Fluent blue rounded tile with white ruled document
            if iy == 0 {
                return 0xff1f6feb;
            }
            // White document sheet
            if (5..=22).contains(&iy) && (5..=22).contains(&ix) {
                // Top-right dog-ear fold
                if ix >= 18 && iy <= 9 && ix + iy >= 27 {
                    return 0xff93c5fd;
                }
                // Ruled blue lines
                if (iy == 9 || iy == 12 || iy == 15 || iy == 18) && (8..=18).contains(&ix) {
                    return 0xff1f6feb;
                }
                return 0xffffffff;
            }
            0xff0969da
        }
        3 => {
            // Windows 11 Sysmon / Task Manager: Obsidian tile with glowing cyan pulse
            if iy == 0 {
                return 0xff30363d;
            }
            // Grid lines
            let is_grid = iy == 8 || iy == 16 || ix == 8 || ix == 16;
            let is_pulse = (ix >= 3 && ix <= 7 && iy == 14)
                || (ix == 8 && (11..=14).contains(&iy))
                || (ix == 9 && (8..=12).contains(&iy))
                || (ix == 10 && (8..=18).contains(&iy))
                || (ix == 11 && (15..=21).contains(&iy))
                || (ix == 12 && (13..=18).contains(&iy))
                || (ix >= 13 && ix <= 24 && iy == 14);
            if is_pulse {
                return if (ix == 9 && iy == 8) || (ix == 11 && iy == 21) {
                    0xffa5f3fc // peak highlight
                } else {
                    0xff38bdf8
                };
            }
            if is_grid {
                return 0xff1e293b;
            }
            0xff0d1117
        }
        4 => {
            // Settings: Slate blue tile with precision gear icon
            if iy == 0 {
                return 0xff3b82f6;
            }
            let dx = (ix as i32 - 14).abs();
            let dy = (iy as i32 - 14).abs();
            let dist_sq = dx * dx + dy * dy;
            if dist_sq <= 9 {
                return 0xff172030;
            }
            if dist_sq <= 42
                || (dx <= 2 && dy <= 9)
                || (dy <= 2 && dx <= 9)
                || ((dx - dy).abs() <= 2 && dx <= 7 && dy <= 7)
            {
                return if iy < 14 { 0xff93c5fd } else { 0xff60a5fa };
            }
            0xff172030
        }
        _ => dock_bg,
    }
}

fn render_dock_icon_scaled(item: usize, dx: usize, dy: usize, size: usize, dock_bg: u32) -> u32 {
    if size == 28 {
        return render_dock_icon(item, dx, dy, dock_bg);
    }
    let cov = theme::rounded_coverage(dx, dy, size, size, (size / 5).max(5));
    if cov == 0 {
        return dock_bg;
    }
    let ix = (dx * 28 / size.max(1)).clamp(2, 25);
    let iy = (dy * 28 / size.max(1)).clamp(2, 25);
    let color = render_dock_icon(item, ix, iy, dock_bg);
    theme::blend_coverage(color, dock_bg, cov)
}

pub fn window_icon_kind(w: &Window) -> usize {
    if let Some(app) = w.app_id {
        return match app {
            AppId::Files => 0,
            AppId::Terminal => 1,
            AppId::Notepad => 2,
            AppId::Calc => 3,
            AppId::Vault => 4,
            AppId::Settings | AppId::Network | AppId::DateTime | AppId::Power => 4,
            AppId::SysInfo => 3,
        };
    }
    if w.id == 5 || w.title.starts_with("Files") {
        0
    } else if w.id == 3 || w.title.starts_with("Terminal") {
        1
    } else if w.id == 4 || w.title.contains("Notes") || w.title.starts_with("Notepad") {
        2
    } else if w.id == 1 || w.id == 6 || w.title.starts_with("System") {
        3
    } else {
        4
    }
}

// Write line on content for monitor
fn content_text(w: &mut Window, x: usize, y: usize, text: &str) {
    let width = w.rect.w - 2;
    let height = w.rect.h - TITLE_HEIGHT - 2;
    for (i, c) in text.chars().enumerate() {
        let c = if (' '..='~').contains(&c) {
            c as usize
        } else {
            b'?' as usize
        };
        for row in 0..16.min(height.saturating_sub(y)) {
            for col in 0..8 {
                let px = x + i * 8 + col;
                if px < width {
                    w.content[(y + row) * width + px] = if GLYPHS[c - 32][row] & (128 >> col) != 0 {
                        TEXT
                    } else {
                        CLIENT
                    };
                }
            }
        }
    }
}

// Formatting telemetry never allocates on the mouse path.
struct Line {
    bytes: [u8; 96],
    len: usize,
}
impl Line {
    fn new() -> Self {
        Self {
            bytes: [0; 96],
            len: 0,
        }
    }
    fn as_str(&self) -> &str {
        core::str::from_utf8(&self.bytes[..self.len]).unwrap_or("")
    }
}
impl Write for Line {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        if s.len() > self.bytes.len() - self.len {
            return Err(fmt::Error);
        }
        self.bytes[self.len..self.len + s.len()].copy_from_slice(s.as_bytes());
        self.len += s.len();
        Ok(())
    }
}

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum Action {
    None,
    SpawnTerminal,
    SpawnNotepad,
    SpawnFileManager,
    #[allow(dead_code)]
    SpawnCalc,
    #[allow(dead_code)]
    SpawnVault,
    OpenDesktopApp(u32),
    WindowResized(u32),
    ClientClick(u32, usize, usize),
    ClientDrag(u32, usize, usize),
    Exit,
    Restart,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LauncherItem {
    App(&'static app_registry::AppManifest),
    Exit,
    Restart,
    Lock,
}

pub struct WindowManager {
    pub windows: Vec<Window>,
    pub demo_mode: bool,
    buttons: u8,
    hover: Option<(u32, Hit)>,
    pub clock_sec: i64,
    pub clock_24h: bool,
    network_state: u8,
    pub start_open: bool,
    pub menu_hover: Option<usize>,
    pub launcher_search: TextField<32>,
    pub launcher_selected: usize,
    pub launcher_focused: bool,
    pub pending_action: Option<Action>,
    #[allow(dead_code)]
    pub dock_hover: Option<usize>,
    pub context_menu: Option<(usize, usize)>,
    pub context_hover: Option<usize>,
    pub switcher_open: bool,
    pub switcher_selected: u32,
    pub battery_status: Option<crate::sys::BatteryStatus>,
    pub show_power_menu: bool,
    pub power_profile: u8,
    pub locked: bool,
    pub lock_password: TextField<64>,
    pub lock_error: bool,
    pub modifiers: u16,
}

impl WindowManager {
    #[inline(never)]
    pub fn demo() -> Option<Self> {
        let mut windows = Vec::new();
        windows.try_reserve_exact(2).ok()?;
        windows.push(Window::new(
            1,
            "System Monitor",
            Rect {
                x: 80,
                y: 120,
                w: 320,
                h: 200,
            },
        )?);
        let mut palette = Window::new(
            2,
            "Palette Canvas",
            Rect {
                x: 260,
                y: 220,
                w: 280,
                h: 180,
            },
        )?;
        let colors = [
            0xffe06c75, 0xffe5c07b, 0xff98c379, 0xff56b6c2, 0xff61afef, 0xffc678dd,
        ];
        for y in 0..palette.rect.h - 26 {
            for x in 0..palette.rect.w - 2 {
                if (16..136).contains(&y) && (16..256).contains(&x) {
                    palette.content[y * (palette.rect.w - 2) + x] = colors[(x - 16) / 40];
                }
            }
        }
        palette.is_focused = true;
        windows.push(palette);

        Some(Self {
            windows,
            demo_mode: true,
            buttons: 0,
            hover: None,
            clock_sec: -1,
            clock_24h: true,
            network_state: 0,
            start_open: false,
            menu_hover: None,
            launcher_search: TextField::new(),
            launcher_selected: 0,
            launcher_focused: false,
            pending_action: None,
            dock_hover: None,
            context_menu: None,
            context_hover: None,
            switcher_open: false,
            switcher_selected: 0,
            battery_status: None,
            show_power_menu: false,
            power_profile: 0,
            locked: false,
            lock_password: TextField::new(),
            lock_error: false,
            modifiers: 0,
        })
    }

    pub fn desktop() -> Self {
        Self {
            windows: Vec::new(),
            demo_mode: false,
            buttons: 0,
            hover: None,
            clock_sec: -1,
            clock_24h: true,
            network_state: 0,
            start_open: false,
            menu_hover: None,
            launcher_search: TextField::new(),
            launcher_selected: 0,
            launcher_focused: false,
            pending_action: None,
            dock_hover: None,
            context_menu: None,
            context_hover: None,
            switcher_open: false,
            switcher_selected: 0,
            battery_status: None,
            show_power_menu: false,
            power_profile: 0,
            locked: false,
            lock_password: TextField::new(),
            lock_error: false,
            modifiers: 0,
        }
    }

    #[allow(dead_code)]
    pub fn new(s: &mut Surface) -> Self {
        let wm = Self::demo().unwrap();
        s.damage(Rect {
            x: 0,
            y: 0,
            w: s.width,
            h: s.height,
        });
        wm
    }

    /// Drops every cached title bar so the next repair re-rasterizes them
    /// (theme or scale changes; host tests use it for a cache-free reference).
    #[allow(dead_code)]
    pub fn clear_render_caches(&self) {
        for w in &self.windows {
            if let Ok(mut cache) = w.title_cache.try_borrow_mut() {
                cache.key = None;
            }
        }
    }

    pub fn hit_test(&self, x: usize, y: usize) -> Option<(u32, Hit)> {
        self.windows
            .iter()
            .rev()
            .find_map(|w| w.hit(x, y).map(|hit| (w.id, hit)))
    }

    pub fn top_bar_rect(width: usize, height: usize) -> Rect {
        Rect {
            x: 0,
            y: 0,
            w: width,
            h: 28 * theme::scale(height),
        }
    }

    pub fn dock_rect(width: usize, height: usize) -> Rect {
        let scale = theme::scale(height);
        let dock_w = 4 * 64 * scale;
        let dock_h = 58 * scale;
        let dock_x = width.saturating_sub(dock_w) / 2;
        let dock_y = height.saturating_sub(66 * scale);
        Rect {
            x: dock_x,
            y: dock_y,
            w: dock_w,
            h: dock_h,
        }
    }

    pub fn start_btn_rect(height: usize) -> Rect {
        let scale = theme::scale(height);
        Rect {
            x: 6 * scale,
            y: 0,
            w: 104 * scale,
            h: 28 * scale,
        }
    }

    pub fn start_menu_rect(height: usize) -> Rect {
        let scale = theme::scale(height);
        Rect {
            x: 8,
            y: 28 * scale,
            w: 220 * scale,
            h: 224 * scale,
        }
    }

    pub fn start_menu_damage_rect(height: usize) -> Rect {
        let r = Self::start_menu_rect(height);
        let scale = theme::scale(height);
        Rect {
            x: r.x.saturating_sub(6 * scale),
            y: r.y.saturating_sub(4 * scale),
            w: r.w + 12 * scale,
            h: r.h + 12 * scale,
        }
    }

    pub fn context_menu_rect(x: usize, y: usize, height: usize) -> Rect {
        let scale = theme::scale(height);
        Rect {
            x,
            y,
            w: 170 * scale,
            h: 114 * scale,
        }
    }

    pub fn context_item_at(px: usize, py: usize) -> Option<usize> {
        if px >= 4 && px < 166 {
            if (4..24).contains(&py) {
                Some(0)
            } else if (24..44).contains(&py) {
                Some(1)
            } else if (47..67).contains(&py) {
                Some(2)
            } else if (70..90).contains(&py) {
                Some(3)
            } else if (90..110).contains(&py) {
                Some(4)
            } else {
                None
            }
        } else {
            None
        }
    }

    pub fn close_context_menu(&mut self, s: &mut Surface) {
        if let Some((mx, my)) = self.context_menu.take() {
            self.context_hover = None;
            s.damage(Self::context_menu_rect(mx, my, s.height));
        }
    }

    fn task_rect(id: u32, height: usize) -> Rect {
        Rect {
            x: if id == 1 {
                88
            } else {
                8 + (id as usize).saturating_sub(1).saturating_mul(168)
            },
            y: height.saturating_sub(28),
            w: if id == 1 { 80 } else { 160 },
            h: 24,
        }
    }

    #[inline(never)]
    pub fn ensure_window_1(&mut self, s: &mut Surface) {
        let id = if self.demo_mode { 1 } else { 6 };
        let scale = theme::scale(s.height);
        if !self.windows.iter().any(|w| w.id == id) {
            if let Some(w) = Window::new(
                id,
                "System Information",
                Rect {
                    x: 80,
                    y: 120,
                    w: if self.demo_mode {
                        320
                    } else {
                        (520 * scale).min(s.width.saturating_sub(96))
                    },
                    h: if self.demo_mode {
                        200
                    } else {
                        (350 * scale).min(s.height.saturating_sub(160))
                    },
                },
            ) {
                self.windows.push(w);
                s.damage(Rect {
                    x: 80,
                    y: 120,
                    w: (520 * scale).min(s.width.saturating_sub(96)),
                    h: (350 * scale).min(s.height.saturating_sub(160)),
                });
            }
        }
        self.unminimize_and_raise(id, s);
    }

    pub fn unminimize_and_raise(&mut self, id: u32, s: &mut Surface) {
        if let Some(w) = self.windows.iter_mut().find(|w| w.id == id) {
            w.is_minimized = false;
        }
        Self::dock_damage(s);
        self.raise(id, s);
    }

    pub fn close_window(&mut self, id: u32, s: &mut Surface) {
        if let Some(index) = self.windows.iter().position(|w| w.id == id) {
            let w = self.windows.remove(index);
            s.damage(w.damage_rect());
            Self::dock_damage(s);
            self.focus_top(s);
        }
    }

    pub fn set_network_state(&mut self, s: &mut Surface, state: u8) {
        if self.network_state != state {
            self.network_state = state;
            s.damage(Self::network_rect(s.width, s.height));
        }
    }

    pub fn network_rect(width: usize, height: usize) -> Rect {
        let scale = theme::scale(height);
        let tray_w = 126 * scale;
        Rect {
            x: width.saturating_sub(tray_w + 6 * scale),
            y: 0,
            w: tray_w + 6 * scale,
            h: 28 * scale,
        }
    }

    pub fn battery_rect(width: usize, height: usize) -> Rect {
        let scale = theme::scale(height);
        let tray_w = 114;
        let tray_x = (width / scale).saturating_sub(tray_w + 8);
        Rect {
            x: (tray_x + 27) * scale,
            y: 7 * scale,
            w: 24 * scale,
            h: 14 * scale,
        }
    }

    pub fn power_menu_rect(width: usize, height: usize) -> Rect {
        let scale = theme::scale(height);
        let tray_w = 114;
        let tray_x = (width / scale).saturating_sub(tray_w + 8);
        let pw = 180;
        let px = (tray_x + 5).min((width / scale).saturating_sub(pw + 8));
        Rect {
            x: px * scale,
            y: 30 * scale,
            w: pw * scale,
            h: 136 * scale,
        }
    }

    pub fn invalidate_panels(&mut self) {
        for window in &mut self.windows {
            window.panel_ready = false;
        }
    }

    pub fn clock_rect(width: usize, height: usize) -> Rect {
        let scale = theme::scale(height);
        let box_w = 220 * scale;
        let box_x = width.saturating_sub(box_w) / 2;
        Rect {
            x: box_x,
            y: 0,
            w: box_w,
            h: 28 * scale,
        }
    }

    pub fn update_clock(&mut self, s: &mut Surface, sec: i64) {
        if sec == self.clock_sec {
            return;
        }
        let previous = self.clock_sec;
        self.clock_sec = sec;
        if previous < 0 || previous.div_euclid(60) != sec.div_euclid(60) {
            s.damage(Self::clock_rect(s.width, s.height));
        }
    }

    pub fn force_clock_damage(&mut self, s: &mut Surface) {
        s.damage(Self::clock_rect(s.width, s.height));
    }

    pub fn cycle_focus(&mut self, s: &mut Surface) {
        if self.windows.is_empty() {
            return;
        }
        let first_id = self.windows[0].id;
        self.unminimize_and_raise(first_id, s);
        self.switcher_selected = first_id;
    }

    pub fn cycle_focus_reverse(&mut self, s: &mut Surface) {
        if self.windows.len() <= 1 {
            if let Some(w) = self.windows.first() {
                let id = w.id;
                self.unminimize_and_raise(id, s);
                self.switcher_selected = id;
            }
            return;
        }
        let prev_idx = self.windows.len() - 2;
        let target_id = self.windows[prev_idx].id;
        let top = self.windows.pop().unwrap();
        s.damage(top.damage_rect());
        self.windows.insert(0, top);
        self.unminimize_and_raise(target_id, s);
        self.switcher_selected = target_id;
    }

    pub fn switcher_ids(&self) -> ([u32; 8], usize) {
        let mut ids = [0u32; 8];
        let mut count = 0;
        for w in &self.windows {
            if count < 8 {
                ids[count] = w.id;
                count += 1;
            }
        }
        // Keep icons in stable ID order left-to-right so the selection highlight
        // moves smoothly across the row as the user presses Tab.
        for i in 1..count {
            let mut j = i;
            while j > 0 && ids[j - 1] > ids[j] {
                ids.swap(j - 1, j);
                j -= 1;
            }
        }
        (ids, count)
    }

    pub fn switcher_card_rect(width: usize, height: usize, count: usize) -> Rect {
        let scale = theme::scale(height);
        let n = count.clamp(1, 8);
        let cw = (n * 140 + 28).max(300) * scale;
        let ch = 192 * scale;
        Rect {
            x: width.saturating_sub(cw) / 2,
            y: height.saturating_sub(ch) / 2,
            w: cw.min(width),
            h: ch.min(height),
        }
    }

    pub fn switcher_damage_rect(width: usize, height: usize) -> Rect {
        let scale = theme::scale(height);
        let max_w = (8 * 140 + 36) * scale;
        let max_h = 204 * scale;
        Rect {
            x: width.saturating_sub(max_w) / 2,
            y: height.saturating_sub(max_h) / 2,
            w: max_w.min(width),
            h: max_h.min(height),
        }
    }

    pub fn close_switcher(&mut self, s: &mut Surface) {
        if self.switcher_open {
            self.switcher_open = false;
            s.damage(Self::switcher_damage_rect(s.width, s.height));
        }
    }

    /// Releases any active pointer capture, closes open desktop menus, and
    /// clears keyboard focus from all windows (SAK step 1).
    pub fn emergency_unfocus(&mut self, s: &mut Surface) {
        self.buttons = 0;
        self.close_switcher(s);
        if self.context_menu.is_some() {
            self.close_context_menu(s);
        }
        if self.start_open {
            self.start_open = false;
            self.menu_hover = None;
            s.damage(Self::start_btn_rect(s.height));
            s.damage(Self::start_menu_rect(s.height));
        }
        for w in &mut self.windows {
            if w.is_dragging || w.is_resizing || w.is_focused {
                s.damage(w.damage_rect());
            }
            w.is_dragging = false;
            w.is_resizing = false;
            w.is_focused = false;
        }
        Self::dock_damage(s);
    }

    pub fn lock(&mut self, s: &mut Surface) {
        self.emergency_unfocus(s);
        self.locked = true;
        self.lock_password.clear();
        self.lock_error = false;
        s.damage(Rect {
            x: 0,
            y: 0,
            w: s.width,
            h: s.height,
        });
    }

    pub fn unlock(&mut self, s: &mut Surface) {
        self.locked = false;
        self.lock_password.clear();
        self.lock_error = false;
        s.damage(Rect {
            x: 0,
            y: 0,
            w: s.width,
            h: s.height,
        });
    }

#[cfg(test)]
pub fn verify_unlock_password(pass: &str) -> bool {
    pass == "secret" || pass == ""
}

#[cfg(not(test))]
pub fn verify_unlock_password(pass: &str) -> bool {
    let mut pipe_fds = [0i32; 2];
    if unsafe { crate::sys::syscall2(crate::sys::SYS_PIPE, pipe_fds.as_mut_ptr() as usize, 0) } < 0 {
        return false;
    }

    let dup = |src, dst| crate::sys::SpawnFdAction {
        action_type: 2,
        dst_fd: dst,
        src_fd: src,
        ..Default::default()
    };
    let open_null = |fd| crate::sys::SpawnFdAction {
        action_type: 1,
        dst_fd: fd,
        flags: crate::sys::VFS_O_RDWR,
        path: c"/dev/null".as_ptr() as u64,
        ..Default::default()
    };
    let close = |fd| crate::sys::SpawnFdAction {
        action_type: 3,
        dst_fd: fd,
        ..Default::default()
    };

    let actions = [
        dup(pipe_fds[0], 0),
        open_null(1),
        open_null(2),
        close(pipe_fds[0]),
        close(pipe_fds[1]),
    ];

    let path = c"/bin/desktop-unlock";
    let argv = [path.as_ptr(), core::ptr::null()];
    let opts = crate::sys::SpawnOpts {
        size: core::mem::size_of::<crate::sys::SpawnOpts>() as u32,
        version: 2,
        argv: argv.as_ptr() as u64,
        fd_actions: actions.as_ptr() as u64,
        action_count: actions.len() as u32,
        ..Default::default()
    };

    let pid = crate::sys::spawn_ext("/bin/desktop-unlock", &opts);
    crate::sys::close(pipe_fds[0]);

    if pid <= 0 {
        crate::sys::close(pipe_fds[1]);
        return false;
    }

    if !pass.is_empty() {
        crate::sys::write(pipe_fds[1], pass.as_bytes());
    }
    crate::sys::write(pipe_fds[1], b"\n");
    crate::sys::close(pipe_fds[1]);

    let mut status = 0u64;
    let waited = unsafe {
        crate::sys::syscall3(
            crate::sys::SYS_WAITPID,
            pid as usize,
            &mut status as *mut _ as usize,
            0,
        )
    };

    waited == pid && status == 0
}

    pub fn toggle_start_menu(&mut self, s: &mut Surface) {
        self.start_open = !self.start_open;
        self.launcher_focused = self.start_open;
        self.launcher_search.clear();
        self.launcher_selected = 0;
        self.menu_hover = if self.start_open { Some(0) } else { None };
        s.damage(Self::start_btn_rect(s.height));
        s.damage(Self::start_menu_damage_rect(s.height));
    }

    #[allow(dead_code)]
    pub fn take_pending_action(&mut self) -> Option<Action> {
        self.pending_action.take()
    }

    pub fn matching_launcher_items(&self) -> ([Option<LauncherItem>; 16], usize) {
        let mut items = [None; 16];
        let mut count = 0;
        let query = self.launcher_search.as_str().trim();

        if query.is_empty() {
            let primary_apps = [
                AppId::Terminal,
                AppId::Files,
                AppId::Notepad,
                AppId::Calc,
                AppId::Vault,
                AppId::Settings,
                AppId::SysInfo,
            ];
            for id in primary_apps {
                if let Some(app) = app_registry::find_manifest(id) {
                    if count < items.len() {
                        items[count] = Some(LauncherItem::App(app));
                        count += 1;
                    }
                }
            }
            if count < items.len() {
                items[count] = Some(LauncherItem::Exit);
                count += 1;
            }
            if count < items.len() {
                items[count] = Some(LauncherItem::Restart);
                count += 1;
            }
        } else {
            // Filter applications matching the search query
            for app in app_registry::APP_CATALOG {
                if app.matches_query(query) {
                    if count < items.len() {
                        items[count] = Some(LauncherItem::App(app));
                        count += 1;
                    }
                }
            }
            // Allow searching session controls
            if app_registry::contains_ignore_case("Lock Screen", query)
                || app_registry::contains_ignore_case("lock sleep protect", query)
            {
                if count < items.len() {
                    items[count] = Some(LauncherItem::Lock);
                    count += 1;
                }
            }
            if app_registry::contains_ignore_case("Exit to Console", query)
                || app_registry::contains_ignore_case("exit quit console shutdown logout", query)
            {
                if count < items.len() {
                    items[count] = Some(LauncherItem::Exit);
                    count += 1;
                }
            }
            if app_registry::contains_ignore_case("Restart", query)
                || app_registry::contains_ignore_case("restart reboot", query)
            {
                if count < items.len() {
                    items[count] = Some(LauncherItem::Restart);
                    count += 1;
                }
            }
        }

        (items, count)
    }

    pub fn activate_launcher_item(&mut self, item: LauncherItem, s: &mut Surface) -> Action {
        match item {
            LauncherItem::App(app) => match app.id {
                AppId::Terminal => Action::SpawnTerminal,
                AppId::Files => Action::SpawnFileManager,
                AppId::Notepad => Action::SpawnNotepad,
                AppId::Calc => Action::SpawnCalc,
                AppId::Vault => Action::SpawnVault,
                AppId::Settings => Action::OpenDesktopApp(7),
                AppId::SysInfo => {
                    self.ensure_window_1(s);
                    Action::None
                }
                AppId::Network => Action::OpenDesktopApp(8),
                AppId::DateTime => Action::OpenDesktopApp(9),
                AppId::Power => Action::OpenDesktopApp(10),
            },
            LauncherItem::Exit => Action::Exit,
            LauncherItem::Restart => Action::Restart,
            LauncherItem::Lock => {
                self.lock(s);
                Action::None
            }
        }
    }

    /// Intercepts compositor-reserved key combinations before any client window
    /// sees them: SAK (`Ctrl + Alt + Esc`), `Alt + Tab` window switching, and
    /// the `Super` key Start Menu toggle. Returns true if consumed.
    pub fn intercept_key(&mut self, ev: crate::proto::KeyEvent, s: &mut Surface) -> bool {
        self.modifiers = ev.modifiers;
        // When screen is locked, intercept ALL keys
        if self.locked {
            if ev.state == crate::proto::KEY_STATE_RELEASE {
                return true;
            }
            if ev.ascii == 8 || ev.ascii == 127 {
                self.lock_password.backspace();
                self.lock_error = false;
                s.damage(Rect { x: 0, y: 0, w: s.width, h: s.height });
                return true;
            }
            if ev.keycode == crate::proto::KEY_ENTER || ev.keycode == 96 || ev.ascii == b'\r' || ev.ascii == b'\n' {
                if Self::verify_unlock_password(self.lock_password.as_str()) {
                    self.unlock(s);
                } else {
                    self.lock_error = true;
                    self.lock_password.clear();
                    s.damage(Rect { x: 0, y: 0, w: s.width, h: s.height });
                }
                return true;
            }
            if ev.ascii >= 32 && ev.ascii <= 126 {
                if self.lock_password.len() < 60 {
                    self.lock_password.push(ev.ascii as char);
                    self.lock_error = false;
                    s.damage(Rect { x: 0, y: 0, w: s.width, h: s.height });
                }
                return true;
            }
            return true;
        }

        // Global shortcut: Super+L locks the screen
        if (ev.modifiers & crate::proto::MOD_SUPER != 0)
            && (ev.keycode == 38 || ev.ascii == b'l' || ev.ascii == b'L')
            && ev.state == crate::proto::KEY_STATE_PRESS
        {
            self.lock(s);
            return true;
        }

        if (ev.modifiers & (crate::proto::MOD_CTRL | crate::proto::MOD_ALT))
            == (crate::proto::MOD_CTRL | crate::proto::MOD_ALT)
            && ev.keycode == crate::proto::KEY_ESC
        {
            if ev.state == crate::proto::KEY_STATE_PRESS {
                self.emergency_unfocus(s);
                self.ensure_window_1(s);
            }
            return true;
        }
        if (ev.keycode == crate::proto::KEY_LEFTALT || ev.keycode == crate::proto::KEY_RIGHTALT)
            && ev.state == crate::proto::KEY_STATE_RELEASE
            && self.switcher_open
        {
            let selected = self.switcher_selected;
            self.close_switcher(s);
            if selected != 0 {
                self.unminimize_and_raise(selected, s);
            }
            return true;
        }
        if ev.keycode == crate::proto::KEY_ESC
            && ev.state == crate::proto::KEY_STATE_PRESS
            && self.switcher_open
        {
            self.close_switcher(s);
            return true;
        }
        if (ev.modifiers & crate::proto::MOD_ALT) != 0
            && (ev.modifiers & crate::proto::MOD_CTRL) == 0
            && ev.keycode == crate::proto::KEY_TAB
        {
            if ev.is_press_or_repeat() && !self.windows.is_empty() {
                if (ev.modifiers & crate::proto::MOD_SHIFT) != 0 {
                    self.cycle_focus_reverse(s);
                } else {
                    self.cycle_focus(s);
                }
                self.switcher_open = true;
                self.switcher_selected = self
                    .focused_window_id()
                    .unwrap_or_else(|| self.windows.last().map(|w| w.id).unwrap_or(0));
                s.damage(Self::switcher_damage_rect(s.width, s.height));
            }
            return true;
        }
        if ev.keycode == crate::proto::KEY_LEFTMETA || ev.keycode == crate::proto::KEY_RIGHTMETA {
            if ev.state == crate::proto::KEY_STATE_PRESS
                && (ev.modifiers
                    & (crate::proto::MOD_CTRL | crate::proto::MOD_ALT | crate::proto::MOD_SHIFT))
                    == 0
            {
                self.toggle_start_menu(s);
            }
            return true;
        }

        // When Application Launcher is open and focused, intercept keyboard navigation & typing
        if self.start_open && self.launcher_focused {
            if ev.keycode == crate::proto::KEY_ESC || ev.ascii == 27 {
                if ev.state == crate::proto::KEY_STATE_PRESS {
                    self.start_open = false;
                    self.launcher_focused = false;
                    self.launcher_search.clear();
                    self.menu_hover = None;
                    s.damage(Self::start_btn_rect(s.height));
                    s.damage(Self::start_menu_damage_rect(s.height));
                }
                return true;
            }
            let (items, count) = self.matching_launcher_items();
            if ev.keycode == 103 /* KEY_UP */ {
                if ev.is_press_or_repeat() && count > 0 {
                    self.launcher_selected = if self.launcher_selected == 0 {
                        count - 1
                    } else {
                        self.launcher_selected - 1
                    };
                    self.menu_hover = Some(self.launcher_selected);
                    s.damage(Self::start_menu_damage_rect(s.height));
                }
                return true;
            }
            if ev.keycode == 108 /* KEY_DOWN */ {
                if ev.is_press_or_repeat() && count > 0 {
                    self.launcher_selected = (self.launcher_selected + 1) % count;
                    self.menu_hover = Some(self.launcher_selected);
                    s.damage(Self::start_menu_damage_rect(s.height));
                }
                return true;
            }
            if ev.keycode == crate::proto::KEY_ENTER
                || ev.keycode == 96
                || ev.ascii == b'\r'
                || ev.ascii == b'\n'
            {
                if ev.state == crate::proto::KEY_STATE_PRESS {
                    if self.launcher_selected < count {
                        if let Some(item) = items[self.launcher_selected] {
                            let action = self.activate_launcher_item(item, s);
                            self.pending_action = Some(action);
                        }
                    }
                    self.start_open = false;
                    self.launcher_focused = false;
                    self.launcher_search.clear();
                    self.menu_hover = None;
                    s.damage(Self::start_btn_rect(s.height));
                    s.damage(Self::start_menu_damage_rect(s.height));
                }
                return true;
            }
            if ev.ascii == 8 || ev.ascii == 127 || ev.keycode == 14 /* Backspace */ {
                if ev.is_press_or_repeat() {
                    self.launcher_search.backspace();
                    self.launcher_selected = 0;
                    self.menu_hover = Some(0);
                    s.damage(Self::start_menu_damage_rect(s.height));
                }
                return true;
            }
            if ev.ascii >= 32 && ev.ascii <= 126 {
                if ev.is_press_or_repeat() {
                    if self.launcher_search.len() < 31 {
                        self.launcher_search.push(ev.ascii as char);
                        self.launcher_selected = 0;
                        self.menu_hover = Some(0);
                        s.damage(Self::start_menu_damage_rect(s.height));
                    }
                }
                return true;
            }
            if ev.is_press_or_repeat() {
                return true;
            }
        }
        false
    }

    #[allow(dead_code)]
    pub fn focused_window_id(&self) -> Option<u32> {
        self.windows
            .iter()
            .rev()
            .find(|w| w.is_focused && !w.is_minimized)
            .map(|w| w.id)
    }

    /// Returns the number of open windows for this application.
    #[allow(dead_code)]
    pub fn app_window_count(&self, app: AppId) -> usize {
        self.windows.iter().filter(|w| w.app_id == Some(app)).count()
    }

    /// Returns true if at least one window of this application is open.
    #[allow(dead_code)]
    pub fn app_is_running(&self, app: AppId) -> bool {
        self.windows.iter().any(|w| w.app_id == Some(app))
    }

    /// Returns true if the currently focused window belongs to this application.
    #[allow(dead_code)]
    pub fn app_is_focused(&self, app: AppId) -> bool {
        self.focused_window_id().and_then(|id| {
            self.windows.iter().find(|w| w.id == id).and_then(|w| w.app_id)
        }) == Some(app)
    }

    /// Returns the frontmost window for this application (highest in stacking order).
    #[allow(dead_code)]
    pub fn app_frontmost_window(&self, app: AppId) -> Option<&Window> {
        self.windows.iter().rev().find(|w| w.app_id == Some(app))
    }

    /// Focuses or unminimizes the frontmost window of this application.
    #[allow(dead_code)]
    pub fn focus_app(&mut self, app: AppId, s: &mut Surface) -> bool {
        if let Some(id) = self.app_frontmost_window(app).map(|w| w.id) {
            self.unminimize_and_raise(id, s);
            true
        } else {
            false
        }
    }

    /// Cycles focus to the next window of this application.
    #[allow(dead_code)]
    pub fn cycle_app_windows(&mut self, app: AppId, s: &mut Surface) -> bool {
        let matching: Vec<u32> = self
            .windows
            .iter()
            .filter(|w| w.app_id == Some(app))
            .map(|w| w.id)
            .collect();
        if matching.len() <= 1 {
            return false;
        }
        if let Some(top) = self.windows.last() {
            if top.app_id == Some(app) {
                let next_id = matching
                    .iter()
                    .find(|&&id| id != top.id)
                    .copied()
                    .unwrap_or(matching[0]);
                self.unminimize_and_raise(next_id, s);
                return true;
            }
        }
        if let Some(&first) = matching.first() {
            self.unminimize_and_raise(first, s);
            true
        } else {
            false
        }
    }

    fn tooltip_rect(width: usize, height: usize) -> Rect {
        let d = Self::dock_rect(width, height);
        let h = 30 * theme::scale(height);
        Rect {
            x: d.x.saturating_sub(100 * theme::scale(height)),
            y: d.y.saturating_sub(h),
            w: d.w + 200 * theme::scale(height),
            h,
        }
    }

    fn dock_damage(s: &mut Surface) {
        let d = Self::dock_rect(s.width, s.height);
        let scale = theme::scale(s.height);
        s.damage(Self::tooltip_rect(s.width, s.height));
        s.damage(Rect {
            x: d.x.saturating_sub(8 * scale),
            y: d.y.saturating_sub(8 * scale),
            w: d.w + 16 * scale,
            h: d.h + 16 * scale,
        });
        s.damage(Rect {
            x: 0,
            y: s.height.saturating_sub(36 * scale),
            w: s.width,
            h: 36 * scale,
        });
    }

    fn focus_top(&mut self, s: &mut Surface) {
        Self::dock_damage(s);
        let top = self.windows.iter().rposition(|w| !w.is_minimized);
        for (i, w) in self.windows.iter_mut().enumerate() {
            let focused = Some(i) == top;
            if w.is_focused != focused {
                s.damage(w.damage_rect());
            }
            w.is_focused = focused;
        }
    }

    pub fn raise(&mut self, id: u32, s: &mut Surface) {
        if let Some(i) = self.windows.iter().position(|w| w.id == id) {
            let w = self.windows.remove(i);
            s.damage(w.damage_rect());
            self.windows.push(w); // retained capacity: no allocation
            self.focus_top(s);
        }
    }

    pub fn mouse(&mut self, x: usize, y: usize, buttons: u8, s: &mut Surface) -> Action {
        if self.locked {
            if buttons & 1 != 0 {
                let cw = 360.min(s.width - 40);
                let ch = 220.min(s.height - 40);
                let cx = (s.width - cw) / 2;
                let cy = (s.height - ch) / 2;
                let btn_r = Rect {
                    x: cx + 32,
                    y: cy + 140,
                    w: cw - 64,
                    h: 36,
                };
                if (btn_r.x..btn_r.x + btn_r.w).contains(&x) && (btn_r.y..btn_r.y + btn_r.h).contains(&y) {
                    if Self::verify_unlock_password(self.lock_password.as_str()) {
                        self.unlock(s);
                    } else {
                        self.lock_error = true;
                        self.lock_password.clear();
                        s.damage(Rect {
                            x: 0,
                            y: 0,
                            w: s.width,
                            h: s.height,
                        });
                    }
                }
            }
            return Action::None;
        }

        let mut ret_action = Action::None;
        // Capture survives leaving the titlebar/window. Apply final release motion.
        if self.buttons & 1 != 0 {
            let mut any_moving = false;
            for w in &mut self.windows {
                if w.is_dragging {
                    any_moving = true;
                    let old = w.damage_rect();
                    w.rect.x = x.saturating_sub(w.drag_offset.0).min(s.width - 1);
                    w.rect.y = y.saturating_sub(w.drag_offset.1).min(s.height - 1);
                    let new = w.damage_rect();
                    if old != new {
                        // The new footprint plus only the strips the move
                        // uncovered, instead of the bounding box of both.
                        s.damage(new);
                        for exposed in old.subtract(new).into_iter().flatten() {
                            s.damage(exposed);
                        }
                    }
                } else if w.is_resizing {
                    any_moving = true;
                    let min_w = 160;
                    let min_h = 100;
                    let max_w = s.width.saturating_sub(w.rect.x);
                    let max_h = s.height.saturating_sub(w.rect.y).saturating_sub(28);
                    let target_w = x.saturating_sub(w.rect.x).max(min_w).min(max_w);
                    let target_h = y.saturating_sub(w.rect.y).max(min_h).min(max_h);
                    if target_w != w.rect.w || target_h != w.rect.h {
                        let old = w.damage_rect();
                        if w.resize(target_w, target_h) {
                            s.damage(old.union(w.damage_rect()));
                            ret_action = Action::WindowResized(w.id);
                        }
                    }
                }
            }
            if !any_moving && (buttons & 1 != 0) {
                if let Some(w) = self.windows.last().filter(|w| !w.is_minimized && w.id >= 3) {
                    if x > w.rect.x
                        && y > w.rect.y + 25
                        && x + 1 < w.rect.x + w.rect.w
                        && y + 1 < w.rect.y + w.rect.h
                    {
                        ret_action = Action::ClientDrag(w.id, x - (w.rect.x + 1), y - (w.rect.y + 25));
                    }
                }
            }
        }
        // Right-click handling: open context menu on empty desktop space
        if (buttons & 2 != 0) && (self.buttons & 2 == 0) {
            if self.start_open {
                self.start_open = false;
                s.damage(Self::start_btn_rect(s.height));
                s.damage(Self::start_menu_rect(s.height));
            }
            self.close_context_menu(s);

            let scale = theme::scale(s.height);
            let on_dock = Self::dock_rect(s.width, s.height).contains(x, y);
            let on_topbar = y < 28 * scale;
            let on_window = self.hit_test(x, y).is_some();

            if !on_dock && !on_topbar && !on_window {
                let menu_w = 170 * scale;
                let menu_h = 114 * scale;
                let mx = x.min(s.width.saturating_sub(menu_w + 4));
                let my = y.min(s.height.saturating_sub(menu_h + 30 * scale));
                self.context_menu = Some((mx, my));
                self.context_hover = None;
                s.damage(Rect { x: mx, y: my, w: menu_w, h: menu_h });
                self.buttons = buttons;
                return Action::None;
            }
        }

        if buttons & 1 == 0 {
            for w in &mut self.windows {
                if w.is_dragging && self.buttons & 1 != 0 && w.id >= 3 {
                    let scale = theme::scale(s.height);
                    let snap = if y <= 28 * scale {
                        Some((8 * scale, 28 * scale, s.width.saturating_sub(16 * scale)))
                    } else if x <= 4 {
                        Some((
                            8 * scale,
                            28 * scale,
                            (s.width / 2).saturating_sub(12 * scale),
                        ))
                    } else if x >= s.width.saturating_sub(5) {
                        Some((
                            s.width / 2 + 4 * scale,
                            28 * scale,
                            (s.width / 2).saturating_sub(12 * scale),
                        ))
                    } else {
                        None
                    };
                    if let Some((left, top, width)) = snap {
                        let old = w.damage_rect();
                        let restore = w.rect;
                        if w.resize(width, s.height.saturating_sub(104 * scale)) {
                            w.rect.x = left;
                            w.rect.y = top;
                            w.saved_rect = Some(restore);
                            s.damage(old.union(w.damage_rect()));
                            ret_action = Action::WindowResized(w.id);
                        }
                    }
                }
                w.is_dragging = false;
                w.is_resizing = false;
            }
        } else if self.buttons & 1 == 0 {
            if self.switcher_open {
                let (ids, count) = self.switcher_ids();
                let card = Self::switcher_card_rect(s.width, s.height, count);
                if card.contains(x, y) && count > 0 {
                    let scale = theme::scale(s.height);
                    let bw = card.w / scale;
                    let tiles_total_w = count * 140 - 12;
                    let tiles_start_x = bw.saturating_sub(tiles_total_w) / 2;
                    let px = (x - card.x) / scale;
                    let py = (y - card.y) / scale;
                    if (32..148).contains(&py) && px >= tiles_start_x {
                        let idx = (px - tiles_start_x) / 140;
                        let in_tile_x = (px - tiles_start_x) % 140;
                        if idx < count && in_tile_x < 128 {
                            let target_id = ids[idx];
                            self.close_switcher(s);
                            self.unminimize_and_raise(target_id, s);
                            self.buttons = buttons;
                            return Action::None;
                        }
                    }
                }
                self.close_switcher(s);
            }
            // Dismiss or interact with context menu
            if let Some((mx, my)) = self.context_menu.take() {
                let scale = theme::scale(s.height);
                let menu_r = Self::context_menu_rect(mx, my, s.height);
                s.damage(menu_r);
                if menu_r.contains(x, y) {
                    let px = (x - mx) / scale;
                    let py = (y - my) / scale;
                    if let Some(idx) = Self::context_item_at(px, py) {
                        self.buttons = buttons;
                        return match idx {
                            0 => Action::SpawnNotepad,
                            1 => Action::SpawnFileManager,
                            2 => Action::SpawnTerminal,
                            3 => Action::OpenDesktopApp(7),
                            4 => Action::OpenDesktopApp(7),
                            _ => Action::None,
                        };
                    }
                }
            }

            let dock_r = Self::dock_rect(s.width, s.height);
            let start_btn = Self::start_btn_rect(s.height);
            let start_menu = Self::start_menu_rect(s.height);

            if self.battery_status.is_some() && Self::battery_rect(s.width, s.height).contains(x, y) {
                self.show_power_menu = !self.show_power_menu;
                s.damage(Self::battery_rect(s.width, s.height));
                s.damage(Self::power_menu_rect(s.width, s.height));
                self.buttons = buttons;
                return Action::None;
            }

            if self.show_power_menu {
                let menu_r = Self::power_menu_rect(s.width, s.height);
                if menu_r.contains(x, y) {
                    let scale = theme::scale(s.height);
                    let lx = (x - menu_r.x) / scale;
                    let ly = (y - menu_r.y) / scale;
                    let bw = menu_r.w / scale;
                    let btn_w = (bw.saturating_sub(36)) / 2;
                    if (74..102).contains(&ly) {
                        if lx >= 16 && lx < 16 + btn_w {
                            self.power_profile = 0;
                        } else if lx >= 18 + btn_w && lx < 18 + 2 * btn_w {
                            self.power_profile = 1;
                        }
                        s.damage(menu_r);
                    } else if (106..132).contains(&ly) {
                        self.show_power_menu = false;
                        s.damage(menu_r);
                        self.buttons = buttons;
                        return Action::OpenDesktopApp(10);
                    }
                    self.buttons = buttons;
                    return Action::None;
                } else {
                    self.show_power_menu = false;
                    s.damage(menu_r);
                }
            }

            // Power icon click in tray
            let has_bat = self.battery_status.is_some();
            let scale = theme::scale(s.height);
            let tray_w = if has_bat { 114 } else { 86 };
            let tray_x = (s.width / scale).saturating_sub(tray_w + 8);
            let pwr_icon_x = if has_bat { tray_x + 70 } else { tray_x + 54 };
            let pwr_btn_r = Rect {
                x: pwr_icon_x * scale,
                y: 7 * scale,
                w: 16 * scale,
                h: 14 * scale,
            };
            if pwr_btn_r.contains(x, y) {
                self.buttons = buttons;
                return Action::OpenDesktopApp(10);
            }

            if Self::clock_rect(s.width, s.height).contains(x, y) {
                self.buttons = buttons;
                return Action::OpenDesktopApp(9);
            }
            if Self::network_rect(s.width, s.height).contains(x, y) {
                self.buttons = buttons;
                return Action::OpenDesktopApp(8);
            }

            let task = self
                .windows
                .iter()
                .position(|w| w.is_minimized && Self::task_rect(w.id, s.height).contains(x, y));
            if let Some(i) = task {
                let id = self.windows[i].id;
                self.windows[i].is_minimized = false;
                Self::dock_damage(s);
                self.raise(id, s);
                self.buttons = buttons;
                return Action::None;
            }

            // Floating macOS Dock interaction
            if dock_r.contains(x, y) {
                let item_idx = (x - dock_r.x) / (64 * theme::scale(s.height));
                let target_app = match item_idx {
                    0 => Some(AppId::Files),
                    1 => Some(AppId::Terminal),
                    2 => Some(AppId::Notepad),
                    3 => Some(AppId::SysInfo),
                    _ => None,
                };
                let shift_held = (self.modifiers & crate::proto::MOD_SHIFT) != 0;
                if let Some(app) = target_app {
                    if app == AppId::SysInfo {
                        self.ensure_window_1(s);
                    } else if shift_held || !self.app_is_running(app) {
                        match app {
                            AppId::Files => ret_action = Action::SpawnFileManager,
                            AppId::Terminal => ret_action = Action::SpawnTerminal,
                            AppId::Notepad => ret_action = Action::SpawnNotepad,
                            _ => {}
                        }
                    } else if self.app_is_focused(app) {
                        // If multiple windows exist, cycle through them
                        if self.app_window_count(app) > 1 {
                            self.cycle_app_windows(app, s);
                        }
                    } else {
                        // Running in background or minimized: focus and restore frontmost window
                        self.focus_app(app, s);
                    }
                }
                Self::dock_damage(s);
                self.buttons = buttons;
                return ret_action;
            }

            if start_btn.contains(x, y) {
                self.toggle_start_menu(s);
                self.buttons = buttons;
                return Action::None;
            }

            if self.start_open {
                if start_menu.contains(x, y) {
                    let py = (y - start_menu.y) / theme::scale(s.height);
                    if py >= 24 {
                        let item_idx = (py - 24) / 22;
                        let (items, count) = self.matching_launcher_items();
                        if item_idx < count {
                            if let Some(item) = items[item_idx] {
                                ret_action = self.activate_launcher_item(item, s);
                            }
                        }
                    }
                }
                self.start_open = false;
                self.launcher_focused = false;
                self.launcher_search.clear();
                s.damage(start_btn);
                s.damage(Self::start_menu_damage_rect(s.height));
                self.buttons = buttons;
                return ret_action;
            }

            if let Some((id, hit)) = self.hit_test(x, y) {
                self.raise(id, s);
                let w = self.windows.last_mut().unwrap();
                match hit {
                    Hit::Close => {
                        let w = self.windows.pop().unwrap();
                        s.damage(w.damage_rect());
                        Self::dock_damage(s);
                        self.focus_top(s);
                    }
                    Hit::Minimize => {
                        w.is_minimized = true;
                        s.damage(w.damage_rect());
                        Self::dock_damage(s);
                        self.focus_top(s);
                    }
                    Hit::Maximize => {
                        let old = w.damage_rect();
                        if let Some(prev) = w.saved_rect.take() {
                            if w.resize(prev.w, prev.h) {
                                w.rect = prev;
                                s.damage(old.union(w.damage_rect()));
                                ret_action = Action::WindowResized(w.id);
                            }
                        } else {
                            let scale = theme::scale(s.height);
                            let work_x = 8 * scale;
                            let work_y = 28 * scale;
                            let work_w = s.width.saturating_sub(16 * scale);
                            let work_h = s.height.saturating_sub(104 * scale);
                            let restore = w.rect;
                            if w.resize(work_w, work_h) {
                                w.saved_rect = Some(restore);
                                w.rect.x = work_x;
                                w.rect.y = work_y;
                                s.damage(old.union(w.damage_rect()));
                                ret_action = Action::WindowResized(w.id);
                            }
                        }
                    }
                    Hit::Title => {
                        w.is_dragging = true;
                        w.drag_offset = (x - w.rect.x, y - w.rect.y);
                    }
                    Hit::Resize => {
                        w.is_resizing = true;
                    }
                    Hit::Client => {
                        let cx = x.saturating_sub(w.rect.x + 1);
                        let cy = y.saturating_sub(w.rect.y + 25);
                        ret_action = Action::ClientClick(id, cx, cy);
                    }
                    _ => (),
                }
            }
        }
        self.buttons = buttons;

        if self.start_open {
            let start_menu = Self::start_menu_rect(s.height);
            let (_items, count) = self.matching_launcher_items();
            let new_menu_hover =
                if start_menu.contains(x, y) && y >= start_menu.y + 24 * theme::scale(s.height) {
                    let idx = ((y - start_menu.y) / theme::scale(s.height) - 24) / 22;
                    if idx < count { Some(idx) } else { None }
                } else {
                    None
                };
            if new_menu_hover != self.menu_hover {
                self.menu_hover = new_menu_hover;
                if let Some(idx) = new_menu_hover {
                    self.launcher_selected = idx;
                }
                s.damage(Self::start_menu_damage_rect(s.height));
            }
        }

        if let Some((mx, my)) = self.context_menu {
            let scale = theme::scale(s.height);
            let menu_r = Self::context_menu_rect(mx, my, s.height);
            let new_ctx_hover = if menu_r.contains(x, y) {
                let px = (x - mx) / scale;
                let py = (y - my) / scale;
                Self::context_item_at(px, py)
            } else {
                None
            };
            if new_ctx_hover != self.context_hover {
                self.context_hover = new_ctx_hover;
                s.damage(menu_r);
            }
        }

        let dock = Self::dock_rect(s.width, s.height);
        let new_dock_hover = if dock.contains(x, y) {
            Some((x - dock.x) / (64 * theme::scale(s.height)))
        } else {
            None
        };
        if new_dock_hover != self.dock_hover {
            s.damage(Self::tooltip_rect(s.width, s.height));
            Self::dock_damage(s);
            self.dock_hover = new_dock_hover;
        }
        let hover = self.hit_test(x, y);
        if hover != self.hover {
            for (id, hit) in self.hover.into_iter().chain(hover) {
                if let Some(r) = self
                    .windows
                    .iter()
                    .find(|w| w.id == id)
                    .and_then(|w| w.hover_damage(hit))
                {
                    s.damage(r);
                }
            }
            self.hover = hover;
        }
        ret_action
    }

    #[inline(never)]
    pub fn monitor(
        &mut self,
        s: &mut Surface,
        pitch: usize,
        heap: (usize, usize),
        mouse: (usize, usize),
    ) {
        let Some(w) = self.windows.iter_mut().find(|w| w.id == 1) else {
            return;
        };
        w.content.fill(CLIENT);
        let mut line = Line::new();
        let _ = write!(line, "Screen: {} x {}", s.width, s.height);
        content_text(w, 12, 12, line.as_str());
        line.len = 0;
        let _ = write!(line, "Pitch: {} bytes", pitch);
        content_text(w, 12, 36, line.as_str());
        line.len = 0;
        let _ = write!(line, "Heap: {} / {} KiB", heap.0 / 1024, heap.1 / 1024);
        content_text(w, 12, 60, line.as_str());
        line.len = 0;
        let _ = write!(line, "Mouse: {}, {}", mouse.0, mouse.1);
        content_text(w, 12, 84, line.as_str());
        content_text(w, 12, 120, "Drag titlebar to move");
        if !w.is_minimized {
            s.damage(w.rect);
        }
    }

    /// System metrics are supplied by the caller; rendering stays host-testable.
    #[inline(never)]
    pub fn system_panel(&mut self, s: &mut Surface, info: Option<crate::sys::SystemInfo>) {
        let Some(w) = self.windows.iter_mut().find(|w| w.id == 6) else {
            return;
        };
        if w.is_minimized {
            return;
        }
        let scale = theme::scale(s.height);
        let initialize = !w.panel_ready;
        let logical_w = (w.rect.w - 2) / scale;
        let x_off = if logical_w > 560 {
            (logical_w - 518) / 2
        } else {
            0
        };
        let card_w = 486.min(logical_w.saturating_sub(32));
        let hdr_bg = 0xff262932;
        let tbl_bg = 0xff22252e;
        let disp_bg = 0xff1c222d;
        let mut line = Line::new();
        if initialize {
            w.content.fill(theme::PANEL);
            // Hero Header Card
            panel_box(w, x_off + 16, 12, card_w, 56, 8, theme::PANEL, hdr_bg, 0xff363e4d, scale);
            // System badge icon
            panel_box(w, x_off + 28, 22, 36, 36, 8, hdr_bg, 0xff1e385b, theme::ACCENT, scale);
            panel_text(
                w,
                s,
                x_off + 41,
                31,
                16,
                "F",
                0xffffffff,
                0xff1e385b,
                true,
                scale,
            );
            panel_text(
                w,
                s,
                x_off + 76,
                20,
                220,
                "FortressOS",
                0xffffffff,
                hdr_bg,
                true,
                scale,
            );
            panel_text(
                w,
                s,
                x_off + 76,
                39,
                260,
                "System Information & Telemetry",
                theme::SUBTEXT,
                hdr_bg,
                false,
                scale,
            );
            if card_w >= 420 {
                panel_box(
                    w,
                    x_off + 16 + card_w - 94,
                    29,
                    80,
                    22,
                    6,
                    hdr_bg,
                    0xff133223,
                    0xff22c55e,
                    scale,
                );
                panel_text(
                    w,
                    s,
                    x_off + 16 + card_w - 80,
                    31,
                    56,
                    "Online",
                    0xff4ade80,
                    0xff133223,
                    false,
                    scale,
                );
            }

            // Telemetry Table Card
            panel_box(w, x_off + 16, 76, card_w, 178, 8, theme::PANEL, tbl_bg, 0xff363e4d, scale);

            // Display Resolution Footer Card
            panel_box(w, x_off + 16, 262, card_w, 34, 8, theme::PANEL, disp_bg, 0xff2e3848, scale);
            let _ = write!(line, "Display  {} x {}", s.width, s.height);
            panel_text(
                w,
                s,
                x_off + 32,
                270,
                card_w.saturating_sub(32),
                line.as_str(),
                0xff9fb0c7,
                disp_bg,
                false,
                scale,
            );
        }
        let val_w = card_w.saturating_sub(226).min(180);
        if let Some(info) = info {
            let rows = [
                ("Logical CPUs", info.cpu_count as u64, ""),
                ("Managed RAM", info.total_ram_bytes / (1024 * 1024), " MiB"),
                ("Available RAM", info.free_ram_bytes / (1024 * 1024), " MiB"),
                ("Processes", info.task_count as u64, ""),
                ("Threads", info.thread_count as u64, ""),
                (
                    "Uptime",
                    info.uptime_ticks.checked_div(info.tick_hz).unwrap_or(0),
                    " seconds",
                ),
            ];
            for (i, (name, value, suffix)) in rows.iter().enumerate() {
                panel_text(
                    w,
                    s,
                    x_off + 32,
                    84 + i * 28,
                    184,
                    name,
                    theme::SUBTEXT,
                    tbl_bg,
                    false,
                    scale,
                );
                line.len = 0;
                let _ = write!(line, "{}{}", value, suffix);
                let val_col = match i {
                    2 => 0xff4ade80,
                    5 => 0xff38bdf8,
                    _ => 0xffffffff,
                };
                panel_text(
                    w,
                    s,
                    x_off + 230,
                    84 + i * 28,
                    val_w,
                    line.as_str(),
                    val_col,
                    tbl_bg,
                    true,
                    scale,
                );
            }
        } else {
            for i in 0..6 {
                panel_text(
                    w,
                    s,
                    x_off + 32,
                    84 + i * 28,
                    184,
                    if i == 0 { "Unavailable" } else { "" },
                    theme::SUBTEXT,
                    tbl_bg,
                    false,
                    scale,
                );
                panel_text(
                    w,
                    s,
                    x_off + 230,
                    84 + i * 28,
                    val_w,
                    "",
                    theme::TEXT,
                    tbl_bg,
                    true,
                    scale,
                );
            }
        }
        if initialize {
            s.damage(w.rect);
        }
        w.panel_ready = true;
    }

    pub fn monitor_mouse(&mut self, s: &mut Surface, mouse: (usize, usize)) {
        let Some(w) = self.windows.iter_mut().find(|w| w.id == 1) else {
            return;
        };
        if w.is_minimized {
            return;
        }
        let width = w.rect.w.saturating_sub(2);
        let height = w.rect.h.saturating_sub(26);
        let text_y = 84;
        let text_h = 16;
        let text_w = 220.min(width.saturating_sub(12));
        if text_y + text_h <= height {
            for row in text_y..text_y + text_h {
                for col in 12..12 + text_w {
                    w.content[row * width + col] = CLIENT;
                }
            }
        }
        let mut line = Line::new();
        let _ = write!(line, "Mouse: {}, {}", mouse.0, mouse.1);
        content_text(w, 12, text_y, line.as_str());

        let damage_rect = Rect {
            x: w.rect.x.saturating_add(1).saturating_add(12),
            y: w.rect.y.saturating_add(25).saturating_add(text_y),
            w: text_w,
            h: text_h,
        };
        s.damage(damage_rect);
    }

    #[allow(dead_code)]
    pub fn render(&self, s: &mut Surface) {
        self.render_with_wallpaper(s, None);
    }

    pub fn render_with_wallpaper(
        &self,
        s: &mut Surface,
        wallpaper: Option<&crate::wallpaper::Wallpaper>,
    ) {
        let mut i = 0;
        while let Some(r) = s.damage_rect(i) {
            // Repair the desktop only where no opaque window core will cover it.
            let occluded = Occluders::collect(&self.windows, r);
            let (sw, sh) = (s.width, s.height);
            for y in r.y..r.y + r.h {
                occluded.spans(y, r.x, r.x + r.w, |a, b| {
                    let row = &mut s.pixels[y * sw + a..y * sw + b];
                    if let Some(wp) = wallpaper {
                        wp.fill_row(a, y, sw, sh, row);
                    } else if !self.demo_mode {
                        theme::desktop_row(a, y, sw, sh, row);
                    } else {
                        for (i, p) in row.iter_mut().enumerate() {
                            let x = a + i;
                            *p = if (32..48).contains(&y) && x >= 32 {
                                label_pixel(
                                    "FortressOS Rust Compositor",
                                    x - 32,
                                    y - 32,
                                    TEXT,
                                    DESKTOP,
                                )
                            } else {
                                DESKTOP
                            };
                        }
                    }
                });
            }

            let scale = theme::scale(s.height);
            let panel = Panel::new(s.width, s.height);
            panel.render(self, s, r);

            for w in &self.windows {
                if w.is_minimized || w.id < 3 || w.saved_rect.is_some() {
                    continue;
                }
                let shadow_r = Rect {
                    x: w.rect.x.saturating_sub(8),
                    y: w.rect.y.saturating_sub(5),
                    w: w.rect.w + 16,
                    h: w.rect.h + 16,
                };
                if let Some(clip) = shadow_r.intersection(r) {
                    let (max_d, mult) = if w.is_focused { (8usize, 26u32) } else { (3usize, 10u32) };
                    let apply_shadow = |under: u32, d: usize, focused: bool| -> u32 {
                        let factor = ((max_d + 1 - d) as u32) * mult;
                        let darkened = blend_darken(under, factor);
                        if focused && d <= 4 {
                            // Subtle electric-sapphire outer elevation halo so the active
                            // window pops clearly even over a dark wallpaper.
                            let glow_cov = (5 - d) as u32 * 2;
                            theme::blend_coverage(0xff2563eb, darkened, glow_cov)
                        } else {
                            darkened
                        }
                    };
                    for y in clip.y..clip.y + clip.h {
                        let in_vert_body = y >= w.rect.y + 3 && y < w.rect.y + w.rect.h + 3;
                        if in_vert_body {
                            // Left shadow fringe
                            let left_end = (w.rect.x).min(clip.x + clip.w);
                            for x in clip.x..left_end {
                                let d = w.rect.x - x;
                                if d <= max_d {
                                    s.pixels[y * s.width + x] =
                                        apply_shadow(s.pixels[y * s.width + x], d, w.is_focused);
                                }
                            }
                            // Right shadow fringe
                            let right_start = (w.rect.x + w.rect.w).max(clip.x);
                            for x in right_start..clip.x + clip.w {
                                let d = x - (w.rect.x + w.rect.w - 1);
                                if d <= max_d {
                                    s.pixels[y * s.width + x] =
                                        apply_shadow(s.pixels[y * s.width + x], d, w.is_focused);
                                }
                            }
                        } else {
                            // Top or bottom shadow row
                            let dy = if y < w.rect.y + 3 {
                                (w.rect.y + 3) - y
                            } else {
                                y - (w.rect.y + w.rect.h + 2)
                            };
                            for x in clip.x..clip.x + clip.w {
                                let dx = if x < w.rect.x {
                                    w.rect.x - x
                                } else if x >= w.rect.x + w.rect.w {
                                    x - (w.rect.x + w.rect.w - 1)
                                } else {
                                    0
                                };
                                let d = dx.max(dy);
                                if d > 0 && d <= max_d {
                                    s.pixels[y * s.width + x] =
                                        apply_shadow(s.pixels[y * s.width + x], d, w.is_focused);
                                }
                            }
                        }
                    }
                }
            }

            for (index, w) in self.windows.iter().enumerate() {
                if w.is_minimized {
                    continue;
                }
                if let Some(clip) = w.rect.intersection(r) {
                    let hover = self.hover.filter(|h| h.0 == w.id).map(|h| h.1);
                    // Windows stacked above this one overwrite their cores.
                    let above = Occluders::collect(&self.windows[index + 1..], clip);
                    for y in clip.y..clip.y + clip.h {
                        let wy = y - w.rect.y;
                        let client_row = wy > TITLE_HEIGHT
                            && wy < w.rect.h - 1
                            && (w.saved_rect.is_some() || wy < w.rect.h.saturating_sub(8));
                        above.spans(y, clip.x, clip.x + clip.w, |a, b| {
                            let start = a.max(w.rect.x + 1);
                            let end = b.min(w.rect.x + w.rect.w - 1);
                            if client_row && start < end {
                                if a <= w.rect.x {
                                    s.pixels[y * s.width + w.rect.x] = if w.id >= 3 {
                                        if w.is_focused { 0xff2563eb } else { 0xff2b303b }
                                    } else {
                                        0xff30363d
                                    };
                                }
                                let src_off = (wy - 25) * (w.rect.w - 2) + (start - (w.rect.x + 1));
                                let len = end - start;
                                let dst_off = y * s.width + start;
                                s.pixels[dst_off..dst_off + len]
                                    .copy_from_slice(&w.content[src_off..src_off + len]);
                                if b >= w.rect.x + w.rect.w {
                                    s.pixels[y * s.width + w.rect.x + w.rect.w - 1] = if w.id >= 3 {
                                        if w.is_focused { 0xff2563eb } else { 0xff232730 }
                                    } else {
                                        0xff30363d
                                    };
                                }
                                return;
                            }
                            let sw = s.width;
                            let dst = &mut s.pixels[y * sw + a..y * sw + b];
                            if wy <= TITLE_HEIGHT {
                                let cached = w.with_title_rows(hover, |rows| {
                                    let row = &rows[wy * w.rect.w..(wy + 1) * w.rect.w];
                                    let src = &row[a - w.rect.x..b - w.rect.x];
                                    for (d, &p) in dst.iter_mut().zip(src) {
                                        if p != 0 {
                                            *d = p;
                                        }
                                    }
                                });
                                if cached.is_some() {
                                    return;
                                }
                            }
                            // Bottom rows: away from the rounded corners, the
                            // bevels and the resize grip, pixel() is content.
                            let content_row = wy > TITLE_HEIGHT && wy < w.rect.h - 1;
                            for (i, d) in dst.iter_mut().enumerate() {
                                let lx = a + i - w.rect.x;
                                let p = if content_row && lx >= 10 && lx + 11 < w.rect.w {
                                    w.content[(wy - 25) * (w.rect.w - 2) + lx - 1]
                                } else {
                                    w.pixel(lx, wy, hover)
                                };
                                if p != 0 {
                                    *d = p;
                                }
                            }
                        });
                    }
                }
            }

            // Floating macOS Translucent Bottom Dock
            let dock_r = Self::dock_rect(s.width, s.height);
            let dock_shadow_r = Rect {
                x: dock_r.x.saturating_sub(4),
                y: dock_r.y + dock_r.h,
                w: dock_r.w + 8,
                h: 8,
            };
            if let Some(clip) = dock_shadow_r.intersection(r) {
                for y in clip.y..clip.y + clip.h {
                    for x in clip.x..clip.x + clip.w {
                        let dy = y - (dock_r.y + dock_r.h) + 1;
                        let factor = ((9 - dy) * 14) as u32;
                        s.pixels[y * s.width + x] = blend_darken(s.pixels[y * s.width + x], factor);
                    }
                }
            }

            if let Some(clip) = dock_r.intersection(r) {
                let has_win5 = self.windows.iter().any(|w| w.id == 5);
                let has_win3 = self.windows.iter().any(|w| w.id == 3);
                let has_win4 = self.windows.iter().any(|w| w.id == 4);
                let has_win1 = self.windows.iter().any(|w| w.id == 1 || w.id == 6);
                for y in clip.y..clip.y + clip.h {
                    for x in clip.x..clip.x + clip.w {
                        let px = (x - dock_r.x) / scale;
                        let py = (y - dock_r.y) / scale;

                        let coverage = theme::rounded_coverage(
                            x - dock_r.x,
                            y - dock_r.y,
                            dock_r.w,
                            dock_r.h,
                            18 * scale,
                        );
                        if coverage == 0 {
                            continue;
                        }
                        let under = s.pixels[y * s.width + x];
                        let dock_w = dock_r.w / scale;
                        let dock_h = dock_r.h / scale;
                        // Frosted glass vibrancy: micro-diffused dark slate over darkened background
                        let frost_tint = if ((px ^ py) & 1) == 0 {
                            0xff1c212b
                        } else {
                            0xff181c25
                        };
                        let glass_bg = theme::blend_coverage(frost_tint, blend_darken(under, 76), 13);
                        let is_dock_rim = px == 0 || px + 1 == dock_w || py == 0 || py + 1 == dock_h;
                        let dock_bg = if py == 0 {
                            0xff4e5c75
                        } else if is_dock_rim {
                            0xff313a4b
                        } else if py == 1 {
                            theme::blend_coverage(0xff384357, glass_bg, 10)
                        } else {
                            glass_bg
                        };
                        let item_idx = px / 64;
                        let in_item_x = px % 64;

                        let pixel = if item_idx < 4 {
                            let target_app = match item_idx {
                                0 => AppId::Files,
                                1 => AppId::Terminal,
                                2 => AppId::Notepad,
                                3 => AppId::SysInfo,
                                _ => AppId::Files,
                            };
                            let win_count = self.app_window_count(target_app);
                            let is_running = win_count > 0 || match item_idx {
                                0 => has_win5,
                                1 => has_win3,
                                2 => has_win4,
                                3 => has_win1,
                                _ => false,
                            };

                            let legacy_id = [5, 3, 4, if self.demo_mode { 1 } else { 6 }][item_idx];
                            let focused = self.app_is_focused(target_app) || self.focused_window_id() == Some(legacy_id);
                            let is_hovered = self.dock_hover == Some(item_idx);
                            let is_neighbor = self
                                .dock_hover
                                .filter(|&h| h < 4)
                                .is_some_and(|h| h.abs_diff(item_idx) == 1);

                            // Dock magnification: hovered icon grows +71% (28->48, ~3x area!) and lifts
                            // right to the top of the Dock; adjacent icons grow +36% (28->38).
                            let (icon_size, icon_y) = if is_hovered {
                                (48usize, 1usize)
                            } else if is_neighbor {
                                (38usize, 7usize)
                            } else {
                                (28usize, 14usize)
                            };
                            let icon_x = (64 - icon_size) / 2;

                            let mut slot_bg = dock_bg;
                            if is_hovered && (1..53).contains(&py) && (4..60).contains(&in_item_x) {
                                let tile_cov =
                                    theme::rounded_coverage(in_item_x - 4, py - 1, 56, 52, 12);
                                if tile_cov > 0 {
                                    let is_tile_edge = in_item_x == 4
                                        || in_item_x == 59
                                        || py == 1
                                        || py == 52;
                                    let tile_col = if py == 1 {
                                        0xff38bdf8 // 1px bright sky-blue top specular rim
                                    } else if is_tile_edge {
                                        0xff2563eb
                                    } else {
                                        0xff1e3352
                                    };
                                    slot_bg = theme::blend_coverage(tile_col, dock_bg, tile_cov);
                                }
                            }

                            if in_item_x >= icon_x
                                && in_item_x < icon_x + icon_size
                                && py >= icon_y
                                && py < icon_y + icon_size
                            {
                                render_dock_icon_scaled(
                                    item_idx,
                                    in_item_x - icon_x,
                                    py - icon_y,
                                    icon_size,
                                    slot_bg,
                                )
                            } else if is_running && (52..=54).contains(&py) {
                                // Running App Accent Indicator:
                                // - Active focused app: 16px pill (24..=39) in theme::ACCENT
                                // - Background app with multiple windows: dual dots (26..=29 and 34..=37)
                                // - Background app with single window: single centered dot (28..=35)
                                let in_pill = if focused {
                                    (24..=39).contains(&in_item_x)
                                } else if win_count > 1 {
                                    (26..=29).contains(&in_item_x) || (34..=37).contains(&in_item_x)
                                } else {
                                    (28..=35).contains(&in_item_x)
                                };
                                if in_pill {
                                    if focused { theme::ACCENT } else { theme::MUTED }
                                } else {
                                    slot_bg
                                }
                            } else {
                                slot_bg
                            }
                        } else {
                            dock_bg
                        };
                        s.pixels[y * s.width + x] = theme::blend_coverage(pixel, under, coverage);
                    }
                }
            }

            if let Some(item) = self.dock_hover.filter(|i| *i < 4) {
                let label = ["Files", "Terminal", "Notepad", "System Information"][item];
                let area = Self::tooltip_rect(s.width, s.height);
                let label_w = (theme::text_width(label) + 20) * scale;
                let center = dock_r.x + (item * 64 + 32) * scale;
                let left = center
                    .saturating_sub(label_w / 2)
                    .min(s.width.saturating_sub(label_w));
                let tip = Rect {
                    x: left,
                    y: area.y + 2 * scale,
                    w: label_w,
                    h: 24 * scale,
                };
                if let Some(clip) = tip.intersection(r) {
                    for y in clip.y..clip.y + clip.h {
                        for x in clip.x..clip.x + clip.w {
                            let lx = (x - tip.x) / scale;
                            let ly = (y - tip.y) / scale;
                            let is_tip_border = lx == 0 || lx == label_w / scale - 1 || ly == 0 || ly == 23;
                            s.pixels[y * s.width + x] = if is_tip_border {
                                theme::BORDER
                            } else if lx >= 10 && (4..20).contains(&ly) {
                                theme::text_pixel(label, lx - 10, ly - 4, 0xffffffff, 0xff282828)
                            } else {
                                0xff282828
                            };
                        }
                    }
                }
            }

            // Minimized windows remain accessible even when all clients are hidden.
            for w in &self.windows {
                if !w.is_minimized {
                    continue;
                }
                let task = Self::task_rect(w.id, s.height);
                if let Some(clip) = task.intersection(r) {
                    for y in clip.y..clip.y + clip.h {
                        for x in clip.x..clip.x + clip.w {
                            s.pixels[y * s.width + x] =
                                if x >= task.x + 8 && (4..20).contains(&(y - task.y)) {
                                    label_pixel(
                                        &w.title,
                                        x - task.x - 8,
                                        y - task.y - 4,
                                        TEXT,
                                        INACTIVE,
                                    )
                                } else {
                                    INACTIVE
                                };
                        }
                    }
                }
            }

            // Windows-Style Start Menu Popup / Application Launcher
            if self.start_open {
                let start_menu = Self::start_menu_rect(s.height);
                if let Some(clip) = start_menu.intersection(r) {
                    let bw = start_menu.w / scale;
                    let bh = start_menu.h / scale;
                    let (items, count) = self.matching_launcher_items();
                    let q_str = self.launcher_search.as_str();
                    for y in clip.y..clip.y + clip.h {
                        for x in clip.x..clip.x + clip.w {
                            let px = (x - start_menu.x) / scale;
                            let py = (y - start_menu.y) / scale;
                            let is_border = px == 0
                                || px == bw - 1
                                || py == 0
                                || py == bh - 1;
                            s.pixels[y * s.width + x] = if is_border {
                                theme::BORDER
                            } else if py < 24 {
                                if py == 23 {
                                    theme::BORDER
                                } else {
                                    // Search Bar in header (px in 8..bw-8, py in 4..20)
                                    let is_search_box = px >= 8 && px < bw - 8 && py >= 4 && py <= 20;
                                    let is_search_border = is_search_box
                                        && (px == 8 || px == bw - 9 || py == 4 || py == 20);
                                    if is_search_border {
                                        if !q_str.is_empty() {
                                            theme::ACCENT
                                        } else {
                                            theme::BORDER
                                        }
                                    } else if is_search_box {
                                        let box_bg = 0xff161b22;
                                        let gx = px.saturating_sub(12);
                                        let gy = py.saturating_sub(8);
                                        let in_glass = px >= 12 && px < 20 && (8..16).contains(&py)
                                            && (((gx == 1 || gx == 4) && (1..=4).contains(&gy))
                                                || ((gy == 1 || gy == 4) && (1..=4).contains(&gx))
                                                || (gx == 5 && gy == 5)
                                                || (gx == 6 && gy == 6));
                                        if in_glass {
                                            0xff8b949e
                                        } else if q_str.is_empty() {
                                            if px >= 24 && px < bw - 12 && (4..20).contains(&py) {
                                                theme::text_pixel("Search...", px - 24, py - 4, theme::MUTED, box_bg)
                                            } else {
                                                box_bg
                                            }
                                        } else {
                                            let text_len = q_str.len();
                                            let cursor_x = 24 + text_len * 8;
                                            if px == cursor_x && (6..18).contains(&py) {
                                                theme::ACCENT
                                            } else if px >= 24 && px < bw - 12 && (4..20).contains(&py) {
                                                theme::text_pixel(q_str, px - 24, py - 4, theme::TEXT, box_bg)
                                            } else {
                                                box_bg
                                            }
                                        }
                                    } else {
                                        0xff202022
                                    }
                                }
                            } else if count == 0 {
                                // Empty state when query matches no apps
                                if (48..64).contains(&py) && px >= 16 && px < bw - 16 {
                                    theme::text_pixel("No applications found", px - 16, py - 48, theme::TEXT, 0xff202022)
                                } else if (68..84).contains(&py) && px >= 16 && px < bw - 16 {
                                    theme::text_pixel("Check spelling or search terms", px - 16, py - 68, theme::MUTED, 0xff202022)
                                } else {
                                    0xff202022
                                }
                            } else {
                                let item_idx = (py - 24) / 22;
                                let item_y = (py - 24) % 22;
                                if item_idx < count {
                                    let is_hover = self.menu_hover == Some(item_idx)
                                        || (self.menu_hover.is_none() && self.launcher_selected == item_idx);
                                    let item_bg = if is_hover && px >= 4 && px < bw - 4 {
                                        0xff3a3a3a
                                    } else {
                                        0xff282828
                                    };

                                    // Divider line above session actions (when showing full menu)
                                    let is_divider = q_str.is_empty() && item_idx == 7 && item_y == 0;
                                    if is_divider {
                                        theme::BORDER
                                    } else {
                                        let item = items[item_idx];
                                        let icon_pixel = if px >= 10 && px < 24 && (4..18).contains(&item_y) {
                                            let ix = px - 10;
                                            let iy = item_y - 4;
                                            match item {
                                                Some(LauncherItem::App(app)) => {
                                                    match app.icon_kind {
                                                        0 => {
                                                            // Files: golden folder
                                                            if (2..=5).contains(&ix) && (2..=3).contains(&iy) {
                                                                Some(0xffd29922)
                                                            } else if (1..=12).contains(&ix) && (4..=11).contains(&iy) {
                                                                Some(if iy >= 6 { 0xfff0b232 } else { 0xffe3b341 })
                                                            } else {
                                                                None
                                                            }
                                                        }
                                                        1 => {
                                                            // Terminal: tile with '>_' prompt
                                                            if (iy == 4 && ix == 4) || (iy == 5 && ix == 5) || (iy == 6 && ix == 6)
                                                                || (iy == 7 && ix == 5) || (iy == 8 && ix == 4)
                                                                || (iy == 9 && (7..=10).contains(&ix))
                                                            {
                                                                Some(0xff56d364)
                                                            } else if ix == 1 || ix == 12 || iy == 1 || iy == 12 {
                                                                Some(0xff30363d)
                                                            } else if ix > 1 && ix < 12 && iy > 1 && iy < 12 {
                                                                Some(0xff0d1117)
                                                            } else {
                                                                None
                                                            }
                                                        }
                                                        2 => {
                                                            // Notepad: document sheet with lines
                                                            if (3..=10).contains(&ix) && (2..=11).contains(&iy) {
                                                                if (iy == 4 || iy == 6 || iy == 8) && (5..=8).contains(&ix) {
                                                                    Some(0xff1f6feb)
                                                                } else {
                                                                    Some(0xffffffff)
                                                                }
                                                            } else if (2..=11).contains(&ix) && (1..=12).contains(&iy) {
                                                                Some(0xff1f6feb)
                                                            } else {
                                                                None
                                                            }
                                                        }
                                                        3 => {
                                                            // Calculator: blue tile with math symbols
                                                            if (2..=11).contains(&ix) && (2..=11).contains(&iy) {
                                                                if (iy == 4 && (4..=6).contains(&ix)) || (ix == 5 && (3..=5).contains(&iy)) {
                                                                    Some(0xffffffff)
                                                                } else if iy == 9 && (4..=6).contains(&ix) {
                                                                    Some(0xffffffff)
                                                                } else if (iy == 4 && (8..=10).contains(&ix)) || (iy == 9 && (8..=10).contains(&ix)) {
                                                                    Some(0xff56d364)
                                                                } else if ix == 2 || ix == 11 || iy == 2 || iy == 11 {
                                                                    Some(0xff30363d)
                                                                } else {
                                                                    Some(0xff1f6feb)
                                                                }
                                                            } else {
                                                                None
                                                            }
                                                        }
                                                        4 => {
                                                            // Vault: Golden Padlock
                                                            if (4..=9).contains(&ix) && (2..=5).contains(&iy) && !(6..=7).contains(&ix) && iy < 5 {
                                                                Some(0xffc9d1d9)
                                                            } else if (4..=9).contains(&ix) && iy == 2 {
                                                                Some(0xffc9d1d9)
                                                            } else if (3..=10).contains(&ix) && (5..=12).contains(&iy) {
                                                                if ix == 6 && (7..=9).contains(&iy) {
                                                                    Some(0xff161b22)
                                                                } else if ix == 3 || ix == 10 || iy == 5 || iy == 12 {
                                                                    Some(0xffb08800)
                                                                } else {
                                                                    Some(0xffe3b341)
                                                                }
                                                            } else {
                                                                None
                                                            }
                                                        }
                                                        5 => {
                                                            // Settings: Gear icon
                                                            let dx = (ix as i32 - 7).abs();
                                                            let dy = (iy as i32 - 7).abs();
                                                            if dx <= 1 && dy <= 1 {
                                                                Some(item_bg)
                                                            } else if (dx <= 4 && dy <= 4 && (dx >= 2 || dy >= 2))
                                                                || ((dx == 5 && dy <= 1) || (dy == 5 && dx <= 1))
                                                            {
                                                                Some(0xff60a5fa)
                                                            } else {
                                                                None
                                                            }
                                                        }
                                                        6 => {
                                                            // System Information: CPU / chip
                                                            if (5..=8).contains(&ix) && (5..=8).contains(&iy) {
                                                                Some(0xffd2a8ff)
                                                            } else if (3..=10).contains(&ix) && (3..=10).contains(&iy) {
                                                                Some(0xff8957e5)
                                                            } else if ((iy == 1 || iy == 12) && (ix == 4 || ix == 6 || ix == 9))
                                                                || ((ix == 1 || ix == 12) && (iy == 4 || iy == 6 || iy == 9))
                                                            {
                                                                Some(0xffa371f7)
                                                            } else {
                                                                None
                                                            }
                                                        }
                                                        _ => None,
                                                    }
                                                }
                                                Some(LauncherItem::Exit) => {
                                                    // Exit: Power glyph
                                                    let dist_sq = (ix as i32 - 7).pow(2) + (iy as i32 - 7).pow(2);
                                                    if dist_sq >= 8 && dist_sq <= 22 && !(ix == 7 && iy <= 7) {
                                                        Some(0xffff6b6b)
                                                    } else if ix == 7 && (2..=6).contains(&iy) {
                                                        Some(0xffff6b6b)
                                                    } else {
                                                        None
                                                    }
                                                }
                                                Some(LauncherItem::Restart) => {
                                                    // Restart: circular arrow
                                                    let dist_sq = (ix as i32 - 7).pow(2) + (iy as i32 - 7).pow(2);
                                                    if dist_sq >= 8 && dist_sq <= 20 && !(ix <= 7 && iy >= 7) {
                                                        Some(0xffe3b341)
                                                    } else if (iy == 3 || iy == 4) && ix == 7 {
                                                        Some(0xffe3b341)
                                                    } else {
                                                        None
                                                    }
                                                }
                                                Some(LauncherItem::Lock) => {
                                                    // Lock Screen: Padlock glyph
                                                    if (4..=9).contains(&ix) && (2..=5).contains(&iy) && !(6..=7).contains(&ix) && iy < 5 {
                                                        Some(0xffc9d1d9)
                                                    } else if (4..=9).contains(&ix) && iy == 2 {
                                                        Some(0xffc9d1d9)
                                                    } else if (3..=10).contains(&ix) && (5..=12).contains(&iy) {
                                                        if ix == 6 && (7..=9).contains(&iy) {
                                                            Some(0xff161b22)
                                                        } else if ix == 3 || ix == 10 || iy == 5 || iy == 12 {
                                                            Some(0xffb08800)
                                                        } else {
                                                            Some(0xff56d364)
                                                        }
                                                    } else {
                                                        None
                                                    }
                                                }
                                                None => None,
                                            }
                                        } else {
                                            None
                                        };

                                        if let Some(p) = icon_pixel {
                                            p
                                        } else {
                                            let (label, is_danger) = match item {
                                                Some(LauncherItem::App(app)) => (app.name, false),
                                                Some(LauncherItem::Exit) => ("Exit to Console", true),
                                                Some(LauncherItem::Restart) => ("Restart", false),
                                                Some(LauncherItem::Lock) => ("Lock Screen", false),
                                                None => ("", false),
                                            };
                                            let fg = if is_hover {
                                                0xffffffff
                                            } else if is_danger {
                                                0xffff7b72
                                            } else {
                                                theme::TEXT
                                            };
                                            if !label.is_empty() && px >= 28 && px < bw - 8 && (3..19).contains(&item_y) {
                                                theme::text_pixel(label, px - 28, item_y - 3, fg, item_bg)
                                            } else {
                                                item_bg
                                            }
                                        }
                                    }
                                } else {
                                    0xff202022
                                }
                            };
                        }
                    }
                }
            }

            // Windows Acrylic Desktop Context Menu
            if let Some((mx, my)) = self.context_menu {
                let menu_rect = Self::context_menu_rect(mx, my, s.height);
                if let Some(clip) = menu_rect.intersection(r) {
                    let bw = menu_rect.w / scale;
                    let bh = menu_rect.h / scale;
                    for y in clip.y..clip.y + clip.h {
                        for x in clip.x..clip.x + clip.w {
                            let px = (x - menu_rect.x) / scale;
                            let py = (y - menu_rect.y) / scale;
                            let is_border = px == 0 || px == bw - 1 || py == 0 || py == bh - 1;
                            s.pixels[y * s.width + x] = if is_border {
                                theme::BORDER
                            } else if py == 45 || py == 68 {
                                if px >= 8 && px < bw - 8 {
                                    theme::BORDER
                                } else {
                                    0xff282828
                                }
                            } else {
                                let item = Self::context_item_at(px, py);
                                let is_hover = item.is_some() && item == self.context_hover;
                                let item_bg = if is_hover && px >= 4 && px < bw - 4 {
                                    0xff3a3a3a
                                } else {
                                    0xff282828
                                };

                                let (item_idx, item_y) = match item {
                                    Some(0) => (0, py - 4),
                                    Some(1) => (1, py - 24),
                                    Some(2) => (2, py - 47),
                                    Some(3) => (3, py - 70),
                                    Some(4) => (4, py - 90),
                                    _ => (99, 0),
                                };

                                let icon_pixel = if item_idx <= 4 && px >= 8 && px < 22 && (3..17).contains(&item_y) {
                                    let ix = px - 8;
                                    let iy = item_y - 3;
                                    match item_idx {
                                        0 => {
                                            if (2..=10).contains(&ix) && (1..=12).contains(&iy) {
                                                if ix >= 8 && iy <= 3 {
                                                    Some(0xff8b949e)
                                                } else if (iy == 5 || iy == 7 || iy == 9) && (4..=7).contains(&ix) {
                                                    Some(0xff38bdf8)
                                                } else {
                                                    Some(0xffffffff)
                                                }
                                            } else {
                                                None
                                            }
                                        }
                                        1 => {
                                            if (2..=5).contains(&ix) && (2..=3).contains(&iy) {
                                                Some(0xffd29922)
                                            } else if (1..=12).contains(&ix) && (4..=11).contains(&iy) {
                                                Some(if iy >= 6 { 0xfff0b232 } else { 0xffe3b341 })
                                            } else {
                                                None
                                            }
                                        }
                                        2 => {
                                            if (iy == 4 && ix == 4) || (iy == 5 && ix == 5) || (iy == 6 && ix == 6)
                                                || (iy == 7 && ix == 5) || (iy == 8 && ix == 4)
                                                || (iy == 9 && (7..=10).contains(&ix))
                                            {
                                                Some(0xff56d364)
                                            } else if ix == 1 || ix == 12 || iy == 1 || iy == 12 {
                                                Some(0xff30363d)
                                            } else if ix > 1 && ix < 12 && iy > 1 && iy < 12 {
                                                Some(0xff0d1117)
                                            } else {
                                                None
                                            }
                                        }
                                        3 => {
                                            if (ix == 1 || ix == 12) && (2..=9).contains(&iy)
                                                || (iy == 2 || iy == 9) && (1..=12).contains(&ix)
                                            {
                                                Some(0xff38bdf8)
                                            } else if ix >= 2 && ix <= 11 && iy >= 3 && iy <= 8 {
                                                Some(0xff0f172a)
                                            } else if (ix == 6 || ix == 7) && (10..=11).contains(&iy)
                                                || iy == 12 && (4..=9).contains(&ix)
                                            {
                                                Some(0xff94a3b8)
                                            } else {
                                                None
                                            }
                                        }
                                        4 => {
                                            let dx = (ix as i32 - 7).abs();
                                            let dy = (iy as i32 - 7).abs();
                                            let dist_sq = dx * dx + dy * dy;
                                            if dist_sq <= 25 {
                                                if ix == 4 && iy == 4 {
                                                    Some(0xff38bdf8)
                                                } else if ix == 8 && iy == 4 {
                                                    Some(0xfff43f5e)
                                                } else if ix == 9 && iy == 7 {
                                                    Some(0xffeab308)
                                                } else if ix == 5 && iy == 8 {
                                                    Some(item_bg)
                                                } else {
                                                    Some(0xff8b5cf6)
                                                }
                                            } else {
                                                None
                                            }
                                        }
                                        _ => None,
                                    }
                                } else {
                                    None
                                };

                                if let Some(p) = icon_pixel {
                                    p
                                } else if item_idx <= 4 {
                                    let label = match item_idx {
                                        0 => "New File",
                                        1 => "New Folder",
                                        2 => "Open in Terminal",
                                        3 => "Display Settings",
                                        4 => "Personalize",
                                        _ => "",
                                    };
                                    let fg = if is_hover { 0xffffffff } else { theme::TEXT };
                                    if !label.is_empty() && px >= 28 && px < bw - 6 && (2..18).contains(&item_y) {
                                        theme::text_pixel(label, px - 28, item_y - 2, fg, item_bg)
                                    } else {
                                        item_bg
                                    }
                                } else {
                                    item_bg
                                }
                            };
                        }
                    }
                }
            }

            // Centered Alt+Tab Window Switcher Glass HUD
            if self.switcher_open && !self.windows.is_empty() {
                let (ids, count) = self.switcher_ids();
                let card = Self::switcher_card_rect(s.width, s.height, count);
                let shadow = Rect {
                    x: card.x.saturating_sub(4 * scale),
                    y: card.y + card.h,
                    w: (card.w + 8 * scale).min(s.width.saturating_sub(card.x.saturating_sub(4 * scale))),
                    h: (6 * scale).min(s.height.saturating_sub(card.y + card.h)),
                };
                if let Some(clip) = shadow.intersection(r) {
                    for y in clip.y..clip.y + clip.h {
                        let dy = (y - (card.y + card.h)) / scale + 1;
                        let factor = ((7usize.saturating_sub(dy)) * 14) as u32;
                        for x in clip.x..clip.x + clip.w {
                            s.pixels[y * s.width + x] = blend_darken(s.pixels[y * s.width + x], factor);
                        }
                    }
                }
                if let Some(clip) = card.intersection(r) {
                    let bw = card.w / scale;
                    let bh = card.h / scale;
                    let selected_id = if self.switcher_selected != 0 {
                        self.switcher_selected
                    } else {
                        self.focused_window_id().unwrap_or(ids[0])
                    };
                    let selected_title = self
                        .windows
                        .iter()
                        .find(|w| w.id == selected_id)
                        .map(|w| w.title.as_str())
                        .unwrap_or("Desktop");
                    let title_w = theme::text_width(selected_title);
                    let title_x = bw.saturating_sub(title_w) / 2;
                    let tiles_total_w = count * 140 - 12;
                    let tiles_start_x = bw.saturating_sub(tiles_total_w) / 2;

                    for y in clip.y..clip.y + clip.h {
                        for x in clip.x..clip.x + clip.w {
                            let cov = theme::rounded_coverage(
                                x - card.x,
                                y - card.y,
                                card.w,
                                card.h,
                                14 * scale,
                            );
                            if cov == 0 {
                                continue;
                            }
                            let under = s.pixels[y * s.width + x];
                            let px = (x - card.x) / scale;
                            let py = (y - card.y) / scale;
                            // Semi-transparent frosted glass backdrop: micro-grain diffusion over darkened scene
                            let frost_tint = if ((px ^ py) & 1) == 0 {
                                0xff181e29
                            } else {
                                0xff141922
                            };
                            let glass_base = theme::blend_coverage(frost_tint, blend_darken(under, 110), 14);
                            let is_border = px == 0 || px == bw - 1 || py == 0 || py == bh - 1;

                            let pixel = if py == 0 {
                                0xff60a5fa
                            } else if is_border {
                                0xff3b475e
                            } else if py == 1 {
                                0xff2b3548
                            } else if (8..26).contains(&py) {
                                if px >= 16 && px < bw.saturating_sub(16) {
                                    theme::text_pixel_bold(
                                        "Switch Windows  (Alt + Tab)",
                                        px - 16,
                                        py - 8,
                                        0xffd5deeb,
                                        glass_base,
                                    )
                                } else {
                                    glass_base
                                }
                            } else if (32..148).contains(&py) && px >= tiles_start_x {
                                let rel_x = px - tiles_start_x;
                                let idx = rel_x / 140;
                                let tx = rel_x % 140;
                                let ty = py - 32;
                                if idx < count && tx < 128 {
                                    let wid = ids[idx];
                                    let is_sel = wid == selected_id;
                                    let tile_cov = theme::rounded_coverage(tx, ty, 128, 116, 12);
                                    if tile_cov == 0 {
                                        glass_base
                                    } else {
                                        let tile_border = tx <= 1 || tx >= 126 || ty <= 1 || ty >= 114;
                                        let tile_bg = if is_sel { 0xff1e3a5f } else { 0xff1d2330 };
                                        let icon_kind = self
                                            .windows
                                            .iter()
                                            .find(|w| w.id == wid)
                                            .map(window_icon_kind)
                                            .unwrap_or(4);
                                        let inner = if ty == 0 {
                                            if is_sel { 0xff7dd3fc } else { 0xff3b475c }
                                        } else if tile_border {
                                            if is_sel { 0xff38bdf8 } else { 0xff2d3646 }
                                        } else if (36..92).contains(&tx) && (12..68).contains(&ty) {
                                            render_dock_icon_scaled(
                                                icon_kind,
                                                tx - 36,
                                                ty - 12,
                                                56,
                                                tile_bg,
                                            )
                                        } else if (78..96).contains(&ty) && (8..120).contains(&tx) {
                                            let app_label = match icon_kind {
                                                0 => "Files",
                                                1 => "Terminal",
                                                2 => "Notepad",
                                                3 => "System Info",
                                                _ => "Settings",
                                            };
                                            let sw = theme::text_width(app_label);
                                            let sx = 64usize.saturating_sub(sw / 2).max(8);
                                            if tx >= sx {
                                                if is_sel {
                                                    theme::text_pixel_bold(
                                                        app_label,
                                                        tx - sx,
                                                        ty - 78,
                                                        0xffffffff,
                                                        tile_bg,
                                                    )
                                                } else {
                                                    theme::text_pixel(
                                                        app_label,
                                                        tx - sx,
                                                        ty - 78,
                                                        0xffb8c7dc,
                                                        tile_bg,
                                                    )
                                                }
                                            } else {
                                                tile_bg
                                            }
                                        } else if is_sel && (104..107).contains(&ty) && (44..84).contains(&tx) {
                                            0xff38bdf8
                                        } else {
                                            tile_bg
                                        };
                                        theme::blend_coverage(inner, glass_base, tile_cov)
                                    }
                                } else {
                                    glass_base
                                }
                            } else if py == 156 && px >= 16 && px < bw.saturating_sub(16) {
                                0xff2a3344
                            } else if (164..182).contains(&py) && px >= title_x.max(16) && px < bw.saturating_sub(16) {
                                theme::text_pixel_bold(
                                    selected_title,
                                    px - title_x.max(16),
                                    py - 164,
                                    0xffffffff,
                                    glass_base,
                                )
                            } else {
                                glass_base
                            };

                            s.pixels[y * s.width + x] = theme::blend_coverage(pixel, under, cov);
                        }
                    }
                }
            }

            // Floating Glassmorphic Power Menu Card
            if self.show_power_menu && self.battery_status.is_some() {
                let bat = self.battery_status.as_ref().unwrap();
                let menu_rect = Self::power_menu_rect(s.width, s.height);
                if let Some(clip) = menu_rect.intersection(r) {
                    let bw = menu_rect.w / scale;
                    let bh = menu_rect.h / scale;

                    // Format percentage string, e.g. "85%"
                    let mut pct_buf = [0u8; 8];
                    let cap = bat.capacity_percent.min(100);
                    let pct_len = if cap >= 100 {
                        pct_buf[0] = b'1'; pct_buf[1] = b'0'; pct_buf[2] = b'0'; pct_buf[3] = b'%';
                        4
                    } else if cap >= 10 {
                        pct_buf[0] = b'0' + (cap / 10) as u8;
                        pct_buf[1] = b'0' + (cap % 10) as u8;
                        pct_buf[2] = b'%';
                        3
                    } else {
                        pct_buf[0] = b'0' + cap as u8;
                        pct_buf[1] = b'%';
                        2
                    };
                    let pct_str = core::str::from_utf8(&pct_buf[..pct_len]).unwrap_or("0%");

                    // Format subtext: "2h 15m remaining" / "Fully Charged" / "Charging"
                    let mut sub_buf = [0u8; 32];
                    let sub_str = if bat.state == crate::sys::BATTERY_STATE_CHARGING {
                        "Charging"
                    } else if bat.state == crate::sys::BATTERY_STATE_FULL {
                        "Fully Charged"
                    } else if bat.time_remaining_mins > 0 {
                        let h = bat.time_remaining_mins / 60;
                        let m = bat.time_remaining_mins % 60;
                        let mut idx = 0;
                        if h >= 10 { sub_buf[idx] = b'0' + (h / 10) as u8; idx += 1; }
                        sub_buf[idx] = b'0' + (h % 10) as u8; idx += 1;
                        sub_buf[idx] = b'h'; idx += 1;
                        sub_buf[idx] = b' '; idx += 1;
                        if m >= 10 { sub_buf[idx] = b'0' + (m / 10) as u8; idx += 1; }
                        sub_buf[idx] = b'0' + (m % 10) as u8; idx += 1;
                        sub_buf[idx] = b'm'; idx += 1;
                        for &b in b" remaining" { sub_buf[idx] = b; idx += 1; }
                        core::str::from_utf8(&sub_buf[..idx]).unwrap_or("Discharging")
                    } else {
                        "Discharging"
                    };

                    for y in clip.y..clip.y + clip.h {
                        for x in clip.x..clip.x + clip.w {
                            let px = (x - menu_rect.x) / scale;
                            let py = (y - menu_rect.y) / scale;

                            let is_border = px == 0 || px == bw - 1 || py == 0 || py == bh - 1;
                            s.pixels[y * s.width + x] = if is_border {
                                theme::BORDER
                            } else if (10..28).contains(&py) && px >= 16 && px < bw - 16 {
                                // Large text: "85%"
                                theme::text_pixel_bold(pct_str, px - 16, py - 10, 0xffffffff, 0xff1c2128)
                            } else if (32..48).contains(&py) && px >= 16 && px < bw - 16 {
                                // Subtext: "2h 15m remaining" / "Charging"
                                theme::text_pixel(sub_str, px - 16, py - 32, theme::MUTED, 0xff1c2128)
                            } else if (52..62).contains(&py) && px >= 16 && px < bw - 16 {
                                // Visual progress bar
                                let bar_w = bw.saturating_sub(32);
                                let rel_x = px - 16;
                                let rel_y = py - 52;
                                let bar_border = rel_x == 0 || rel_x == bar_w - 1 || rel_y == 0 || rel_y == 9;
                                if bar_border {
                                    0xff334155
                                } else {
                                    let fill_w = ((cap as usize) * (bar_w - 2)) / 100;
                                    if rel_x - 1 < fill_w {
                                        if cap < 10 {
                                            theme::RED
                                        } else if cap < 20 {
                                            theme::AMBER
                                        } else {
                                            theme::ACCENT_BLUE
                                        }
                                    } else {
                                        0xff222a36
                                    }
                                }
                            } else if (74..102).contains(&py) && px >= 16 && px < bw - 16 {
                                // Segmented toggle: Performance vs Battery Saver
                                let btn_w = (bw.saturating_sub(36)) / 2;
                                let rel_y = py - 74;
                                let is_left = px >= 16 && px < 16 + btn_w;
                                let is_right = px >= 18 + btn_w && px < 18 + 2 * btn_w;

                                if is_left {
                                    let active = self.power_profile == 0;
                                    let btn_bg = if active { theme::ACCENT } else { 0xff252c38 };
                                    let btn_fg = if active { 0xffffffff } else { theme::MUTED };
                                    let border_btn = px == 16 || px == 15 + btn_w || rel_y == 0 || rel_y == 27;
                                    if border_btn {
                                        0xff3a4556
                                    } else if (5..23).contains(&rel_y) && px >= 20 && px < 14 + btn_w {
                                        theme::text_pixel("Performance", px - 20, rel_y - 5, btn_fg, btn_bg)
                                    } else {
                                        btn_bg
                                    }
                                } else if is_right {
                                    let active = self.power_profile == 1;
                                    let btn_bg = if active { theme::ACCENT } else { 0xff252c38 };
                                    let btn_fg = if active { 0xffffffff } else { theme::MUTED };
                                    let border_btn = px == 18 + btn_w || px == 17 + 2 * btn_w || rel_y == 0 || rel_y == 27;
                                    if border_btn {
                                        0xff3a4556
                                    } else if (5..23).contains(&rel_y) && px >= 24 + btn_w && px < 16 + 2 * btn_w {
                                        theme::text_pixel("Battery Saver", px - (24 + btn_w), rel_y - 5, btn_fg, btn_bg)
                                    } else {
                                        btn_bg
                                    }
                                } else {
                                    0xff1c2128
                                }
                            } else if py == 106 && px >= 16 && px < bw - 16 {
                                0xff2c313c
                            } else if (110..128).contains(&py) && px >= 16 && px < bw - 16 {
                                theme::text_pixel("Power Settings...", px - 16, py - 110, theme::ACCENT, 0xff1c2128)
                            } else {
                                0xff1c2128
                            };
                        }
                    }
                }
            }

            if self.locked {
                // 75% dark frosted glass scrim over this damage rectangle
                for y in r.y..r.y + r.h {
                    for x in r.x..r.x + r.w {
                        let under = s.pixels[y * s.width + x];
                        let dimmed = blend_darken(under, 192); // 75% dark
                        let dither = if ((x ^ y) & 1) == 0 { 0xff101520 } else { 0xff0d1118 };
                        s.pixels[y * s.width + x] = blend_color(dither, dimmed, 50);
                    }
                }

                // Centered Unlock Card (360 x 220)
                let cw = 360.min(s.width - 40);
                let ch = 220.min(s.height - 40);
                let cx = (s.width - cw) / 2;
                let cy = (s.height - ch) / 2;
                let card_r = Rect { x: cx, y: cy, w: cw, h: ch };

                if let Some(clip) = card_r.intersection(r) {
                    let card_bg = 0xff161b22;
                    let card_border = 0xff30363d;
                    for y in clip.y..clip.y + clip.h {
                        for x in clip.x..clip.x + clip.w {
                            let lx = x - cx;
                            let ly = y - cy;
                            let cov = theme::rounded_coverage(lx, ly, cw, ch, 12);
                            if cov == 0 {
                                continue;
                            }
                            let under = s.pixels[y * s.width + x];
                            let is_border = lx == 0 || lx + 1 == cw || ly == 0 || ly + 1 == ch;
                            let bg = if is_border { card_border } else { card_bg };
                            s.pixels[y * s.width + x] = theme::blend_coverage(bg, under, cov);
                        }
                    }

                    // Render card content
                    let mut clock_buf = [0u8; 32];
                    let clock_str = format_clock(self.clock_sec, self.clock_24h, &mut clock_buf);

                    for y in clip.y..clip.y + clip.h {
                        for x in clip.x..clip.x + clip.w {
                            let lx = x - cx;
                            let ly = y - cy;

                            // Title: "FortressOS Locked" (cx + 32, cy + 20)
                            if (20..38).contains(&ly) && lx >= 32 && lx < cw - 32 {
                                let p = theme::text_pixel_bold("FortressOS Locked", lx - 32, ly - 20, 0xffffffff, card_bg);
                                if p != card_bg { s.pixels[y * s.width + x] = p; }
                            }
                            // Clock subtitle (cx + 32, cy + 44)
                            else if (44..62).contains(&ly) && lx >= 32 && lx < cw - 32 {
                                let p = theme::text_pixel(clock_str, lx - 32, ly - 44, theme::MUTED, card_bg);
                                if p != card_bg { s.pixels[y * s.width + x] = p; }
                            }
                            // Error banner if any (cx + 32, cy + 66)
                            else if self.lock_error && (66..84).contains(&ly) && lx >= 32 && lx < cw - 32 {
                                let p = theme::text_pixel("Incorrect password", lx - 32, ly - 66, theme::RED, card_bg);
                                if p != card_bg { s.pixels[y * s.width + x] = p; }
                            }
                            // Password text field well (cx + 32, cy + 90, w: cw - 64, h: 34)
                            else if (90..124).contains(&ly) && (32..cw - 32).contains(&lx) {
                                let fx = lx - 32;
                                let fy = ly - 90;
                                let fw = cw - 64;
                                let fh = 34;
                                let fcov = theme::rounded_coverage(fx, fy, fw, fh, 6);
                                let is_fborder = fx == 0 || fx + 1 == fw || fy == 0 || fy + 1 == fh;
                                let fbg = if is_fborder { 0xff2563eb } else { 0xff0d1117 };

                                let masked_buf = [b'*'; 64];
                                let plen = self.lock_password.len().min(64);
                                let masked_str = core::str::from_utf8(&masked_buf[..plen]).unwrap_or("");
                                let text_y = (fh.saturating_sub(18)) / 2;

                                let pixel = if fy >= text_y && fy < text_y + 18 && fx >= 10 && fx < fw - 10 {
                                    theme::text_pixel(masked_str, fx - 10, fy - text_y, 0xffffffff, fbg)
                                } else {
                                    fbg
                                };
                                s.pixels[y * s.width + x] = theme::blend_coverage(pixel, card_bg, fcov);
                            }
                            // Unlock Button (cx + 32, cy + 140, w: cw - 64, h: 36)
                            else if (140..176).contains(&ly) && (32..cw - 32).contains(&lx) {
                                let bx = lx - 32;
                                let by = ly - 140;
                                let bw = cw - 64;
                                let bh = 36;
                                let bcov = theme::rounded_coverage(bx, by, bw, bh, 6);
                                let btn_bg = 0xff2563eb;
                                let text_y = (bh.saturating_sub(18)) / 2;
                                let text_x = (bw.saturating_sub(theme::text_width("Unlock"))) / 2;

                                let pixel = if by >= text_y && by < text_y + 18 && bx >= text_x && bx < text_x + theme::text_width("Unlock") {
                                    theme::text_pixel("Unlock", bx - text_x, by - text_y, 0xffffffff, btn_bg)
                                } else {
                                    btn_bg
                                };
                                s.pixels[y * s.width + x] = theme::blend_coverage(pixel, card_bg, bcov);
                            }
                        }
                    }
                }
            }

            i += 1;
        }
    }
}
