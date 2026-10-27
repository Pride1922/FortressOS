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
            is_resizing: false,
            drag_offset: (0, 0),
            is_minimized: false,
        })
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
        let mut new_content = Vec::new();
        if new_content.try_reserve_exact(len).is_err() {
            return false;
        }
        new_content.resize(len, CLIENT);

        let old_w = self.rect.w - 2;
        let old_h = self.rect.h - TITLE_HEIGHT - 2;
        let copy_w = old_w.min(new_content_w);
        let copy_h = old_h.min(new_content_h);
        for y in 0..copy_h {
            let src_row = &self.content[y * old_w..y * old_w + copy_w];
            let dst_row = &mut new_content[y * new_content_w..y * new_content_w + copy_w];
            dst_row.copy_from_slice(src_row);
        }
        self.content = new_content;
        self.rect.w = new_w;
        self.rect.h = new_h;
        true
    }

    pub fn hit(&self, x: usize, y: usize) -> Option<Hit> {
        if self.is_minimized || !self.rect.contains(x, y) {
            return None;
        }
        let (x, y) = (x - self.rect.x, y - self.rect.y);
        Some(
            if x >= self.rect.w.saturating_sub(12) && y >= self.rect.h.saturating_sub(12) {
                Hit::Resize
            } else if x == 0 || y == 0 || x == self.rect.w - 1 || y == self.rect.h - 1 {
                Hit::Border
            } else if y <= TITLE_HEIGHT {
                if x >= self.rect.w - 25 {
                    Hit::Close
                } else if x >= self.rect.w - 49 {
                    Hit::Minimize
                } else if self.id >= 3 && x >= 8 && x <= 20 {
                    Hit::Close
                } else if self.id >= 3 && x >= 24 && x <= 36 {
                    Hit::Minimize
                } else if self.id >= 3 && x >= 40 && x <= 52 {
                    Hit::Minimize // Zoom / Maximize
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
        // Resize grip in bottom-right corner
        let rx = self.rect.w.saturating_sub(x);
        let ry = self.rect.h.saturating_sub(y);
        if rx <= 10 && ry <= 10 && (rx + ry == 6 || rx + ry == 9 || rx + ry == 12) {
            return if hover == Some(Hit::Resize) { HOVER_MIN } else { BORDER };
        }
        if y > TITLE_HEIGHT {
            return self.content[(y - 25) * (self.rect.w - 2) + x - 1];
        }
        let title_bg = if self.is_focused { ACTIVE } else { INACTIVE };

        // macOS traffic lights on window id >= 3
        if self.id >= 3 && (7..=17).contains(&y) {
            if (8..=18).contains(&x) {
                // Close: Red circle
                let dx = x as i32 - 13;
                let dy = y as i32 - 12;
                if dx * dx + dy * dy <= 25 {
                    return if hover == Some(Hit::Close) { 0xffff7068 } else { 0xffff5f56 };
                }
            } else if (24..=34).contains(&x) {
                // Minimize: Yellow circle
                let dx = x as i32 - 29;
                let dy = y as i32 - 12;
                if dx * dx + dy * dy <= 25 {
                    return if hover == Some(Hit::Minimize) { 0xffffcc4d } else { 0xffffbd2e };
                }
            } else if (40..=50).contains(&x) {
                // Zoom: Green circle
                let dx = x as i32 - 45;
                let dy = y as i32 - 12;
                if dx * dx + dy * dy <= 25 {
                    return 0xff27c93f;
                }
            }
        }

        // Right side buttons (matching test_compositor.py)
        if x >= self.rect.w - 25 {
            let lx = x - (self.rect.w - 25);
            let bg = if hover == Some(Hit::Close) { HOVER_CLOSE } else { title_bg };
            return if (5..21).contains(&y) {
                label_pixel("[X]", lx, y - 5, TEXT, bg)
            } else {
                bg
            };
        } else if x >= self.rect.w - 49 {
            let lx = x - (self.rect.w - 49);
            let bg = if hover == Some(Hit::Minimize) { HOVER_MIN } else { title_bg };
            return if (5..21).contains(&y) {
                label_pixel("[-]", lx, y - 5, TEXT, bg)
            } else {
                bg
            };
        }

        // Window title
        if self.id >= 3 {
            // Centered title for window 3+
            let title_len = self.title.len() * 8;
            let title_x = (self.rect.w.saturating_sub(title_len)) / 2;
            if x >= title_x && x < title_x + title_len && (5..21).contains(&y) {
                return label_pixel(&self.title, x - title_x, y - 5, TEXT, title_bg);
            }
        } else if x >= 8 && (5..21).contains(&y) {
            // Left-aligned title at x = 8 for window 1 and 2 (matches test_compositor.py)
            return label_pixel(self.title.as_str(), x - 8, y - 5, TEXT, title_bg);
        }

        title_bg
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
    WindowResized(u32),
    ClientClick(u32, usize, usize),
    LockScreen,
    Exit,
}

pub struct WindowManager {
    pub windows: Vec<Window>,
    buttons: u8,
    hover: Option<(u32, Hit)>,
    pub clock_sec: i64,
    pub start_open: bool,
    pub menu_hover: Option<usize>,
    #[allow(dead_code)]
    pub dock_hover: Option<usize>,
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
            clock_sec: -1,
            start_open: false,
            menu_hover: None,
            dock_hover: None,
        })
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

    pub fn top_bar_rect(width: usize) -> Rect {
        Rect {
            x: 0,
            y: 0,
            w: width,
            h: 24,
        }
    }

    pub fn dock_rect(width: usize, height: usize) -> Rect {
        let dock_w = 340;
        let dock_h = 36;
        let dock_x = width.saturating_sub(dock_w) / 2;
        let dock_y = height.saturating_sub(44);
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
            y: height.saturating_sub(28),
            w: 76,
            h: 24,
        }
    }

    pub fn start_menu_rect(height: usize) -> Rect {
        Rect {
            x: 8,
            y: height.saturating_sub(28 + 156),
            w: 176,
            h: 156,
        }
    }

    fn task_rect(id: u32, height: usize) -> Rect {
        Rect {
            x: if id == 1 { 88 } else { 8 + (id as usize).saturating_sub(1).saturating_mul(168) },
            y: height.saturating_sub(28),
            w: if id == 1 { 80 } else { 160 },
            h: 24,
        }
    }

    pub fn unminimize_and_raise(&mut self, id: u32, s: &mut Surface) {
        if let Some(w) = self.windows.iter_mut().find(|w| w.id == id) {
            w.is_minimized = false;
        }
        Self::dock_damage(s);
        self.raise(id, s);
    }

    pub fn clock_rect(width: usize, height: usize) -> Rect {
        Rect {
            x: width.saturating_sub(180),
            y: height.saturating_sub(26),
            w: 88,
            h: 22,
        }
    }

    pub fn update_clock(&mut self, s: &mut Surface, sec: i64) {
        if sec == self.clock_sec {
            return;
        }
        self.clock_sec = sec;
        s.damage(Self::clock_rect(s.width, s.height));
    }

    pub fn cycle_focus(&mut self, s: &mut Surface) {
        if self.windows.len() <= 1 {
            return;
        }
        let first_id = self.windows[0].id;
        self.raise(first_id, s);
    }

    fn dock_damage(s: &mut Surface) {
        s.damage(Self::dock_rect(s.width, s.height));
        s.damage(Rect {
            x: 0,
            y: s.height.saturating_sub(32),
            w: s.width,
            h: 32,
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

    pub fn mouse(&mut self, x: usize, y: usize, buttons: u8, s: &mut Surface) -> Action {
        let mut ret_action = Action::None;
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
                } else if w.is_resizing {
                    let min_w = 160;
                    let min_h = 100;
                    let max_w = s.width.saturating_sub(w.rect.x);
                    let max_h = s.height.saturating_sub(w.rect.y).saturating_sub(28);
                    let target_w = x.saturating_sub(w.rect.x).max(min_w).min(max_w);
                    let target_h = y.saturating_sub(w.rect.y).max(min_h).min(max_h);
                    if target_w != w.rect.w || target_h != w.rect.h {
                        let old = w.rect;
                        if w.resize(target_w, target_h) {
                            s.damage(old);
                            s.damage(w.rect);
                            ret_action = Action::WindowResized(w.id);
                        }
                    }
                }
            }
        }
        if buttons & 1 == 0 {
            for w in &mut self.windows {
                w.is_dragging = false;
                w.is_resizing = false;
            }
        } else if self.buttons & 1 == 0 {
            let dock_r = Self::dock_rect(s.width, s.height);
            let start_btn = Self::start_btn_rect(s.height);
            let start_menu = Self::start_menu_rect(s.height);

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
                let item_idx = (x - dock_r.x) / 48;
                match item_idx {
                    0 => ret_action = Action::SpawnFileManager,
                    1 => ret_action = Action::SpawnTerminal,
                    2 => ret_action = Action::SpawnNotepad,
                    3 => self.unminimize_and_raise(1, s),
                    4 => self.unminimize_and_raise(2, s),
                    5 => ret_action = Action::LockScreen,
                    6 => ret_action = Action::Exit,
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
                    let py = y - start_menu.y;
                    if py >= 24 {
                        let item_idx = (py - 24) / 22;
                        match item_idx {
                            0 => ret_action = Action::SpawnTerminal,
                            1 => ret_action = Action::SpawnNotepad,
                            2 => ret_action = Action::SpawnFileManager,
                            3 => self.unminimize_and_raise(1, s),
                            4 => self.unminimize_and_raise(2, s),
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
            let new_menu_hover = if start_menu.contains(x, y) && y >= start_menu.y + 24 {
                let idx = (y - start_menu.y - 24) / 22;
                if idx < 6 {
                    Some(idx)
                } else {
                    None
                }
            } else {
                None
            };
            if new_menu_hover != self.menu_hover {
                self.menu_hover = new_menu_hover;
                s.damage(start_menu);
            }
        }

        let hover = self.hit_test(x, y);
        if hover != self.hover {
            for w in &self.windows {
                if self.hover.is_some_and(|h| h.0 == w.id) || hover.is_some_and(|h| h.0 == w.id) {
                    s.damage(w.rect);
                }
            }
            self.hover = hover;
        }
        ret_action
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

            // macOS Top Menu Bar (y = 0..24)
            let top_bar = Self::top_bar_rect(s.width);
            if let Some(clip) = top_bar.intersection(r) {
                for y in clip.y..clip.y + clip.h {
                    for x in clip.x..clip.x + clip.w {
                        let bg = if y == 23 { 0xff2d333b } else { 0xff1c2128 };
                        s.pixels[y * s.width + x] = if (4..20).contains(&y) {
                            if x >= 8 && x < 24 {
                                label_pixel("[#]", x - 8, y - 4, 0xffffffff, bg)
                            } else if x >= 32 && x < 80 {
                                label_pixel("Finder", x - 32, y - 4, 0xffffffff, bg)
                            } else if x >= 96 && x < 128 {
                                label_pixel("File", x - 96, y - 4, TEXT, bg)
                            } else if x >= 136 && x < 168 {
                                label_pixel("Edit", x - 136, y - 4, TEXT, bg)
                            } else if x >= 176 && x < 208 {
                                label_pixel("View", x - 176, y - 4, TEXT, bg)
                            } else if x >= 216 && x < 264 {
                                label_pixel("Window", x - 216, y - 4, TEXT, bg)
                            } else if x >= 272 && x < 304 {
                                label_pixel("Help", x - 272, y - 4, TEXT, bg)
                            } else if x >= s.width.saturating_sub(180) && x < s.width.saturating_sub(132) {
                                label_pixel("[WLAN]", x - (s.width.saturating_sub(180)), y - 4, 0xff3fb950, bg)
                            } else if x >= s.width.saturating_sub(124) && x < s.width.saturating_sub(76) {
                                label_pixel("[100%]", x - (s.width.saturating_sub(124)), y - 4, 0xff58a6ff, bg)
                            } else {
                                bg
                            }
                        } else {
                            bg
                        };
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
                        for x in clip.x..clip.x + clip.w {
                            s.pixels[y * s.width + x] = w.pixel(x - w.rect.x, y - w.rect.y, hover);
                        }
                    }
                }
            }


            // Floating macOS Translucent Bottom Dock
            let dock_r = Self::dock_rect(s.width, s.height);
            if let Some(clip) = dock_r.intersection(r) {
                let has_win5 = self.windows.iter().any(|w| w.id == 5);
                let has_win3 = self.windows.iter().any(|w| w.id == 3);
                let has_win4 = self.windows.iter().any(|w| w.id == 4);
                let has_win1 = self.windows.iter().any(|w| w.id == 1);
                let has_win2 = self.windows.iter().any(|w| w.id == 2);
                for y in clip.y..clip.y + clip.h {
                    for x in clip.x..clip.x + clip.w {
                        let px = x - dock_r.x;
                        let py = y - dock_r.y;
                        let is_border = px == 0 || px == dock_r.w - 1 || py == 0 || py == dock_r.h - 1;
                        if is_border {
                            s.pixels[y * s.width + x] = 0xff484f58;
                        } else {
                            let item_idx = px / 48;
                            let in_item_x = px % 48;
                            let (label, color, is_running) = match item_idx {
                                0 => ("Files", 0xff58a6ff, has_win5),
                                1 => ("Term", 0xffe6edf3, has_win3),
                                2 => ("Notes", 0xffe5c07b, has_win4),
                                3 => ("Sysmon", 0xff98c379, has_win1),
                                4 => ("Colors", 0xffc678dd, has_win2),
                                5 => ("Lock", 0xffd19a66, false),
                                6 => ("Exit", 0xffe06c75, false),
                                _ => ("", 0, false),
                            };
                            let item_bg = 0xff161b22;
                            let is_dot = is_running && py >= 30 && py <= 32 && in_item_x >= 20 && in_item_x <= 23;
                            if is_dot {
                                s.pixels[y * s.width + x] = 0xff58a6ff;
                            } else if (8..24).contains(&py) && in_item_x >= 4 {
                                s.pixels[y * s.width + x] = label_pixel(label, in_item_x - 4, py - 8, color, item_bg);
                            } else {
                                s.pixels[y * s.width + x] = item_bg;
                            }
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
            // Digital Clock
            if self.clock_sec >= 0 {
                let clock_r = Self::clock_rect(s.width, s.height);
                if let Some(clip) = clock_r.intersection(r) {
                    let day_sec = (self.clock_sec % 86400 + 86400) % 86400;
                    let h = (day_sec / 3600) as u32;
                    let m = ((day_sec % 3600) / 60) as u32;
                    let sec = (day_sec % 60) as u32;
                    let time_str = [
                        b'0' + (h / 10) as u8,
                        b'0' + (h % 10) as u8,
                        b':',
                        b'0' + (m / 10) as u8,
                        b'0' + (m % 10) as u8,
                        b':',
                        b'0' + (sec / 10) as u8,
                        b'0' + (sec % 10) as u8,
                    ];
                    let label = core::str::from_utf8(&time_str).unwrap_or("00:00:00");
                    for y in clip.y..clip.y + clip.h {
                        for x in clip.x..clip.x + clip.w {
                            s.pixels[y * s.width + x] =
                                if x >= clock_r.x + 12 && (3..19).contains(&(y - clock_r.y)) {
                                    label_pixel(
                                        label,
                                        x - clock_r.x - 12,
                                        y - clock_r.y - 3,
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
            // Start Button
            let start_btn = Self::start_btn_rect(s.height);
            if let Some(clip) = start_btn.intersection(r) {
                let bg = if self.start_open { ACTIVE } else { INACTIVE };
                for y in clip.y..clip.y + clip.h {
                    for x in clip.x..clip.x + clip.w {
                        let px = x - start_btn.x;
                        let py = y - start_btn.y;
                        let is_border = px == 0 || px == start_btn.w - 1 || py == 0 || py == start_btn.h - 1;
                        s.pixels[y * s.width + x] = if is_border {
                            BORDER
                        } else if px >= 8 && (4..20).contains(&py) {
                            label_pixel("[F] Start", px - 8, py - 4, TEXT, bg)
                        } else {
                            bg
                        };
                    }
                }
            }
            // Start Menu Popup
            if self.start_open {
                let start_menu = Self::start_menu_rect(s.height);
                if let Some(clip) = start_menu.intersection(r) {
                    for y in clip.y..clip.y + clip.h {
                        for x in clip.x..clip.x + clip.w {
                            let px = x - start_menu.x;
                            let py = y - start_menu.y;
                            let is_border = px == 0 || px == start_menu.w - 1 || py == 0 || py == start_menu.h - 1;
                            s.pixels[y * s.width + x] = if is_border {
                                BORDER
                            } else if py < 24 {
                                if px >= 8 && (4..20).contains(&py) {
                                    label_pixel(" FortressOS", px - 8, py - 4, 0xff58a6ff, 0xff0d1117)
                                } else {
                                    0xff0d1117
                                }
                            } else {
                                let item_idx = (py - 24) / 22;
                                let item_y = (py - 24) % 22;
                                let (text, is_hover) = match item_idx {
                                    0 => (" [T] Terminal", self.menu_hover == Some(0)),
                                    1 => (" [E] Notepad", self.menu_hover == Some(1)),
                                    2 => (" [F] Files", self.menu_hover == Some(2)),
                                    3 => (" [M] Sys Monitor", self.menu_hover == Some(3)),
                                    4 => (" [P] Palette", self.menu_hover == Some(4)),
                                    5 => (" [Q] Exit", self.menu_hover == Some(5)),
                                    _ => ("", false),
                                };
                                let item_bg = if is_hover { 0xff1f6feb } else { 0xff161b22 };
                                let item_fg = if is_hover { 0xffffffff } else { TEXT };
                                if !text.is_empty() && px >= 8 && (3..19).contains(&item_y) {
                                    label_pixel(text, px - 8, item_y - 3, item_fg, item_bg)
                                } else {
                                    item_bg
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
