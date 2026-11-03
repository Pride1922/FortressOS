//! term_engine.rs - Fortress Terminal Advanced VT100/Xterm Emulation Engine
//!
//! Features:
//! 1. Full ANSI SGR 16 and 256-color support with text attributes (bold, underline, reverse, dim).
//! 2. 2,000-line circular scrollback history buffer.
//! 3. Global find highlighting (Ctrl+Shift+F) across the entire scrollback buffer with Next/Prev jumping.
//! 4. Smart path-aware double-click token selection (preserving '/', '.', '-', '_', '~', ':').
//! 5. Zero-loss formatted clipboard text generation.

extern crate alloc;
use alloc::string::String;
use alloc::vec::Vec;
use crate::canvas::Canvas;

pub const TERM_MAX_COLS: usize = 160;
pub const TERM_MAX_ROWS: usize = 60;
pub const SCROLLBACK_MAX: usize = 2000;

pub const COLOR_DEFAULT_BG: u32 = 0xff0d1117;
pub const COLOR_DEFAULT_FG: u32 = 0xffe6edf3;
pub const COLOR_DEFAULT_CURSOR: u32 = 0xff58a6ff;
pub const COLOR_SELECTION_BG: u32 = 0xff264f78;
pub const COLOR_SELECTION_FG: u32 = 0xffffffff;
pub const COLOR_MATCH_BG: u32 = 0xffd7a000;
pub const COLOR_MATCH_FG: u32 = 0xff000000;
pub const COLOR_ACTIVE_MATCH_BG: u32 = 0xffff6600;
pub const COLOR_ACTIVE_MATCH_FG: u32 = 0xffffffff;

pub const ATTR_BOLD: u8 = 1 << 0;
pub const ATTR_DIM: u8 = 1 << 1;
pub const ATTR_UNDERLINE: u8 = 1 << 2;
pub const ATTR_REVERSE: u8 = 1 << 3;

pub const COLOR_CODE_DEFAULT_FG: u16 = 0x100;
pub const COLOR_CODE_DEFAULT_BG: u16 = 0x101;

// 16 Standard ANSI colors (Tango / Modern Dark palette)
pub const ANSI_PALETTE: [u32; 16] = [
    0xff1e1e1e, // 0: Black
    0xfff44747, // 1: Red
    0xff608b4e, // 2: Green
    0xffdcdcaa, // 3: Yellow
    0xff569cd6, // 4: Blue
    0xffc586c0, // 5: Magenta
    0xff4ec9b0, // 6: Cyan
    0xffd4d4d4, // 7: White (Light Gray)
    0xff808080, // 8: Bright Black (Gray)
    0xffd16969, // 9: Bright Red
    0xffb5cea8, // 10: Bright Green
    0xffce9178, // 11: Bright Yellow
    0xff9cdcfe, // 12: Bright Blue
    0xffd7ba7d, // 13: Bright Magenta
    0xff4fc1ff, // 14: Bright Cyan
    0xffffffff, // 15: Bright White
];

pub fn ansi_256_to_rgb(idx: u16) -> u32 {
    if idx < 16 {
        return ANSI_PALETTE[idx as usize];
    }
    if idx <= 231 {
        // 6x6x6 color cube
        let i = idx as u32 - 16;
        let r = i / 36;
        let g = (i / 6) % 6;
        let b = i % 6;
        let conv = |c: u32| if c == 0 { 0 } else { 55 + c * 40 };
        return 0xff000000 | (conv(r) << 16) | (conv(g) << 8) | conv(b);
    }
    if idx <= 255 {
        // Grayscale ramp
        let gray = 8 + (idx as u32 - 232) * 10;
        return 0xff000000 | (gray << 16) | (gray << 8) | gray;
    }
    COLOR_DEFAULT_FG
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Cell {
    pub ch: u8,
    pub fg: u16,
    pub bg: u16,
    pub attrs: u8,
}

impl Default for Cell {
    fn default() -> Self {
        Self {
            ch: b' ',
            fg: COLOR_CODE_DEFAULT_FG,
            bg: COLOR_CODE_DEFAULT_BG,
            attrs: 0,
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub struct SearchMatch {
    pub total_row: usize, // Row index across scrollback + live grid
    pub col: usize,
    pub len: usize,
}

pub struct TermEngine {
    pub cols: usize,
    pub rows: usize,
    pub cursor_col: usize,
    pub cursor_row: usize,
    pub cursor_visible: bool,

    // Active screen grid
    pub grid: Vec<[Cell; TERM_MAX_COLS]>,

    // Circular scrollback buffer
    pub scrollback: Vec<[Cell; TERM_MAX_COLS]>,
    pub scroll_offset: usize, // 0 = viewing live grid; >0 = viewing history

    // Current ANSI text attributes
    pub cur_fg: u16,
    pub cur_bg: u16,
    pub cur_attrs: u8,

    // Selection
    pub sel_anchor: Option<(usize, usize)>, // (total_row, col)
    pub sel_end: Option<(usize, usize)>,

    // ANSI parser state
    ansi_state: u8,
    ansi_params: [usize; 8],
    ansi_param_count: usize,
    ansi_private: bool,

    // Global Buffer Find
    pub search_query: String,
    pub search_matches: Vec<SearchMatch>,
    pub active_match_idx: usize,

    // Dirty row tracking
    pub dirty_min_row: usize,
    pub dirty_max_row: usize,
    pub full_dirty: bool,
}

impl TermEngine {
    pub fn new() -> Self {
        let mut grid = Vec::new();
        let _ = grid.try_reserve_exact(TERM_MAX_ROWS);
        grid.resize(TERM_MAX_ROWS, [Cell::default(); TERM_MAX_COLS]);

        Self {
            cols: 80,
            rows: 24,
            cursor_col: 0,
            cursor_row: 0,
            cursor_visible: true,
            grid,
            scrollback: Vec::new(),
            scroll_offset: 0,
            cur_fg: COLOR_CODE_DEFAULT_FG,
            cur_bg: COLOR_CODE_DEFAULT_BG,
            cur_attrs: 0,
            sel_anchor: None,
            sel_end: None,
            ansi_state: 0,
            ansi_params: [0; 8],
            ansi_param_count: 0,
            ansi_private: false,
            search_query: String::new(),
            search_matches: Vec::new(),
            active_match_idx: 0,
            dirty_min_row: 0,
            dirty_max_row: 24,
            full_dirty: true,
        }
    }

    pub fn total_lines(&self) -> usize {
        self.scrollback.len() + self.rows
    }

    pub fn viewport_to_total_row(&self, vrow: usize) -> usize {
        let total = self.total_lines();
        let max_start = total.saturating_sub(self.rows);
        let start = max_start.saturating_sub(self.scroll_offset);
        start + vrow
    }

    pub fn get_line(&self, total_row: usize) -> Option<&[Cell; TERM_MAX_COLS]> {
        if total_row < self.scrollback.len() {
            self.scrollback.get(total_row)
        } else {
            let grid_row = total_row - self.scrollback.len();
            self.grid.get(grid_row)
        }
    }

    pub fn cell_at_pixel(&self, px: usize, py: usize, top_offset: usize) -> (usize, usize) {
        let col = (px.saturating_sub(10) / 8).min(self.cols.saturating_sub(1));
        let vrow = (py.saturating_sub(top_offset) / 16).min(self.rows.saturating_sub(1));
        let total_row = self.viewport_to_total_row(vrow);
        (total_row, col)
    }

    // Smart Token Boundary (Double Click)
    pub fn is_token_char(ch: u8) -> bool {
        // Delimiters: " \t\r\n\"'()[]{}<>|;,"
        // Preserved in paths/tokens: '/', '.', '-', '_', '~', ':', '@', '%'
        match ch {
            b' ' | b'\t' | b'\r' | b'\n' | b'"' | b'\'' | b'(' | b')' |
            b'[' | b']' | b'{' | b'}' | b'<' | b'>' | b'|' | b';' | b',' => false,
            33..=126 => true,
            _ => false,
        }
    }

    pub fn select_word_at(&mut self, total_row: usize, col: usize) {
        let Some(line) = self.get_line(total_row) else { return; };
        let c = col.min(self.cols.saturating_sub(1));
        if !Self::is_token_char(line[c].ch) {
            self.sel_anchor = Some((total_row, c));
            self.sel_end = Some((total_row, c));
            self.full_dirty = true;
            return;
        }

        let mut start_c = c;
        while start_c > 0 && Self::is_token_char(line[start_c - 1].ch) {
            start_c -= 1;
        }

        let mut end_c = c;
        while end_c + 1 < self.cols && Self::is_token_char(line[end_c + 1].ch) {
            end_c += 1;
        }

        self.sel_anchor = Some((total_row, start_c));
        self.sel_end = Some((total_row, end_c));
        self.full_dirty = true;
    }

    pub fn select_line_at(&mut self, total_row: usize) {
        let Some(line) = self.get_line(total_row) else { return; };
        let mut last_c = 0;
        for c in 0..self.cols {
            if line[c].ch != b' ' {
                last_c = c;
            }
        }
        self.sel_anchor = Some((total_row, 0));
        self.sel_end = Some((total_row, last_c.max(1)));
        self.full_dirty = true;
    }

    pub fn handle_click(&mut self, px: usize, py: usize, top_offset: usize) -> bool {
        let had_sel = self.sel_end.is_some();
        self.sel_anchor = Some(self.cell_at_pixel(px, py, top_offset));
        self.sel_end = None;
        if had_sel {
            self.full_dirty = true;
        }
        had_sel
    }

    pub fn handle_drag(&mut self, px: usize, py: usize, top_offset: usize) -> bool {
        let pos = self.cell_at_pixel(px, py, top_offset);
        if self.sel_anchor.is_none() {
            self.sel_anchor = Some(pos);
        }
        if self.sel_end != Some(pos) {
            self.sel_end = Some(pos);
            self.full_dirty = true;
            return true;
        }
        false
    }

    pub fn clear_selection(&mut self) {
        if self.sel_anchor.is_some() || self.sel_end.is_some() {
            self.sel_anchor = None;
            self.sel_end = None;
            self.full_dirty = true;
        }
    }

    fn ordered_selection(&self) -> Option<((usize, usize), (usize, usize))> {
        let a = self.sel_anchor?;
        let b = self.sel_end?;
        if a == b {
            return None;
        }
        if a.0 < b.0 || (a.0 == b.0 && a.1 <= b.1) {
            Some((a, b))
        } else {
            Some((b, a))
        }
    }

    pub fn is_cell_selected(&self, total_row: usize, col: usize) -> bool {
        let Some(((r0, c0), (r1, c1))) = self.ordered_selection() else {
            return false;
        };
        if total_row < r0 || total_row > r1 {
            return false;
        }
        if r0 == r1 {
            col >= c0 && col <= c1
        } else if total_row == r0 {
            col >= c0
        } else if total_row == r1 {
            col <= c1
        } else {
            true
        }
    }

    // Plain text extraction
    pub fn selected_text_plain(&self) -> Option<String> {
        let ((r0, c0), (r1, c1)) = self.ordered_selection()?;
        let mut out = String::new();
        for r in r0..=r1 {
            let Some(line) = self.get_line(r) else { continue; };
            let start_c = if r == r0 { c0 } else { 0 };
            let end_c = if r == r1 { c1.min(self.cols.saturating_sub(1)) } else { self.cols.saturating_sub(1) };
            if start_c <= end_c {
                let slice = &line[start_c..=end_c];
                let trimmed = slice.iter().rposition(|c| c.ch != b' ').map(|i| i + 1).unwrap_or(0);
                for c in &slice[..trimmed] {
                    if (32..=126).contains(&c.ch) {
                        out.push(c.ch as char);
                    }
                }
            }
            if r < r1 {
                out.push('\n');
            }
        }
        (!out.is_empty()).then_some(out)
    }

    // Formatted text extraction (with ANSI styling preserved)
    pub fn selected_text_formatted(&self) -> Option<String> {
        let ((r0, c0), (r1, c1)) = self.ordered_selection()?;
        let mut out = String::new();
        let mut last_fg = COLOR_CODE_DEFAULT_FG;
        let mut last_bg = COLOR_CODE_DEFAULT_BG;
        let mut last_attrs = 0u8;

        for r in r0..=r1 {
            let Some(line) = self.get_line(r) else { continue; };
            let start_c = if r == r0 { c0 } else { 0 };
            let end_c = if r == r1 { c1.min(self.cols.saturating_sub(1)) } else { self.cols.saturating_sub(1) };
            if start_c <= end_c {
                let slice = &line[start_c..=end_c];
                let trimmed = slice.iter().rposition(|c| c.ch != b' ').map(|i| i + 1).unwrap_or(0);
                for c in &slice[..trimmed] {
                    if c.fg != last_fg || c.bg != last_bg || c.attrs != last_attrs {
                        out.push_str("\x1b[0m");
                        if c.attrs & ATTR_BOLD != 0 { out.push_str("\x1b[1m"); }
                        if c.attrs & ATTR_UNDERLINE != 0 { out.push_str("\x1b[4m"); }
                        if c.fg < 16 {
                            let code = if c.fg < 8 { 30 + c.fg } else { 90 + (c.fg - 8) };
                            append_ansi_param(&mut out, code as u32);
                        } else if c.fg < 256 {
                            out.push_str("\x1b[38;5;");
                            append_num(&mut out, c.fg as usize);
                            out.push('m');
                        }
                        last_fg = c.fg;
                        last_bg = c.bg;
                        last_attrs = c.attrs;
                    }
                    if (32..=126).contains(&c.ch) {
                        out.push(c.ch as char);
                    }
                }
            }
            if r < r1 {
                out.push('\n');
            }
        }
        if last_attrs != 0 || last_fg != COLOR_CODE_DEFAULT_FG || last_bg != COLOR_CODE_DEFAULT_BG {
            out.push_str("\x1b[0m");
        }
        (!out.is_empty()).then_some(out)
    }

    // Scrolling
    pub fn scroll_up(&mut self, lines: usize) {
        let max_scroll = self.scrollback.len();
        self.scroll_offset = (self.scroll_offset + lines).min(max_scroll);
        self.full_dirty = true;
    }

    pub fn scroll_down(&mut self, lines: usize) {
        self.scroll_offset = self.scroll_offset.saturating_sub(lines);
        self.full_dirty = true;
    }

    pub fn scroll_to_bottom(&mut self) {
        if self.scroll_offset != 0 {
            self.scroll_offset = 0;
            self.full_dirty = true;
        }
    }

    // Global Buffer Find (Ctrl+Shift+F)
    pub fn perform_search(&mut self, query: &str) {
        self.search_query.clear();
        self.search_matches.clear();
        self.active_match_idx = 0;

        if query.is_empty() {
            self.full_dirty = true;
            return;
        }

        self.search_query.push_str(query);
        let q_bytes = query.as_bytes();
        let q_len = q_bytes.len();
        let total = self.total_lines();

        for r in 0..total {
            let Some(line_ref) = self.get_line(r) else { continue; };
            let line = *line_ref;
            if self.cols < q_len { continue; }
            for c in 0..=(self.cols - q_len) {
                let mut matched = true;
                for i in 0..q_len {
                    let mut b1 = line[c + i].ch;
                    let mut b2 = q_bytes[i];
                    if b1 >= b'A' && b1 <= b'Z' { b1 += 32; }
                    if b2 >= b'A' && b2 <= b'Z' { b2 += 32; }
                    if b1 != b2 {
                        matched = false;
                        break;
                    }
                }
                if matched {
                    self.search_matches.push(SearchMatch {
                        total_row: r,
                        col: c,
                        len: q_len,
                    });
                }
            }
        }

        if !self.search_matches.is_empty() {
            self.jump_to_match(0);
        }
        self.full_dirty = true;
    }

    pub fn next_match(&mut self) {
        if self.search_matches.is_empty() { return; }
        self.active_match_idx = (self.active_match_idx + 1) % self.search_matches.len();
        self.jump_to_match(self.active_match_idx);
        self.full_dirty = true;
    }

    pub fn prev_match(&mut self) {
        if self.search_matches.is_empty() { return; }
        if self.active_match_idx == 0 {
            self.active_match_idx = self.search_matches.len() - 1;
        } else {
            self.active_match_idx -= 1;
        }
        self.jump_to_match(self.active_match_idx);
        self.full_dirty = true;
    }

    fn jump_to_match(&mut self, idx: usize) {
        let Some(m) = self.search_matches.get(idx) else { return; };
        let total = self.total_lines();
        let max_start = total.saturating_sub(self.rows);

        // Center the match in the viewport
        let target_start = m.total_row.saturating_sub(self.rows / 2).min(max_start);
        self.scroll_offset = max_start.saturating_sub(target_start);
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

    // Stream byte ingestion
    pub fn write_byte(&mut self, b: u8) {
        let old_row = self.cursor_row;

        // ANSI escape sequence state machine
        match self.ansi_state {
            1 => {
                if b == b'[' {
                    self.ansi_state = 2;
                    self.ansi_params = [0; 8];
                    self.ansi_param_count = 0;
                    self.ansi_private = false;
                } else if b == 0x1b {
                    self.ansi_state = 1;
                } else {
                    self.ansi_state = 0;
                }
                return;
            }
            2 => {
                if b == 0x1b {
                    self.ansi_state = 1;
                    return;
                }
                if b == b'?' {
                    self.ansi_private = true;
                    return;
                }
                if (b'0'..=b'9').contains(&b) {
                    if self.ansi_param_count == 0 {
                        self.ansi_param_count = 1;
                    }
                    let idx = self.ansi_param_count - 1;
                    if idx < 8 {
                        self.ansi_params[idx] = self.ansi_params[idx]
                            .saturating_mul(10)
                            .saturating_add((b - b'0') as usize);
                    }
                    return;
                }
                if b == b';' {
                    if self.ansi_param_count == 0 {
                        self.ansi_param_count = 2;
                    } else if self.ansi_param_count < 8 {
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
                self.feed_scroll_down();
            }
            0x08 => {
                self.cursor_col = self.cursor_col.saturating_sub(1);
            }
            0x7f => {
                if self.cursor_col > 0 {
                    self.cursor_col -= 1;
                    self.grid[self.cursor_row][self.cursor_col] = Cell::default();
                }
            }
            b'\t' => {
                let next_stop = ((self.cursor_col + 8) / 8 * 8).min(self.cols.saturating_sub(1));
                while self.cursor_col < next_stop {
                    self.grid[self.cursor_row][self.cursor_col] = Cell {
                        ch: b' ',
                        fg: self.cur_fg,
                        bg: self.cur_bg,
                        attrs: self.cur_attrs,
                    };
                    self.cursor_col += 1;
                }
            }
            32..=126 => {
                if self.cursor_col >= self.cols {
                    self.cursor_col = 0;
                    self.feed_scroll_down();
                }
                self.grid[self.cursor_row][self.cursor_col] = Cell {
                    ch: b,
                    fg: self.cur_fg,
                    bg: self.cur_bg,
                    attrs: self.cur_attrs,
                };
                self.cursor_col += 1;
            }
            _ => {}
        }
        self.mark_dirty(old_row);
        self.mark_dirty(self.cursor_row);
    }

    fn feed_scroll_down(&mut self) {
        if self.cursor_row + 1 < self.rows {
            self.cursor_row += 1;
            self.mark_dirty(self.cursor_row);
        } else {
            // Push top line into scrollback ring buffer
            if self.scrollback.len() >= SCROLLBACK_MAX {
                self.scrollback.remove(0);
            }
            self.scrollback.push(self.grid[0]);

            for r in 0..self.rows.saturating_sub(1) {
                self.grid[r] = self.grid[r + 1];
            }
            self.grid[self.rows.saturating_sub(1)] = [Cell::default(); TERM_MAX_COLS];
            self.full_dirty = true;
        }
    }

    fn execute_csi(&mut self, cmd: u8) {
        let p0 = self.ansi_params[0];
        let p1 = self.ansi_params[1];
        let max_cols = self.cols;
        let max_rows = self.rows;

        match cmd {
            b'm' => {
                // SGR (Select Graphic Rendition) - Colors & Attributes
                if self.ansi_param_count == 0 {
                    self.cur_fg = COLOR_CODE_DEFAULT_FG;
                    self.cur_bg = COLOR_CODE_DEFAULT_BG;
                    self.cur_attrs = 0;
                    return;
                }
                let mut i = 0;
                while i < self.ansi_param_count {
                    let p = self.ansi_params[i];
                    match p {
                        0 => {
                            self.cur_fg = COLOR_CODE_DEFAULT_FG;
                            self.cur_bg = COLOR_CODE_DEFAULT_BG;
                            self.cur_attrs = 0;
                        }
                        1 => self.cur_attrs |= ATTR_BOLD,
                        2 => self.cur_attrs |= ATTR_DIM,
                        4 => self.cur_attrs |= ATTR_UNDERLINE,
                        7 => self.cur_attrs |= ATTR_REVERSE,
                        22 => self.cur_attrs &= !(ATTR_BOLD | ATTR_DIM),
                        24 => self.cur_attrs &= !ATTR_UNDERLINE,
                        27 => self.cur_attrs &= !ATTR_REVERSE,
                        30..=37 => self.cur_fg = (p - 30) as u16,
                        38 => {
                            // Extended FG: 38;5;n (256-color)
                            if i + 2 < self.ansi_param_count && self.ansi_params[i + 1] == 5 {
                                self.cur_fg = (self.ansi_params[i + 2] as u16).min(255);
                                i += 2;
                            }
                        }
                        39 => self.cur_fg = COLOR_CODE_DEFAULT_FG,
                        40..=47 => self.cur_bg = (p - 40) as u16,
                        48 => {
                            // Extended BG: 48;5;n (256-color)
                            if i + 2 < self.ansi_param_count && self.ansi_params[i + 1] == 5 {
                                self.cur_bg = (self.ansi_params[i + 2] as u16).min(255);
                                i += 2;
                            }
                        }
                        49 => self.cur_bg = COLOR_CODE_DEFAULT_BG,
                        90..=97 => self.cur_fg = (8 + (p - 90)) as u16,
                        100..=107 => self.cur_bg = (8 + (p - 100)) as u16,
                        _ => {}
                    }
                    i += 1;
                }
            }
            b'J' => {
                self.full_dirty = true;
                if p0 == 2 || p0 == 3 {
                    for r in 0..max_rows {
                        self.grid[r] = [Cell::default(); TERM_MAX_COLS];
                    }
                    self.cursor_row = 0;
                    self.cursor_col = 0;
                } else if p0 == 0 {
                    for c in self.cursor_col..max_cols {
                        self.grid[self.cursor_row][c] = Cell::default();
                    }
                    for r in (self.cursor_row + 1)..max_rows {
                        self.grid[r] = [Cell::default(); TERM_MAX_COLS];
                    }
                } else if p0 == 1 {
                    for r in 0..self.cursor_row {
                        self.grid[r] = [Cell::default(); TERM_MAX_COLS];
                    }
                    for c in 0..=self.cursor_col.min(max_cols.saturating_sub(1)) {
                        self.grid[self.cursor_row][c] = Cell::default();
                    }
                }
            }
            b'K' => {
                self.mark_dirty(self.cursor_row);
                if p0 == 0 {
                    for c in self.cursor_col..max_cols {
                        self.grid[self.cursor_row][c] = Cell::default();
                    }
                } else if p0 == 1 {
                    for c in 0..=self.cursor_col.min(max_cols.saturating_sub(1)) {
                        self.grid[self.cursor_row][c] = Cell::default();
                    }
                } else if p0 == 2 {
                    for c in 0..max_cols {
                        self.grid[self.cursor_row][c] = Cell::default();
                    }
                }
            }
            b'H' | b'f' => {
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
                let n = p0.max(1);
                self.cursor_row = self.cursor_row.saturating_sub(n);
                self.mark_dirty(self.cursor_row);
            }
            b'B' => {
                let n = p0.max(1);
                self.cursor_row = (self.cursor_row + n).min(max_rows.saturating_sub(1));
                self.mark_dirty(self.cursor_row);
            }
            b'C' => {
                let n = p0.max(1);
                self.cursor_col = (self.cursor_col + n).min(max_cols.saturating_sub(1));
            }
            b'D' => {
                let n = p0.max(1);
                self.cursor_col = self.cursor_col.saturating_sub(n);
            }
            b'G' => {
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

    pub fn write_bytes(&mut self, bytes: &[u8]) {
        for &b in bytes {
            self.write_byte(b);
        }
    }

    // Canvas Rendering
    pub fn render_dirty(&mut self, c: &mut Canvas, top_offset: usize) -> Option<(usize, usize)> {
        let (width, height) = (c.width, c.height);
        if self.full_dirty {
            self.full_dirty = false;
            self.dirty_min_row = 0;
            self.dirty_max_row = self.rows.saturating_sub(1);
            c.fill_rect(0, top_offset, width, height.saturating_sub(top_offset), COLOR_DEFAULT_BG);
        } else if self.dirty_min_row > self.dirty_max_row {
            return None;
        }

        let start_vrow = self.dirty_min_row.min(self.rows.saturating_sub(1));
        let end_vrow = self.dirty_max_row.min(self.rows.saturating_sub(1));
        self.dirty_min_row = usize::MAX;
        self.dirty_max_row = 0;

        for vrow in start_vrow..=end_vrow {
            let py = top_offset + vrow * 16;
            if py + 16 > height {
                break;
            }
            c.fill_rect(0, py, width, 16, COLOR_DEFAULT_BG);

            let total_row = self.viewport_to_total_row(vrow);
            let Some(line) = self.get_line(total_row) else { continue; };

            for col in 0..self.cols {
                let px = 10 + col * 8;
                if px + 8 > width {
                    break;
                }

                let cell = line[col];
                let is_selected = self.is_cell_selected(total_row, col);

                // Find search match on this cell
                let mut match_kind = 0; // 0=none, 1=normal match, 2=active match
                if !self.search_matches.is_empty() {
                    for (i, m) in self.search_matches.iter().enumerate() {
                        if m.total_row == total_row && col >= m.col && col < m.col + m.len {
                            match_kind = if i == self.active_match_idx { 2 } else { 1 };
                            break;
                        }
                    }
                }

                let (mut cell_fg, cell_bg) = if is_selected {
                    (COLOR_SELECTION_FG, COLOR_SELECTION_BG)
                } else if match_kind == 2 {
                    (COLOR_ACTIVE_MATCH_FG, COLOR_ACTIVE_MATCH_BG)
                } else if match_kind == 1 {
                    (COLOR_MATCH_FG, COLOR_MATCH_BG)
                } else {
                    let fg = if cell.fg == COLOR_CODE_DEFAULT_FG {
                        COLOR_DEFAULT_FG
                    } else {
                        ansi_256_to_rgb(cell.fg)
                    };
                    let bg = if cell.bg == COLOR_CODE_DEFAULT_BG {
                        COLOR_DEFAULT_BG
                    } else {
                        ansi_256_to_rgb(cell.bg)
                    };
                    if cell.attrs & ATTR_REVERSE != 0 {
                        (bg, fg)
                    } else {
                        (fg, bg)
                    }
                };

                if cell.attrs & ATTR_BOLD != 0 && !is_selected && match_kind == 0 {
                    // Brighten bold colors if using default
                    if cell.fg == COLOR_CODE_DEFAULT_FG {
                        cell_fg = 0xffffffff;
                    }
                }

                if cell_bg != COLOR_DEFAULT_BG {
                    c.fill_rect(px, py, 8, 16, cell_bg);
                }

                if cell.ch != b' ' {
                    c.glyph(px, py, cell.ch, cell_fg, cell_bg);
                }

                if cell.attrs & ATTR_UNDERLINE != 0 {
                    c.fill_rect(px, py + 14, 8, 1, cell_fg);
                }

                // Live cursor (only visible when viewing live grid at scroll_offset == 0)
                let is_cursor = self.scroll_offset == 0 && self.cursor_visible &&
                                vrow == self.cursor_row && col == self.cursor_col;
                if is_cursor {
                    c.fill_rect(px, py + 13, 8, 3, COLOR_DEFAULT_CURSOR);
                }
            }
        }

        let y = top_offset + start_vrow * 16;
        (y < height).then(|| (y, ((end_vrow - start_vrow + 1) * 16).min(height - y)))
    }

    pub fn render(&mut self, c: &mut Canvas, top_offset: usize) {
        self.full_dirty = true;
        let _ = self.render_dirty(c, top_offset);
    }
}

fn append_ansi_param(out: &mut String, code: u32) {
    out.push_str("\x1b[");
    append_num(out, code as usize);
    out.push('m');
}

fn append_num(out: &mut String, mut val: usize) {
    if val == 0 {
        out.push('0');
        return;
    }
    let mut buf = [0u8; 10];
    let mut i = 0;
    while val > 0 {
        buf[i] = b'0' + (val % 10) as u8;
        val /= 10;
        i += 1;
    }
    while i > 0 {
        i -= 1;
        out.push(buf[i] as char);
    }
}
