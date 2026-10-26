#![no_std]
#![no_main]

extern crate alloc;

mod allocator;
mod panic;
#[allow(dead_code)]
mod sys;

mod cursor;
mod font;
mod surface;
mod terminal;
mod window;
use cursor::Cursor;
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
pub extern "C" fn rust_main() -> i32 {
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
    let status = run();
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

    let ws = sys::WinSize {
        ws_row: terminal::TERM_ROWS as u16,
        ws_col: terminal::TERM_COLS as u16,
        ws_xpixel: 0,
        ws_ypixel: 0,
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

    let mut win = Window::new(
        3,
        "Terminal - /bin/sh",
        surface::Rect {
            x: 140,
            y: 160,
            w: 500,
            h: 260,
        },
    )?;
    win.content.fill(terminal::TERM_BG);

    let term = Terminal::new(3, master_fd, pid as i64);
    term.render(&mut win);

    Some((win, term))
}

fn run() -> i32 {
    sys::write(1, b"[COMPOSITOR] Phase 4 starting (right click to exit)\n");
    let fb = match sys::get_fb_info(0) {
        Ok(fb) => fb,
        Err(_) => {
            sys::write(2, b"[COMPOSITOR] [FAIL] SYS_FBINFO\n");
            return 1;
        }
    };
    let mut scene = match Surface::new(fb.width as usize, fb.height as usize) {
        Some(s) => s,
        None => {
            sys::write(2, b"[COMPOSITOR] [FAIL] Backbuffer exceeds bounded heap\n");
            return 1;
        }
    };
    scene.fill(window::DESKTOP);
    let mut windows = match WindowManager::demo() {
        Some(w) => w,
        None => {
            sys::write(2, b"[COMPOSITOR] [FAIL] Window allocation\n");
            return 1;
        }
    };
    let mut cursor = match Cursor::new(scene.width / 2, scene.height / 2) {
        Some(c) => c,
        None => {
            sys::write(2, b"[COMPOSITOR] [FAIL] Cursor allocation\n");
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
    let mut ts = sys::Timespec::default();
    if sys::clock_gettime(0, &mut ts) == 0 {
        windows.update_clock(&mut scene, ts.tv_sec);
    }
    windows.monitor(
        &mut scene,
        fb.pitch as usize,
        allocator::usage(),
        (cursor.x, cursor.y),
    );
    windows.render(&mut scene);
    cursor.show(&mut scene);
    if !unsafe { scene.present(&fb) } {
        sys::close(fd as i32);
        return 1;
    }
    let mut term: Option<Terminal> = None;
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
                    let is_term_focused = windows
                        .windows
                        .last()
                        .map(|w| w.id == 3 && w.is_focused)
                        .unwrap_or(false);
                    for &b in &kbuf[..kn as usize] {
                        if is_term_focused && term.is_some() {
                            if let Some(ref t) = term {
                                sys::write(t.master_fd, &[b]);
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
                window::Action::None => {}
            }
            if frame_count % 8 == 0 {
                windows.monitor_mouse(&mut scene, (cursor.x, cursor.y));
            }
        }

        // Cleanup terminal if Window 3 was closed via [X] button
        if let Some(ref t) = term {
            if !windows.windows.iter().any(|w| w.id == t.window_id) {
                sys::close(t.master_fd);
                term = None;
            }
        }

        if frame_count % 8 == 0 {
            if sys::clock_gettime(0, &mut ts) == 0 {
                windows.update_clock(&mut scene, ts.tv_sec);
            }
        }

        windows.render(&mut scene);
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
