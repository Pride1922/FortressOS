#![no_std]
#![no_main]

extern crate alloc;

mod allocator;
mod panic;
#[allow(dead_code)]
mod sys;

mod cursor;
mod files;
mod font;
mod notepad;
mod surface;
mod terminal;
mod wallpaper;
mod window;
use cursor::Cursor;
use files::FileManager;
use font::GLYPHS;
use notepad::Notepad;
use surface::Surface;
use terminal::Terminal;
use window::{Window, WindowManager};

// 16-byte stack aligned entry point satisfying System V ABI & FortressOS loader
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

#[unsafe(no_mangle)]
pub extern "C" fn rust_main(argc: usize, argv: *const *const u8) -> i32 {
    let mut is_test = false;
    if argc > 1 && !argv.is_null() {
        for i in 1..argc {
            let arg_ptr = unsafe { *argv.add(i) };
            if !arg_ptr.is_null() {
                let mut len = 0;
                while unsafe { *arg_ptr.add(len) } != 0 {
                    len += 1;
                }
                let arg = unsafe { core::slice::from_raw_parts(arg_ptr, len) };
                if arg == b"--test" {
                    is_test = true;
                    break;
                }
            }
        }
    }

    let acquired = sys::fb_acquire();
    if acquired != 0 {
        sys::write(
            2,
            if acquired == sys::EBUSY {
                b"[compositor] Display is owned by another process\n"
            } else {
                b"[compositor] Display acquisition failed\n"
            },
        );
        return 1;
    }
    if !allocator::init_heap() {
        sys::write(2, b"[COMPOSITOR] [FAIL] Anonymous heap allocation\n");
        sys::fb_release();
        return 1;
    }
    let status = run(is_test);
    if sys::fb_release() != 0 {
        return 1;
    }
    status
}

fn spawn_terminal() -> Option<(Window, Terminal)> {
    let mut pty_fds = [-1i32; 2];
    if sys::pty_pair(&mut pty_fds, sys::VFS_O_NONBLOCK) != 0 {
        return None;
    }
    let master_fd = pty_fds[0];
    let slave_fd = pty_fds[1];

    let mut win = Window::new(
        3,
        "Terminal - /bin/sh",
        surface::Rect {
            x: 140,
            y: 160,
            w: 520,
            h: 280,
        },
    )?;
    win.content.fill(terminal::TERM_BG);

    let cols = ((win.rect.w.saturating_sub(22)) / 8).clamp(20, terminal::TERM_MAX_COLS);
    let rows = ((win.rect.h.saturating_sub(38)) / 16).clamp(5, terminal::TERM_MAX_ROWS);
    let ws = sys::WinSize {
        ws_row: rows as u16,
        ws_col: cols as u16,
        ws_xpixel: (cols * 8) as u16,
        ws_ypixel: (rows * 16) as u16,
    };
    sys::set_winsize(master_fd, &ws);

    let actions = [
        sys::SpawnFdAction {
            action_type: 2, // DUP2
            dst_fd: 0,
            src_fd: slave_fd,
            ..Default::default()
        },
        sys::SpawnFdAction {
            action_type: 2, // DUP2
            dst_fd: 1,
            src_fd: slave_fd,
            ..Default::default()
        },
        sys::SpawnFdAction {
            action_type: 2, // DUP2
            dst_fd: 2,
            src_fd: slave_fd,
            ..Default::default()
        },
    ];

    let sh_path = b"/bin/sh\0";
    let argv: [*const u8; 2] = [sh_path.as_ptr(), core::ptr::null()];
    let opts = sys::SpawnOpts {
        size: core::mem::size_of::<sys::SpawnOpts>() as u32,
        version: 2,
        flags: sys::SPAWN_SETSID | sys::SPAWN_STAGED,
        reserved0: 0,
        argv: argv.as_ptr() as u64,
        envp: 0,
        cwd: 0,
        fd_actions: actions.as_ptr() as u64,
        action_count: 3,
        reserved1: 0,
        reserved2: 0,
    };

    let pid = sys::spawn_ext("/bin/sh", &opts);
    if pid <= 0 {
        sys::close(master_fd);
        sys::close(slave_fd);
        return None;
    }

    // Zero-race controlling terminal session leader binding
    let bind_err = sys::pty_set_session(master_fd, pid as u64);
    if bind_err != 0 {
        sys::group_release(pid as i64, sys::GROUP_CANCEL as usize);
        sys::close(master_fd);
        sys::close(slave_fd);
        return None;
    }

    // Release staged child to execute
    sys::group_release(pid as i64, sys::GROUP_RELEASE as usize);
    sys::close(slave_fd);

    let mut term = Terminal::new(3, master_fd, pid as i64);
    term.set_dimensions(cols, rows);
    term.render(&mut win);

    Some((win, term))
}

fn draw_label_pixel(label: &str, x: usize, y: usize, fg: u32, bg: u32) -> u32 {
    let Some(c) = label.chars().nth(x / 8) else {
        return bg;
    };
    let c = if (' '..='~').contains(&c) {
        c as usize
    } else {
        b'?' as usize
    };
    if GLYPHS[c - 32][y] & (128 >> (x % 8)) != 0 {
        fg
    } else {
        bg
    }
}

fn run(is_test: bool) -> i32 {
    sys::write(1, b"[COMPOSITOR] Phase 4 starting (right click to exit)\n");
    let fb = match sys::get_fb_info(0) {
        Ok(fb) if fb.version == 1 && fb.bpp == 32 => fb,
        _ => {
            sys::write(2, b"[compositor] Unsupported framebuffer format\n");
            return 1;
        }
    };
    let mut scene = match Surface::new(fb.width as usize, fb.height as usize) {
        Some(s) => s,
        None => {
            sys::write(2, b"[compositor] Surface allocation failed\n");
            return 1;
        }
    };
    let mut cursor = match Cursor::new(scene.width / 2, scene.height / 2) {
        Some(c) => c,
        None => {
            sys::write(2, b"[compositor] Cursor allocation failed\n");
            return 1;
        }
    };
    let mut fd = sys::open("/dev/input/mouse", 0);
    if fd < 0 {
        fd = sys::open("/dev/mouse0", 0);
    }
    if fd < 0 {
        sys::write(2, b"[COMPOSITOR] [FAIL] Mouse open (root required)\n");
        return 1;
    }
    // Console writes share VRAM: finish the startup diagnostic before painting.
    sys::write(1, b"[COMPOSITOR] [OK] Window server ready; polling\n");

    let wallpaper = if is_test {
        None
    } else {
        wallpaper::Wallpaper::load("/usr/share/wallpapers/hello-world.bin")
    };

    let mut windows = match WindowManager::demo() {
        Some(mut wm) => {
            if !is_test {
                wm.windows.clear();
            }
            wm
        }
        None => {
            sys::close(fd as i32);
            return 1;
        }
    };
    scene.damage(surface::Rect {
        x: 0,
        y: 0,
        w: scene.width,
        h: scene.height,
    });
    let mut ts = sys::Timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    if sys::clock_gettime(0, &mut ts) == 0 {
        windows.update_clock(&mut scene, ts.tv_sec);
    }
    windows.monitor(
        &mut scene,
        fb.pitch as usize,
        allocator::usage(),
        (cursor.x, cursor.y),
    );
    windows.render_with_wallpaper(&mut scene, wallpaper.as_ref());
    cursor.show(&mut scene);
    if !unsafe { scene.present(&fb) } {
        sys::close(fd as i32);
        return 1;
    }
    let mut term: Option<Terminal> = None;
    let mut notepad: Option<Notepad> = None;
    let mut files: Option<FileManager> = None;
    let mut is_locked: bool = false;
    let mut fds = [
        sys::PollFd {
            fd: fd as i32,
            events: sys::POLLIN,
            revents: 0,
        },
        sys::PollFd {
            fd: 0,
            events: sys::POLLIN,
            revents: 0,
        },
        sys::PollFd {
            fd: -1,
            events: sys::POLLIN,
            revents: 0,
        },
    ];
    let mut pending_packet: Option<[u8; 4]> = None;
    let mut frame_count: u32 = 0;
    let status = 'event_loop: loop {
        let mut had_mouse = false;
        let mut packet = [0u8; 4];

        if let Some(p) = pending_packet.take() {
            packet = p;
            had_mouse = true;
        } else {
            fds[2].fd = term.as_ref().map(|t| t.master_fd).unwrap_or(-1);
            let ready = sys::poll(&mut fds, -1);
            if ready == sys::EINTR {
                continue;
            }
            if ready < 0 || (fds[0].revents | fds[1].revents) & (sys::POLLERR | 0x10 | 0x20) != 0 {
                break 1;
            }

            // Output from /bin/sh over PTY master
            if fds[2].revents & sys::POLLIN != 0 {
                if let Some(ref mut t) = term {
                    let mut term_buf = [0u8; 128];
                    let n = sys::read(t.master_fd, &mut term_buf);
                    if n > 0 {
                        t.write_bytes(&term_buf[..n as usize]);
                        if let Some(w) = windows.windows.iter_mut().find(|w| w.id == t.window_id) {
                            t.render(w);
                            if !w.is_minimized {
                                scene.damage(w.rect);
                            }
                        }
                    }
                }
            }

            // Keyboard input from stdin (/dev/tty)
            if fds[1].revents & sys::POLLIN != 0 {
                let mut kbuf = [0u8; 16];
                let kn = sys::read(0, &mut kbuf);
                if kn > 0 {
                    let top_window_id = windows.focused_window_id();
                    for &b in &kbuf[..kn as usize] {
                        if is_locked {
                            is_locked = false;
                            scene.damage(surface::Rect { x: 0, y: 0, w: scene.width, h: scene.height });
                        } else if top_window_id == Some(3) && term.is_some() {
                            if let Some(ref t) = term {
                                sys::write(t.master_fd, &[b]);
                            }
                        } else if top_window_id == Some(4) && notepad.is_some() {
                            if let Some(ref mut np) = notepad {
                                if np.handle_key(b) {
                                    if let Some(w) = windows.windows.iter_mut().find(|w| w.id == 4) {
                                        np.render(w);
                                        scene.damage(w.rect);
                                    }
                                }
                            }
                        } else if top_window_id == Some(5) && files.is_some() {
                            if let Some(ref mut fm) = files {
                                if b == b'\r' || b == b'\n' {
                                    if let Some(file_path) = fm.enter_selected() {
                                        // Open selected file in Notepad
                                        if notepad.is_none() {
                                            if let Some(mut win) = Window::new(4, "Notes", surface::Rect { x: 180, y: 140, w: 480, h: 280 }) {
                                                let np = Notepad::new(4, &file_path);
                                                np.render(&mut win);
                                                windows.windows.push(win);
                                                windows.raise(4, &mut scene);
                                                notepad = Some(np);
                                            }
                                        } else {
                                            if let Some(ref mut np) = notepad {
                                                *np = Notepad::new(4, &file_path);
                                                if let Some(w) = windows.windows.iter_mut().find(|w| w.id == 4) {
                                                    np.render(w);
                                                    scene.damage(w.rect);
                                                }
                                            }
                                            windows.unminimize_and_raise(4, &mut scene);
                                        }
                                    } else {
                                        if let Some(w) = windows.windows.iter_mut().find(|w| w.id == 5) {
                                            fm.render(w);
                                            scene.damage(w.rect);
                                        }
                                    }
                                } else if b == b'u' || b == b'U' || b == 0x08 {
                                    fm.navigate_up();
                                    if let Some(w) = windows.windows.iter_mut().find(|w| w.id == 5) {
                                        fm.render(w);
                                        scene.damage(w.rect);
                                    }
                                } else if b == b'j' || b == b'J' || b == b's' || b == b'S' {
                                    if fm.selected + 1 < fm.entries.len() {
                                        fm.selected += 1;
                                        if let Some(w) = windows.windows.iter_mut().find(|w| w.id == 5) {
                                            fm.render(w);
                                            scene.damage(w.rect);
                                        }
                                    }
                                } else if b == b'k' || b == b'K' || b == b'w' || b == b'W' {
                                    fm.selected = fm.selected.saturating_sub(1);
                                    if let Some(w) = windows.windows.iter_mut().find(|w| w.id == 5) {
                                        fm.render(w);
                                        scene.damage(w.rect);
                                    }
                                }
                            }
                        } else {
                            if b == b'q' || b == b'Q' || b == 3 {
                                break 'event_loop 0;
                            } else if b == 27 {
                                if windows.start_open {
                                    windows.start_open = false;
                                    scene.damage(WindowManager::start_btn_rect(scene.height));
                                    scene.damage(WindowManager::start_menu_rect(scene.height));
                                } else {
                                    break 'event_loop 0;
                                }
                            } else if b == b'\t' {
                                windows.cycle_focus(&mut scene);
                            } else if b == b't' || b == b'T' {
                                if term.is_none() {
                                    if let Some((win, t)) = spawn_terminal() {
                                        windows.windows.push(win);
                                        windows.raise(3, &mut scene);
                                        term = Some(t);
                                    }
                                } else {
                                    windows.unminimize_and_raise(3, &mut scene);
                                }
                            } else if b == b'e' || b == b'E' {
                                if notepad.is_none() {
                                    if let Some(mut win) = Window::new(4, "Notes - /tmp/notes.txt", surface::Rect { x: 180, y: 140, w: 480, h: 280 }) {
                                        let np = Notepad::new(4, "/tmp/notes.txt");
                                        np.render(&mut win);
                                        windows.windows.push(win);
                                        windows.raise(4, &mut scene);
                                        notepad = Some(np);
                                    }
                                } else {
                                    windows.unminimize_and_raise(4, &mut scene);
                                }
                            } else if b == b'f' || b == b'F' {
                                if files.is_none() {
                                    if let Some(mut win) = Window::new(5, "Files - /", surface::Rect { x: 220, y: 180, w: 460, h: 300 }) {
                                        let fm = FileManager::new(5, "/");
                                        fm.render(&mut win);
                                        windows.windows.push(win);
                                        windows.raise(5, &mut scene);
                                        files = Some(fm);
                                    }
                                } else {
                                    windows.unminimize_and_raise(5, &mut scene);
                                }
                            } else if b == b'm' || b == b'M' {
                                windows.unminimize_and_raise(1, &mut scene);
                            } else if b == b'p' || b == b'P' {
                                windows.unminimize_and_raise(2, &mut scene);
                            }
                        }
                    }
                }
            }

            if fds[0].revents & sys::POLLIN != 0 {
                let n = sys::read(fd as i32, &mut packet);
                if n == 4 {
                    had_mouse = true;
                } else if n != sys::EINTR && n != 0 {
                    break 1;
                }
            }
        }

        if had_mouse {
            if packet[0] & 2 != 0 {
                break 0;
            }

            let buttons = packet[0];
            let mut dx_total: i32 = packet[1] as i8 as i32;
            let mut dy_total: i32 = packet[2] as i8 as i32;

            // Drain pending motion packets from kernel FIFO to coalesce rapid movement
            let mut extra = [0u8; 4];
            while sys::read(fd as i32, &mut extra) == 4 {
                if extra[0] & 2 != 0 {
                    pending_packet = Some(extra);
                    break;
                }
                if extra[0] != buttons {
                    pending_packet = Some(extra);
                    break;
                }
                dx_total += extra[1] as i8 as i32;
                dy_total += extra[2] as i8 as i32;
            }

            cursor.advance(&mut scene, dx_total, dy_total);
            let action = windows.mouse(cursor.x, cursor.y, buttons, &mut scene);
            match action {
                window::Action::Exit => break 0,
                window::Action::LockScreen => {
                    is_locked = !is_locked;
                    scene.damage(surface::Rect { x: 0, y: 0, w: scene.width, h: scene.height });
                }
                window::Action::ClientClick(id, cx, cy) => {
                    if is_locked {
                        is_locked = false;
                        scene.damage(surface::Rect { x: 0, y: 0, w: scene.width, h: scene.height });
                    } else if id == 4 {
                        if let Some(ref mut np) = notepad {
                            if np.handle_click(cx, cy) {
                                if let Some(w) = windows.windows.iter_mut().find(|w| w.id == 4) {
                                    np.render(w);
                                    scene.damage(w.rect);
                                }
                            }
                        }
                    } else if id == 5 {
                        if let Some(ref mut fm) = files {
                            if let Some(file_action) = fm.handle_click(cx, cy) {
                                match file_action {
                                    files::FileAction::OpenPath(path) => {
                                        if notepad.is_none() {
                                            if let Some(mut win) = Window::new(4, "Notes", surface::Rect { x: 180, y: 140, w: 480, h: 280 }) {
                                                let np = Notepad::new(4, &path);
                                                np.render(&mut win);
                                                windows.windows.push(win);
                                                windows.raise(4, &mut scene);
                                                notepad = Some(np);
                                            }
                                        } else {
                                            if let Some(ref mut np) = notepad {
                                                *np = Notepad::new(4, &path);
                                                if let Some(w) = windows.windows.iter_mut().find(|w| w.id == 4) {
                                                    np.render(w);
                                                    scene.damage(w.rect);
                                                }
                                            }
                                            windows.unminimize_and_raise(4, &mut scene);
                                        }
                                    }
                                    files::FileAction::Refresh => {
                                        if let Some(w) = windows.windows.iter_mut().find(|w| w.id == 5) {
                                            fm.render(w);
                                            scene.damage(w.rect);
                                        }
                                    }
                                }
                            } else {
                                if let Some(w) = windows.windows.iter_mut().find(|w| w.id == 5) {
                                    fm.render(w);
                                    scene.damage(w.rect);
                                }
                            }
                        }
                    }
                }
                window::Action::SpawnTerminal => {
                    if term.is_none() {
                        if let Some((win, t)) = spawn_terminal() {
                            windows.windows.push(win);
                            windows.raise(3, &mut scene);
                            term = Some(t);
                        }
                    } else {
                        windows.unminimize_and_raise(3, &mut scene);
                    }
                }
                window::Action::SpawnNotepad => {
                    if notepad.is_none() {
                        if let Some(mut win) = Window::new(4, "Notes - /tmp/notes.txt", surface::Rect { x: 180, y: 140, w: 480, h: 280 }) {
                            let np = Notepad::new(4, "/tmp/notes.txt");
                            np.render(&mut win);
                            windows.windows.push(win);
                            windows.raise(4, &mut scene);
                            notepad = Some(np);
                        }
                    } else {
                        windows.unminimize_and_raise(4, &mut scene);
                    }
                }
                window::Action::SpawnFileManager => {
                    if files.is_none() {
                        if let Some(mut win) = Window::new(5, "Files - /", surface::Rect { x: 220, y: 180, w: 460, h: 300 }) {
                            let fm = FileManager::new(5, "/");
                            fm.render(&mut win);
                            windows.windows.push(win);
                            windows.raise(5, &mut scene);
                            files = Some(fm);
                        }
                    } else {
                        windows.unminimize_and_raise(5, &mut scene);
                    }
                }
                window::Action::WindowResized(id) => {
                    if id == 3 {
                        if let Some(ref mut t) = term {
                            if let Some(w) = windows.windows.iter_mut().find(|w| w.id == 3) {
                                let cols = ((w.rect.w.saturating_sub(22)) / 8).clamp(20, terminal::TERM_MAX_COLS);
                                let rows = ((w.rect.h.saturating_sub(38)) / 16).clamp(5, terminal::TERM_MAX_ROWS);
                                t.set_dimensions(cols, rows);
                                let ws = sys::WinSize {
                                    ws_row: rows as u16,
                                    ws_col: cols as u16,
                                    ws_xpixel: (cols * 8) as u16,
                                    ws_ypixel: (rows * 16) as u16,
                                };
                                sys::set_winsize(t.master_fd, &ws);
                                t.render(w);
                                scene.damage(w.rect);
                            }
                        }
                    } else if id == 4 {
                        if let Some(ref np) = notepad {
                            if let Some(w) = windows.windows.iter_mut().find(|w| w.id == 4) {
                                np.render(w);
                                scene.damage(w.rect);
                            }
                        }
                    } else if id == 5 {
                        if let Some(ref fm) = files {
                            if let Some(w) = windows.windows.iter_mut().find(|w| w.id == 5) {
                                fm.render(w);
                                scene.damage(w.rect);
                            }
                        }
                    }
                }
                window::Action::None => {}
            }
            if frame_count % 8 == 0 {
                windows.monitor_mouse(&mut scene, (cursor.x, cursor.y));
            }
        }

        // Cleanup windows closed via [X] button
        if let Some(ref t) = term {
            if !windows.windows.iter().any(|w| w.id == t.window_id) {
                sys::close(t.master_fd);
                term = None;
            }
        }
        if let Some(ref np) = notepad {
            if !windows.windows.iter().any(|w| w.id == np.window_id) {
                notepad = None;
            }
        }
        if let Some(ref fm) = files {
            if !windows.windows.iter().any(|w| w.id == fm.window_id) {
                files = None;
            }
        }

        if frame_count % 8 == 0 {
            if sys::clock_gettime(0, &mut ts) == 0 {
                windows.update_clock(&mut scene, ts.tv_sec);
            }
        }

        windows.render_with_wallpaper(&mut scene, wallpaper.as_ref());

        if is_locked {
            // Draw macOS Lock Screen overlay
            let card_w = 320;
            let card_h = 180;
            let card_x = scene.width.saturating_sub(card_w) / 2;
            let card_y = scene.height.saturating_sub(card_h) / 2;

            for y in 0..scene.height {
                for x in 0..scene.width {
                    if x >= card_x && x < card_x + card_w && y >= card_y && y < card_y + card_h {
                        let cx = x - card_x;
                        let cy = y - card_y;
                        let is_card_border = cx == 0 || cx == card_w - 1 || cy == 0 || cy == card_h - 1;
                        scene.pixels[y * scene.width + x] = if is_card_border {
                            0xff484f58
                        } else if cy >= 20 && cy < 36 && cx >= 70 {
                            draw_label_pixel("Fortress Administrator", cx - 70, cy - 20, 0xffffffff, 0xff161b22)
                        } else if cy >= 60 && cy < 76 && cx >= 60 {
                            draw_label_pixel("Password: [ ******** ]", cx - 60, cy - 60, 0xff58a6ff, 0xff161b22)
                        } else if cy >= 120 && cy < 136 && cx >= 32 {
                            draw_label_pixel("Click or Press Enter to Unlock", cx - 32, cy - 120, 0xff8b949e, 0xff161b22)
                        } else {
                            0xff161b22
                        };
                    } else {
                        // Frosted dark acrylic scrim
                        scene.pixels[y * scene.width + x] = 0xff0d1117;
                    }
                }
            }
        }

        cursor.show(&mut scene);
        if !unsafe { scene.present(&fb) } {
            break 1;
        }
        frame_count = frame_count.wrapping_add(1);

        // Frame pacing / micro-sleep: space out blits to prevent tearing and display controller bus stuttering
        if pending_packet.is_none() {
            let p_ts = sys::Timespec {
                tv_sec: 0,
                tv_nsec: 1_000_000,
            };
            sys::nanosleep(&p_ts, None);
        }
    };
    if let Some(t) = term {
        sys::close(t.master_fd);
    }
    cursor.hide(&mut scene);
    unsafe {
        scene.present(&fb);
    }
    sys::close(fd as i32);
    sys::write(1, b"[COMPOSITOR] Event loop stopped\n");
    status
}
