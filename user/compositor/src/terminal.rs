//! Embedded Graphical Terminal Window connected to /bin/sh over a PTY.

use crate::{
    font::GLYPHS,
    window::Window,
};

pub const TERM_COLS: usize = 60;
pub const TERM_ROWS: usize = 14;
pub const TERM_BG: u32 = 0xff0d1117;
pub const TERM_FG: u32 = 0xffe6edf3;
pub const TERM_CURSOR: u32 = 0xff58a6ff;

#[allow(dead_code)]
pub struct Terminal {
    pub window_id: u32,
    pub master_fd: i32,
    pub child_pid: i64,
    pub cursor_col: usize,
    pub cursor_row: usize,
    pub grid: [[u8; TERM_COLS]; TERM_ROWS],
    ansi_state: u8,
}

impl Terminal {
    pub fn new(window_id: u32, master_fd: i32, child_pid: i64) -> Self {
        Self {
            window_id,
            master_fd,
            child_pid,
            cursor_col: 0,
            cursor_row: 0,
            grid: [[b' '; TERM_COLS]; TERM_ROWS],
            ansi_state: 0,
        }
    }

    pub fn write_byte(&mut self, b: u8) {
        // ANSI escape sequence filter (strips CSI color/styling commands)
        match self.ansi_state {
            1 => {
                self.ansi_state = if b == b'[' { 2 } else { 0 };
                return;
            }
            2 => {
                if (0x40..=0x7e).contains(&b) {
                    self.ansi_state = 0;
                }
                return;
            }
            _ => {
                if b == 0x1b {
                    self.ansi_state = 1;
                    return;
                }
            }
        }

        match b {
            b'\r' => {
                self.cursor_col = 0;
            }
            b'\n' => {
                self.cursor_col = 0;
                self.scroll_down();
            }
            0x08 | 0x7f => {
                // Backspace / DEL
                if self.cursor_col > 0 {
                    self.cursor_col -= 1;
                    self.grid[self.cursor_row][self.cursor_col] = b' ';
                }
            }
            b'\t' => {
                let next_stop = ((self.cursor_col + 8) / 8 * 8).min(TERM_COLS - 1);
                while self.cursor_col < next_stop {
                    self.grid[self.cursor_row][self.cursor_col] = b' ';
                    self.cursor_col += 1;
                }
            }
            32..=126 => {
                if self.cursor_col >= TERM_COLS {
                    self.cursor_col = 0;
                    self.scroll_down();
                }
                self.grid[self.cursor_row][self.cursor_col] = b;
                self.cursor_col += 1;
                if self.cursor_col >= TERM_COLS {
                    self.cursor_col = 0;
                    self.scroll_down();
                }
            }
            _ => {}
        }
    }

    fn scroll_down(&mut self) {
        if self.cursor_row + 1 < TERM_ROWS {
            self.cursor_row += 1;
        } else {
            for r in 0..TERM_ROWS - 1 {
                self.grid[r] = self.grid[r + 1];
            }
            self.grid[TERM_ROWS - 1] = [b' '; TERM_COLS];
        }
    }

    pub fn write_bytes(&mut self, bytes: &[u8]) {
        for &b in bytes {
            self.write_byte(b);
        }
    }

    pub fn render(&self, w: &mut Window) {
        let width = w.rect.w.saturating_sub(2);
        let height = w.rect.h.saturating_sub(26);
        for row in 0..TERM_ROWS {
            let py = 5 + row * 16;
            if py + 16 > height {
                break;
            }
            for col in 0..TERM_COLS {
                let px = 9 + col * 8;
                if px + 8 > width {
                    break;
                }
                let ch = self.grid[row][col];
                let c_idx = if (' '..='~').contains(&(ch as char)) {
                    ch as usize - 32
                } else {
                    0
                };
                let glyph = &GLYPHS[c_idx];
                let is_cursor = row == self.cursor_row && col == self.cursor_col;
                for r in 0..16 {
                    for c in 0..8 {
                        let bit = (glyph[r] & (128 >> c)) != 0;
                        let color = if is_cursor && r >= 13 {
                            TERM_CURSOR
                        } else if bit {
                            TERM_FG
                        } else {
                            TERM_BG
                        };
                        w.content[(py + r) * width + px + c] = color;
                    }
                }
            }
        }
    }
}
