//! Embedded Graphical Terminal Window connected to /bin/sh over a PTY.

use alloc::vec::Vec;
use crate::{
    font::GLYPHS,
    window::Window,
};

#[allow(dead_code)]
pub const TERM_COLS: usize = 60;
#[allow(dead_code)]
pub const TERM_ROWS: usize = 14;
pub const TERM_MAX_COLS: usize = 160;
pub const TERM_MAX_ROWS: usize = 60;
pub const TERM_BG: u32 = 0xff0d1117;
pub const TERM_FG: u32 = 0xffe6edf3;
pub const TERM_CURSOR: u32 = 0xff58a6ff;

#[allow(dead_code)]
pub struct Terminal {
    pub window_id: u32,
    pub master_fd: i32,
    pub child_pid: i64,
    pub cols: usize,
    pub rows: usize,
    pub cursor_col: usize,
    pub cursor_row: usize,
    pub cursor_visible: bool,
    pub grid: Vec<[u8; TERM_MAX_COLS]>,
    ansi_state: u8,
    ansi_params: [usize; 4],
    ansi_param_count: usize,
    ansi_private: bool,
}

impl Terminal {
    pub fn new(window_id: u32, master_fd: i32, child_pid: i64) -> Self {
        let mut grid = Vec::new();
        let _ = grid.try_reserve_exact(TERM_MAX_ROWS);
        grid.resize(TERM_MAX_ROWS, [b' '; TERM_MAX_COLS]);

        Self {
            window_id,
            master_fd,
            child_pid,
            cols: 60,
            rows: 14,
            cursor_col: 0,
            cursor_row: 0,
            cursor_visible: true,
            grid,
            ansi_state: 0,
            ansi_params: [0; 4],
            ansi_param_count: 0,
            ansi_private: false,
        }
    }

    pub fn set_dimensions(&mut self, new_cols: usize, new_rows: usize) {
        self.cols = new_cols.clamp(20, TERM_MAX_COLS);
        self.rows = new_rows.clamp(5, TERM_MAX_ROWS);
        if self.cursor_col >= self.cols {
            self.cursor_col = self.cols.saturating_sub(1);
        }
        if self.cursor_row >= self.rows {
            self.cursor_row = self.rows.saturating_sub(1);
        }
    }

    pub fn write_byte(&mut self, b: u8) {
        // ANSI escape sequence state machine
        match self.ansi_state {
            1 => {
                if b == b'[' {
                    self.ansi_state = 2;
                    self.ansi_params = [0; 4];
                    self.ansi_param_count = 0;
                    self.ansi_private = false;
                } else {
                    self.ansi_state = 0;
                }
                return;
            }
            2 => {
                if b == b'?' {
                    self.ansi_private = true;
                    return;
                }
                if (b'0'..=b'9').contains(&b) {
                    if self.ansi_param_count == 0 {
                        self.ansi_param_count = 1;
                    }
                    let idx = self.ansi_param_count - 1;
                    if idx < 4 {
                        self.ansi_params[idx] = self.ansi_params[idx]
                            .saturating_mul(10)
                            .saturating_add((b - b'0') as usize);
                    }
                    return;
                }
                if b == b';' {
                    if self.ansi_param_count < 4 {
                        self.ansi_param_count += 1;
                    }
                    return;
                }
                if (0x40..=0x7e).contains(&b) {
                    self.execute_csi(b);
                    self.ansi_state = 0;
                    return;
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
            0x08 => {
                // Backspace: move cursor left 1 column
                self.cursor_col = self.cursor_col.saturating_sub(1);
            }
            0x7f => {
                // DEL: move cursor left and blank out character
                if self.cursor_col > 0 {
                    self.cursor_col -= 1;
                    self.grid[self.cursor_row][self.cursor_col] = b' ';
                }
            }
            b'\t' => {
                let next_stop = ((self.cursor_col + 8) / 8 * 8).min(self.cols.saturating_sub(1));
                while self.cursor_col < next_stop {
                    self.grid[self.cursor_row][self.cursor_col] = b' ';
                    self.cursor_col += 1;
                }
            }
            32..=126 => {
                if self.cursor_col >= self.cols {
                    self.cursor_col = 0;
                    self.scroll_down();
                }
                self.grid[self.cursor_row][self.cursor_col] = b;
                self.cursor_col += 1;
            }
            _ => {}
        }
    }

    fn execute_csi(&mut self, cmd: u8) {
        let p0 = self.ansi_params[0];
        let p1 = self.ansi_params[1];
        let max_cols = self.cols;
        let max_rows = self.rows;
        match cmd {
            b'J' => {
                // Erase in Display (clear screen)
                if p0 == 2 || p0 == 3 {
                    for r in 0..max_rows {
                        self.grid[r] = [b' '; TERM_MAX_COLS];
                    }
                    self.cursor_row = 0;
                    self.cursor_col = 0;
                } else if p0 == 0 {
                    // Erase from cursor to end of screen
                    for c in self.cursor_col..max_cols {
                        self.grid[self.cursor_row][c] = b' ';
                    }
                    for r in (self.cursor_row + 1)..max_rows {
                        self.grid[r] = [b' '; TERM_MAX_COLS];
                    }
                } else if p0 == 1 {
                    // Erase from beginning of screen to cursor
                    for r in 0..self.cursor_row {
                        self.grid[r] = [b' '; TERM_MAX_COLS];
                    }
                    for c in 0..=self.cursor_col.min(max_cols.saturating_sub(1)) {
                        self.grid[self.cursor_row][c] = b' ';
                    }
                }
            }
            b'K' => {
                // Erase in Line
                if p0 == 0 {
                    // Erase from cursor to end of line
                    for c in self.cursor_col..max_cols {
                        self.grid[self.cursor_row][c] = b' ';
                    }
                } else if p0 == 1 {
                    // Erase from beginning of line to cursor
                    for c in 0..=self.cursor_col.min(max_cols.saturating_sub(1)) {
                        self.grid[self.cursor_row][c] = b' ';
                    }
                } else if p0 == 2 {
                    // Erase entire line
                    for c in 0..max_cols {
                        self.grid[self.cursor_row][c] = b' ';
                    }
                }
            }
            b'H' | b'f' => {
                // Cursor position: row, col (1-based)
                if self.ansi_param_count == 0 {
                    self.cursor_row = 0;
                    self.cursor_col = 0;
                } else {
                    let r = p0.saturating_sub(1).min(max_rows.saturating_sub(1));
                    let c = if self.ansi_param_count > 1 {
                        p1.saturating_sub(1).min(max_cols.saturating_sub(1))
                    } else {
                        0
                    };
                    self.cursor_row = r;
                    self.cursor_col = c;
                }
            }
            b'A' => {
                // Cursor up
                let n = p0.max(1);
                self.cursor_row = self.cursor_row.saturating_sub(n);
            }
            b'B' => {
                // Cursor down
                let n = p0.max(1);
                self.cursor_row = (self.cursor_row + n).min(max_rows.saturating_sub(1));
            }
            b'C' => {
                // Cursor forward
                let n = p0.max(1);
                self.cursor_col = (self.cursor_col + n).min(max_cols.saturating_sub(1));
            }
            b'D' => {
                // Cursor back
                let n = p0.max(1);
                self.cursor_col = self.cursor_col.saturating_sub(n);
            }
            b'G' => {
                // Cursor horizontal absolute
                self.cursor_col = p0.saturating_sub(1).min(max_cols.saturating_sub(1));
            }
            b'h' => {
                if self.ansi_private && p0 == 25 {
                    self.cursor_visible = true;
                }
            }
            b'l' => {
                if self.ansi_private && p0 == 25 {
                    self.cursor_visible = false;
                }
            }
            _ => {}
        }
    }

    fn scroll_down(&mut self) {
        if self.cursor_row + 1 < self.rows {
            self.cursor_row += 1;
        } else {
            for r in 0..self.rows.saturating_sub(1) {
                self.grid[r] = self.grid[r + 1];
            }
            self.grid[self.rows.saturating_sub(1)] = [b' '; TERM_MAX_COLS];
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
        // Fill entire window content area with terminal background
        w.content.fill(TERM_BG);
        for row in 0..self.rows {
            let py = 5 + row * 16;
            if py + 16 > height {
                break;
            }
            for col in 0..self.cols {
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
                let is_cursor = self.cursor_visible && row == self.cursor_row && col == self.cursor_col;
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
