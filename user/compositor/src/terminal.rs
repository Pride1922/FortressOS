//! Terminal emulator: parses the shell's output (untrusted bytes) into a
//! character grid and draws it into a Canvas. Runs inside /bin/terminal
//! (src/bin/terminal.rs), which owns the PTY and the shell.

use crate::canvas::Canvas;
use alloc::vec::Vec;

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
    dirty_min_row: usize,
    dirty_max_row: usize,
    full_dirty: bool,
}

impl Terminal {
    pub fn new() -> Self {
        let mut grid = Vec::new();
        let _ = grid.try_reserve_exact(TERM_MAX_ROWS);
        grid.resize(TERM_MAX_ROWS, [b' '; TERM_MAX_COLS]);

        Self {
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
            dirty_min_row: 0,
            dirty_max_row: 14,
            full_dirty: true,
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
        self.full_dirty = true;
    }

    fn mark_dirty(&mut self, row: usize) {
        self.dirty_min_row = self.dirty_min_row.min(row);
        self.dirty_max_row = self.dirty_max_row.max(row);
    }

    pub fn write_byte(&mut self, b: u8) {
        let old_row = self.cursor_row;
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
                    self.mark_dirty(old_row);
                    self.mark_dirty(self.cursor_row);
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
        self.mark_dirty(old_row);
        self.mark_dirty(self.cursor_row);
    }

    fn execute_csi(&mut self, cmd: u8) {
        let p0 = self.ansi_params[0];
        let p1 = self.ansi_params[1];
        let max_cols = self.cols;
        let max_rows = self.rows;
        match cmd {
            b'J' => {
                // Erase in Display (clear screen)
                self.full_dirty = true;
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
                self.mark_dirty(self.cursor_row);
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
                self.mark_dirty(self.cursor_row);
            }
            b'A' => {
                // Cursor up
                let n = p0.max(1);
                self.cursor_row = self.cursor_row.saturating_sub(n);
                self.mark_dirty(self.cursor_row);
            }
            b'B' => {
                // Cursor down
                let n = p0.max(1);
                self.cursor_row = (self.cursor_row + n).min(max_rows.saturating_sub(1));
                self.mark_dirty(self.cursor_row);
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
                    self.mark_dirty(self.cursor_row);
                }
            }
            b'l' => {
                if self.ansi_private && p0 == 25 {
                    self.cursor_visible = false;
                    self.mark_dirty(self.cursor_row);
                }
            }
            _ => {}
        }
    }

    fn scroll_down(&mut self) {
        if self.cursor_row + 1 < self.rows {
            self.cursor_row += 1;
            self.mark_dirty(self.cursor_row);
        } else {
            for r in 0..self.rows.saturating_sub(1) {
                self.grid[r] = self.grid[r + 1];
            }
            self.grid[self.rows.saturating_sub(1)] = [b' '; TERM_MAX_COLS];
            self.full_dirty = true;
        }
    }

    pub fn write_bytes(&mut self, bytes: &[u8]) {
        for &b in bytes {
            self.write_byte(b);
        }
    }

    /// Grid geometry for a content area: 10 px left margin, 8 px top.
    pub fn grid_for(width: usize, height: usize) -> (usize, usize) {
        (
            (width.saturating_sub(20) / 8).clamp(20, TERM_MAX_COLS),
            (height.saturating_sub(12) / 16).clamp(5, TERM_MAX_ROWS),
        )
    }

    /// Redraws changed rows; returns the content rows touched as (y, h).
    pub fn render_dirty(&mut self, c: &mut Canvas) -> Option<(usize, usize)> {
        let (width, height) = (c.width, c.height);
        if self.full_dirty || self.dirty_min_row > self.dirty_max_row {
            self.full_dirty = false;
            self.dirty_min_row = 0;
            self.dirty_max_row = self.rows.saturating_sub(1);
            c.fill(TERM_BG);
        }

        let start_row = self.dirty_min_row.min(self.rows.saturating_sub(1));
        let end_row = self.dirty_max_row.min(self.rows.saturating_sub(1));
        self.dirty_min_row = usize::MAX;
        self.dirty_max_row = 0;

        for row in start_row..=end_row {
            let py = 8 + row * 16;
            if py + 16 > height {
                break;
            }
            c.fill_rect(0, py, width, 16, TERM_BG);
            for col in 0..self.cols {
                let px = 10 + col * 8;
                if px + 8 > width {
                    break;
                }
                let ch = self.grid[row][col];
                let is_cursor =
                    self.cursor_visible && row == self.cursor_row && col == self.cursor_col;
                if ch != b' ' {
                    c.glyph(px, py, ch, TERM_FG, TERM_BG);
                }
                if is_cursor {
                    c.fill_rect(px, py + 13, 8, 3, TERM_CURSOR);
                }
            }
        }
        let y = 8 + start_row * 16;
        (y < height).then(|| (y, ((end_row - start_row + 1) * 16).min(height - y)))
    }

    pub fn render(&mut self, c: &mut Canvas) {
        self.full_dirty = true;
        let _ = self.render_dirty(c);
    }
}
