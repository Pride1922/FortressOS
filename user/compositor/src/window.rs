//! Single-process window server. Stack order is back to front; no client IPC yet.
#[path = "theme.rs"]
pub mod theme;

use crate::{
    font::GLYPHS,
    surface::{Rect, Surface},
};
use alloc::{string::String, vec::Vec};
use core::fmt::{self, Write};

pub const DESKTOP: u32 = 0xff141923;
pub const ACTIVE: u32 = 0xff2a3a5e;
pub const INACTIVE: u32 = 0xff1e2533;
pub const BORDER: u32 = 0xff607089;
pub const CLIENT: u32 = 0xff202b3b;
pub const HOVER_CLOSE: u32 = 0xffa63c50;
pub const HOVER_MIN: u32 = 0xff486080;
const TEXT: u32 = 0xffe0e8ff;
const TITLE_HEIGHT: usize = 24;

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
    pub title: String,
    /// Outer bounds. Content starts at (1,25), excluding the titlebar and border.
    pub rect: Rect,
    pub content: Vec<u32>,
    pub is_focused: bool,
    pub is_dragging: bool,
    pub is_resizing: bool,
    pub drag_offset: (usize, usize),
    pub is_minimized: bool,
    pub bg_color: u32,
    pub saved_rect: Option<Rect>,
    panel_ready: bool,
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
        Some(Self {
            id,
            title: name,
            rect,
            content,
            is_focused: false,
            is_dragging: false,
            is_resizing: false,
            drag_offset: (0, 0),
            is_minimized: false,
            bg_color,
            saved_rect: None,
            panel_ready: false,
        })
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
            let reserve_target = len + len / 4;
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
        self.content.resize(len, self.bg_color);
        self.rect.w = new_w;
        self.rect.h = new_h;
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
                if x == 0 || x == self.rect.w - 1 || (is_maximized && y == self.rect.h - 1) {
                    return 0xff30363d;
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

                // Sleek dark window border (1px outline and rounded rim)
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
                    return 0xff30363d;
                }

                // Bottom-right resize grip
                let rx = self.rect.w.saturating_sub(x);
                let ry = self.rect.h.saturating_sub(y);
                if rx <= 10 && ry <= 10 && (rx + ry == 6 || rx + ry == 9 || rx + ry == 12) {
                    return if hover == Some(Hit::Resize) {
                        0xff58a6ff
                    } else {
                        0xff484f58
                    };
                }
            } else {
                if x == 0 || y == 0 || x == self.rect.w - 1 || y == self.rect.h - 1 {
                    return 0xff30363d;
                }
            }

            if y > TITLE_HEIGHT {
                return self.content[(y - 25) * (self.rect.w - 2) + x - 1];
            }

            let title_bg = if self.is_focused {
                0xff1c2128
            } else {
                0xff161b22
            };

            // Windows-style top-right window controls (Close, Maximize/Restore, Minimize)
            if x >= self.rect.w.saturating_sub(108) {
                let is_hover = |btn: Hit| hover == Some(btn);
                let fg = |btn: Hit| {
                    if is_hover(btn) {
                        0xffffffff
                    } else if self.is_focused {
                        0xffc9d1d9
                    } else {
                        0xff8b949e
                    }
                };

                // Close button (x in [w - 36, w - 1))
                if x >= self.rect.w.saturating_sub(36) {
                    let bg = if is_hover(Hit::Close) {
                        0xffe81123 // Windows red hover
                    } else if y == 1 {
                        0xff3d444d // top highlight when not hovered
                    } else {
                        title_bg
                    };
                    let bx = x - self.rect.w.saturating_sub(36); // 0..=34 (center is 17)
                    let dx = (bx as i32 - 17).abs();
                    let dy = (y as i32 - 12).abs();
                    // 10x10 crisp diagonal cross 'X'
                    if dx == dy && dx <= 4 {
                        return if is_hover(Hit::Close) {
                            0xffffffff
                        } else {
                            fg(Hit::Close)
                        };
                    }
                    return bg;
                }

                // Maximize / Restore button (x in [w - 72, w - 36))
                if x >= self.rect.w.saturating_sub(72) {
                    let bg = if is_hover(Hit::Maximize) {
                        0xff2d333b // Windows dark hover
                    } else if y == 1 {
                        0xff3d444d
                    } else {
                        title_bg
                    };
                    let bx = x - self.rect.w.saturating_sub(72); // 0..=35 (center is 18)
                    let icon_fg = fg(Hit::Maximize);

                    if is_maximized {
                        // Windows "Restore" overlapping squares icon
                        let in_back = (bx >= 15 && bx <= 23 && y == 7)
                            || (bx == 23 && y >= 7 && y <= 14)
                            || (bx >= 20 && bx <= 23 && y == 14)
                            || (bx == 15 && y >= 7 && y <= 10);
                        let in_front = (bx >= 13 && bx <= 20 && (y == 10 || y == 17))
                            || (y >= 10 && y <= 17 && (bx == 13 || bx == 20));
                        if in_back || in_front {
                            return icon_fg;
                        }
                    } else {
                        // Windows "Maximize" single square icon (10x10 outline: 13..=22, 7..=16)
                        let in_square = (bx >= 13 && bx <= 22 && (y == 7 || y == 16))
                            || (y >= 7 && y <= 16 && (bx == 13 || bx == 22));
                        if in_square {
                            return icon_fg;
                        }
                    }
                    return bg;
                }

                // Minimize button (x in [w - 108, w - 72))
                let bg = if is_hover(Hit::Minimize) {
                    0xff2d333b // Windows dark hover
                } else if y == 1 {
                    0xff3d444d
                } else {
                    title_bg
                };
                let bx = x - self.rect.w.saturating_sub(108); // 0..=35 (center is 18)
                // 10px horizontal bar '—' at y = 12
                if y == 12 && bx >= 13 && bx <= 22 {
                    return fg(Hit::Minimize);
                }
                return bg;
            }

            // Titlebar top 1px subtle glass highlight for non-button title area
            if y == 1 {
                return 0xff3d444d;
            }

            // Left-aligned clean title with Windows styling
            let title_len = theme::text_width(&self.title);
            let title_max_x = self.rect.w.saturating_sub(116);
            if x >= 12 && x < (12 + title_len).min(title_max_x) && (3..21).contains(&y) {
                return theme::text_pixel(&self.title, x - 12, y - 3, theme::TEXT, title_bg);
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

fn panel_text(
    w: &mut Window,
    s: &mut Surface,
    x: usize,
    y: usize,
    field_width: usize,
    text: &str,
    color: u32,
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
                theme::text_pixel(
                    text,
                    (px - left) / scale,
                    (py - top) / scale,
                    color,
                    theme::PANEL,
                )
            } else {
                theme::PANEL
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
            // Files (Finder): Blue tile with Folder silhouette
            if iy == 0 {
                return 0xff60a5fa;
            }
            if (6..=8).contains(&iy) && (6..=12).contains(&ix) {
                return 0xffffffff;
            }
            if (8..=19).contains(&iy) && (6..=21).contains(&ix) {
                return 0xffffffff;
            }
            if (12..=21).contains(&iy) && (5..=22).contains(&ix) {
                return if iy == 12 { 0xff93c5fd } else { 0xff60a5fa };
            }
            0xff2563eb
        }
        1 => {
            // Term: Dark graphite tile with prompt and cursor
            if iy == 0 {
                return 0xff3b4252;
            }
            if (3..=5).contains(&iy) {
                if (5..=6).contains(&ix) {
                    return 0xffff5f56;
                }
                if (8..=9).contains(&ix) {
                    return 0xffffbd2e;
                }
                if (11..=12).contains(&ix) {
                    return 0xff27c93f;
                }
                return 0xff2e3440;
            }
            // Prompt >
            if (ix == 7 && (iy == 10 || iy == 14))
                || (ix == 8 && (iy == 11 || iy == 13))
                || (ix == 9 && iy == 12)
            {
                return 0xff4ade80;
            }
            // Cursor _
            if (12..=17).contains(&ix) && (iy == 14 || iy == 15) {
                return 0xffffffff;
            }
            0xff1e222a
        }
        2 => {
            // Notes: Amber tile with paper sheet and ruled lines
            if iy == 0 {
                return 0xfffbbf24;
            }
            if (6..=21).contains(&ix) && (5..=22).contains(&iy) {
                if ix >= 17 && iy <= 8 && ix + iy >= 25 {
                    return 0xfffcd34d;
                }
                if (iy == 10 || iy == 13 || iy == 16) && (9..=18).contains(&ix) {
                    return 0xffb45309;
                }
                if ix == 8 && (7..=21).contains(&iy) {
                    return 0xfff87171;
                }
                return 0xfffffbeb;
            }
            0xfff59e0b
        }
        3 => {
            // Sysmon: Obsidian tile with glowing cyan ECG pulse wave
            if iy == 0 {
                return 0xff334155;
            }
            let is_pulse = (ix >= 4 && ix <= 8 && iy == 15)
                || (ix == 9 && (12..=15).contains(&iy))
                || (ix == 10 && (9..=13).contains(&iy))
                || (ix == 11 && (9..=18).contains(&iy))
                || (ix == 12 && (16..=21).contains(&iy))
                || (ix == 13 && (14..=18).contains(&iy))
                || (ix >= 14 && ix <= 23 && iy == 15);
            if is_pulse {
                return 0xff38bdf8;
            }
            if (ix == 10 && iy == 8) || (ix == 12 && iy == 22) {
                return 0xffa5f3fc;
            }
            0xff0f172a
        }
        _ => dock_bg,
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
    OpenDesktopApp(u32),
    WindowResized(u32),
    ClientClick(u32, usize, usize),
    Exit,
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
    #[allow(dead_code)]
    pub dock_hover: Option<usize>,
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
            dock_hover: None,
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
            dock_hover: None,
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
            h: 24 * theme::scale(height),
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
        Rect {
            x: 8,
            y: 0,
            w: 120 * theme::scale(height),
            h: 24 * theme::scale(height),
        }
    }

    pub fn start_menu_rect(height: usize) -> Rect {
        Rect {
            x: 8,
            y: 24 * theme::scale(height),
            w: 220 * theme::scale(height),
            h: 157 * theme::scale(height),
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
        Rect {
            x: width.saturating_sub(196 * scale),
            y: 0,
            w: 76 * scale,
            h: 24 * scale,
        }
    }

    pub fn invalidate_panels(&mut self) {
        for window in &mut self.windows {
            window.panel_ready = false;
        }
    }

    pub fn clock_rect(width: usize, height: usize) -> Rect {
        Rect {
            x: width.saturating_sub(120 * theme::scale(height)),
            y: 0,
            w: 120 * theme::scale(height),
            h: 24 * theme::scale(height),
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

    pub fn cycle_focus(&mut self, s: &mut Surface) {
        if self.windows.len() <= 1 {
            return;
        }
        let first_id = self.windows[0].id;
        self.raise(first_id, s);
    }

    #[allow(dead_code)]
    pub fn focused_window_id(&self) -> Option<u32> {
        self.windows
            .iter()
            .rev()
            .find(|w| w.is_focused && !w.is_minimized)
            .map(|w| w.id)
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
        s.damage(Self::tooltip_rect(s.width, s.height));
        s.damage(Rect {
            x: d.x.saturating_sub(4),
            y: d.y,
            w: d.w + 8,
            h: d.h + 8,
        });
        s.damage(Rect {
            x: 0,
            y: s.height.saturating_sub(32),
            w: s.width,
            h: 32,
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
        let mut ret_action = Action::None;
        // Capture survives leaving the titlebar/window. Apply final release motion.
        if self.buttons & 1 != 0 {
            for w in &mut self.windows {
                if w.is_dragging {
                    let old = w.damage_rect();
                    w.rect.x = x.saturating_sub(w.drag_offset.0).min(s.width - 1);
                    w.rect.y = y.saturating_sub(w.drag_offset.1).min(s.height - 1);
                    if old != w.damage_rect() {
                        s.damage(old.union(w.damage_rect()));
                    }
                } else if w.is_resizing {
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
        }
        if buttons & 1 == 0 {
            for w in &mut self.windows {
                if w.is_dragging && self.buttons & 1 != 0 && w.id >= 3 {
                    let scale = theme::scale(s.height);
                    let snap = if y <= 24 * scale {
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
            let dock_r = Self::dock_rect(s.width, s.height);
            let start_btn = Self::start_btn_rect(s.height);
            let start_menu = Self::start_menu_rect(s.height);

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
                match item_idx {
                    0 => ret_action = Action::SpawnFileManager,
                    1 => ret_action = Action::SpawnTerminal,
                    2 => ret_action = Action::SpawnNotepad,
                    3 => self.ensure_window_1(s),
                    _ => {}
                }
                Self::dock_damage(s);
                self.buttons = buttons;
                return ret_action;
            }

            if start_btn.contains(x, y) {
                self.start_open = !self.start_open;
                s.damage(start_btn);
                s.damage(start_menu);
                self.buttons = buttons;
                return Action::None;
            }

            if self.start_open {
                if start_menu.contains(x, y) {
                    let py = (y - start_menu.y) / theme::scale(s.height);
                    if py >= 24 {
                        let item_idx = (py - 24) / 22;
                        match item_idx {
                            0 => ret_action = Action::SpawnTerminal,
                            1 => ret_action = Action::SpawnNotepad,
                            2 => ret_action = Action::SpawnFileManager,
                            3 => self.ensure_window_1(s),
                            4 => ret_action = Action::OpenDesktopApp(7),
                            5 => ret_action = Action::Exit,
                            _ => {}
                        }
                    }
                }
                self.start_open = false;
                s.damage(start_btn);
                s.damage(start_menu);
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
            let new_menu_hover =
                if start_menu.contains(x, y) && y >= start_menu.y + 24 * theme::scale(s.height) {
                    let idx = ((y - start_menu.y) / theme::scale(s.height) - 24) / 22;
                    if idx < 6 { Some(idx) } else { None }
                } else {
                    None
                };
            if new_menu_hover != self.menu_hover {
                self.menu_hover = new_menu_hover;
                s.damage(start_menu);
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
        let field_width = (w.rect.w - 2) / scale;
        let mut line = Line::new();
        if initialize {
            w.content.fill(theme::PANEL);
            panel_text(
                w,
                s,
                24,
                18,
                field_width.saturating_sub(24),
                "FortressOS",
                theme::TEXT,
                scale,
            );
            panel_text(
                w,
                s,
                24,
                42,
                field_width.saturating_sub(24),
                "System Information",
                theme::MUTED,
                scale,
            );
            let _ = write!(line, "Display  {} x {}", s.width, s.height);
            panel_text(
                w,
                s,
                24,
                270,
                field_width.saturating_sub(24),
                line.as_str(),
                theme::MUTED,
                scale,
            );
        }
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
                panel_text(w, s, 24, 84 + i * 28, 190, name, theme::MUTED, scale);
                line.len = 0;
                let _ = write!(line, "{}{}", value, suffix);
                panel_text(
                    w,
                    s,
                    230,
                    84 + i * 28,
                    field_width.saturating_sub(230),
                    line.as_str(),
                    theme::TEXT,
                    scale,
                );
            }
        } else {
            for i in 0..6 {
                panel_text(
                    w,
                    s,
                    24,
                    84 + i * 28,
                    190,
                    if i == 0 { "Unavailable" } else { "" },
                    theme::MUTED,
                    scale,
                );
                panel_text(
                    w,
                    s,
                    230,
                    84 + i * 28,
                    field_width.saturating_sub(230),
                    "",
                    theme::TEXT,
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
            let top_maximized = self
                .windows
                .last()
                .and_then(|w| (!w.is_minimized && w.saved_rect.is_some()).then_some(w.rect));

            // Repair desktop first, skipping regions occluded by an opaque maximized window
            for y in r.y..r.y + r.h {
                for x in r.x..r.x + r.w {
                    if let Some(max_r) = top_maximized {
                        if max_r.contains(x, y) {
                            continue;
                        }
                    }
                    s.pixels[y * s.width + x] = if let Some(wp) = wallpaper {
                        wp.pixel(x, y, s.width, s.height)
                    } else if !self.demo_mode {
                        theme::desktop_pixel(x, y, s.width, s.height)
                    } else if (32..48).contains(&y) && x >= 32 {
                        label_pixel("FortressOS Rust Compositor", x - 32, y - 32, TEXT, DESKTOP)
                    } else {
                        DESKTOP
                    };
                }
            }

            let scale = theme::scale(s.height);
            let top_bar = Self::top_bar_rect(s.width, s.height);
            if let Some(clip) = top_bar.intersection(r) {
                for y in clip.y..clip.y + clip.h {
                    for x in clip.x..clip.x + clip.w {
                        let lx = x / scale;
                        let ly = y / scale;
                        let bg = if ly == 23 {
                            theme::BORDER
                        } else {
                            theme::PANEL
                        };
                        let logical_width = s.width / scale;
                        let is_start_btn = lx >= 6 && lx < 120 && ly < 23;
                        let start_bg = if self.start_open && is_start_btn {
                            0xff2d333b
                        } else {
                            bg
                        };
                        let in_win_logo = ((6..=10).contains(&ly) || (12..=16).contains(&ly))
                            && ((10..=13).contains(&lx) || (15..=18).contains(&lx));
                        let p = if in_win_logo {
                            0xff00a4ef
                        } else if (3..21).contains(&ly) && (23..120).contains(&lx) {
                            theme::text_pixel("FortressOS", lx - 23, ly - 3, theme::TEXT, start_bg)
                        } else if is_start_btn {
                            start_bg
                        } else if (3..21).contains(&ly)
                            && (logical_width.saturating_sub(196)
                                ..logical_width.saturating_sub(120))
                                .contains(&lx)
                        {
                            theme::text_pixel(
                                match self.network_state {
                                    2 => "Online",
                                    1 => "Offline",
                                    _ => "Network",
                                },
                                lx - logical_width.saturating_sub(196),
                                ly - 3,
                                theme::MUTED,
                                bg,
                            )
                        } else if (3..21).contains(&ly) && lx >= logical_width.saturating_sub(100) {
                            let mut bytes = *b"--:--   ";
                            if self.clock_sec >= 0 {
                                let seconds = self.clock_sec.rem_euclid(86400) as u32;
                                let hour = seconds / 3600;
                                let minute = seconds / 60 % 60;
                                let display = if self.clock_24h {
                                    hour
                                } else {
                                    let h = hour % 12;
                                    if h == 0 { 12 } else { h }
                                };
                                bytes[0] = b'0' + (display / 10) as u8;
                                bytes[1] = b'0' + (display % 10) as u8;
                                bytes[3] = b'0' + (minute / 10) as u8;
                                bytes[4] = b'0' + (minute % 10) as u8;
                                if !self.clock_24h {
                                    bytes[6] = if hour < 12 { b'A' } else { b'P' };
                                    bytes[7] = b'M';
                                }
                            }
                            theme::text_pixel(
                                core::str::from_utf8(&bytes).unwrap_or(""),
                                lx - logical_width.saturating_sub(100),
                                ly - 3,
                                theme::TEXT,
                                bg,
                            )
                        } else {
                            bg
                        };
                        s.pixels[y * s.width + x] = p;
                    }
                }
            }

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
                    for y in clip.y..clip.y + clip.h {
                        let in_vert_body = y >= w.rect.y + 3 && y < w.rect.y + w.rect.h + 3;
                        if in_vert_body {
                            // Left shadow fringe
                            let left_end = (w.rect.x).min(clip.x + clip.w);
                            for x in clip.x..left_end {
                                let d = w.rect.x - x;
                                if d <= 8 {
                                    let factor = ((9 - d) * 14) as u32;
                                    s.pixels[y * s.width + x] =
                                        blend_darken(s.pixels[y * s.width + x], factor);
                                }
                            }
                            // Right shadow fringe
                            let right_start = (w.rect.x + w.rect.w).max(clip.x);
                            for x in right_start..clip.x + clip.w {
                                let d = x - (w.rect.x + w.rect.w - 1);
                                if d <= 8 {
                                    let factor = ((9 - d) * 14) as u32;
                                    s.pixels[y * s.width + x] =
                                        blend_darken(s.pixels[y * s.width + x], factor);
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
                                if d > 0 && d <= 8 {
                                    let factor = ((9 - d) * 14) as u32;
                                    s.pixels[y * s.width + x] =
                                        blend_darken(s.pixels[y * s.width + x], factor);
                                }
                            }
                        }
                    }
                }
            }

            for w in &self.windows {
                if w.is_minimized {
                    continue;
                }
                if let Some(clip) = w.rect.intersection(r) {
                    let hover = self.hover.filter(|h| h.0 == w.id).map(|h| h.1);
                    for y in clip.y..clip.y + clip.h {
                        let wy = y - w.rect.y;
                        if wy > TITLE_HEIGHT && wy < w.rect.h - 1 {
                            let clip_start_x = clip.x.max(w.rect.x + 1);
                            let clip_end_x = (clip.x + clip.w).min(w.rect.x + w.rect.w - 1);
                            if clip_start_x < clip_end_x
                                && (w.saved_rect.is_some() || wy < w.rect.h.saturating_sub(8))
                            {
                                if clip.x <= w.rect.x {
                                    s.pixels[y * s.width + w.rect.x] = 0xff30363d;
                                }
                                let src_off =
                                    (wy - 25) * (w.rect.w - 2) + (clip_start_x - (w.rect.x + 1));
                                let copy_len = clip_end_x - clip_start_x;
                                let dst_off = y * s.width + clip_start_x;
                                s.pixels[dst_off..dst_off + copy_len]
                                    .copy_from_slice(&w.content[src_off..src_off + copy_len]);
                                if clip.x + clip.w >= w.rect.x + w.rect.w {
                                    s.pixels[y * s.width + w.rect.x + w.rect.w - 1] = 0xff30363d;
                                }
                                continue;
                            }
                        }
                        for x in clip.x..clip.x + clip.w {
                            let p = w.pixel(x - w.rect.x, wy, hover);
                            if p != 0 {
                                s.pixels[y * s.width + x] = p;
                            }
                        }
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
                        if coverage < 16 {
                            let under = s.pixels[y * s.width + x];
                            s.pixels[y * s.width + x] =
                                theme::blend_coverage(theme::PANEL, under, coverage);
                            continue;
                        }
                        let dock_bg = theme::PANEL;
                        let item_idx = px / 64;
                        let in_item_x = px % 64;

                        if item_idx < 4 {
                            let is_running = match item_idx {
                                0 => has_win5,
                                1 => has_win3,
                                2 => has_win4,
                                3 => has_win1,
                                _ => false,
                            };

                            let app_id = [5, 3, 4, if self.demo_mode { 1 } else { 6 }][item_idx];
                            let focused = self.focused_window_id() == Some(app_id);
                            if self.dock_hover == Some(item_idx)
                                && (6..52).contains(&py)
                                && (8..56).contains(&in_item_x)
                            {
                                s.pixels[y * s.width + x] = theme::SURFACE;
                            }
                            // Graphical icon tile: 28x28 centered in 64x58 item: in_item_x = 18..46, py = 12..40
                            if in_item_x >= 18 && in_item_x < 46 && (12..40).contains(&py) {
                                s.pixels[y * s.width + x] =
                                    render_dock_icon(item_idx, in_item_x - 18, py - 12, dock_bg);
                            } else if is_running
                                && in_item_x >= 30
                                && in_item_x <= 33
                                && py >= 48
                                && py <= 51
                            {
                                // Glowing active app dot
                                s.pixels[y * s.width + x] =
                                    if focused { theme::ACCENT } else { theme::MUTED };
                            } else {
                                if self.dock_hover != Some(item_idx)
                                    || !(6..52).contains(&py)
                                    || !(8..56).contains(&in_item_x)
                                {
                                    s.pixels[y * s.width + x] = dock_bg;
                                }
                            }
                        } else {
                            s.pixels[y * s.width + x] = dock_bg;
                        }
                    }
                }
            }

            if let Some(item) = self.dock_hover.filter(|i| *i < 4) {
                let label = ["Files", "Terminal", "Notes", "System Information"][item];
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
                            s.pixels[y * s.width + x] = if lx >= 10 && ly >= 3 {
                                theme::text_pixel(label, lx - 10, ly - 3, theme::TEXT, theme::PANEL)
                            } else {
                                theme::PANEL
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

            // Windows-Style Start Menu Popup
            if self.start_open {
                let start_menu = Self::start_menu_rect(s.height);
                if let Some(clip) = start_menu.intersection(r) {
                    let bw = start_menu.w / scale;
                    let bh = start_menu.h / scale;
                    for y in clip.y..clip.y + clip.h {
                        for x in clip.x..clip.x + clip.w {
                            let px = (x - start_menu.x) / scale;
                            let py = (y - start_menu.y) / scale;
                            let is_border = px == 0
                                || px == bw - 1
                                || py == 0
                                || py == bh - 1;
                            s.pixels[y * s.width + x] = if is_border {
                                0xff30363d
                            } else if py < 24 {
                                if py == 23 {
                                    0xff30363d
                                } else {
                                    let in_logo = ((6..=10).contains(&py) || (12..=16).contains(&py))
                                        && ((10..=13).contains(&px) || (15..=18).contains(&px));
                                    if in_logo {
                                        0xff00a4ef
                                    } else if px >= 24 && px < 110 && (4..20).contains(&py) {
                                        theme::text_pixel("FortressOS", px - 24, py - 4, 0xffe6edf3, 0xff161b22)
                                    } else if px >= bw.saturating_sub(44) && px < bw.saturating_sub(8) && (4..20).contains(&py) {
                                        theme::text_pixel("v0.2", px - bw.saturating_sub(44), py - 4, 0xff8b949e, 0xff161b22)
                                    } else {
                                        0xff161b22
                                    }
                                }
                            } else {
                                let item_idx = (py - 24) / 22;
                                let item_y = (py - 24) % 22;
                                let is_hover = self.menu_hover == Some(item_idx);
                                let item_bg = if is_hover && px >= 4 && px < bw - 4 {
                                    0xff28313d
                                } else {
                                    0xff1c2128
                                };

                                // Application Icon (14x14 at px in 10..24, item_y in 4..18)
                                let icon_pixel = if px >= 10 && px < 24 && (4..18).contains(&item_y) {
                                    let ix = px - 10;
                                    let iy = item_y - 4;
                                    match item_idx {
                                        0 => {
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
                                        1 => {
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
                                        2 => {
                                            // Files: golden folder
                                            if (2..=5).contains(&ix) && (2..=3).contains(&iy) {
                                                Some(0xffd29922)
                                            } else if (1..=12).contains(&ix) && (4..=11).contains(&iy) {
                                                Some(if iy >= 6 { 0xfff0b232 } else { 0xffe3b341 })
                                            } else {
                                                None
                                            }
                                        }
                                        3 => {
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
                                        4 => {
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
                                        5 => {
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
                                        _ => None,
                                    }
                                } else {
                                    None
                                };

                                if let Some(p) = icon_pixel {
                                    p
                                } else {
                                    let (label, is_danger) = match item_idx {
                                        0 => ("Terminal", false),
                                        1 => ("Notepad", false),
                                        2 => ("Files", false),
                                        3 => ("System Information", false),
                                        4 => ("Settings", false),
                                        5 => ("Exit to Console", true),
                                        _ => ("", false),
                                    };
                                    let fg = if is_hover {
                                        0xffffffff
                                    } else if is_danger {
                                        0xffff7b72
                                    } else {
                                        0xffe6edf3
                                    };
                                    if !label.is_empty() && px >= 28 && px < bw - 8 && (3..19).contains(&item_y) {
                                        theme::text_pixel(label, px - 28, item_y - 3, fg, item_bg)
                                    } else {
                                        item_bg
                                    }
                                }
                            };
                        }
                    }
                }
            }

            i += 1;
        }
    }
}
