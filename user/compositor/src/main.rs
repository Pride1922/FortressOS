#![no_std]
#![no_main]

extern crate alloc;

mod allocator;
mod bench;
mod panic;
mod clients;
mod perf;
mod proto;
mod privilege;
#[allow(dead_code)]
mod sys;

mod cursor;
mod desktop;
mod files;
mod font;
mod preferences;
mod surface;
mod terminal;
mod ui;
mod wallpaper;
mod window;
use cursor::Cursor;
use files::FileManager;
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
        call rust_main /* rdi=argc, rsi=argv, rdx=envp from the loader */
        mov rdi, rax
        xor eax, eax
        syscall
        ud2
    "#
);

#[unsafe(no_mangle)]
pub extern "C" fn rust_main(argc: usize, argv: *const *const u8, envp: *const *const u8) -> i32 {
    let mut is_test = false;
    let mut perf_hud = false;
    let mut bench_drag = false;
    if argc > 1 && !argv.is_null() {
        for i in 1..argc {
            let arg_ptr = unsafe { *argv.add(i) };
            if !arg_ptr.is_null() {
                let mut len = 0;
                while unsafe { *arg_ptr.add(len) } != 0 {
                    len += 1;
                }
                let arg = unsafe { core::slice::from_raw_parts(arg_ptr, len) };
                if arg == b"--version" {
                    use core::fmt::Write;
                    let mut version = preferences::Text::<128>::default();
                    let _ = writeln!(
                        version,
                        "FortressOS Desktop {} (app API {})",
                        desktop::VERSION,
                        ui::APP_API_VERSION
                    );
                    sys::write(1, version.as_bytes());
                    return 0;
                }
                if arg == b"--perf" {
                    perf_hud = true;
                    continue;
                }
                if arg == b"--bench-drag" {
                    bench_drag = true;
                    continue;
                }
                if arg == b"--test" {
                    is_test = true;
                    continue;
                }
                if let Some(path) = arg.strip_prefix(b"--app=") {
                    // What the `h` shortcut launches (an absolute path). Apps
                    // run as the session user, who may run any binary anyway.
                    if path.first() != Some(&b'/') || !clients::set_app_path(path) {
                        sys::write(
                            2,
                            b"compositor: --app needs an absolute path under 128 bytes\n",
                        );
                        return 2;
                    }
                    continue;
                }
                if arg == b"--settings-broker" && argc == 2 {
                    return privilege::broker_main();
                }
                // Unknown options must not silently start the desktop.
                sys::write(
                    2,
                    b"usage: compositor [--perf] [--bench-drag] [--test] [--app=PATH] | --version\n",
                );
                return 2;
            }
        }
    }

    // Writes to a dead app or settings helper must fail with EPIPE instead
    // of killing the whole desktop.
    sys::ignore_sigpipe();
    let launch = match privilege::launch_from_env(envp) {
        Ok(launch) => launch,
        Err(why) => {
            privilege::report(2, "[FAIL]", why, None);
            return 1;
        }
    };

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
    let Some(mut desktop) = desktop::AppHost::load(is_test) else {
        sys::write(
            2,
            b"[compositor] Cannot allocate desktop application host\n",
        );
        sys::fb_release();
        return 1;
    };
    let mut perf = perf::FrameStats::new(perf_hud && !is_test);
    let status = run(
        is_test,
        bench_drag && !is_test,
        &mut desktop,
        &mut perf,
        &launch,
    );
    if sys::fb_release() != 0 {
        return 1;
    }
    // Text console is visible again: leave the whole-session numbers on screen.
    if !is_test && perf.session.frames > 0 {
        use core::fmt::Write;
        let mut line = preferences::Text::<192>::from_str("[COMPOSITOR] [PERF] session: ");
        perf.session.format(&mut line);
        let _ = line.write_str("\n");
        sys::write(1, line.as_bytes());
    }
    status
}

#[inline(never)]
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

#[inline(never)]
fn run(
    is_test: bool,
    bench_drag: bool,
    desktop: &mut desktop::AppHost,
    perf: &mut perf::FrameStats,
    launch: &privilege::Launch,
) -> i32 {
    sys::write(1, b"[COMPOSITOR] Desktop starting (system menu to exit)\n");
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
    // Close-on-exec: the terminal shell must not inherit the mouse.
    let mut fd = sys::open("/dev/input/mouse", sys::VFS_O_CLOEXEC);
    if fd < 0 {
        fd = sys::open("/dev/mouse0", sys::VFS_O_CLOEXEC);
    }
    if fd < 0 {
        sys::write(2, b"[COMPOSITOR] [FAIL] Mouse open (root required)\n");
        return 1;
    }
    // Last root-only step done: hand system settings to the root helper and
    // become the invoking user for everything else, or stop here.
    match launch {
        privilege::Launch::Drop(id) => {
            match privilege::spawn_broker() {
                Ok(pid) => privilege::report(1, "[OK]", "Settings helper started", Some(pid)),
                Err(e) => privilege::report(
                    2,
                    "[WARN]",
                    "No settings helper; system settings cannot be saved",
                    Some(e),
                ),
            }
            if let Err(why) = privilege::drop_to(id) {
                privilege::report(2, "[FAIL] Privilege drop:", why, None);
                sys::close(fd as i32);
                return 1;
            }
            privilege::report(
                1,
                "[OK]",
                "Dropped root; desktop runs as uid",
                Some(id.uid as isize),
            );
        }
        privilege::Launch::StayRoot => privilege::report(
            1,
            "[WARN]",
            "Started by root without sudo: desktop apps run as root",
            None,
        ),
    }
    // Console writes share VRAM: finish the startup diagnostic before painting.
    sys::write(1, b"[COMPOSITOR] [OK] Window server ready; polling\n");

    let mut wallpaper = if is_test || !desktop.preferences.wallpaper {
        None
    } else {
        wallpaper::Wallpaper::load("/usr/share/wallpapers/hello-world.bin")
    };
    let mut windows = if is_test {
        match WindowManager::demo() {
            Some(wm) => wm,
            None => {
                sys::close(fd as i32);
                return 1;
            }
        }
    } else {
        WindowManager::desktop()
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
    if !is_test {
        desktop.refresh(ts.tv_sec, &mut windows, &mut scene);
    }
    windows.render_with_wallpaper(&mut scene, wallpaper.as_ref());
    cursor.show(&mut scene);
    if !unsafe { scene.present(&fb) } {
        sys::close(fd as i32);
        return 1;
    }
    if bench_drag {
        let status = bench::drag(&mut scene, &mut windows, &fb, wallpaper.as_ref(), perf);
        sys::close(fd as i32);
        return status;
    }
    event_loop(
        &mut scene,
        &mut cursor,
        &mut windows,
        fd as i32,
        &fb,
        is_test,
        &mut wallpaper,
        desktop,
        perf,
    )
}

#[inline(never)]
fn event_loop(
    scene: &mut Surface,
    cursor: &mut Cursor,
    windows: &mut WindowManager,
    fd: i32,
    fb: &sys::FbInfoV1,
    is_test: bool,
    wallpaper: &mut Option<wallpaper::Wallpaper>,
    desktop: &mut desktop::AppHost,
    perf: &mut perf::FrameStats,
) -> i32 {
    let mut ts = sys::Timespec::default();
    let mut term: Option<Terminal> = None;
    let mut files: Option<FileManager> = None;
    let mut clients = clients::Clients::default();
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
        // Request pipes of out-of-process apps (clients.rs).
        sys::PollFd::default(),
        sys::PollFd::default(),
        sys::PollFd::default(),
        sys::PollFd::default(),
    ];
    const _: () = assert!(clients::MAX_CLIENTS == 4);
    let mut pending_packet: Option<[u8; 4]> = None;
    let mut frame_count: u32 = 0;
    let mut last_info_sec = -1;
    let status = 'event_loop: loop {
        // Timer and keyboard updates can mutate the scene without a mouse
        // packet. Always restore save-under before any scene changes.
        cursor.hide(scene);
        // Pending-packet frames start here; poll frames restart after waking.
        let mut t_wake = perf::now_ns();
        let mut had_mouse = false;
        let mut packet = [0u8; 4];

        if let Some(p) = pending_packet.take() {
            packet = p;
            had_mouse = true;
        } else {
            fds[2].fd = term.as_ref().map(|t| t.master_fd).unwrap_or(-1);
            let mut app_fds = [sys::PollFd::default(); clients::MAX_CLIENTS];
            clients.poll_set(&mut app_fds);
            fds[3..].copy_from_slice(&app_fds);
            let ready = sys::poll(&mut fds, if is_test { -1 } else { 1000 });
            t_wake = perf::now_ns();
            if ready == sys::EINTR {
                continue;
            }
            if ready < 0 || (fds[0].revents | fds[1].revents) & (sys::POLLERR | 0x10 | 0x20) != 0 {
                break 1;
            }

            // Output from /bin/sh over PTY master. Once the shell is gone the
            // master stays readable (EOF/hangup) forever: close the terminal
            // instead of spinning on it. The [X] cleanup below closes the fd.
            if fds[2].revents & (sys::POLLIN | 0x10 | 0x08) != 0 {
                if let Some(ref mut t) = term {
                    let hung_up = fds[2].revents & (0x10 | 0x08) != 0;
                    if !drain_terminal(t, windows, scene) || hung_up {
                        windows.close_window(t.window_id, scene);
                    }
                }
            }

            for slot in 0..clients::MAX_CLIENTS {
                clients.ready(slot, fds[3 + slot].revents, windows, scene);
            }

            // Keyboard input from stdin (/dev/tty)
            if fds[1].revents & sys::POLLIN != 0 {
                let mut kbuf = [0u8; 16];
                let kn = sys::read(0, &mut kbuf);
                if kn == 0 {
                    // EOF (e.g. `sudo -b` gives /dev/null): stop polling
                    // stdin, or poll reports it readable forever and the
                    // loop spins at full speed.
                    fds[1].fd = -1;
                }
                if kn > 0 {
                    for &b in &kbuf[..kn as usize] {
                        if let Some(id) = windows.focused_window_id() {
                            if clients.key(id, b, windows, scene) {
                                continue;
                            }
                        }
                        if !is_test {
                            if let Some(id) =
                                windows.focused_window_id().filter(|id| ui::registered(*id))
                            {
                                desktop.key(id, b, windows, scene);
                                continue;
                            }
                            if windows.focused_window_id().is_none()
                                || windows.focused_window_id() == Some(6)
                            {
                                let app = match b {
                                    b's' | b'S' => Some(7),
                                    b'n' | b'N' => Some(8),
                                    b'c' | b'C' => Some(9),
                                    _ => None,
                                };
                                if let Some(id) = app {
                                    desktop.open(id, windows, scene);
                                    continue;
                                }
                                if b == b'h' || b == b'H' {
                                    match clients.launch(clients::app_path(), &[], scene) {
                                        Ok(pid) => {
                                            privilege::report(1, "[OK]", "App started", Some(pid))
                                        }
                                        Err(e) => privilege::report(
                                            2,
                                            "[WARN]",
                                            "App launch failed",
                                            Some(e),
                                        ),
                                    }
                                    continue;
                                }
                            }
                        }
                        if handle_key(
                            b,
                            scene,
                            windows,
                            &mut term,
                            &mut clients,
                            &mut files,
                            is_test,
                        ) {
                            break 'event_loop 0;
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
            if is_test && packet[0] & 2 != 0 {
                break 0;
            }

            let buttons = packet[0];
            let mut dx_total: i32 = packet[1] as i8 as i32;
            let mut dy_total: i32 = packet[2] as i8 as i32;
            let mut wheel_total: i32 = packet[3] as i8 as i32;

            // Drain pending motion packets from kernel FIFO to coalesce rapid movement
            let mut extra = [0u8; 4];
            // Bound coalescing so a continuous high-rate input stream cannot
            // postpone presentation indefinitely. Leave remaining packets queued.
            for _ in 0..16 {
                if sys::read(fd as i32, &mut extra) != 4 {
                    break;
                }
                if is_test && extra[0] & 2 != 0 {
                    pending_packet = Some(extra);
                    break;
                }
                if extra[0] != buttons {
                    pending_packet = Some(extra);
                    break;
                }
                dx_total += extra[1] as i8 as i32;
                dy_total += extra[2] as i8 as i32;
                wheel_total += extra[3] as i8 as i32;
            }

            cursor.advance(scene, dx_total, dy_total);
            if !is_test && wheel_total != 0 {
                if let Some((id, _)) = windows.hit_test(cursor.x, cursor.y) {
                    desktop.scroll(id, -wheel_total, windows, scene);
                }
            }
            let action = windows.mouse(cursor.x, cursor.y, buttons, scene);
            let handled = if !is_test {
                match action {
                    window::Action::OpenDesktopApp(id) => {
                        desktop.open(id, windows, scene);
                        true
                    }
                    window::Action::ClientClick(id, x, y) if ui::registered(id) => {
                        desktop.pointer(id, x, y, windows, scene);
                        true
                    }
                    window::Action::WindowResized(id) if ui::registered(id) => {
                        desktop.render(id, windows, scene);
                        true
                    }
                    window::Action::ClientClick(id, x, y) if clients.owns(id) => {
                        clients.click(id, x, y, windows, scene);
                        true
                    }
                    window::Action::WindowResized(id) if clients.owns(id) => {
                        clients.resized(id, windows, scene);
                        true
                    }
                    _ => false,
                }
            } else {
                false
            };
            if !handled
                && handle_action(
                    action,
                    scene,
                    windows,
                    &mut term,
                    &mut clients,
                    &mut files,
                    &mut last_info_sec,
                )
            {
                break 0;
            }
            if frame_count % 8 == 0 {
                windows.monitor_mouse(scene, (cursor.x, cursor.y));
            }
        }

        // Cleanup windows closed via [X] button
        clients.maintain(windows, scene);
        if let Some(ref t) = term {
            if !windows.windows.iter().any(|w| w.id == t.window_id) {
                sys::close(t.master_fd);
                term = None;
            }
        }
        if let Some(ref fm) = files {
            if !windows.windows.iter().any(|w| w.id == fm.window_id) {
                files = None;
            }
        }

        if !is_test || frame_count % 8 == 0 {
            if sys::clock_gettime(0, &mut ts) == 0 {
                if is_test {
                    windows.update_clock(scene, ts.tv_sec);
                }
            }
        }
        if !is_test {
            if desktop.changed {
                desktop.changed = false;
                if desktop.wallpaper_changed {
                    desktop.wallpaper_changed = false;
                    *wallpaper = if desktop.preferences.wallpaper {
                        wallpaper::Wallpaper::load("/usr/share/wallpapers/hello-world.bin")
                    } else {
                        None
                    };
                    scene.damage(surface::Rect {
                        x: 0,
                        y: 0,
                        w: scene.width,
                        h: scene.height,
                    });
                }
                if desktop.layout_changed {
                    desktop.layout_changed = false;
                    desktop.reflow(windows, scene);
                    last_info_sec = -1;
                    for id in [3, 4, 5] {
                        handle_action(
                            window::Action::WindowResized(id),
                            scene,
                            windows,
                            &mut term,
                            &mut clients,
                            &mut files,
                            &mut last_info_sec,
                        );
                    }
                }
                windows.clock_24h = desktop.preferences.clock_24h;
                for descriptor in ui::APPS {
                    desktop.render(descriptor.id, windows, scene);
                }
                scene.damage(WindowManager::clock_rect(scene.width, scene.height));
            }
            desktop.paint_invalid(windows, scene);
            desktop.refresh(ts.tv_sec, windows, scene);
        }

        if !is_test
            && windows.windows.iter().any(|w| w.id == 6 && !w.is_minimized)
            && (last_info_sec != ts.tv_sec
                || windows
                    .windows
                    .iter()
                    .any(|w| w.id == 6 && w.content.first() != Some(&window::theme::PANEL)))
        {
            windows.system_panel(scene, sys::system_info());
            last_info_sec = ts.tv_sec;
        }

        windows.render_with_wallpaper(scene, wallpaper.as_ref());

        perf.draw_hud(scene);
        cursor.show(scene);
        let px = scene.damage_pixels();
        let t_ready = perf::now_ns();
        if !unsafe { scene.present(fb) } {
            break 1;
        }
        let t_done = perf::now_ns();
        perf.record(
            t_ready.saturating_sub(t_wake),
            t_done.saturating_sub(t_ready),
            px,
            t_done,
        );
        frame_count = frame_count.wrapping_add(1);

        // SYS_POLL already blocks when there is no work. Sleeping after a
        // presentation adds timer-granularity latency to the next mouse packet
        // and cannot synchronize scanout without a vblank API.
    };
    clients.shutdown(windows, scene);
    if let Some(t) = term {
        sys::close(t.master_fd);
    }
    cursor.hide(scene);
    unsafe {
        scene.present(fb);
    }
    sys::close(fd as i32);
    sys::write(1, b"[COMPOSITOR] Event loop stopped\n");
    status
}

/// Opens `path` (default /tmp/notes.txt) in a new /bin/notepad process.
fn open_notepad(clients: &mut clients::Clients, path: Option<&str>, scene: &Surface) {
    let mut buf = [0u8; 128];
    let arg = path.and_then(|p| {
        let ok = p.starts_with('/') && p.len() < buf.len() && !p.as_bytes().contains(&0);
        ok.then(|| {
            buf[..p.len()].copy_from_slice(p.as_bytes());
            core::ffi::CStr::from_bytes_until_nul(&buf).ok()
        })
        .flatten()
    });
    let args: &[&core::ffi::CStr] = match arg.as_ref() {
        Some(a) => core::slice::from_ref(a),
        None => &[],
    };
    match clients.launch(c"/bin/notepad", args, scene) {
        Ok(pid) => privilege::report(1, "[OK]", "Notepad started", Some(pid)),
        Err(e) => privilege::report(2, "[WARN]", "Notepad launch failed", Some(e)),
    }
}

#[inline(never)]
fn handle_key(
    b: u8,
    scene: &mut Surface,
    windows: &mut WindowManager,
    term: &mut Option<Terminal>,
    clients: &mut clients::Clients,
    files: &mut Option<FileManager>,
    is_test: bool,
) -> bool {
    if b == 27 && windows.context_menu.is_some() {
        windows.close_context_menu(scene);
        return false;
    }
    let top_window_id = windows.focused_window_id();
    if top_window_id == Some(3) && term.is_some() {
        if let Some(t) = term.as_ref() {
            sys::write(t.master_fd, &[b]);
        }
    } else if top_window_id == Some(5) && files.is_some() {
        if let Some(fm) = files.as_mut() {
            if b == b'\r' || b == b'\n' {
                if let Some(file_path) = fm.enter_selected() {
                    // Open selected file in Notepad
                    open_notepad(clients, Some(&file_path), scene);
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
            return true;
        } else if b == 27 {
            if windows.start_open {
                windows.start_open = false;
                scene.damage(WindowManager::start_btn_rect(scene.height));
                scene.damage(WindowManager::start_menu_rect(scene.height));
            } else {
                return true;
            }
        } else if b == b'\t' {
            windows.cycle_focus(scene);
        } else if b == b't' || b == b'T' {
            if term.is_none() {
                if let Some((win, t)) = spawn_terminal() {
                    windows.windows.push(win);
                    windows.raise(3, scene);
                    *term = Some(t);
                }
            } else {
                windows.unminimize_and_raise(3, scene);
            }
        } else if b == b'e' || b == b'E' {
            open_notepad(clients, None, scene);
        } else if b == b'f' || b == b'F' {
            if files.is_none() {
                if let Some(mut win) = Window::new(
                    5,
                    "Files - /",
                    surface::Rect {
                        x: 220,
                        y: 180,
                        w: 460,
                        h: 300,
                    },
                ) {
                    let fm = FileManager::new(5, "/");
                    fm.render(&mut win);
                    windows.windows.push(win);
                    windows.raise(5, scene);
                    *files = Some(fm);
                }
            } else {
                windows.unminimize_and_raise(5, scene);
            }
        } else if b == b'm' || b == b'M' {
            windows.ensure_window_1(scene);
        } else if is_test && (b == b'p' || b == b'P') {
            windows.unminimize_and_raise(2, scene);
        }
    }

    false
}

#[inline(never)]
fn handle_action(
    action: window::Action,
    scene: &mut Surface,
    windows: &mut WindowManager,
    term: &mut Option<Terminal>,
    clients: &mut clients::Clients,
    files: &mut Option<FileManager>,
    last_info_sec: &mut i64,
) -> bool {
    match action {
        window::Action::OpenDesktopApp(_) => {}
        window::Action::Exit => return true,
        window::Action::ClientClick(id, cx, cy) => {
            if id == 5 {
                if let Some(fm) = files.as_mut() {
                    if let Some(file_action) = fm.handle_click(cx, cy) {
                        match file_action {
                            files::FileAction::OpenPath(path) => {
                                open_notepad(clients, Some(&path), scene);
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
                    windows.raise(3, scene);
                    *term = Some(t);
                }
            } else {
                windows.unminimize_and_raise(3, scene);
            }
        }
        window::Action::SpawnNotepad => {
            open_notepad(clients, None, scene);
        }
        window::Action::SpawnFileManager => {
            if files.is_none() {
                if let Some(mut win) = Window::new(
                    5,
                    "Files - /",
                    surface::Rect {
                        x: 220,
                        y: 180,
                        w: 460,
                        h: 300,
                    },
                ) {
                    let fm = FileManager::new(5, "/");
                    fm.render(&mut win);
                    windows.windows.push(win);
                    windows.raise(5, scene);
                    *files = Some(fm);
                }
            } else {
                windows.unminimize_and_raise(5, scene);
            }
        }
        window::Action::WindowResized(id) => {
            if id == 6 {
                *last_info_sec = -1;
            }
            if id == 3 {
                if let Some(t) = term.as_mut() {
                    if let Some(w) = windows.windows.iter_mut().find(|w| w.id == 3) {
                        let cols =
                            ((w.rect.w.saturating_sub(22)) / 8).clamp(20, terminal::TERM_MAX_COLS);
                        let rows =
                            ((w.rect.h.saturating_sub(38)) / 16).clamp(5, terminal::TERM_MAX_ROWS);
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
            } else if id == 5 {
                if let Some(fm) = files.as_ref() {
                    if let Some(w) = windows.windows.iter_mut().find(|w| w.id == 5) {
                        fm.render(w);
                        scene.damage(w.rect);
                    }
                }
            }
        }
        window::Action::None => {}
    }
    false
}

#[inline(never)]
/// False when the shell side is gone (EOF on the first read after poll
/// reported the master readable).
fn drain_terminal(t: &mut Terminal, windows: &mut WindowManager, scene: &mut Surface) -> bool {
    let mut term_buf = [0u8; 256];
    let mut total_read = 0;
    let mut alive = true;
    loop {
        let n = sys::read(t.master_fd, &mut term_buf);
        if n <= 0 {
            alive = !(n == 0 && total_read == 0);
            break;
        }
        t.write_bytes(&term_buf[..n as usize]);
        total_read += n;
        if total_read >= 4096 {
            break;
        }
    }
    if total_read > 0 {
        if let Some(w) = windows.windows.iter_mut().find(|w| w.id == t.window_id) {
            let dirty = t.render_dirty(w);
            if !w.is_minimized {
                scene.damage(dirty);
            }
        }
    }
    alive
}
