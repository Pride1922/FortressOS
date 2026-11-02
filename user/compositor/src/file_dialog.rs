//! Reusable Graphical File Dialog ("Open / Save Modal") for FortressOS Desktop.
//! Renders as an elevated modal sheet on top of an application's canvas,
//! featuring a mini File Manager with folder navigation, existing file list,
//! and editable filename input field.

use crate::{canvas::Canvas, sys, theme};
use alloc::{format, string::String, vec::Vec};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FileDialogMode {
    Save,
    Open,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum FileDialogAction {
    Save(String),
    Open(String),
    Cancel,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FileDialogFocus {
    Filename,
    List,
}

#[derive(Clone, Debug)]
pub struct FileEntry {
    pub name: String,
    pub is_dir: bool,
    pub is_exe: bool,
    pub size: u64,
}

pub struct FileDialog {
    pub mode: FileDialogMode,
    pub current_dir: String,
    pub filename: String,
    pub entries: Vec<FileEntry>,
    pub selected: usize,
    pub scroll: usize,
    pub focus: FileDialogFocus,
}

impl FileDialog {
    pub fn new(mode: FileDialogMode, initial_path: &str) -> Self {
        let (dir, file) = Self::split_path(initial_path);
        let mut dlg = Self {
            mode,
            current_dir: dir,
            filename: file,
            entries: Vec::new(),
            selected: 0,
            scroll: 0,
            focus: FileDialogFocus::Filename,
        };
        dlg.refresh();
        dlg
    }

    fn split_path(path: &str) -> (String, String) {
        if path.is_empty() {
            return (String::from("/tmp"), String::from("notes.txt"));
        }
        if let Some(idx) = path.rfind('/') {
            let dir = if idx == 0 {
                String::from("/")
            } else {
                String::from(&path[..idx])
            };
            let file = String::from(&path[idx + 1..]);
            let final_file = if file.is_empty() {
                String::from("notes.txt")
            } else {
                file
            };
            (dir, final_file)
        } else {
            (String::from("/tmp"), String::from(path))
        }
    }

    pub fn full_path(&self) -> String {
        if self.filename.starts_with('/') {
            return self.filename.clone();
        }
        if self.current_dir == "/" {
            format!("/{}", self.filename)
        } else {
            format!("{}/{}", self.current_dir, self.filename)
        }
    }

    pub fn refresh(&mut self) {
        self.entries.clear();

        // Parent directory entry if not at filesystem root
        if self.current_dir != "/" {
            self.entries.push(FileEntry {
                name: String::from(".. (Parent Dir)"),
                is_dir: true,
                is_exe: false,
                size: 0,
            });
        }

        let mut raw_names = Vec::new();
        if raw_names.try_reserve_exact(32).is_err() {
            return;
        }
        raw_names.resize(32, [0u8; 64]);
        let count = sys::list_dir(&self.current_dir, &mut raw_names, 32);
        if count > 0 {
            let mut dirs = Vec::new();
            let mut files = Vec::new();

            for i in 0..(count as usize).min(32) {
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
                    let _ = full_path.try_reserve_exact(self.current_dir.len() + len + 2);
                    full_path.push_str(&self.current_dir);
                    if !self.current_dir.ends_with('/') {
                        full_path.push('/');
                    }
                    full_path.push_str(name_str);

                    let mut st = sys::StatExtV1::default();
                    let is_dir = if sys::stat_ext(&full_path, &mut st) == 0 {
                        st.file_type == 2
                    } else {
                        false
                    };
                    let is_exe = !is_dir && (self.current_dir == "/bin" || self.current_dir == "/sbin");

                    let entry = FileEntry {
                        name: String::from(name_str),
                        is_dir,
                        is_exe,
                        size: st.size as u64,
                    };
                    if is_dir {
                        dirs.push(entry);
                    } else {
                        files.push(entry);
                    }
                }
            }

            dirs.sort_by(|a, b| a.name.cmp(&b.name));
            files.sort_by(|a, b| a.name.cmp(&b.name));
            self.entries.extend(dirs);
            self.entries.extend(files);
        }
    }

    pub fn navigate_up(&mut self) {
        if self.current_dir == "/" {
            return;
        }
        if let Some(idx) = self.current_dir.rfind('/') {
            if idx == 0 {
                self.current_dir.truncate(1);
            } else {
                self.current_dir.truncate(idx);
            }
            self.selected = 0;
            self.scroll = 0;
            self.refresh();
        }
    }

    pub fn navigate_to(&mut self, path: &str) {
        self.current_dir.clear();
        self.current_dir.push_str(path);
        self.selected = 0;
        self.scroll = 0;
        self.refresh();
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
                if self.current_dir == "/" {
                    self.current_dir.push_str(&name);
                } else {
                    self.current_dir.push('/');
                    self.current_dir.push_str(&name);
                }
                self.selected = 0;
                self.scroll = 0;
                self.refresh();
            }
            None
        } else {
            self.filename.clear();
            self.filename.push_str(&entry.name);
            Some(self.full_path())
        }
    }

    pub fn ensure_visible(&mut self, rows: usize) {
        let rows = rows.max(1);
        if self.selected < self.scroll {
            self.scroll = self.selected;
        } else if self.selected >= self.scroll + rows {
            self.scroll = self.selected + 1 - rows;
        }
    }

    pub fn handle_key(&mut self, b: u8) -> Option<FileDialogAction> {
        if b == 0x1b {
            // Esc: Cancel
            return Some(FileDialogAction::Cancel);
        }
        if b == b'\r' || b == b'\n' {
            // Enter
            if self.focus == FileDialogFocus::List
                && self.selected < self.entries.len()
                && self.entries[self.selected].is_dir
            {
                self.enter_selected();
                return None;
            }
            if !self.filename.is_empty() {
                let path = self.full_path();
                return match self.mode {
                    FileDialogMode::Save => Some(FileDialogAction::Save(path)),
                    FileDialogMode::Open => Some(FileDialogAction::Open(path)),
                };
            }
            return None;
        }
        if b == 0x08 || b == 127 {
            // Backspace
            if self.focus == FileDialogFocus::Filename {
                self.filename.pop();
            } else {
                self.navigate_up();
            }
            return None;
        }
        if b == b'\t' {
            // Tab: toggle focus between Filename and List
            self.focus = match self.focus {
                FileDialogFocus::Filename => FileDialogFocus::List,
                FileDialogFocus::List => FileDialogFocus::Filename,
            };
            return None;
        }

        // When focused on List, allow Up/Down list navigation
        if self.focus == FileDialogFocus::List {
            if b == b'A' || b == b'a' {
                // Up
                self.selected = self.selected.saturating_sub(1);
                self.ensure_visible(5);
                if self.selected < self.entries.len() && !self.entries[self.selected].is_dir {
                    self.filename.clear();
                    self.filename.push_str(&self.entries[self.selected].name);
                }
                return None;
            }
            if b == b'B' || b == b'b' {
                // Down
                if self.selected + 1 < self.entries.len() {
                    self.selected += 1;
                    self.ensure_visible(5);
                    if !self.entries[self.selected].is_dir {
                        self.filename.clear();
                        self.filename.push_str(&self.entries[self.selected].name);
                    }
                }
                return None;
            }
        }

        // Printable ASCII
        if (32..=126).contains(&b) {
            if self.filename.len() < 64 {
                self.filename.push(b as char);
                self.focus = FileDialogFocus::Filename;
            }
        }
        None
    }

    pub fn handle_click(
        &mut self,
        cx: usize,
        cy: usize,
        width: usize,
        height: usize,
    ) -> Option<FileDialogAction> {
        let mw: usize = 360usize.min(width.saturating_sub(20));
        let mh: usize = 210usize.min(height.saturating_sub(14));
        let mx = (width.saturating_sub(mw)) / 2;
        let my = (height.saturating_sub(mh)) / 2;

        if cx < mx || cx >= mx + mw || cy < my || cy >= my + mh {
            return None;
        }

        // [X] Close button
        if cx >= mx + mw - 26 && cx <= mx + mw - 6 && cy >= my + 2 && cy <= my + 22 {
            return Some(FileDialogAction::Cancel);
        }

        // Quick Navigation Buttons (nav_y = my + 28)
        let nav_y = my + 28;
        if cy >= nav_y && cy <= nav_y + 18 {
            if cx >= mx + 8 && cx <= mx + 48 {
                self.navigate_up();
                return None;
            } else if cx >= mx + 52 && cx <= mx + 70 {
                self.navigate_to("/");
                return None;
            } else if cx >= mx + 74 && cx <= mx + 112 {
                self.navigate_to("/tmp");
                return None;
            } else if cx >= mx + 116 && cx <= mx + 160 {
                self.navigate_to("/home");
                return None;
            }
        }

        // Entry List Area
        let list_y = my + 50;
        let list_h = mh.saturating_sub(106);
        if cy >= list_y && cy <= list_y + list_h && cx >= mx + 8 && cx <= mx + mw - 8 {
            let clicked_row = (cy - list_y) / 20;
            let actual_idx = self.scroll + clicked_row;
            if actual_idx < self.entries.len() {
                self.selected = actual_idx;
                self.focus = FileDialogFocus::List;
                if self.entries[actual_idx].is_dir {
                    self.enter_selected();
                } else {
                    self.filename.clear();
                    self.filename.push_str(&self.entries[actual_idx].name);
                }
            }
            return None;
        }

        // Filename Input Area
        let input_y = my + mh - 52;
        if cy >= input_y - 2 && cy <= input_y + 20 && cx >= mx + 46 && cx <= mx + mw - 8 {
            self.focus = FileDialogFocus::Filename;
            return None;
        }

        // Action Buttons Row (btn_y = my + mh - 28)
        let btn_y = my + mh - 28;
        if cy >= btn_y && cy <= btn_y + 20 {
            // Save / Open Action Button
            if cx >= mx + mw - 146 && cx <= mx + mw - 80 {
                if !self.filename.is_empty() {
                    let path = self.full_path();
                    return match self.mode {
                        FileDialogMode::Save => Some(FileDialogAction::Save(path)),
                        FileDialogMode::Open => Some(FileDialogAction::Open(path)),
                    };
                }
            }
            // Cancel Button
            if cx >= mx + mw - 74 && cx <= mx + mw - 8 {
                return Some(FileDialogAction::Cancel);
            }
        }

        None
    }

    pub fn render(&self, c: &mut Canvas) {
        let (width, height) = (c.width, c.height);
        if width < 30 || height < 30 {
            return;
        }

        // Step 1: Backdrop Dimming (darken the underlying app window by 50%)
        for row in 0..height {
            let row_offset = row * c.stride;
            for col in 0..width {
                let p = c.pixels[row_offset + col];
                let r = ((p >> 16) & 0xff) / 2;
                let g = ((p >> 8) & 0xff) / 2;
                let b = (p & 0xff) / 2;
                c.pixels[row_offset + col] = 0xff000000 | (r << 16) | (g << 8) | b;
            }
        }

        // Step 2: Elevated Modal Card Frame
        let mw: usize = 360usize.min(width.saturating_sub(20));
        let mh: usize = 210usize.min(height.saturating_sub(14));
        let mx = (width.saturating_sub(mw)) / 2;
        let my = (height.saturating_sub(mh)) / 2;

        let card_bg = 0xff20242c;
        let card_border = 0xff3b82f6; // FortressOS Accent Blue
        c.rounded_box(mx, my, mw, mh, 6, 0x00000000, card_bg, card_border);

        // Step 3: Modal Title Bar
        let hdr_h = 24;
        let hdr_bg = 0xff161d28;
        c.fill_rect(mx + 1, my + 1, mw - 2, hdr_h, hdr_bg);
        c.fill_rect(mx + 1, my + hdr_h, mw - 2, 1, 0xff283548);
        let title = match self.mode {
            FileDialogMode::Save => "Save File",
            FileDialogMode::Open => "Open File",
        };
        c.text_bold(mx + 10, my + 4, title, 0xffffffff, hdr_bg);
        c.pill_button(
            mx + mw - 24,
            my + 3,
            18,
            18,
            "X",
            0xff9fb0c7,
            0xff1f2736,
            0xff334155,
            hdr_bg,
        );

        // Step 4: Quick Navigation Bar
        let nav_y = my + 28;
        c.pill_button(mx + 8, nav_y, 40, 18, "^ Up", 0xff38bdf8, 0xff1b2432, 0xff2e3d52, card_bg);
        c.pill_button(mx + 52, nav_y, 18, 18, "/", 0xff9fb0c7, 0xff1a1f29, 0xff2a3342, card_bg);
        c.pill_button(mx + 74, nav_y, 38, 18, "/tmp", 0xff9fb0c7, 0xff1a1f29, 0xff2a3342, card_bg);
        c.pill_button(mx + 116, nav_y, 44, 18, "/home", 0xff9fb0c7, 0xff1a1f29, 0xff2a3342, card_bg);

        if mw > 180 {
            let path_x = mx + 164;
            let path_w = (mx + mw - 8).saturating_sub(path_x);
            c.rounded_box(path_x, nav_y, path_w, 18, 4, card_bg, 0xff0d1015, 0xff252e3d);
            c.text(path_x + 6, nav_y + 1, &self.current_dir, 0xffd5deeb, 0xff0d1015);
        }

        // Step 5: Mini File Manager Entry List
        let list_y = my + 50;
        let list_h = mh.saturating_sub(106);
        let list_w = mw.saturating_sub(16);
        c.rounded_box(mx + 8, list_y, list_w, list_h, 4, card_bg, 0xff15181f, 0xff252e3d);

        let max_items = list_h / 20;
        for (idx, entry) in self.entries.iter().skip(self.scroll).take(max_items).enumerate() {
            let actual_idx = self.scroll + idx;
            let is_sel = actual_idx == self.selected;
            let row_y = list_y + 2 + idx * 20;
            let row_w = list_w.saturating_sub(4);
            let row_bg = if is_sel {
                0xff1e385b
            } else if actual_idx % 2 == 0 {
                0xff181c24
            } else {
                0xff15181f
            };

            if is_sel {
                c.rounded_box(mx + 10, row_y, row_w, 18, 3, 0xff15181f, row_bg, 0xff3b82f6);
            } else {
                c.fill_rect(mx + 10, row_y, row_w, 18, row_bg);
            }

            let icon_kind = if entry.is_dir { 0 } else if entry.is_exe { 1 } else { 2 };
            c.file_icon(mx + 14, row_y + 2, icon_kind, row_bg);

            let name_fg = if is_sel {
                0xffffffff
            } else if entry.is_dir {
                0xff38bdf8
            } else {
                0xffe2e8f0
            };
            if entry.is_dir || is_sel {
                c.text_bold(mx + 34, row_y + 1, &entry.name, name_fg, row_bg);
            } else {
                c.text(mx + 34, row_y + 1, &entry.name, name_fg, row_bg);
            }

            if row_w > 180 && !entry.is_dir {
                let size_str = format_bytes(entry.size);
                c.text(
                    mx + 10 + row_w.saturating_sub(60),
                    row_y + 1,
                    &size_str,
                    0xff7e8796,
                    row_bg,
                );
            }
        }

        // Step 6: Filename Input Row
        let input_y = my + mh - 52;
        c.text(mx + 10, input_y, "File:", 0xff9fb0c7, card_bg);
        let input_x = mx + 46;
        let input_w = mw.saturating_sub(54);
        let input_border = if self.focus == FileDialogFocus::Filename {
            0xff3b82f6
        } else {
            0xff334155
        };
        c.rounded_box(input_x, input_y - 2, input_w, 20, 4, card_bg, 0xff0d1015, input_border);
        c.text(input_x + 6, input_y, &self.filename, 0xffffffff, 0xff0d1015);
        if self.focus == FileDialogFocus::Filename {
            let tw = theme::text_width(&self.filename);
            let cur_x = (input_x + 6 + tw).min(input_x + input_w - 4);
            c.fill_rect(cur_x, input_y + 2, 2, 12, 0xff007aff);
        }

        // Step 7: Action Buttons Row
        let btn_y = my + mh - 28;
        let action_label = match self.mode {
            FileDialogMode::Save => "Save",
            FileDialogMode::Open => "Open",
        };
        c.pill_button(
            mx + mw - 146,
            btn_y,
            66,
            20,
            action_label,
            0xffffffff,
            0xff1e385b,
            0xff3b82f6,
            card_bg,
        );
        c.pill_button(
            mx + mw - 74,
            btn_y,
            66,
            20,
            "Cancel",
            0xff9fb0c7,
            0xff1b2432,
            0xff2e3d52,
            card_bg,
        );
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
        if let Ok(st) = core::str::from_utf8(&buf[..len]) {
            s.push_str(st);
            s.push_str(" B");
        }
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
        if let Ok(st) = core::str::from_utf8(&buf[..len]) {
            s.push_str(st);
            s.push_str(" KB");
        }
    }
    s
}
