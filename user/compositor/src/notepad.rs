//! Graphical Text Editor ("Notepad") for FortressOS Desktop.

use crate::{
    font::GLYPHS,
    sys,
    window::Window,
};
use alloc::{string::String, vec::Vec};

pub const NOTEPAD_BG: u32 = 0xff161b22;
pub const NOTEPAD_FG: u32 = 0xffe6edf3;
pub const NOTEPAD_HEADER: u32 = 0xff0d1117;
pub const NOTEPAD_LINENUM: u32 = 0xff484f58;
pub const NOTEPAD_CURSOR: u32 = 0xff58a6ff;

pub struct Notepad {
    #[allow(dead_code)]
    pub window_id: u32,
    pub path: String,
    pub text: Vec<u8>,
    pub cursor: usize,
    #[allow(dead_code)]
    pub scroll_line: usize,
    pub modified: bool,
    pub status: Option<&'static str>,
}

impl Notepad {
    pub fn new(window_id: u32, path: &str) -> Self {
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
            let default_text = b"# FortressOS Notepad\n# Edit configuration files & notes\n# Press 'Ctrl+S' to Save\n\nWelcome to FortressOS Desktop!\n";
            text.extend_from_slice(default_text);
        }

        Self {
            window_id,
            path: path_str,
            text,
            cursor: 0,
            scroll_line: 0,
            modified: false,
            status: None,
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

    pub fn render(&self, w: &mut Window) {
        let width = w.rect.w.saturating_sub(2);
        let height = w.rect.h.saturating_sub(26);
        if width < 30 || height < 30 {
            return;
        }
        w.content.fill(NOTEPAD_BG);

        // Header toolbar (20px)
        for y in 0..20.min(height) {
            for x in 0..width {
                w.content[y * width + x] = NOTEPAD_HEADER;
            }
        }
        draw_text(w, 8, 2, "[S] Save", 0xff58a6ff, NOTEPAD_HEADER);
        draw_text(w, 88, 2, "Path:", 0xff8b949e, NOTEPAD_HEADER);
        draw_text(w, 136, 2, &self.path, NOTEPAD_FG, NOTEPAD_HEADER);
        if self.modified {
            draw_text(w, width.saturating_sub(90), 2, "[Modified]", 0xffe5c07b, NOTEPAD_HEADER);
        } else if let Some(st) = self.status {
            draw_text(w, width.saturating_sub(120), 2, st, 0xff3fb950, NOTEPAD_HEADER);
        }

        // Text display area (from y = 24 to height - 20)
        let text_area_h = height.saturating_sub(44);
        let max_rows = text_area_h / 16;
        let max_cols = width.saturating_sub(48) / 8;

        let mut line_idx = 0;
        let mut col_idx = 0;
        let mut cursor_px = (48usize, 24usize);

        // Render line numbers and characters
        let mut cur_y = 24;
        let mut row_count = 0;

        // Print first line number
        draw_line_num(w, 8, cur_y, line_idx + 1);

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
                draw_line_num(w, 8, cur_y, line_idx + 1);
            } else if (32..=126).contains(&b) {
                if col_idx < max_cols && row_count < max_rows {
                    draw_char(w, 48 + col_idx * 8, cur_y, b, NOTEPAD_FG, NOTEPAD_BG);
                }
                col_idx += 1;
            }
        }
        if self.cursor >= self.text.len() {
            cursor_px = (48 + col_idx * 8, cur_y);
        }

        // Draw cursor line
        let (cx, cy) = cursor_px;
        if cx + 8 <= width && cy + 16 <= height {
            for r in 13..16 {
                for c in 0..8 {
                    if cy + r < height && cx + c < width {
                        w.content[(cy + r) * width + cx + c] = NOTEPAD_CURSOR;
                    }
                }
            }
        }

        // Footer status bar (bottom 18px)
        let footer_y = height.saturating_sub(18);
        for y in footer_y..height {
            for x in 0..width {
                w.content[y * width + x] = NOTEPAD_HEADER;
            }
        }
        draw_text(w, 8, footer_y + 1, "UTF-8", 0xff8b949e, NOTEPAD_HEADER);
        draw_text(w, 80, footer_y + 1, "FortressOS Notepad v1.0", 0xff8b949e, NOTEPAD_HEADER);
    }
}

fn draw_line_num(w: &mut Window, x: usize, y: usize, num: usize) {
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
        draw_char(w, x + i * 8, y, b, NOTEPAD_LINENUM, NOTEPAD_BG);
    }
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
