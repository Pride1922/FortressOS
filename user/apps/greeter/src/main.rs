//! /bin/desktop-greeter: Graphical Display Manager & Login Screen for FortressOS.
//! Built with the `libui` App SDK. Runs with root display authorization at boot,
//! draws the login interface on the framebuffer, authenticates credentials via
//! /bin/desktop-unlock, drops privileges, and launches the user desktop session.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::vec::Vec;
use libui::client::raw_sys::{self, FbInfoV1, PollFd, POLLIN};
use libui::client::Args;
use libui::gfx::{rect, Rect};
use libui::painter::Painter;
use libui::proto::KeyEvent;
use libui::state::TextField;
use libui::theme;

libui::app_main!(main, heap = 8 << 20);

const KBD_PATH: &[u8] = b"/dev/input/keyboard\0";
const KBD_FALLBACK: &[u8] = b"/dev/kbd0\0";
const UNLOCK_BIN: &[u8] = b"/bin/desktop-unlock\0";
const DESKTOP_BIN: &[u8] = b"/usr/bin/desktop\0";
const COMPOSITOR_BIN: &[u8] = b"/bin/compositor\0";

#[repr(C)]
#[derive(Copy, Clone, Debug, Default)]
struct SpawnFdAction {
    action_type: u32,
    dst_fd: i32,
    src_fd: i32,
    flags: u32,
    mode: u32,
    reserved: u32,
    path: u64,
}

#[repr(C)]
#[derive(Copy, Clone, Debug, Default)]
struct SpawnOpts {
    size: u32,
    version: u32,
    flags: u32,
    reserved0: u32,
    argv: u64,
    envp: u64,
    cwd: u64,
    fd_actions: u64,
    action_count: u32,
    reserved1: u32,
    reserved2: u64,
}

fn is_leap_year(year: u32) -> bool {
    year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)
}

fn epoch_to_datetime(epoch: u64) -> Option<(u32, u8, u8, u8, u8, u8)> {
    let mut days = epoch / 86400;
    let sec_in_day = (epoch % 86400) as u32;

    let hour = (sec_in_day / 3600) as u8;
    let min = ((sec_in_day % 3600) / 60) as u8;
    let sec = (sec_in_day % 60) as u8;

    let mut year = 1970u32;
    loop {
        let diy = if is_leap_year(year) { 366 } else { 365 };
        if days < diy {
            break;
        }
        days -= diy;
        year += 1;
        if year > 2200 {
            return None;
        }
    }

    let days_in_months = [31u64, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31];
    let mut month = 1u8;
    for (m, &dim_val) in days_in_months.iter().enumerate() {
        let mut dim = dim_val;
        if m == 1 && is_leap_year(year) {
            dim = 29;
        }
        if days < dim {
            month = (m + 1) as u8;
            break;
        }
        days -= dim;
    }
    let day = (days + 1) as u8;
    Some((year, month, day, hour, min, sec))
}

fn append_u32_digits(buf: &mut [u8], pos: &mut usize, val: u32, digits: usize) {
    let mut divisor = 1u32;
    for _ in 1..digits {
        divisor *= 10;
    }
    for _ in 0..digits {
        buf[*pos] = b'0' + ((val / divisor) % 10) as u8;
        *pos += 1;
        divisor /= 10;
    }
}

fn log_greeter_msg(msg: &str) {
    let mut buf = [0u8; 384];
    let mut pos = 0;

    let mut ts = raw_sys::Timespec::default();
    let r = unsafe {
        raw_sys::syscall2(
            raw_sys::SYS_CLOCK_GETTIME,
            0, // CLOCK_REALTIME
            &mut ts as *mut _ as usize,
        )
    };

    if r == 0 && ts.tv_sec > 1000000 {
        if let Some((y, m, d, hh, mm, ss)) = epoch_to_datetime(ts.tv_sec as u64) {
            buf[pos] = b'['; pos += 1;
            append_u32_digits(&mut buf, &mut pos, y, 4);
            buf[pos] = b'-'; pos += 1;
            append_u32_digits(&mut buf, &mut pos, m as u32, 2);
            buf[pos] = b'-'; pos += 1;
            append_u32_digits(&mut buf, &mut pos, d as u32, 2);
            buf[pos] = b'T'; pos += 1;
            append_u32_digits(&mut buf, &mut pos, hh as u32, 2);
            buf[pos] = b':'; pos += 1;
            append_u32_digits(&mut buf, &mut pos, mm as u32, 2);
            buf[pos] = b':'; pos += 1;
            append_u32_digits(&mut buf, &mut pos, ss as u32, 2);
            buf[pos] = b'Z'; pos += 1;
            buf[pos] = b']'; pos += 1;
            buf[pos] = b' '; pos += 1;
        }
    } else {
        // Monotonic fallback: [0012.345s]
        let mut mono = raw_sys::Timespec::default();
        let mr = unsafe {
            raw_sys::syscall2(
                raw_sys::SYS_CLOCK_GETTIME,
                1, // CLOCK_MONOTONIC
                &mut mono as *mut _ as usize,
            )
        };
        if mr == 0 {
            buf[pos] = b'['; pos += 1;
            append_u32_digits(&mut buf, &mut pos, (mono.tv_sec as u32) % 10000, 4);
            buf[pos] = b'.'; pos += 1;
            append_u32_digits(&mut buf, &mut pos, ((mono.tv_nsec / 1_000_000) as u32) % 1000, 3);
            buf[pos] = b's'; pos += 1;
            buf[pos] = b']'; pos += 1;
            buf[pos] = b' '; pos += 1;
        }
    }

    let mbytes = msg.as_bytes();
    let copy_len = mbytes.len().min(buf.len() - pos);
    buf[pos..pos + copy_len].copy_from_slice(&mbytes[..copy_len]);
    pos += copy_len;

    let payload = &buf[..pos];

    // 1. Write to stderr (fd 2), visible on COM1 UART console
    raw_sys::write(2, payload);

    // 2. Append to log file: prefer /var/log/greeter.log, fallback to /run/greeter.log
    let mut fd = unsafe {
        raw_sys::syscall2(
            raw_sys::SYS_OPEN,
            b"/var/log/greeter.log\0".as_ptr() as usize,
            0x441, // VFS_O_WRONLY | VFS_O_CREAT | VFS_O_APPEND
        )
    };
    if fd < 0 {
        fd = unsafe {
            raw_sys::syscall2(
                raw_sys::SYS_OPEN,
                b"/run/greeter.log\0".as_ptr() as usize,
                0x441,
            )
        };
    }
    if fd >= 0 {
        raw_sys::write(fd as i32, payload);
        raw_sys::close(fd as i32);
    }
}

fn log_greeter_auth(user: &str, success: bool, reason: &str) {
    let mut buf = [0u8; 256];
    let mut len = 0;

    let prefix = b"[greeter] auth ";
    buf[len..len + prefix.len()].copy_from_slice(prefix);
    len += prefix.len();

    let res_str = if success { b"PASS user='" } else { b"FAIL user='" };
    buf[len..len + res_str.len()].copy_from_slice(res_str);
    len += res_str.len();

    let ubytes = user.as_bytes();
    let ulen = ubytes.len().min(32);
    buf[len..len + ulen].copy_from_slice(&ubytes[..ulen]);
    len += ulen;

    let mid = b"' reason='";
    buf[len..len + mid.len()].copy_from_slice(mid);
    len += mid.len();

    let rbytes = reason.as_bytes();
    let rlen = rbytes.len().min(64);
    buf[len..len + rlen].copy_from_slice(&rbytes[..rlen]);
    len += rlen;

    let suffix = b"'\n";
    buf[len..len + suffix.len()].copy_from_slice(suffix);
    len += suffix.len();

    if let Ok(s) = core::str::from_utf8(&buf[..len]) {
        log_greeter_msg(s);
    }
}

struct GreeterState {
    username: TextField<32>,
    password: TextField<64>,
    focus: usize, // 0 = user, 1 = pass, 2 = login btn, 3 = restart, 4 = shutdown
    error: Option<&'static str>,
    card_rect: Rect,
    user_rect: Rect,
    pass_rect: Rect,
    login_btn_rect: Rect,
    restart_btn_rect: Rect,
    shutdown_btn_rect: Rect,
}

impl GreeterState {
    fn new(screen_w: usize, screen_h: usize) -> Self {
        let mut username = TextField::new();
        username.set_text("operator");

        let cw = 400.min(screen_w.saturating_sub(40));
        let ch = 340.min(screen_h.saturating_sub(40));
        let cx = (screen_w.saturating_sub(cw)) / 2;
        let cy = (screen_h.saturating_sub(ch)) / 2;

        let card_rect = rect(cx, cy, cw, ch);
        let user_rect = rect(cx + 32, cy + 110, cw - 64, 34);
        let pass_rect = rect(cx + 32, cy + 180, cw - 64, 34);
        let login_btn_rect = rect(cx + 32, cy + 240, cw - 64, 38);
        let restart_btn_rect = rect(cx + 32, cy + 295, (cw - 74) / 2, 28);
        let shutdown_btn_rect = rect(cx + 32 + (cw - 74) / 2 + 10, cy + 295, (cw - 74) / 2, 28);

        Self {
            username,
            password: TextField::new(),
            focus: 1, // Start focused on password since operator is default
            error: None,
            card_rect,
            user_rect,
            pass_rect,
            login_btn_rect,
            restart_btn_rect,
            shutdown_btn_rect,
        }
    }

    fn handle_key(&mut self, ev: KeyEvent) -> Action {
        if ev.state == 0 {
            return Action::None;
        }

        // Tab / Shift+Tab navigation
        if ev.keycode == 15 { // KEY_TAB
            if ev.modifiers & 1 != 0 { // Shift
                self.focus = (self.focus + 4) % 5;
            } else {
                self.focus = (self.focus + 1) % 5;
            }
            return Action::Redraw;
        }

        // Arrow keys
        if ev.keycode == 103 { // KEY_UP
            self.focus = (self.focus + 4) % 5;
            return Action::Redraw;
        }
        if ev.keycode == 108 { // KEY_DOWN
            self.focus = (self.focus + 1) % 5;
            return Action::Redraw;
        }

        // Enter key
        if ev.keycode == 28 || ev.keycode == 96 || ev.ascii == b'\r' || ev.ascii == b'\n' {
            match self.focus {
                0 => {
                    self.focus = 1;
                    return Action::Redraw;
                }
                1 | 2 => return Action::AttemptLogin,
                3 => return Action::Restart,
                4 => return Action::Shutdown,
                _ => return Action::AttemptLogin,
            }
        }

        // Backspace
        if ev.ascii == 8 || ev.ascii == 127 {
            if self.focus == 0 {
                self.username.backspace();
                self.error = None;
                return Action::Redraw;
            } else if self.focus == 1 {
                self.password.backspace();
                self.error = None;
                return Action::Redraw;
            }
        }

        // Printable text
        if ev.ascii >= 32 && ev.ascii <= 126 {
            let ch = ev.ascii as char;
            if self.focus == 0 {
                self.username.push(ch);
                self.error = None;
                return Action::Redraw;
            } else if self.focus == 1 {
                self.password.push(ch);
                self.error = None;
                return Action::Redraw;
            }
        }

        Action::None
    }
}

enum Action {
    None,
    Redraw,
    AttemptLogin,
    Restart,
    Shutdown,
}

fn open_keyboard() -> i32 {
    let fd = unsafe {
        raw_sys::syscall3(
            raw_sys::SYS_OPEN,
            KBD_PATH.as_ptr() as usize,
            0, // O_RDONLY
            0,
        )
    };
    if fd >= 0 {
        return fd as i32;
    }
    let fd = unsafe {
        raw_sys::syscall3(
            raw_sys::SYS_OPEN,
            KBD_FALLBACK.as_ptr() as usize,
            0,
            0,
        )
    };
    if fd >= 0 {
        fd as i32
    } else {
        0 // fallback to stdin
    }
}

fn authenticate(user: &str, pass: &str) -> bool {
    let mut fds = [-1i32; 2];
    let pipe_res = unsafe {
        raw_sys::syscall2(
            raw_sys::SYS_PIPE,
            fds.as_mut_ptr() as usize,
            0x80000, // VFS_O_CLOEXEC
        )
    };
    if pipe_res < 0 {
        log_greeter_auth(user, false, "pipe allocation failed");
        return false;
    }

    let mut actions = [SpawnFdAction::default(); 3];
    let mut act_count = 0;
    if fds[0] != 0 {
        actions[act_count] = SpawnFdAction {
            action_type: 2, // SPAWN_FD_ACTION_DUP2
            dst_fd: 0,
            src_fd: fds[0],
            ..Default::default()
        };
        act_count += 1;
        actions[act_count] = SpawnFdAction {
            action_type: 3, // SPAWN_FD_ACTION_CLOSE
            dst_fd: fds[0],
            ..Default::default()
        };
        act_count += 1;
    }
    actions[act_count] = SpawnFdAction {
        action_type: 3, // SPAWN_FD_ACTION_CLOSE
        dst_fd: fds[1],
        ..Default::default()
    };
    act_count += 1;

    let mut user_cstr = [0u8; 36];
    let ulen = user.len().min(32);
    user_cstr[..ulen].copy_from_slice(user.as_bytes());
    user_cstr[ulen] = 0;

    let args: [*const u8; 3] = [
        UNLOCK_BIN.as_ptr(),
        user_cstr.as_ptr(),
        core::ptr::null(),
    ];

    let opts = SpawnOpts {
        size: core::mem::size_of::<SpawnOpts>() as u32,
        version: 2,
        argv: args.as_ptr() as u64,
        fd_actions: actions.as_ptr() as u64,
        action_count: act_count as u32,
        ..Default::default()
    };

    let pid = unsafe {
        raw_sys::syscall3(
            raw_sys::SYS_SPAWN_EXT,
            UNLOCK_BIN.as_ptr() as usize,
            &opts as *const _ as usize,
            opts.size as usize,
        )
    };

    // Close read end in parent immediately
    raw_sys::close(fds[0]);

    if pid <= 0 {
        raw_sys::close(fds[1]);
        log_greeter_auth(user, false, "failed to spawn unlock helper");
        return false;
    }

    // Write password to helper pipe
    if !pass.is_empty() {
        raw_sys::write(fds[1], pass.as_bytes());
    }
    raw_sys::write(fds[1], b"\n");
    raw_sys::close(fds[1]);

    // Wait for helper exit status
    let mut status = 0u64;
    let waited = unsafe {
        raw_sys::syscall3(
            raw_sys::SYS_WAITPID,
            pid as usize,
            &mut status as *mut _ as usize,
            0,
        )
    };

    let success = waited == pid && status == 0;
    if success {
        log_greeter_auth(user, true, "login successful");
    } else {
        log_greeter_auth(user, false, "authentication failed (helper status non-zero)");
    }

    success
}

fn render_greeter(
    p: &mut Painter,
    state: &GreeterState,
    screen_w: usize,
    screen_h: usize,
) {
    // 1. Wallpaper background: Dark slate gradient
    for y in 0..screen_h {
        let ny = (y * 1024 / screen_h.max(1)) as i32;
        let dy = ny - 380;
        let dy2 = dy * dy;
        for x in 0..screen_w {
            let nx = (x * 1024 / screen_w.max(1)) as i32;
            let dx = nx - 580;
            let dist2 = (dx * dx + dy2) / 1024;
            let glow = (900 - dist2).max(0) as u32;
            let r = 14 + glow * 24 / 900;
            let g = 18 + glow * 38 / 900;
            let b = 28 + glow * 65 / 900;
            p.put(x, y, 0xff000000 | (r << 16) | (g << 8) | b);
        }
    }

    // 2. Central Glass Card
    let cr = state.card_rect;
    let card_bg = 0xff161b22;
    p.sheet_card(cr, card_bg, 0xff30363d, true);

    // 3. Brand Header
    p.label_bold(cr.x + 32, cr.y + 24, cr.w - 64, 28, "FortressOS", 0xffffffff, card_bg);
    p.label(cr.x + 32, cr.y + 54, cr.w - 64, "Sign in to your secure session", theme::MUTED);

    // Error message banner
    if let Some(err) = state.error {
        p.label(cr.x + 32, cr.y + 76, cr.w - 64, err, theme::RED);
    }

    // 4. Username Field
    p.label(cr.x + 32, cr.y + 92, 120, "Username", theme::SUBTEXT);
    p.text_input(state.user_rect, state.username.as_str(), state.focus == 0);

    // 5. Password Field (Masked with bullet characters)
    p.label(cr.x + 32, cr.y + 162, 120, "Password", theme::SUBTEXT);
    let masked_buf = [b'*'; 64];
    let plen = state.password.len().min(64);
    let masked_str = core::str::from_utf8(&masked_buf[..plen]).unwrap_or("");
    p.text_input(state.pass_rect, masked_str, state.focus == 1);

    // 6. Login Button
    p.button_accent(state.login_btn_rect, "Sign In", state.focus == 2);

    // 7. Power Buttons
    p.button(state.restart_btn_rect, "Restart", false, state.focus == 3);
    p.button(state.shutdown_btn_rect, "Shut Down", false, state.focus == 4);
}

fn launch_user_session(_username: &str) {
    log_greeter_msg("[greeter] releasing framebuffer lease and launching session\n");
    // Release framebuffer so the compositor user session can acquire it
    raw_sys::fb_release();

    let args: [*const u8; 2] = [
        DESKTOP_BIN.as_ptr(),
        core::ptr::null(),
    ];

    let mut pid = unsafe {
        raw_sys::syscall2(
            raw_sys::SYS_SPAWN,
            DESKTOP_BIN.as_ptr() as usize,
            args.as_ptr() as usize,
        )
    };

    if pid <= 0 {
        // Fallback to /bin/compositor directly
        let c_args: [*const u8; 2] = [
            COMPOSITOR_BIN.as_ptr(),
            core::ptr::null(),
        ];
        pid = unsafe {
            raw_sys::syscall2(
                raw_sys::SYS_SPAWN,
                COMPOSITOR_BIN.as_ptr() as usize,
                c_args.as_ptr() as usize,
            )
        };
    }

    if pid > 0 {
        log_greeter_msg("[greeter] user session running\n");
        let mut status = 0u64;
        unsafe {
            raw_sys::syscall3(
                raw_sys::SYS_WAITPID,
                pid as usize,
                &mut status as *mut _ as usize,
                0,
            );
        }
        log_greeter_msg("[greeter] user session terminated, returning to greeter\n");
    } else {
        log_greeter_msg("[greeter] failed to spawn desktop session\n");
    }

    // Re-acquire framebuffer for greeter when session ends
    raw_sys::fb_acquire();
}

fn main(_args: Args) -> i32 {
    log_greeter_msg("[greeter] starting graphical login manager\n");

    // 1. Acquire exclusive framebuffer lease
    let acq = raw_sys::fb_acquire();
    if acq != 0 {
        log_greeter_msg("[greeter] failed to acquire framebuffer lease; falling back to console login\n");
        return 1; // Fall back to console /bin/login
    }

    let mut fb = FbInfoV1::default();
    if raw_sys::fb_info(&mut fb, 0) != 0 || fb.bpp != 32 {
        log_greeter_msg("[greeter] invalid framebuffer info\n");
        raw_sys::fb_release();
        return 1;
    }

    let screen_w = fb.width as usize;
    let screen_h = fb.height as usize;
    let pitch = fb.pitch as usize / 4;
    let vram = fb.fb_user_vaddr as *mut u32;

    if screen_w == 0 || screen_h == 0 || vram.is_null() {
        log_greeter_msg("[greeter] null framebuffer address\n");
        raw_sys::fb_release();
        return 1;
    }

    // 2. Double-buffer surface in heap
    let mut back_buf = Vec::new();
    if back_buf.try_reserve_exact(screen_w * screen_h).is_err() {
        raw_sys::fb_release();
        return 1;
    }
    back_buf.resize(screen_w * screen_h, 0);

    let kbd_fd = open_keyboard();
    let mut state = GreeterState::new(screen_w, screen_h);

    let present = |buf: &[u32]| {
        for y in 0..screen_h {
            unsafe {
                let src = &buf[y * screen_w..(y + 1) * screen_w];
                let dst = core::slice::from_raw_parts_mut(vram.add(y * pitch), screen_w);
                dst.copy_from_slice(src);
            }
        }
    };

    if let Some(mut p) = Painter::new(&mut back_buf, screen_w, screen_h, screen_w) {
        render_greeter(&mut p, &state, screen_w, screen_h);
        present(&back_buf);
    }

    let mut poll_fds = [PollFd {
        fd: kbd_fd,
        events: POLLIN,
        revents: 0,
    }];

    loop {
        let poll_res = unsafe {
            raw_sys::syscall3(
                raw_sys::SYS_POLL,
                poll_fds.as_mut_ptr() as usize,
                1,
                100, // 100ms timeout
            )
        };

        if poll_res > 0 && (poll_fds[0].revents & POLLIN != 0) {
            let mut kbuf = [0u8; 8 * 8]; // up to 8 key events
            let kn = raw_sys::read(kbd_fd, &mut kbuf);
            if kn > 0 {
                let valid_bytes = (kn as usize) & !7;
                let mut needs_redraw = false;

                for chunk in kbuf[..valid_bytes].chunks_exact(8) {
                    let raw: &[u8; 8] = chunk.try_into().unwrap();
                    let ev = KeyEvent::from_bytes(raw);

                    match state.handle_key(ev) {
                        Action::Redraw => {
                            needs_redraw = true;
                        }
                        Action::AttemptLogin => {
                            let u = state.username.as_str();
                            let p = state.password.as_str();

                            if authenticate(u, p) {
                                state.error = None;
                                state.password.clear();
                                launch_user_session(u);
                                needs_redraw = true;
                            } else {
                                state.error = Some("Authentication failed");
                                state.password.clear();
                                raw_sys::sleep_ms(500);
                                needs_redraw = true;
                            }
                        }
                        Action::Restart => {
                            log_greeter_msg("[greeter] reboot requested by user\n");
                            unsafe {
                                raw_sys::syscall1(raw_sys::SYS_REBOOT, 1); // REBOOT_CMD_RESTART
                            }
                        }
                        Action::Shutdown => {
                            log_greeter_msg("[greeter] poweroff requested by user\n");
                            unsafe {
                                raw_sys::syscall1(raw_sys::SYS_REBOOT, 2); // REBOOT_CMD_POWEROFF
                            }
                        }
                        Action::None => {}
                    }
                }

                if needs_redraw {
                    if let Some(mut p) = Painter::new(&mut back_buf, screen_w, screen_h, screen_w) {
                        render_greeter(&mut p, &state, screen_w, screen_h);
                        present(&back_buf);
                    }
                }
            }
        }
    }
}
