//! Single-process window server. Stack order is back to front; no client IPC yet.
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
    Border,
}

pub struct Window {
    pub id: u32,
    pub title: String,
    /// Outer bounds. Content starts at (1,25), excluding the titlebar and border.
    pub rect: Rect,
    pub content: Vec<u32>,
    pub is_focused: bool,
    pub is_dragging: bool,
    pub drag_offset: (usize, usize),
    pub is_minimized: bool,
}

impl Window {
    pub fn new(id: u32, title: &str, rect: Rect) -> Option<Self> {
        if rect.w < 96 || rect.h < TITLE_HEIGHT + 3 {
            return None;
        }
        let mut content = Vec::new();
        let len = (rect.w - 2).checked_mul(rect.h - TITLE_HEIGHT - 2)?;
        content.try_reserve_exact(len).ok()?;
        content.resize(len, CLIENT);
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
            drag_offset: (0, 0),
            is_minimized: false,
        })
    }

    pub fn hit(&self, x: usize, y: usize) -> Option<Hit> {
        if self.is_minimized || !self.rect.contains(x, y) {
            return None;
        }
        let (x, y) = (x - self.rect.x, y - self.rect.y);
        Some(
            if x == 0 || y == 0 || x == self.rect.w - 1 || y == self.rect.h - 1 {
                Hit::Border
            } else if y <= TITLE_HEIGHT {
                if x >= self.rect.w - 25 {
                    Hit::Close
                } else if x >= self.rect.w - 49 {
                    Hit::Minimize
                } else {
                    Hit::Title
                }
            } else {
                Hit::Client
            },
        )
    }

    fn pixel(&self, x: usize, y: usize, hover: Option<Hit>) -> u32 {
        if x == 0 || y == 0 || x == self.rect.w - 1 || y == self.rect.h - 1 {
            return BORDER;
        }
        if y > TITLE_HEIGHT {
            return self.content[(y - 25) * (self.rect.w - 2) + x - 1];
        }
        let title_bg = if self.is_focused { ACTIVE } else { INACTIVE };
        let (label, lx, bg) = if x >= self.rect.w - 25 {
            (
                "[X]",
                x - (self.rect.w - 25),
                if hover == Some(Hit::Close) {
                    HOVER_CLOSE
                } else {
                    title_bg
                },
            )
        } else if x >= self.rect.w - 49 {
            (
                "[-]",
                x - (self.rect.w - 49),
                if hover == Some(Hit::Minimize) {
                    HOVER_MIN
                } else {
                    title_bg
                },
            )
        } else if x >= 8 {
            (self.title.as_str(), x - 8, title_bg)
        } else {
            return title_bg;
        };
        if (5..21).contains(&y) {
            label_pixel(label, lx, y - 5, TEXT, bg)
        } else {
            bg
        }
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

fn content_text(w: &mut Window, x: usize, y: usize, label: &str) {
    let width = w.rect.w - 2;
    let height = w.rect.h - 26;
    for (i, c) in label.chars().enumerate() {
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

pub struct WindowManager {
    pub windows: Vec<Window>,
    buttons: u8,
    hover: Option<(u32, Hit)>,
}

impl WindowManager {
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
            buttons: 0,
            hover: None,
        })
    }

    pub fn hit_test(&self, x: usize, y: usize) -> Option<(u32, Hit)> {
        self.windows
            .iter()
            .rev()
            .find_map(|w| w.hit(x, y).map(|hit| (w.id, hit)))
    }

    fn task_rect(id: u32, height: usize) -> Rect {
        Rect {
            x: 8 + (id as usize).saturating_sub(1).saturating_mul(168),
            y: height.saturating_sub(28),
            w: 160,
            h: 24,
        }
    }

    fn dock_damage(s: &mut Surface) {
        s.damage(Rect {
            x: 0,
            y: s.height.saturating_sub(28),
            w: s.width,
            h: 28,
        });
    }

    fn focus_top(&mut self, s: &mut Surface) {
        let top = self.windows.iter().rposition(|w| !w.is_minimized);
        for (i, w) in self.windows.iter_mut().enumerate() {
            let focused = Some(i) == top;
            if w.is_focused != focused {
                s.damage(w.rect);
            }
            w.is_focused = focused;
        }
    }

    pub fn raise(&mut self, id: u32, s: &mut Surface) {
        if let Some(i) = self.windows.iter().position(|w| w.id == id) {
            let w = self.windows.remove(i);
            s.damage(w.rect);
            self.windows.push(w); // retained capacity: no allocation
            self.focus_top(s);
        }
    }

    pub fn mouse(&mut self, x: usize, y: usize, buttons: u8, s: &mut Surface) {
        // Capture survives leaving the titlebar/window. Apply final release motion.
        if self.buttons & 1 != 0 {
            for w in &mut self.windows {
                if w.is_dragging {
                    let old = w.rect;
                    w.rect.x = x.saturating_sub(w.drag_offset.0).min(s.width - 1);
                    w.rect.y = y.saturating_sub(w.drag_offset.1).min(s.height - 1);
                    if old != w.rect {
                        s.damage(old);
                        s.damage(w.rect);
                    }
                }
            }
        }
        if buttons & 1 == 0 {
            for w in &mut self.windows {
                w.is_dragging = false;
            }
        } else if self.buttons & 1 == 0 {
            let task = self
                .windows
                .iter()
                .position(|w| w.is_minimized && Self::task_rect(w.id, s.height).contains(x, y));
            if let Some(i) = task {
                let id = self.windows[i].id;
                self.windows[i].is_minimized = false;
                Self::dock_damage(s);
                self.raise(id, s);
            } else if let Some((id, hit)) = self.hit_test(x, y) {
                self.raise(id, s);
                let w = self.windows.last_mut().unwrap();
                match hit {
                    Hit::Close => {
                        let w = self.windows.pop().unwrap();
                        s.damage(w.rect);
                        Self::dock_damage(s);
                        self.focus_top(s);
                    }
                    Hit::Minimize => {
                        w.is_minimized = true;
                        s.damage(w.rect);
                        Self::dock_damage(s);
                        self.focus_top(s);
                    }
                    Hit::Title => {
                        w.is_dragging = true;
                        w.drag_offset = (x - w.rect.x, y - w.rect.y);
                    }
                    _ => (),
                }
            }
        }
        self.buttons = buttons;
        let hover = self.hit_test(x, y);
        if hover != self.hover {
            for w in &self.windows {
                if self.hover.is_some_and(|h| h.0 == w.id) || hover.is_some_and(|h| h.0 == w.id) {
                    s.damage(w.rect);
                }
            }
            self.hover = hover;
        }
    }

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

    pub fn render(&self, s: &mut Surface) {
        let mut i = 0;
        while let Some(r) = s.damage_rect(i) {
            // Repair desktop first, then composite every intersecting window.
            for y in r.y..r.y + r.h {
                for x in r.x..r.x + r.w {
                    s.pixels[y * s.width + x] = if (32..48).contains(&y) && x >= 32 {
                        label_pixel("FortressOS Rust Compositor", x - 32, y - 32, TEXT, DESKTOP)
                    } else {
                        DESKTOP
                    };
                }
            }
            for w in &self.windows {
                if w.is_minimized {
                    continue;
                }
                if let Some(clip) = w.rect.intersection(r) {
                    let hover = self.hover.filter(|h| h.0 == w.id).map(|h| h.1);
                    for y in clip.y..clip.y + clip.h {
                        for x in clip.x..clip.x + clip.w {
                            s.pixels[y * s.width + x] = w.pixel(x - w.rect.x, y - w.rect.y, hover);
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
            i += 1;
        }
    }
}
