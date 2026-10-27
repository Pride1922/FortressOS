//! Graphical File Manager ("Files") for FortressOS Desktop.

use crate::{
    font::GLYPHS,
    sys,
    window::Window,
};
use alloc::{string::String, vec::Vec};

pub const FILES_BG: u32 = 0xff161b22;
pub const FILES_FG: u32 = 0xffe6edf3;
pub const FILES_HEADER: u32 = 0xff0d1117;
pub const FILES_DIR: u32 = 0xff58a6ff;
pub const FILES_EXE: u32 = 0xff3fb950;
pub const FILES_SEL: u32 = 0xff1f6feb;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum FileAction {
    OpenPath(String),
    Refresh,
}

#[derive(Clone, Debug)]
pub struct FileEntry {
    pub name: String,
    pub is_dir: bool,
    pub is_exe: bool,
    pub size: u64,
}

pub struct FileManager {
    #[allow(dead_code)]
    pub window_id: u32,
    pub current_path: String,
    pub entries: Vec<FileEntry>,
    pub selected: usize,
    pub scroll: usize,
}

impl FileManager {
    pub fn new(window_id: u32, initial_path: &str) -> Self {
        let mut path = String::new();
        let _ = path.try_reserve_exact(initial_path.len().max(32));
        path.push_str(initial_path);

        let mut mgr = Self {
            window_id,
            current_path: path,
            entries: Vec::new(),
            selected: 0,
            scroll: 0,
        };
        mgr.refresh();
        mgr
    }

    pub fn refresh(&mut self) {
        self.entries.clear();

        // Add parent directory entry if not at root
        if self.current_path != "/" {
            let mut parent = String::new();
            let _ = parent.try_reserve_exact(16);
            parent.push_str(".. (Parent Dir)");
            self.entries.push(FileEntry {
                name: parent,
                is_dir: true,
                is_exe: false,
                size: 0,
            });
        }

        let mut raw_names = [[0u8; 64]; 16];
        let count = sys::list_dir(&self.current_path, &mut raw_names, 16);
        if count > 0 {
            for i in 0..(count as usize).min(16) {
                let raw = &raw_names[i];
                let mut len = 0;
                while len < 64 && raw[len] != 0 {
                    len += 1;
                }
                if len == 0 || (len == 1 && raw[0] == b'.') || (len == 2 && raw[0] == b'.' && raw[1] == b'.') {
                    continue;
                }
                if let Ok(name_str) = core::str::from_utf8(&raw[..len]) {
                    let mut full_path = String::new();
                    let _ = full_path.try_reserve_exact(self.current_path.len() + len + 2);
                    full_path.push_str(&self.current_path);
                    if !self.current_path.ends_with('/') {
                        full_path.push('/');
                    }
                    full_path.push_str(name_str);

                    let mut st = sys::StatExtV1::default();
                    let is_dir = if sys::stat_ext(&full_path, &mut st) == 0 {
                        st.file_type == 2
                    } else {
                        false
                    };
                    let is_exe = !is_dir && (self.current_path == "/bin" || self.current_path == "/sbin");

                    let mut entry_name = String::new();
                    let _ = entry_name.try_reserve_exact(len);
                    entry_name.push_str(name_str);

                    self.entries.push(FileEntry {
                        name: entry_name,
                        is_dir,
                        is_exe,
                        size: st.size as u64,
                    });
                }
            }
        }
    }

    pub fn enter_selected(&mut self) -> Option<String> {
        if self.selected >= self.entries.len() {
            return None;
        }
        let entry = &self.entries[self.selected];
        if entry.is_dir {
            if entry.name.starts_with("..") {
                self.navigate_up();
            } else {
                let name = entry.name.clone();
                if self.current_path == "/" {
                    self.current_path.push_str(&name);
                } else {
                    self.current_path.push('/');
                    self.current_path.push_str(&name);
                }
                self.selected = 0;
                self.refresh();
            }
            None
        } else {
            // Return full path of file
            let mut full = String::new();
            let _ = full.try_reserve_exact(self.current_path.len() + entry.name.len() + 2);
            full.push_str(&self.current_path);
            if !self.current_path.ends_with('/') {
                full.push('/');
            }
            full.push_str(&entry.name);
            Some(full)
        }
    }

    pub fn navigate_up(&mut self) {
        if self.current_path == "/" {
            return;
        }
        if let Some(idx) = self.current_path.rfind('/') {
            if idx == 0 {
                self.current_path.truncate(1);
            } else {
                self.current_path.truncate(idx);
            }
            self.selected = 0;
            self.refresh();
        }
    }

    pub fn set_path(&mut self, path: &str) {
        self.current_path.clear();
        self.current_path.push_str(path);
        self.selected = 0;
        self.refresh();
    }

    /// Handles mouse clicks in client coordinates. Returns FileAction if action triggered.
    pub fn handle_click(&mut self, cx: usize, cy: usize) -> Option<FileAction> {
        if cy <= 22 {
            // Header buttons
            if cx >= 8 && cx <= 56 {
                self.navigate_up();
                return Some(FileAction::Refresh);
            } else if cx >= 64 && cx <= 88 {
                self.set_path("/");
                return Some(FileAction::Refresh);
            } else if cx >= 96 && cx <= 144 {
                self.set_path("/bin");
                return Some(FileAction::Refresh);
            } else if cx >= 152 && cx <= 200 {
                self.set_path("/etc");
                return Some(FileAction::Refresh);
            }
            return None;
        }

        if cy >= 26 {
            let clicked = self.scroll + (cy - 26) / 20;
            if clicked < self.entries.len() {
                if self.selected == clicked {
                    // Double click / Enter action
                    if let Some(path) = self.enter_selected() {
                        return Some(FileAction::OpenPath(path));
                    } else {
                        return Some(FileAction::Refresh);
                    }
                } else {
                    // First click selects
                    self.selected = clicked;
                    return Some(FileAction::Refresh);
                }
            }
        }
        None
    }

    pub fn render(&self, w: &mut Window) {
        let width = w.rect.w.saturating_sub(2);
        let height = w.rect.h.saturating_sub(26);
        if width < 30 || height < 30 {
            return;
        }
        w.content.fill(FILES_BG);

        // Header toolbar (22px)
        for y in 0..22.min(height) {
            for x in 0..width {
                w.content[y * width + x] = if y == 21 { 0xff30363d } else { FILES_HEADER };
            }
        }
        draw_text(w, 8, 3, "[^ Up]", 0xff58a6ff, FILES_HEADER);
        draw_text(w, 64, 3, "[/]", 0xff8b949e, FILES_HEADER);
        draw_text(w, 96, 3, "[/bin]", 0xff8b949e, FILES_HEADER);
        draw_text(w, 152, 3, "[/etc]", 0xff8b949e, FILES_HEADER);
        draw_text(w, 208, 3, "Path:", 0xff8b949e, FILES_HEADER);
        draw_text(w, 256, 3, &self.current_path, FILES_FG, FILES_HEADER);

        // File list (y = 26 to height - 20)
        let max_items = (height.saturating_sub(46)) / 20;
        let mut cur_y = 26;

        for (idx, entry) in self.entries.iter().skip(self.scroll).take(max_items).enumerate() {
            let actual_idx = self.scroll + idx;
            let is_sel = actual_idx == self.selected;
            let row_bg = if is_sel { FILES_SEL } else { FILES_BG };

            // Fill row background
            for y in cur_y..(cur_y + 20).min(height.saturating_sub(20)) {
                for x in 0..width {
                    w.content[y * width + x] = row_bg;
                }
            }

            // Draw icon tag
            let (tag, tag_fg) = if entry.is_dir {
                ("[DIR]", FILES_DIR)
            } else if entry.is_exe {
                ("[BIN]", FILES_EXE)
            } else {
                ("[FILE]", 0xff8b949e)
            };
            draw_text(w, 8, cur_y + 2, tag, tag_fg, row_bg);

            // Draw name
            let name_fg = if is_sel { 0xffffffff } else { FILES_FG };
            draw_text(w, 64, cur_y + 2, &entry.name, name_fg, row_bg);

            // Draw size for files
            if !entry.is_dir && width >= 320 {
                let size_str = format_bytes(entry.size);
                draw_text(w, width.saturating_sub(90), cur_y + 2, &size_str, 0xff8b949e, row_bg);
            }

            cur_y += 20;
        }

        // Footer status bar (bottom 18px)
        let footer_y = height.saturating_sub(18);
        for y in footer_y..height {
            for x in 0..width {
                w.content[y * width + x] = FILES_HEADER;
            }
        }
        let total_items = self.entries.len();
        let mut status_msg = String::new();
        let _ = status_msg.try_reserve_exact(32);
        let _ = status_msg.push_str("Items: ");
        let mut n_buf = [0u8; 8];
        let mut n = total_items;
        let mut n_len = 0;
        if n == 0 {
            n_buf[0] = b'0';
            n_len = 1;
        } else {
            while n > 0 && n_len < 8 {
                n_buf[n_len] = b'0' + (n % 10) as u8;
                n /= 10;
                n_len += 1;
            }
            n_buf[..n_len].reverse();
        }
        if let Ok(s) = core::str::from_utf8(&n_buf[..n_len]) {
            status_msg.push_str(s);
        }
        draw_text(w, 8, footer_y + 1, &status_msg, 0xff8b949e, FILES_HEADER);
        draw_text(w, width.saturating_sub(180), footer_y + 1, "Click to Select / Open", 0xff8b949e, FILES_HEADER);
    }
}

fn format_bytes(bytes: u64) -> String {
    let mut s = String::new();
    let _ = s.try_reserve_exact(16);
    if bytes < 1024 {
        let mut n = bytes;
        let mut buf = [0u8; 8];
        let mut len = 0;
        if n == 0 {
            buf[0] = b'0';
            len = 1;
        } else {
            while n > 0 && len < 8 {
                buf[len] = b'0' + (n % 10) as u8;
                n /= 10;
                len += 1;
            }
            buf[..len].reverse();
        }
        if let Ok(num_str) = core::str::from_utf8(&buf[..len]) {
            s.push_str(num_str);
        }
        s.push_str(" B");
    } else {
        let kb = bytes / 1024;
        let mut n = kb;
        let mut buf = [0u8; 8];
        let mut len = 0;
        while n > 0 && len < 8 {
            buf[len] = b'0' + (n % 10) as u8;
            n /= 10;
            len += 1;
        }
        buf[..len].reverse();
        if let Ok(num_str) = core::str::from_utf8(&buf[..len]) {
            s.push_str(num_str);
        }
        s.push_str(" KB");
    }
    s
}

fn draw_char(w: &mut Window, px: usize, py: usize, ch: u8, fg: u32, bg: u32) {
    let width = w.rect.w.saturating_sub(2);
    let height = w.rect.h.saturating_sub(26);
    if px + 8 > width || py + 16 > height {
        return;
    }
    let c_idx = if (' '..='~').contains(&(ch as char)) {
        ch as usize - 32
    } else {
        0
    };
    let glyph = &GLYPHS[c_idx];
    for r in 0..16 {
        for c in 0..8 {
            let bit = (glyph[r] & (128 >> c)) != 0;
            w.content[(py + r) * width + px + c] = if bit { fg } else { bg };
        }
    }
}

fn draw_text(w: &mut Window, mut px: usize, py: usize, text: &str, fg: u32, bg: u32) {
    for c in text.chars() {
        if px + 8 > w.rect.w.saturating_sub(2) {
            break;
        }
        draw_char(w, px, py, c as u8, fg, bg);
        px += 8;
    }
}
