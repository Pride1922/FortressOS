//! Graphical Text Editor ("Notepad") for FortressOS Desktop. Runs as its
//! own unprivileged process (src/bin/notepad.rs) and draws into a Canvas.

use crate::{
    canvas::Canvas,
    clipboard,
    file_dialog::{FileDialog, FileDialogAction, FileDialogMode},
    sys,
};
use alloc::{string::String, vec::Vec};
use core::sync::atomic::{AtomicUsize, Ordering};

#[allow(dead_code)]
pub const NOTEPAD_BG: u32 = 0xff252525;
pub const NOTEPAD_FG: u32 = 0xffe0e0e0;
#[allow(dead_code)]
pub const NOTEPAD_HEADER: u32 = 0xff1e1e1e;
#[allow(dead_code)]
pub const NOTEPAD_LINENUM: u32 = 0xff666666;
pub const NOTEPAD_CURSOR: u32 = 0xff007aff;

pub struct Notepad {
    pub path: String,
    pub text: Vec<u8>,
    pub cursor: usize,
    pub selection_anchor: Option<usize>,
    last_click_idx: Option<usize>,
    #[allow(dead_code)]
    pub scroll_line: usize,
    pub modified: bool,
    pub status: Option<&'static str>,
    pub cursor_visible: bool,
    ansi_state: u8,
    pub dialog: Option<FileDialog>,
    canvas_width: AtomicUsize,
    canvas_height: AtomicUsize,
}

impl Notepad {
    #[inline(never)]
    pub fn new(path: &str) -> Self {
        let mut path_str = String::new();
        let _ = path_str.try_reserve_exact(path.len());
        path_str.push_str(path);

        let mut text = Vec::new();
        let mut loaded = false;

        // Try reading existing file
        let fd = sys::open(path, sys::VFS_O_RDONLY as i32);
        if fd >= 0 {
            let mut buf = [0u8; 512];
            let mut total = 0;
            while total < 4096 {
                let n = sys::read(fd as i32, &mut buf);
                if n <= 0 {
                    break;
                }
                let chunk = &buf[..n as usize];
                text.extend_from_slice(chunk);
                total += n as usize;
            }
            sys::close(fd as i32);
            loaded = true;
        }

        if !loaded || text.is_empty() {
            let default_text = b"# FortressOS Notes\n# Edit system files & personal notes\n\nWelcome to FortressOS Desktop!\n";
            text.extend_from_slice(default_text);
        }

        Self {
            path: path_str,
            text,
            cursor: 0,
            selection_anchor: None,
            last_click_idx: None,
            scroll_line: 0,
            modified: false,
            status: None,
            cursor_visible: true,
            ansi_state: 0,
            dialog: None,
            canvas_width: AtomicUsize::new(480),
            canvas_height: AtomicUsize::new(280),
        }
    }

    #[inline]
    fn is_word_byte(b: u8) -> bool {
        b.is_ascii_alphanumeric() || b == b'_' || b == b'-'
    }

    pub fn move_cursor_word_left(&mut self) {
        let mut i = self.cursor.min(self.text.len());
        while i > 0 && !Self::is_word_byte(self.text[i - 1]) {
            i -= 1;
        }
        while i > 0 && Self::is_word_byte(self.text[i - 1]) {
            i -= 1;
        }
        self.cursor = i;
    }

    pub fn move_cursor_word_right(&mut self) {
        let len = self.text.len();
        let mut i = self.cursor.min(len);
        while i < len && !Self::is_word_byte(self.text[i]) {
            i += 1;
        }
        while i < len && Self::is_word_byte(self.text[i]) {
            i += 1;
        }
        self.cursor = i;
    }

    pub fn select_word_at(&mut self, idx: usize) -> bool {
        let len = self.text.len();
        if len == 0 {
            return false;
        }
        let mut pos = idx.min(len);
        if pos == len || !Self::is_word_byte(self.text[pos]) {
            if pos > 0 && Self::is_word_byte(self.text[pos - 1]) {
                pos -= 1;
            } else {
                return false;
            }
        }
        let mut start = pos;
        while start > 0 && Self::is_word_byte(self.text[start - 1]) {
            start -= 1;
        }
        let mut end = pos;
        while end < len && Self::is_word_byte(self.text[end]) {
            end += 1;
        }
        if start < end {
            self.selection_anchor = Some(start);
            self.cursor = end;
            self.status = Some("Selected word");
            true
        } else {
            false
        }
    }

    pub fn selected_range(&self) -> Option<(usize, usize)> {
        let anchor = self.selection_anchor?.min(self.text.len());
        let cur = self.cursor.min(self.text.len());
        if anchor != cur {
            Some((anchor.min(cur), anchor.max(cur)))
        } else {
            None
        }
    }

    pub fn select_all(&mut self) {
        if !self.text.is_empty() {
            self.selection_anchor = Some(0);
            self.cursor = self.text.len();
            self.status = Some("Selected all");
        }
    }

    fn delete_selection(&mut self) -> bool {
        if let Some((start, end)) = self.selected_range() {
            self.text.drain(start..end);
            self.cursor = start;
            self.selection_anchor = None;
            self.modified = true;
            self.status = None;
            true
        } else {
            self.selection_anchor = None;
            false
        }
    }

    pub fn insert_char(&mut self, c: u8) {
        let _ = self.delete_selection();
        if self.text.len() >= 4096 {
            return;
        }
        let pos = self.cursor.min(self.text.len());
        self.text.insert(pos, c);
        self.cursor += 1;
        self.modified = true;
        self.status = None;
    }

    pub fn delete_back(&mut self) {
        if self.delete_selection() {
            return;
        }
        if self.cursor > 0 && !self.text.is_empty() {
            self.cursor -= 1;
            let pos = self.cursor.min(self.text.len().saturating_sub(1));
            self.text.remove(pos);
            self.modified = true;
            self.status = None;
        }
    }

    pub fn delete_forward(&mut self) {
        if self.delete_selection() {
            return;
        }
        if self.cursor < self.text.len() {
            self.text.remove(self.cursor);
            self.modified = true;
            self.status = None;
        }
    }

    pub fn move_cursor_left(&mut self) {
        self.cursor = self.cursor.saturating_sub(1);
    }

    pub fn move_cursor_right(&mut self) {
        self.cursor = (self.cursor + 1).min(self.text.len());
    }

    pub fn move_cursor_home(&mut self) {
        while self.cursor > 0 && self.text[self.cursor - 1] != b'\n' {
            self.cursor -= 1;
        }
    }

    pub fn move_cursor_end(&mut self) {
        while self.cursor < self.text.len() && self.text[self.cursor] != b'\n' {
            self.cursor += 1;
        }
    }

    pub fn move_cursor_up(&mut self) {
        // Find current col
        let mut col = 0;
        let mut idx = self.cursor;
        while idx > 0 && self.text[idx - 1] != b'\n' {
            idx -= 1;
            col += 1;
        }
        if idx == 0 {
            // Already on first line
            self.cursor = 0;
            return;
        }
        // idx - 1 is the newline of the previous line
        let prev_line_end = idx - 1;
        let mut prev_line_start = prev_line_end;
        while prev_line_start > 0 && self.text[prev_line_start - 1] != b'\n' {
            prev_line_start -= 1;
        }
        let prev_line_len = prev_line_end - prev_line_start;
        self.cursor = prev_line_start + col.min(prev_line_len);
    }

    pub fn move_cursor_down(&mut self) {
        // Find current col
        let mut col = 0;
        let mut idx = self.cursor;
        while idx > 0 && self.text[idx - 1] != b'\n' {
            idx -= 1;
            col += 1;
        }
        // Find start of next line
        let mut next_start = self.cursor;
        while next_start < self.text.len() && self.text[next_start] != b'\n' {
            next_start += 1;
        }
        if next_start >= self.text.len() {
            // Already on last line
            self.cursor = self.text.len();
            return;
        }
        next_start += 1; // skip '\n'
        let mut next_end = next_start;
        while next_end < self.text.len() && self.text[next_end] != b'\n' {
            next_end += 1;
        }
        let next_len = next_end - next_start;
        self.cursor = next_start + col.min(next_len);
    }

    pub fn handle_key(&mut self, b: u8) -> bool {
        if let Some(ref mut dlg) = self.dialog {
            match dlg.handle_key(b) {
                Some(FileDialogAction::Save(full_path)) => {
                    self.save_to(&full_path);
                    self.dialog = None;
                }
                Some(FileDialogAction::Open(full_path)) => {
                    self.load_from(&full_path);
                    self.dialog = None;
                }
                Some(FileDialogAction::Cancel) => {
                    self.dialog = None;
                }
                None => {}
            }
            return true;
        }

        self.cursor_visible = true;
        match self.ansi_state {
            1 => {
                if b == b'[' {
                    self.ansi_state = 2;
                } else {
                    self.ansi_state = 0;
                }
                return false;
            }
            2 => {
                self.ansi_state = 0;
                let extend_sel = matches!(b, b'a'..=b'd' | b'f' | b'h' | b'q' | b'w' | b'Q' | b'W');
                if extend_sel {
                    if self.selection_anchor.is_none() {
                        self.selection_anchor = Some(self.cursor);
                    }
                } else if matches!(b, b'A'..=b'D' | b'F' | b'H') {
                    self.selection_anchor = None;
                }
                match b {
                    b'A' | b'a' => {
                        self.move_cursor_up();
                        return true;
                    }
                    b'B' | b'b' => {
                        self.move_cursor_down();
                        return true;
                    }
                    b'C' | b'c' => {
                        self.move_cursor_right();
                        return true;
                    }
                    b'D' | b'd' => {
                        self.move_cursor_left();
                        return true;
                    }
                    b'W' | b'w' => {
                        self.move_cursor_word_right();
                        return true;
                    }
                    b'Q' | b'q' => {
                        self.move_cursor_word_left();
                        return true;
                    }
                    b'H' | b'h' => {
                        self.move_cursor_home();
                        return true;
                    }
                    b'F' | b'f' => {
                        self.move_cursor_end();
                        return true;
                    }
                    b'3' => {
                        self.ansi_state = 3;
                        return false;
                    }
                    _ => return false,
                }
            }
            3 => {
                self.ansi_state = 0;
                if b == b'~' {
                    self.delete_forward();
                    return true;
                }
                return false;
            }
            _ => {
                if b == 0x1b {
                    self.ansi_state = 1;
                    return false;
                }
            }
        }

        if b == 19 || b == 18 {
            // Ctrl+S / Ctrl+R: Quick save to current file if set, or open save dialog if empty
            if self.path.is_empty() {
                self.open_save_dialog();
            } else {
                self.save();
            }
            true
        } else if b == 15 {
            // Ctrl+O: Open file dialog
            self.open_load_dialog();
            true
        } else if b == 1 {
            // Ctrl+A: Select all text
            self.select_all();
            true
        } else if b == 23 || b == 4 {
            // Ctrl+W / Ctrl+D: Select word under cursor
            self.select_word_at(self.cursor)
        } else if b == 3 {
            // Ctrl+C: Copy selection (or current line) to clipboard
            self.copy_current_line()
        } else if b == 24 {
            // Ctrl+X: Cut selection (or current line) to clipboard
            self.cut_current_line()
        } else if b == 22 {
            // Ctrl+V: Paste from clipboard at cursor (replacing selection if any)
            self.paste_clipboard()
        } else if b == 0x08 || b == 127 {
            // Backspace
            self.delete_back();
            true
        } else if b == b'\r' || b == b'\n' {
            // Enter
            self.insert_char(b'\n');
            true
        } else if b == b'\t' {
            // Tab
            self.insert_char(b' ');
            self.insert_char(b' ');
            self.insert_char(b' ');
            self.insert_char(b' ');
            true
        } else if (32..=126).contains(&b) {
            // Printable ASCII
            self.insert_char(b);
            true
        } else {
            false
        }
    }

    pub fn current_line_bounds(&self) -> (usize, usize) {
        let mut start = self.cursor.min(self.text.len());
        while start > 0 && self.text[start - 1] != b'\n' {
            start -= 1;
        }
        let mut end = self.cursor.min(self.text.len());
        while end < self.text.len() && self.text[end] != b'\n' {
            end += 1;
        }
        (start, end)
    }

    pub fn copy_current_line(&mut self) -> bool {
        if let Some((start, end)) = self.selected_range() {
            if let Ok(s) = core::str::from_utf8(&self.text[start..end]) {
                if clipboard::set(s) {
                    self.status = Some("Copied selection");
                    return true;
                }
            }
            return false;
        }
        let (start, end) = self.current_line_bounds();
        let slice = if start < end {
            &self.text[start..end]
        } else {
            &self.text[..]
        };
        if let Ok(s) = core::str::from_utf8(slice) {
            if clipboard::set(s) {
                self.status = Some("Copied to clipboard");
                return true;
            }
        }
        false
    }

    pub fn cut_current_line(&mut self) -> bool {
        if self.text.is_empty() {
            return false;
        }
        if let Some((start, end)) = self.selected_range() {
            if let Ok(s) = core::str::from_utf8(&self.text[start..end]) {
                let _ = clipboard::set(s);
            }
            self.text.drain(start..end);
            self.cursor = start.min(self.text.len());
            self.selection_anchor = None;
            self.modified = true;
            self.status = Some("Cut selection");
            return true;
        }
        let (start, mut end) = self.current_line_bounds();
        let slice = &self.text[start..end];
        if let Ok(s) = core::str::from_utf8(slice) {
            let _ = clipboard::set(s);
        }
        if end < self.text.len() && self.text[end] == b'\n' {
            end += 1;
        }
        if start < end {
            self.text.drain(start..end);
            self.cursor = start.min(self.text.len());
            self.selection_anchor = None;
            self.modified = true;
            self.status = Some("Cut to clipboard");
            return true;
        }
        false
    }

    pub fn paste_clipboard(&mut self) -> bool {
        let Some(clip) = clipboard::get() else {
            return false;
        };
        let mut inserted = self.delete_selection();
        for &b in clip.as_bytes() {
            if self.text.len() >= 4096 {
                break;
            }
            if (32..=126).contains(&b) || b == b'\n' {
                let pos = self.cursor.min(self.text.len());
                self.text.insert(pos, b);
                self.cursor += 1;
                inserted = true;
            }
        }
        if inserted {
            self.modified = true;
            self.status = Some("Pasted from clipboard");
        }
        inserted
    }

    fn index_at_pixel(&self, cx: usize, cy: usize) -> usize {
        let target_line = (cy.saturating_sub(24)) / 16;
        let target_col = cx.saturating_sub(48) / 8;

        let mut cur_line = 0;
        let mut cur_col = 0;
        let mut target_idx = self.text.len();

        for (idx, &b) in self.text.iter().enumerate() {
            if cur_line == target_line {
                if cur_col >= target_col || b == b'\n' {
                    target_idx = idx;
                    break;
                }
                cur_col += 1;
            } else if b == b'\n' {
                cur_line += 1;
                cur_col = 0;
            }
        }
        target_idx.min(self.text.len())
    }

    pub fn open_save_dialog(&mut self) {
        self.dialog = Some(FileDialog::new(FileDialogMode::Save, &self.path));
    }

    pub fn open_load_dialog(&mut self) {
        self.dialog = Some(FileDialog::new(FileDialogMode::Open, &self.path));
    }

    pub fn save_to(&mut self, path: &str) -> bool {
        self.path.clear();
        self.path.push_str(path);
        self.save()
    }

    pub fn load_from(&mut self, path: &str) -> bool {
        let fd = sys::open(path, sys::VFS_O_RDONLY as i32);
        if fd < 0 {
            self.status = Some("Error opening file");
            return false;
        }
        self.text.clear();
        let mut buf = [0u8; 512];
        let mut total = 0;
        while total < 4096 {
            let n = sys::read(fd as i32, &mut buf);
            if n <= 0 {
                break;
            }
            let chunk = &buf[..n as usize];
            self.text.extend_from_slice(chunk);
            total += n as usize;
        }
        sys::close(fd as i32);
        self.path.clear();
        self.path.push_str(path);
        self.cursor = 0;
        self.selection_anchor = None;
        self.modified = false;
        self.status = Some("Opened file");
        true
    }

    /// Handles mouse clicks in client coordinates. Returns true if re-render is needed.
    pub fn handle_click(&mut self, cx: usize, cy: usize) -> bool {
        self.cursor_visible = true;
        if let Some(ref mut dlg) = self.dialog {
            let cw = self.canvas_width.load(Ordering::Relaxed);
            let ch = self.canvas_height.load(Ordering::Relaxed);
            match dlg.handle_click(cx, cy, cw, ch) {
                Some(FileDialogAction::Save(full_path)) => {
                    self.save_to(&full_path);
                    self.dialog = None;
                }
                Some(FileDialogAction::Open(full_path)) => {
                    self.load_from(&full_path);
                    self.dialog = None;
                }
                Some(FileDialogAction::Cancel) => {
                    self.dialog = None;
                }
                None => {}
            }
            return true;
        }

        if cy <= 20 {
            // Clicked header toolbar
            if (8..=72).contains(&cx) {
                self.open_save_dialog();
                return true;
            } else if (78..=130).contains(&cx) {
                return self.copy_current_line();
            } else if (136..=196).contains(&cx) {
                return self.paste_clipboard();
            } else if (202..=266).contains(&cx) {
                self.select_all();
                return true;
            } else if (272..=318).contains(&cx) {
                return self.cut_current_line();
            } else if (324..=374).contains(&cx) {
                self.open_load_dialog();
                return true;
            }
            return false;
        }

        let idx = self.index_at_pixel(cx, cy);
        if self.last_click_idx == Some(idx) && self.selected_range().is_none() {
            self.last_click_idx = None;
            if self.select_word_at(idx) {
                return true;
            }
        }
        self.last_click_idx = Some(idx);
        self.cursor = idx;
        self.selection_anchor = Some(idx);
        true
    }

    /// Handles mouse drag motion while left button is held down to select text.
    pub fn handle_drag(&mut self, cx: usize, cy: usize) -> bool {
        if self.dialog.is_some() || cy <= 20 {
            return false;
        }
        let idx = self.index_at_pixel(cx, cy);
        if self.selection_anchor.is_none() {
            self.selection_anchor = Some(self.cursor);
        }
        if self.cursor != idx {
            self.last_click_idx = None;
            self.cursor = idx;
            self.cursor_visible = true;
            return true;
        }
        false
    }

    #[allow(dead_code)]
    pub fn save(&mut self) -> bool {
        let flags = sys::VFS_O_WRONLY | sys::VFS_O_CREAT | sys::VFS_O_TRUNC;
        let fd = sys::open(&self.path, flags as i32);
        if fd < 0 {
            self.status = Some("Error saving file");
            return false;
        }
        let written = sys::write(fd as i32, &self.text);
        sys::close(fd as i32);

        if written >= 0 {
            self.modified = false;
            self.status = Some("Saved successfully");
            true
        } else {
            self.status = Some("Write failed");
            false
        }
    }

    pub fn render(&self, c: &mut Canvas) {
        let (width, height) = (c.width, c.height);
        if width < 30 || height < 30 {
            return;
        }
        self.canvas_width.store(width, Ordering::Relaxed);
        self.canvas_height.store(height, Ordering::Relaxed);

        let ed_bg = 0xff181b22;
        let gutter_bg = 0xff12151b;
        let hdr_bg = 0xff12151b;
        c.fill(ed_bg);

        // Dedicated line-number gutter background
        let footer_y = height.saturating_sub(18);
        if footer_y > 23 {
            c.fill_rect(0, 23, 42.min(width), footer_y - 23, gutter_bg);
            if width > 42 {
                c.fill_rect(42, 23, 1, footer_y - 23, 0xff242b38);
            }
        }

        // Header toolbar (22px)
        c.fill_rect(0, 0, width, 22, hdr_bg);
        c.fill_rect(0, 22, width, 1, 0xff28303e);
        c.pill_button(8, 2, 64, 18, "Save", 0xffffffff, 0xff1e385b, 0xff3b82f6, hdr_bg);
        c.pill_button(78, 2, 52, 18, "Copy", 0xffd5deeb, 0xff1b2432, 0xff2e3d52, hdr_bg);
        c.pill_button(136, 2, 60, 18, "Paste", 0xffd5deeb, 0xff1b2432, 0xff2e3d52, hdr_bg);
        c.pill_button(202, 2, 64, 18, "Select", 0xffd5deeb, 0xff1b2432, 0xff2e3d52, hdr_bg);
        c.pill_button(272, 2, 46, 18, "Cut", 0xffd5deeb, 0xff1b2432, 0xff2e3d52, hdr_bg);
        c.pill_button(324, 2, 50, 18, "Open", 0xffd5deeb, 0xff1b2432, 0xff2e3d52, hdr_bg);
        if width > 450 {
            let path_w = width.saturating_sub(444);
            c.rounded_box(380, 2, path_w, 18, 5, hdr_bg, 0xff0d1015, 0xff252e3d);
            c.text(388, 2, &self.path, NOTEPAD_FG, 0xff0d1015);
        }
        if self.modified && width >= 170 {
            c.pill_button(
                width.saturating_sub(88),
                2,
                80,
                18,
                "Modified",
                0xfffacc15,
                0xff332b14,
                0xffeab308,
                hdr_bg,
            );
        } else if let Some(st) = self.status {
            if width >= 200 {
                c.text(width.saturating_sub(136), 2, st, 0xff4ade80, hdr_bg);
            }
        }

        // Determine active cursor (line, col) first so we can highlight the active row
        let mut active_line = 0usize;
        let mut active_col = 0usize;
        for (idx, &b) in self.text.iter().enumerate() {
            if idx == self.cursor {
                break;
            }
            if b == b'\n' {
                active_line += 1;
                active_col = 0;
            } else {
                active_col += 1;
            }
        }

        // Text display area (from y = 24 to height - 20)
        let text_area_h = height.saturating_sub(44);
        let max_rows = text_area_h / 16;
        let max_cols = width.saturating_sub(48) / 8;

        let mut line_idx = 0;
        let mut col_idx = 0;
        let mut cursor_px = (48usize, 24usize);
        let mut cur_y = 24;
        let mut row_count = 0;

        let draw_row_chrome = |c: &mut Canvas, row_y: usize, l_idx: usize| {
            let is_active = l_idx == active_line;
            let g_bg = if is_active { 0xff19202c } else { gutter_bg };
            let r_bg = if is_active { 0xff1f2634 } else { ed_bg };
            if is_active && row_y + 16 <= footer_y {
                c.fill_rect(0, row_y, 42.min(width), 16, g_bg);
                if width > 43 {
                    c.fill_rect(43, row_y, width - 43, 16, r_bg);
                }
            }
            draw_line_num(c, 8, row_y, l_idx + 1, is_active, g_bg);
            r_bg
        };

        let mut cur_row_bg = draw_row_chrome(c, cur_y, line_idx);
        let sel_range = self.selected_range();

        for (idx, &b) in self.text.iter().enumerate() {
            if idx == self.cursor {
                cursor_px = (48 + col_idx * 8, cur_y);
            }
            let is_selected = sel_range.is_some_and(|(s, e)| idx >= s && idx < e);
            if b == b'\n' {
                if is_selected && col_idx < max_cols && row_count < max_rows {
                    c.fill_rect(48 + col_idx * 8, cur_y, 8, 16, 0xff264f78);
                }
                line_idx += 1;
                col_idx = 0;
                row_count += 1;
                cur_y += 16;
                if row_count >= max_rows {
                    break;
                }
                cur_row_bg = draw_row_chrome(c, cur_y, line_idx);
            } else if (32..=126).contains(&b) {
                if col_idx < max_cols && row_count < max_rows {
                    let cell_bg = if is_selected { 0xff264f78 } else { cur_row_bg };
                    let cell_fg = if is_selected { 0xffffffff } else { NOTEPAD_FG };
                    if is_selected {
                        c.fill_rect(48 + col_idx * 8, cur_y, 8, 16, cell_bg);
                    }
                    c.glyph(48 + col_idx * 8, cur_y, b, cell_fg, cell_bg);
                }
                col_idx += 1;
            }
        }
        if self.cursor >= self.text.len() {
            cursor_px = (48 + col_idx * 8, cur_y);
        }

        // Crisp 2px vertical bar cursor
        if self.cursor_visible {
            let (cx, cy) = cursor_px;
            if cx + 2 <= width && cy + 16 <= height {
                c.fill_rect(cx, cy + 1, 2, 14, NOTEPAD_CURSOR);
            }
        }

        // Footer status bar (bottom 18px)
        if footer_y > 0 {
            c.fill_rect(0, footer_y - 1, width, 1, 0xff28303e);
        }
        c.fill_rect(0, footer_y, width, height - footer_y, hdr_bg);
        let mut pos_label = alloc::string::String::new();
        let _ = pos_label.try_reserve_exact(24);
        let _ = core::fmt::Write::write_fmt(
            &mut pos_label,
            format_args!("Ln {}, Col {}", active_line + 1, active_col + 1),
        );
        c.text(10, footer_y + 1, &pos_label, 0xff9fb0c7, hdr_bg);
        if width >= 260 {
            c.text(width.saturating_sub(188), footer_y + 1, "UTF-8", 0xff7e8796, hdr_bg);
            c.text(
                width.saturating_sub(124),
                footer_y + 1,
                "Fortress Notes",
                0xff7e8796,
                hdr_bg,
            );
        }

        if let Some(ref dlg) = self.dialog {
            dlg.render(c);
        }
    }
}

fn draw_line_num(c: &mut Canvas, x: usize, y: usize, num: usize, active: bool, bg: u32) {
    let mut buf = [b' '; 3];
    let n = num % 1000;
    if n >= 100 {
        buf[0] = b'0' + (n / 100) as u8;
        buf[1] = b'0' + ((n / 10) % 10) as u8;
        buf[2] = b'0' + (n % 10) as u8;
    } else if n >= 10 {
        buf[1] = b'0' + (n / 10) as u8;
        buf[2] = b'0' + (n % 10) as u8;
    } else {
        buf[2] = b'0' + n as u8;
    }
    let fg = if active { 0xff9fb0c7 } else { 0xff525c6e };
    for (i, &b) in buf.iter().enumerate() {
        c.glyph(x + i * 8, y, b, fg, bg);
    }
}
