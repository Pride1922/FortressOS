//! /bin/gterm - Fortress Terminal: Advanced Multi-Tab Desktop Terminal Emulator
//!
//! Features:
//! - Multi-tab session management with tab strip and PTY multiplexing (SYS_POLL)
//! - ANSI 16 and 256-color support with SGR text attributes
//! - 2,000-line circular scrollback buffer per tab with interactive scrollbar
//! - Global Buffer Find (Ctrl+Shift+F) highlighting across scrollback + Next/Prev
//! - Smart path-aware double-click selection (preserving '/', '.', '-', '_', '~', ':')
//! - Formatted clipboard copy (Ctrl+Shift+C) and paste (Ctrl+Shift+V)
//! - Session & workspace restore (/tmp/.gterm_session)
//! - Shortcuts: Ctrl+Shift+T (new tab), Ctrl+Shift+W (close tab), Alt+1..9 (switch tab)

#![no_std]
#![no_main]
#![allow(dead_code)]

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;

#[path = "../allocator.rs"]
mod allocator;
#[path = "../app.rs"]
mod app;
#[path = "../canvas.rs"]
mod canvas;
#[path = "../clipboard.rs"]
mod clipboard;
#[path = "../font.rs"]
mod font;
#[path = "../panic.rs"]
mod panic;
#[path = "../preferences.rs"]
mod preferences;
#[path = "../proto.rs"]
mod proto;
#[path = "../sys.rs"]
mod sys;
#[path = "../theme.rs"]
mod theme;
#[path = "../term_engine.rs"]
mod term_engine;

use app::{App, Event};
use canvas::Canvas;
use term_engine::{TermEngine, TERM_MAX_COLS, TERM_MAX_ROWS};

core::arch::global_asm!(
    r#"
    .global _start
    .section .text
    _start:
        and rsp, -16
        xor rbp, rbp
        call rust_main
        mov rdi, rax
        xor eax, eax
        syscall
        ud2
    "#
);

const HEAP_BYTES: usize = 8 << 20;
const EAGAIN: isize = -21;
const TAB_BAR_HEIGHT: usize = 28;
const MAX_TABS: usize = 8;
const SESSION_FILE: &str = "/tmp/.gterm_session";

static mut SESSION_ENVP: *const *const u8 = core::ptr::null();

fn winsize(cols: usize, rows: usize) -> sys::WinSize {
    sys::WinSize {
        ws_row: rows as u16,
        ws_col: cols as u16,
        ws_xpixel: (cols * 8) as u16,
        ws_ypixel: (rows * 16) as u16,
    }
}

fn start_shell(cols: usize, rows: usize) -> Option<(i32, i32)> {
    let mut pty = [-1i32; 2];
    if sys::pty_pair(&mut pty, sys::VFS_O_NONBLOCK | sys::VFS_O_CLOEXEC as u32) != 0 {
        return None;
    }
    let (master, slave) = (pty[0], pty[1]);
    sys::set_winsize(master, &winsize(cols, rows));
    let dup = |dst| sys::SpawnFdAction {
        action_type: 2,
        dst_fd: dst,
        src_fd: slave,
        ..Default::default()
    };
    let actions = [dup(0), dup(1), dup(2)];
    let argv: [*const u8; 2] = [c"/bin/sh".as_ptr() as *const u8, core::ptr::null()];

    let envp = unsafe { SESSION_ENVP };
    let mut cwd_ptr = 0u64;
    if !envp.is_null() {
        let mut i = 0;
        loop {
            let entry = unsafe { *envp.add(i) };
            if entry.is_null() {
                break;
            }
            let is_home = unsafe {
                *entry == b'H'
                    && *entry.add(1) == b'O'
                    && *entry.add(2) == b'M'
                    && *entry.add(3) == b'E'
                    && *entry.add(4) == b'='
            };
            if is_home {
                cwd_ptr = unsafe { entry.add(5) } as u64;
                break;
            }
            i += 1;
        }
    }

    let opts = sys::SpawnOpts {
        size: core::mem::size_of::<sys::SpawnOpts>() as u32,
        version: 2,
        flags: sys::SPAWN_SETSID | sys::SPAWN_STAGED,
        argv: argv.as_ptr() as u64,
        envp: envp as u64,
        cwd: cwd_ptr,
        fd_actions: actions.as_ptr() as u64,
        action_count: actions.len() as u32,
        ..Default::default()
    };
    let pid = sys::spawn_ext("/bin/sh", &opts);
    if pid <= 0 {
        sys::close(master);
        sys::close(slave);
        return None;
    }
    if sys::pty_set_session(master, pid as u64) != 0 {
        sys::group_release(pid as i64, sys::GROUP_CANCEL);
        sys::close(master);
        sys::close(slave);
        return None;
    }
    sys::group_release(pid as i64, sys::GROUP_RELEASE);
    sys::close(slave);
    Some((master, pid as i32))
}

struct Tab {
    title: String,
    master: i32,
    pid: i32,
    term: TermEngine,
    unread: bool,
}

impl Tab {
    fn new(title: &str, master: i32, pid: i32, cols: usize, rows: usize) -> Self {
        let mut term = TermEngine::new();
        term.set_dimensions(cols, rows);
        Self {
            title: String::from(title),
            master,
            pid,
            term,
            unread: false,
        }
    }

    fn close(&mut self) {
        if self.master >= 0 {
            sys::close(self.master);
            self.master = -1;
        }
    }
}

struct GTermApp {
    tabs: Vec<Tab>,
    active_tab: usize,
    search_open: bool,
    search_input: String,
    last_click_pos: Option<(usize, usize)>,
    last_click_ticks: u64,
    click_count: usize,
}

impl GTermApp {
    fn new() -> Self {
        Self {
            tabs: Vec::new(),
            active_tab: 0,
            search_open: false,
            search_input: String::new(),
            last_click_pos: None,
            last_click_ticks: 0,
            click_count: 0,
        }
    }

    fn active(&mut self) -> Option<&mut Tab> {
        self.tabs.get_mut(self.active_tab)
    }

    fn grid_for_window(width: usize, height: usize) -> (usize, usize) {
        let term_w = width.saturating_sub(28); // 10 px left margin + 10 right + 8 scrollbar
        let term_h = height.saturating_sub(TAB_BAR_HEIGHT + 12);
        (
            (term_w / 8).clamp(20, TERM_MAX_COLS),
            (term_h / 16).clamp(5, TERM_MAX_ROWS),
        )
    }

    fn add_tab(&mut self, width: usize, height: usize, title_override: Option<&str>) -> bool {
        if self.tabs.len() >= MAX_TABS {
            return false;
        }
        let (cols, rows) = Self::grid_for_window(width, height);
        let Some((master, pid)) = start_shell(cols, rows) else {
            return false;
        };

        let num = self.tabs.len() + 1;
        let mut title = String::new();
        if let Some(t) = title_override {
            title.push_str(t);
        } else {
            title.push_str("Shell ");
            title.push((b'0' + num as u8) as char);
        }

        self.tabs.push(Tab::new(&title, master, pid, cols, rows));
        self.active_tab = self.tabs.len() - 1;
        true
    }

    fn close_tab(&mut self, idx: usize) -> bool {
        if idx >= self.tabs.len() {
            return false;
        }
        self.tabs[idx].close();
        self.tabs.remove(idx);
        if self.active_tab >= self.tabs.len() && !self.tabs.is_empty() {
            self.active_tab = self.tabs.len() - 1;
        }
        !self.tabs.is_empty()
    }

    fn render_tab_bar(&self, c: &mut Canvas) {
        let w = c.width;
        // Tab bar background
        c.fill_rect(0, 0, w, TAB_BAR_HEIGHT, 0xff161b22);
        c.fill_rect(0, TAB_BAR_HEIGHT - 1, w, 1, 0xff30363d);

        let ntabs = self.tabs.len();
        if ntabs == 0 {
            return;
        }

        let tab_avail = w.saturating_sub(120);
        let tab_w = (tab_avail / ntabs).clamp(80, 160);

        for (i, tab) in self.tabs.iter().enumerate() {
            let x = 6 + i * (tab_w + 4);
            let is_active = i == self.active_tab;

            let bg = if is_active { 0xff21262d } else { 0xff161b22 };
            let fg = if is_active { 0xfff0f6fc } else { 0xff8b949e };

            c.fill_rect(x, 3, tab_w, TAB_BAR_HEIGHT - 4, bg);

            if is_active {
                // Bottom blue accent line for active tab
                c.fill_rect(x, TAB_BAR_HEIGHT - 3, tab_w, 2, 0xff58a6ff);
            }

            // Tab title
            let mut label = String::new();
            if tab.unread && !is_active {
                label.push_str("* ");
            }
            label.push_str(&tab.title);
            c.text(x + 8, 7, &label, fg, bg);

            // Close button 'x'
            let close_x = x + tab_w - 14;
            c.text(close_x, 7, "x", if is_active { 0xff8b949e } else { 0xff484f58 }, bg);
        }

        // '+' New Tab button
        let plus_x = 8 + ntabs * (tab_w + 4);
        c.fill_rect(plus_x, 3, 24, TAB_BAR_HEIGHT - 6, 0xff21262d);
        c.text(plus_x + 8, 7, "+", 0xffc9d1d9, 0xff21262d);

        // Search trigger indicator / shortcut badge
        let search_x = w.saturating_sub(84);
        let s_bg = if self.search_open { 0xff388bfd } else { 0xff21262d };
        let s_fg = if self.search_open { 0xffffffff } else { 0xff8b949e };
        c.fill_rect(search_x, 4, 76, 20, s_bg);
        c.text(search_x + 8, 6, "Find ^F", s_fg, s_bg);
    }

    fn render_search_overlay(&self, c: &mut Canvas) {
        if !self.search_open {
            return;
        }
        let w = c.width;
        let box_w = 260;
        let box_h = 30;
        let x = w.saturating_sub(box_w + 12);
        let y = TAB_BAR_HEIGHT + 4;

        c.fill_rect(x, y, box_w, box_h, 0xff1f242c);
        c.fill_rect(x, y, box_w, 1, 0xff388bfd);
        c.fill_rect(x, y + box_h - 1, box_w, 1, 0xff388bfd);

        // Search input text
        let mut display_text = String::new();
        display_text.push('/');
        display_text.push_str(&self.search_input);
        c.text(x + 8, y + 7, &display_text, 0xffffffff, 0xff1f242c);

        // Match count badge
        if let Some(tab) = self.tabs.get(self.active_tab) {
            let matches_len = tab.term.search_matches.len();
            let mut badge = String::new();
            if matches_len > 0 {
                append_num(&mut badge, tab.term.active_match_idx + 1);
                badge.push('/');
                append_num(&mut badge, matches_len);
            } else if !self.search_input.is_empty() {
                badge.push_str("0/0");
            }
            if !badge.is_empty() {
                c.text(x + box_w - 70, y + 7, &badge, 0xffe3b341, 0xff1f242c);
            }
        }

        // Close button
        c.text(x + box_w - 18, y + 7, "x", 0xff8b949e, 0xff1f242c);
    }

    fn render_scrollbar(&self, c: &mut Canvas) {
        let Some(tab) = self.tabs.get(self.active_tab) else { return; };
        let total = tab.term.total_lines();
        if total <= tab.term.rows {
            return;
        }

        let w = c.width;
        let h = c.height;
        let sb_x = w.saturating_sub(8);
        let sb_y = TAB_BAR_HEIGHT;
        let sb_h = h.saturating_sub(TAB_BAR_HEIGHT);

        c.fill_rect(sb_x, sb_y, 8, sb_h, 0xff161b22);

        let thumb_h = (sb_h * tab.term.rows / total).clamp(16, sb_h);
        let max_scroll = total - tab.term.rows;
        let current_scroll = max_scroll.saturating_sub(tab.term.scroll_offset);
        let thumb_y = sb_y + (current_scroll * (sb_h - thumb_h) / max_scroll.max(1));

        c.fill_rect(sb_x + 1, thumb_y, 6, thumb_h, 0xff484f58);
    }

    fn save_session(&self) {
        // Store open tab count to enable workspace restoration
        let count = self.tabs.len();
        if count == 0 { return; }

        let fd = sys::open(SESSION_FILE, (sys::VFS_O_CREAT | sys::VFS_O_WRONLY | sys::VFS_O_TRUNC) as i32);
        if fd >= 0 {
            let fd_i32 = fd as i32;
            let mut buf = [0u8; 16];
            buf[0] = b'0' + (count as u8).min(9);
            buf[1] = b'\n';
            let _ = sys::write(fd_i32, &buf[..2]);
            sys::close(fd_i32);
        }
    }

    fn restore_session(&mut self, width: usize, height: usize) {
        let fd = sys::open(SESSION_FILE, sys::VFS_O_RDONLY as i32);
        if fd < 0 {
            self.add_tab(width, height, None);
            return;
        }
        let fd_i32 = fd as i32;
        let mut buf = [0u8; 16];
        let n = sys::read(fd_i32, &mut buf);
        sys::close(fd_i32);

        if n > 0 && (b'1'..=b'8').contains(&buf[0]) {
            let target_tabs = (buf[0] - b'0') as usize;
            for _ in 0..target_tabs {
                self.add_tab(width, height, None);
            }
        } else {
            self.add_tab(width, height, None);
        }
    }
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

#[unsafe(no_mangle)]
pub extern "C" fn rust_main(_argc: usize, _argv: *const *const u8, envp: *const *const u8) -> i32 {
    unsafe { SESSION_ENVP = envp; }
    if !allocator::init_heap_bytes(HEAP_BYTES) {
        return 1;
    }

    let Some(mut app) = App::connect(680, 440, "Fortress Terminal") else {
        return 2;
    };

    let mut gterm = GTermApp::new();
    gterm.restore_session(app.width, app.height);

    // Keep PTY throughput buffering off the bounded process stack. A large
    // local is reserved in the entry prologue, before heap initialization.
    let mut buf = alloc::vec![0u8; 16384];

    'main_loop: loop {
        // Build PollFd array: FD_EVENTS (0) + active master fds
        let mut poll_fds = [sys::PollFd::default(); MAX_TABS + 1];
        poll_fds[0] = sys::PollFd {
            fd: 4, // FD_EVENTS
            events: sys::POLLIN,
            revents: 0,
        };

        let ntabs = gterm.tabs.len();
        if ntabs == 0 {
            break;
        }

        for (i, tab) in gterm.tabs.iter().enumerate() {
            poll_fds[i + 1] = sys::PollFd {
                fd: tab.master,
                events: sys::POLLIN,
                revents: 0,
            };
        }

        let ret = sys::poll(&mut poll_fds[..ntabs + 1], -1);
        if ret == sys::EINTR {
            continue;
        }
        if ret < 0 {
            break;
        }

        // 1. Process desktop UI events
        if poll_fds[0].revents & (sys::POLLIN | sys::POLLHUP | sys::POLLERR) != 0 {
            while let Some(ev) = app.next_event_timeout(0) {
                match ev {
                    Event::Configure => {
                        let (cols, rows) = GTermApp::grid_for_window(app.width, app.height);
                        for tab in &mut gterm.tabs {
                            sys::set_winsize(tab.master, &winsize(cols, rows));
                            tab.term.set_dimensions(cols, rows);
                        }
                        if let Some(tab) = gterm.active() {
                            tab.term.render(&mut app.canvas(), TAB_BAR_HEIGHT);
                        }
                        gterm.render_tab_bar(&mut app.canvas());
                        gterm.render_scrollbar(&mut app.canvas());
                        app.damage_all();
                    }
                    // Ctrl+Shift+T: New Tab (ASCII 20)
                    Event::Key(20) => {
                        if gterm.add_tab(app.width, app.height, None) {
                            let (cols, rows) = GTermApp::grid_for_window(app.width, app.height);
                            if let Some(tab) = gterm.active() {
                                sys::set_winsize(tab.master, &winsize(cols, rows));
                                tab.term.render(&mut app.canvas(), TAB_BAR_HEIGHT);
                            }
                            gterm.render_tab_bar(&mut app.canvas());
                            gterm.render_scrollbar(&mut app.canvas());
                            app.damage_all();
                        }
                    }
                    // Ctrl+Shift+W: Close Tab (ASCII 23)
                    Event::Key(23) => {
                        if !gterm.close_tab(gterm.active_tab) {
                            break 'main_loop;
                        }
                        if let Some(tab) = gterm.active() {
                            tab.term.render(&mut app.canvas(), TAB_BAR_HEIGHT);
                        }
                        gterm.render_tab_bar(&mut app.canvas());
                        gterm.render_scrollbar(&mut app.canvas());
                        app.damage_all();
                    }
                // Ctrl+Shift+F: Toggle Search (ASCII 6)
                Event::Key(6) => {
                    gterm.search_open = !gterm.search_open;
                    if !gterm.search_open {
                        gterm.search_input.clear();
                        if let Some(tab) = gterm.active() {
                            tab.term.perform_search("");
                            tab.term.render(&mut app.canvas(), TAB_BAR_HEIGHT);
                        }
                    }
                    gterm.render_tab_bar(&mut app.canvas());
                    gterm.render_search_overlay(&mut app.canvas());
                    app.damage_all();
                }
                // Escape key (27)
                Event::Key(27) => {
                    if gterm.search_open {
                        gterm.search_open = false;
                        gterm.search_input.clear();
                        if let Some(tab) = gterm.active() {
                            tab.term.perform_search("");
                            tab.term.render(&mut app.canvas(), TAB_BAR_HEIGHT);
                        }
                        gterm.render_tab_bar(&mut app.canvas());
                        app.damage_all();
                    } else if let Some(tab) = gterm.active() {
                        let _ = sys::write(tab.master, &[27]);
                    }
                }
                // Ctrl+Shift+C: Formatted & plain text copy (ASCII 3 with selection)
                Event::Key(3) => {
                    if let Some(tab) = gterm.active() {
                        if let Some(sel) = tab.term.selected_text_formatted().or_else(|| tab.term.selected_text_plain()) {
                            let _ = clipboard::set(&sel);
                            tab.term.clear_selection();
                            if let Some((y, h)) = tab.term.render_dirty(&mut app.canvas(), TAB_BAR_HEIGHT) {
                                app.damage(0, y, app.width, h);
                            }
                        } else {
                            let _ = sys::write(tab.master, &[3]); // SIGINT
                        }
                    }
                }
                // Ctrl+Shift+V: Paste from clipboard (ASCII 22)
                Event::Key(22) => {
                    if let Some(tab) = gterm.active() {
                        tab.term.clear_selection();
                        if let Some(clip) = clipboard::get() {
                            let _ = sys::write_all(tab.master, clip.as_bytes());
                        }
                    }
                }
                // Search input routing when search bar is open
                Event::Key(b) if gterm.search_open => {
                    match b {
                        b'\r' | b'\n' => {
                            if let Some(tab) = gterm.active() {
                                tab.term.next_match();
                                tab.term.render(&mut app.canvas(), TAB_BAR_HEIGHT);
                                gterm.render_search_overlay(&mut app.canvas());
                                app.damage_all();
                            }
                        }
                        8 | 127 => {
                            // Backspace
                            gterm.search_input.pop();
                            let q = gterm.search_input.clone();
                            if let Some(tab) = gterm.active() {
                                tab.term.perform_search(&q);
                                tab.term.render(&mut app.canvas(), TAB_BAR_HEIGHT);
                                gterm.render_search_overlay(&mut app.canvas());
                                app.damage_all();
                            }
                        }
                        32..=126 => {
                            gterm.search_input.push(b as char);
                            let q = gterm.search_input.clone();
                            if let Some(tab) = gterm.active() {
                                tab.term.perform_search(&q);
                                tab.term.render(&mut app.canvas(), TAB_BAR_HEIGHT);
                                gterm.render_search_overlay(&mut app.canvas());
                                app.damage_all();
                            }
                        }
                        _ => {}
                    }
                }
                // Regular keys forwarded to active terminal
                Event::Key(b) => {
                    if let Some(tab) = gterm.active() {
                        tab.term.scroll_to_bottom();
                        tab.term.clear_selection();
                        let _ = sys::write(tab.master, &[b]);
                    }
                }
                Event::Click(x, y) => {
                    // Check Tab Bar interaction
                    if y < TAB_BAR_HEIGHT {
                        let ntabs = gterm.tabs.len();
                        let tab_avail = app.width.saturating_sub(120);
                        let tab_w = (tab_avail / ntabs.max(1)).clamp(80, 160);

                        for i in 0..ntabs {
                            let tx = 6 + i * (tab_w + 4);
                            if x >= tx && x < tx + tab_w {
                                // Check close button
                                if x >= tx + tab_w - 18 {
                                    if !gterm.close_tab(i) {
                                        break 'main_loop;
                                    }
                                } else {
                                    gterm.active_tab = i;
                                    gterm.tabs[i].unread = false;
                                }
                                if let Some(tab) = gterm.active() {
                                    tab.term.render(&mut app.canvas(), TAB_BAR_HEIGHT);
                                }
                                gterm.render_tab_bar(&mut app.canvas());
                                gterm.render_scrollbar(&mut app.canvas());
                                app.damage_all();
                                break;
                            }
                        }

                        // '+' Button
                        let plus_x = 8 + ntabs * (tab_w + 4);
                        if x >= plus_x && x < plus_x + 24 {
                            if gterm.add_tab(app.width, app.height, None) {
                                let (cols, rows) = GTermApp::grid_for_window(app.width, app.height);
                                if let Some(tab) = gterm.active() {
                                    sys::set_winsize(tab.master, &winsize(cols, rows));
                                    tab.term.render(&mut app.canvas(), TAB_BAR_HEIGHT);
                                }
                                gterm.render_tab_bar(&mut app.canvas());
                                gterm.render_scrollbar(&mut app.canvas());
                                app.damage_all();
                            }
                        }

                        // Search Toggle
                        let search_x = app.width.saturating_sub(84);
                        if x >= search_x {
                            gterm.search_open = !gterm.search_open;
                            gterm.render_tab_bar(&mut app.canvas());
                            gterm.render_search_overlay(&mut app.canvas());
                            app.damage_all();
                        }
                    } else if gterm.active_tab < gterm.tabs.len() {
                        let active_idx = gterm.active_tab;
                        // Double-click detection for smart token selection
                        let (total_row, col) = gterm.tabs[active_idx].term.cell_at_pixel(x, y, TAB_BAR_HEIGHT);
                        let is_double_click = gterm.last_click_pos == Some((total_row, col));
                        gterm.last_click_pos = Some((total_row, col));
                        gterm.click_count = if is_double_click { gterm.click_count + 1 } else { 1 };
                        let count = gterm.click_count;

                        let tab = &mut gterm.tabs[active_idx];
                        if count == 2 {
                            tab.term.select_word_at(total_row, col);
                        } else if count >= 3 {
                            tab.term.select_line_at(total_row);
                            gterm.click_count = 0;
                        } else {
                            tab.term.handle_click(x, y, TAB_BAR_HEIGHT);
                        }

                        if let Some((dy, dh)) = tab.term.render_dirty(&mut app.canvas(), TAB_BAR_HEIGHT) {
                            app.damage(0, dy, app.width, dh);
                        }
                    }
                }
                Event::Drag(x, y) => {
                    if y >= TAB_BAR_HEIGHT {
                        if let Some(tab) = gterm.active() {
                            if tab.term.handle_drag(x, y, TAB_BAR_HEIGHT) {
                                if let Some((dy, dh)) = tab.term.render_dirty(&mut app.canvas(), TAB_BAR_HEIGHT) {
                                    app.damage(0, dy, app.width, dh);
                                }
                            }
                        }
                    }
                }
                Event::Close => break 'main_loop,
            }
        }
        if poll_fds[0].revents & (sys::POLLHUP | sys::POLLERR) != 0 {
            break 'main_loop;
        }
    }

        // 2. Read output from PTY masters
        let mut closed_indices = Vec::new();
        for i in 0..gterm.tabs.len() {
            let rev = poll_fds[i + 1].revents;
            if rev & sys::POLLIN != 0 {
                let master = gterm.tabs[i].master;
                let mut total = 0;
                let mut eof = false;

                while total < 16384 {
                    let n = sys::read(master, &mut buf);
                    if n == EAGAIN || n == sys::EINTR {
                        break;
                    }
                    if n <= 0 {
                        eof = total == 0;
                        break;
                    }
                    gterm.tabs[i].term.write_bytes(&buf[..n as usize]);
                    total += n as usize;
                }

                if i == gterm.active_tab {
                    if let Some((y, h)) = gterm.tabs[i].term.render_dirty(&mut app.canvas(), TAB_BAR_HEIGHT) {
                        app.damage(0, y, app.width, h);
                    }
                } else if total > 0 {
                    gterm.tabs[i].unread = true;
                    gterm.render_tab_bar(&mut app.canvas());
                    app.damage(0, 0, app.width, TAB_BAR_HEIGHT);
                }

                if eof {
                    closed_indices.push(i);
                }
            } else if rev & (sys::POLLHUP | sys::POLLERR) != 0 {
                closed_indices.push(i);
            }
        }

        // Close tabs whose child shell terminated
        while let Some(idx) = closed_indices.pop() {
            if !gterm.close_tab(idx) {
                break;
            }
            if let Some(tab) = gterm.active() {
                tab.term.render(&mut app.canvas(), TAB_BAR_HEIGHT);
            }
            gterm.render_tab_bar(&mut app.canvas());
            gterm.render_scrollbar(&mut app.canvas());
            app.damage_all();
        }
    }

    gterm.save_session();
    for tab in &mut gterm.tabs {
        tab.close();
    }
    0
}
