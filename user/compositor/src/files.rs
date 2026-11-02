//! Graphical File Manager ("Files") for FortressOS Desktop. Runs as its
//! own unprivileged process (src/bin/files.rs) and draws into a Canvas;
//! file names come from disk and are untrusted.

use crate::{canvas::Canvas, sys};
use alloc::{string::String, vec::Vec};

pub const FILES_BG: u32 = 0xff252525;
pub const FILES_FG: u32 = 0xffe0e0e0;
pub const FILES_HEADER: u32 = 0xff1e1e1e;
pub const FILES_DIR: u32 = 0xff007aff;
pub const FILES_EXE: u32 = 0xff34c759;
pub const FILES_SEL: u32 = 0xff007aff;

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
    pub current_path: String,
    pub entries: Vec<FileEntry>,
    pub selected: usize,
    pub scroll: usize,
}

impl FileManager {
    #[inline(never)]
    pub fn new(initial_path: &str) -> Self {
        let mut path = String::new();
        let _ = path.try_reserve_exact(initial_path.len().max(32));
        path.push_str(initial_path);

        let mut mgr = Self {
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

        // Keep the 1 KiB directory batch off the bounded user stack.
        let mut raw_names = Vec::new();
        if raw_names.try_reserve_exact(16).is_err() {
            return;
        }
        raw_names.resize(16, [0u8; 64]);
        let count = sys::list_dir(&self.current_path, &mut raw_names, 16);
        if count > 0 {
            for i in 0..(count as usize).min(16) {
                let raw = &raw_names[i];
                let mut len = 0;
                while len < 64 && raw[len] != 0 {
                    len += 1;
                }
                if len == 0
                    || (len == 1 && raw[0] == b'.')
                    || (len == 2 && raw[0] == b'.' && raw[1] == b'.')
                {
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
                    let is_exe =
                        !is_dir && (self.current_path == "/bin" || self.current_path == "/sbin");

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

    /// Scrolls so the selection is within `rows` visible rows.
    pub fn ensure_visible(&mut self, rows: usize) {
        let rows = rows.max(1);
        if self.selected < self.scroll {
            self.scroll = self.selected;
        } else if self.selected >= self.scroll + rows {
            self.scroll = self.selected + 1 - rows;
        }
    }

    /// Visible list rows for a content height (matches render()).
    pub fn rows_for(height: usize) -> usize {
        height.saturating_sub(46) / 20
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
                self.selected = clicked;
                if self.entries[clicked].is_dir {
                    // Clicking a directory opens it immediately!
                    self.enter_selected();
                    return Some(FileAction::Refresh);
                } else {
                    // Clicking a file opens it in Notepad!
                    if let Some(path) = self.enter_selected() {
                        return Some(FileAction::OpenPath(path));
                    } else {
                        return Some(FileAction::Refresh);
                    }
                }
            }
        }
        None
    }

    pub fn render(&self, c: &mut Canvas) {
        let (width, height) = (c.width, c.height);
        if width < 30 || height < 30 {
            return;
        }
        c.fill(0xff181b21);

        // Header toolbar (24px)
        let hdr_bg = 0xff12151b;
        c.fill_rect(0, 0, width, 23, hdr_bg);
        c.fill_rect(0, 23, width, 1, 0xff28303e);

        c.pill_button(8, 2, 48, 19, "^ Up", FILES_DIR, 0xff1b2432, 0xff2e3d52, hdr_bg);
        let is_root = self.current_path == "/";
        c.pill_button(
            64,
            2,
            24,
            19,
            "/",
            if is_root { 0xffffffff } else { 0xff9fb0c7 },
            if is_root { 0xff1e385b } else { 0xff1a1f29 },
            if is_root { 0xff3b82f6 } else { 0xff2a3342 },
            hdr_bg,
        );
        let is_bin = self.current_path == "/bin";
        c.pill_button(
            96,
            2,
            48,
            19,
            "/bin",
            if is_bin { 0xffffffff } else { 0xff9fb0c7 },
            if is_bin { 0xff1e385b } else { 0xff1a1f29 },
            if is_bin { 0xff3b82f6 } else { 0xff2a3342 },
            hdr_bg,
        );
        let is_etc = self.current_path == "/etc";
        c.pill_button(
            152,
            2,
            48,
            19,
            "/etc",
            if is_etc { 0xffffffff } else { 0xff9fb0c7 },
            if is_etc { 0xff1e385b } else { 0xff1a1f29 },
            if is_etc { 0xff3b82f6 } else { 0xff2a3342 },
            hdr_bg,
        );
        if width > 220 {
            let path_w = width.saturating_sub(214);
            c.rounded_box(206, 2, path_w, 19, 5, hdr_bg, 0xff0d1015, 0xff252e3d);
            c.text(214, 3, &self.current_path, FILES_FG, 0xff0d1015);
        }

        // File list (y = 26 to height - 20)
        let max_items = (height.saturating_sub(46)) / 20;
        let mut cur_y = 26;

        for (idx, entry) in self
            .entries
            .iter()
            .skip(self.scroll)
            .take(max_items)
            .enumerate()
        {
            let actual_idx = self.scroll + idx;
            let is_sel = actual_idx == self.selected;
            let stripe_bg = if idx % 2 == 0 { 0xff1c2028 } else { 0xff181b21 };
            let row_h = (cur_y + 20).min(height.saturating_sub(20)).saturating_sub(cur_y);
            c.fill_rect(0, cur_y, width, row_h, stripe_bg);

            let row_bg = if is_sel {
                let sel_bg = 0xff1e385b;
                if width > 12 && row_h >= 6 {
                    c.rounded_box(4, cur_y, width.saturating_sub(8), row_h, 5, stripe_bg, sel_bg, 0xff3b82f6);
                    c.fill_rect(6, cur_y + 4, 2, row_h.saturating_sub(8), 0xff38bdf8);
                }
                sel_bg
            } else {
                stripe_bg
            };

            let icon_kind = if entry.is_dir {
                0
            } else if entry.is_exe {
                1
            } else {
                2
            };
            c.file_icon(12, cur_y + 3, icon_kind, row_bg);

            // Draw name
            let name_fg = if is_sel {
                0xffffffff
            } else if entry.is_dir {
                0xffeef2f6
            } else if entry.is_exe {
                FILES_EXE
            } else {
                FILES_FG
            };
            if entry.is_dir || is_sel {
                c.text_bold(36, cur_y + 1, &entry.name, name_fg, row_bg);
            } else {
                c.text(36, cur_y + 1, &entry.name, name_fg, row_bg);
            }

            // Draw type & size for entries on wide windows
            if width >= 320 {
                if entry.is_dir {
                    c.text(width.saturating_sub(84), cur_y + 1, "Folder", FILES_DIR, row_bg);
                } else {
                    let size_str = format_bytes(entry.size);
                    c.text(
                        width.saturating_sub(84),
                        cur_y + 1,
                        &size_str,
                        0xff7e8796,
                        row_bg,
                    );
                }
            }

            cur_y += 20;
        }

        // Footer status bar (bottom 18px)
        let footer_y = height.saturating_sub(18);
        if footer_y > 0 {
            c.fill_rect(0, footer_y - 1, width, 1, 0xff28303e);
        }
        let ftr_bg = 0xff12151b;
        c.fill_rect(0, footer_y, width, height - footer_y, ftr_bg);
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
        c.text(10, footer_y + 1, &status_msg, 0xff7e8796, ftr_bg);
        if width >= 260 {
            c.text(
                width.saturating_sub(184),
                footer_y + 1,
                "Click to Select / Open",
                0xff7e8796,
                ftr_bg,
            );
        }
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
