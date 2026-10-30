//! Graphical Text Editor ("Notepad") for FortressOS Desktop. Runs as its
//! own unprivileged process (src/bin/notepad.rs) and draws into a Canvas.

use crate::{canvas::Canvas, sys};
use alloc::{string::String, vec::Vec};

pub const NOTEPAD_BG: u32 = 0xff252525;
pub const NOTEPAD_FG: u32 = 0xffe0e0e0;
pub const NOTEPAD_HEADER: u32 = 0xff1e1e1e;
pub const NOTEPAD_LINENUM: u32 = 0xff666666;
pub const NOTEPAD_CURSOR: u32 = 0xff007aff;

pub struct Notepad {
    pub path: String,
    pub text: Vec<u8>,
    pub cursor: usize,
    #[allow(dead_code)]
    pub scroll_line: usize,
    pub modified: bool,
    pub status: Option<&'static str>,
    pub cursor_visible: bool,
    ansi_state: u8,
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
            scroll_line: 0,
            modified: false,
            status: None,
            cursor_visible: true,
            ansi_state: 0,
        }
    }

    pub fn insert_char(&mut self, c: u8) {
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
        if self.cursor > 0 && !self.text.is_empty() {
            self.cursor -= 1;
            let pos = self.cursor.min(self.text.len().saturating_sub(1));
            self.text.remove(pos);
            self.modified = true;
            self.status = None;
        }
    }

    pub fn delete_forward(&mut self) {
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
                match b {
                    b'A' => {
                        self.move_cursor_up();
                        return true;
                    }
                    b'B' => {
                        self.move_cursor_down();
                        return true;
                    }
                    b'C' => {
                        self.move_cursor_right();
                        return true;
                    }
                    b'D' => {
                        self.move_cursor_left();
                        return true;
                    }
                    b'H' => {
                        self.move_cursor_home();
                        return true;
                    }
                    b'F' => {
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
            // Ctrl+S / Ctrl+R
            self.save();
            true
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

    /// Handles mouse clicks in client coordinates. Returns true if re-render is needed.
    pub fn handle_click(&mut self, cx: usize, cy: usize) -> bool {
        self.cursor_visible = true;
        if cy <= 20 {
            // Clicked header toolbar
            if cx >= 8 && cx <= 76 {
                // Clicked [S] Save
                self.save();
                return true;
            }
            return false;
        }

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

        self.cursor = target_idx.min(self.text.len());
        true
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
        c.fill(NOTEPAD_BG);

        // Header toolbar (20px)
        c.fill_rect(0, 0, width, 20, NOTEPAD_HEADER);
        c.text(8, 2, "[S] Save", 0xff58a6ff, NOTEPAD_HEADER);
        c.text(88, 2, "Path:", 0xff8b949e, NOTEPAD_HEADER);
        c.text(136, 2, &self.path, NOTEPAD_FG, NOTEPAD_HEADER);
        if self.modified {
            c.text(
                width.saturating_sub(90),
                2,
                "[Modified]",
                0xffe5c07b,
                NOTEPAD_HEADER,
            );
        } else if let Some(st) = self.status {
            c.text(width.saturating_sub(120), 2, st, 0xff3fb950, NOTEPAD_HEADER);
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
        draw_line_num(c, 8, cur_y, line_idx + 1);

        for (idx, &b) in self.text.iter().enumerate() {
            if idx == self.cursor {
                cursor_px = (48 + col_idx * 8, cur_y);
            }
            if b == b'\n' {
                line_idx += 1;
                col_idx = 0;
                row_count += 1;
                cur_y += 16;
                if row_count >= max_rows {
                    break;
                }
                draw_line_num(c, 8, cur_y, line_idx + 1);
            } else if (32..=126).contains(&b) {
                if col_idx < max_cols && row_count < max_rows {
                    c.glyph(48 + col_idx * 8, cur_y, b, NOTEPAD_FG, NOTEPAD_BG);
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
        let footer_y = height.saturating_sub(18);
        c.fill_rect(0, footer_y, width, height - footer_y, NOTEPAD_HEADER);
        c.text(8, footer_y + 1, "UTF-8", 0xff8b949e, NOTEPAD_HEADER);
        c.text(
            80,
            footer_y + 1,
            "FortressOS Notes",
            0xff8b949e,
            NOTEPAD_HEADER,
        );
    }
}

fn draw_line_num(c: &mut Canvas, x: usize, y: usize, num: usize) {
    let mut buf = [b' '; 4];
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
    buf[3] = b'|';
    for (i, &b) in buf.iter().enumerate() {
        c.glyph(x + i * 8, y, b, NOTEPAD_LINENUM, NOTEPAD_BG);
    }
}
