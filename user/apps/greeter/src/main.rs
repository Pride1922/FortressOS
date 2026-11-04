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
const MOUSE_PATH: &[u8] = b"/dev/input/mouse\0";
const MOUSE_FALLBACK: &[u8] = b"/dev/mouse0\0";
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

const DAYS: [&str; 7] = [
    "Thursday", "Friday", "Saturday", "Sunday", "Monday", "Tuesday", "Wednesday"
];
const MONTHS: [&str; 12] = [
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
];

fn read_timezone_offset() -> i64 {
    let fd = unsafe {
        raw_sys::syscall2(
            raw_sys::SYS_OPEN,
            b"/etc/timezone\0".as_ptr() as usize,
            0, // VFS_O_RDONLY
        )
    };
    if fd < 0 {
        return 0;
    }
    let mut buf = [0u8; 64];
    let n = raw_sys::read(fd as i32, &mut buf);
    raw_sys::close(fd as i32);
    if n <= 0 {
        return 0;
    }
    let len = (n as usize).min(buf.len());
    let tz_str = core::str::from_utf8(&buf[..len]).unwrap_or("").trim_ascii();

    if tz_str.contains("Brussels") || tz_str.contains("Paris") || tz_str.contains("Berlin")
        || tz_str.contains("Amsterdam") || tz_str.contains("Madrid") || tz_str.contains("Rome")
        || tz_str == "CET" {
        return 3600;
    }
    if tz_str.contains("Helsinki") || tz_str.contains("Athens") || tz_str.contains("Kyiv") || tz_str == "EET" {
        return 7200;
    }
    0
}

fn find_primary_user(out: &mut [u8; 32]) -> usize {
    let fd = unsafe {
        raw_sys::syscall2(
            raw_sys::SYS_OPEN,
            b"/etc/passwd\0".as_ptr() as usize,
            0, // VFS_O_RDONLY
        )
    };
    if fd < 0 {
        let def = b"operator";
        out[..def.len()].copy_from_slice(def);
        return def.len();
    }
    let fd_i32 = fd as i32;
    let mut buf = [0u8; 1024];
    let n = raw_sys::read(fd_i32, &mut buf);
    raw_sys::close(fd_i32);
    if n <= 0 {
        let def = b"operator";
        out[..def.len()].copy_from_slice(def);
        return def.len();
    }

    let slice = &buf[..(n as usize).min(buf.len())];
    for line in slice.split(|&b| b == b'\n') {
        if line.is_empty() { continue; }
        let mut parts = line.split(|&b| b == b':');
        let Some(name) = parts.next() else { continue; };
        let Some(_pw) = parts.next() else { continue; };
        let Some(uid_bytes) = parts.next() else { continue; };
        let mut uid = 0u32;
        let mut valid = false;
        for &b in uid_bytes {
            if b >= b'0' && b <= b'9' {
                uid = uid.saturating_mul(10).saturating_add((b - b'0') as u32);
                valid = true;
            } else {
                break;
            }
        }
        if valid && uid >= 1000 && !name.is_empty() {
            let len = name.len().min(out.len());
            out[..len].copy_from_slice(&name[..len]);
            return len;
        }
    }

    let def = b"operator";
    out[..def.len()].copy_from_slice(def);
    def.len()
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
        let tz_offset = read_timezone_offset();
        let local_epoch = (ts.tv_sec as i64 + tz_offset).max(0) as u64;
        if let Some((y, m, d, hh, mm, ss)) = epoch_to_datetime(local_epoch) {
            buf[pos] = b'['; pos += 1;
            append_u32_digits(&mut buf, &mut pos, y, 4);
            buf[pos] = b'-'; pos += 1;
            append_u32_digits(&mut buf, &mut pos, m as u32, 2);
            buf[pos] = b'-'; pos += 1;
            append_u32_digits(&mut buf, &mut pos, d as u32, 2);
            buf[pos] = b' '; pos += 1;
            append_u32_digits(&mut buf, &mut pos, hh as u32, 2);
            buf[pos] = b':'; pos += 1;
            append_u32_digits(&mut buf, &mut pos, mm as u32, 2);
            buf[pos] = b':'; pos += 1;
            append_u32_digits(&mut buf, &mut pos, ss as u32, 2);
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
    focus: usize, // 0 = user, 1 = pass, 2 = login btn, 3 = restart, 4 = shutdown, 5 = console
    error: Option<&'static str>,
    card_rect: Rect,
    user_rect: Rect,
    pass_rect: Rect,
    login_btn_rect: Rect,
    restart_btn_rect: Rect,
    shutdown_btn_rect: Rect,
    console_btn_rect: Rect,
}

impl GreeterState {
    fn new(screen_w: usize, screen_h: usize) -> Self {
        let mut user_buf = [0u8; 32];
        let ulen = find_primary_user(&mut user_buf);
        let user_str = core::str::from_utf8(&user_buf[..ulen]).unwrap_or("operator");
        let mut username = TextField::new();
        username.set_text(user_str);

        let cw = 420.min(screen_w.saturating_sub(40));
        let ch = 340.min(screen_h.saturating_sub(40));
        let cx = (screen_w.saturating_sub(cw)) / 2;
        let cy = (screen_h.saturating_sub(ch)) / 2;

        let card_rect = rect(cx, cy, cw, ch);
        let user_rect = rect(cx + 32, cy + 110, cw - 64, 34);
        let pass_rect = rect(cx + 32, cy + 180, cw - 64, 34);
        let login_btn_rect = rect(cx + 32, cy + 240, cw - 64, 38);

        let btn_w = (cw.saturating_sub(64 + 16)) / 3;
        let restart_btn_rect = rect(cx + 32, cy + 295, btn_w, 28);
        let shutdown_btn_rect = rect(cx + 32 + btn_w + 8, cy + 295, btn_w, 28);
        let console_btn_rect = rect(cx + 32 + (btn_w + 8) * 2, cy + 295, btn_w, 28);

        Self {
            username,
            password: TextField::new(),
            focus: 1, // Start focused on password
            error: None,
            card_rect,
            user_rect,
            pass_rect,
            login_btn_rect,
            restart_btn_rect,
            shutdown_btn_rect,
            console_btn_rect,
        }
    }

    fn handle_key(&mut self, ev: KeyEvent) -> Action {
        if ev.state == 0 {
            return Action::None;
        }

        // F2 key switches directly to console login
        if ev.keycode == 60 { // KEY_F2
            return Action::Console;
        }

        // Tab / Shift+Tab navigation (6 focus items)
        if ev.keycode == 15 { // KEY_TAB
            if ev.modifiers & 1 != 0 { // Shift
                self.focus = (self.focus + 5) % 6;
            } else {
                self.focus = (self.focus + 1) % 6;
            }
            return Action::Redraw;
        }

        // Arrow keys
        if ev.keycode == 103 { // KEY_UP
            self.focus = (self.focus + 5) % 6;
            return Action::Redraw;
        }
        if ev.keycode == 108 { // KEY_DOWN
            self.focus = (self.focus + 1) % 6;
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
                5 => return Action::Console,
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
    Console,
}

// Standard 16x16 pointer arrow bitmap:
// '.' transparent, '#' opaque outline, '+' half-alpha edge, 'o' white fill.
const ARROW: [&[u8; 16]; 16] = [
    b"#+..............",
    b"#o#+............",
    b"#oo#+...........",
    b"#ooo#+..........",
    b"#oooo#+.........",
    b"#ooooo#+........",
    b"#oooooo#+.......",
    b"#ooooooo#+......",
    b"#oooooooo#+.....",
    b"#oooo#####......",
    b"#oo#oo#+........",
    b"#o#+#oo#+.......",
    b"##..#oo#+.......",
    b"+....#oo#+......",
    b".....#oo#+......",
    b"......##+.......",
];

fn blit_cursor(
    vram: *mut u32,
    pitch: usize,
    back_buf: &[u32],
    screen_w: usize,
    screen_h: usize,
    cx: usize,
    cy: usize,
) {
    if vram.is_null() {
        return;
    }
    for y in 0..16.min(screen_h.saturating_sub(cy)) {
        for x in 0..16.min(screen_w.saturating_sub(cx)) {
            let bg = back_buf[(cy + y) * screen_w + (cx + x)];
            let pixel = match ARROW[y][x] {
                b'#' => 0xff101010,
                b'o' => 0xffffffff,
                b'+' => {
                    let r = (((bg >> 16) & 0xff) + 0x10) / 2;
                    let g = (((bg >> 8) & 0xff) + 0x10) / 2;
                    let b = ((bg & 0xff) + 0x10) / 2;
                    0xff000000 | (r << 16) | (g << 8) | b
                }
                _ => bg,
            };
            unsafe {
                *vram.add((cy + y) * pitch + (cx + x)) = pixel;
            }
        }
    }
}

fn restore_cursor_bg(
    vram: *mut u32,
    pitch: usize,
    back_buf: &[u32],
    screen_w: usize,
    screen_h: usize,
    cx: usize,
    cy: usize,
) {
    if vram.is_null() {
        return;
    }
    for y in 0..16.min(screen_h.saturating_sub(cy)) {
        for x in 0..16.min(screen_w.saturating_sub(cx)) {
            let bg = back_buf[(cy + y) * screen_w + (cx + x)];
            unsafe {
                *vram.add((cy + y) * pitch + (cx + x)) = bg;
            }
        }
    }
}

fn open_keyboard() -> i32 {
    let fd = unsafe {
        raw_sys::syscall3(
            raw_sys::SYS_OPEN,
            KBD_PATH.as_ptr() as usize,
            0x80000, // VFS_O_CLOEXEC
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
            0x80000, // VFS_O_CLOEXEC
            0,
        )
    };
    if fd >= 0 {
        fd as i32
    } else {
        0 // fallback to stdin
    }
}

fn open_mouse() -> i32 {
    let fd = unsafe {
        raw_sys::syscall3(
            raw_sys::SYS_OPEN,
            MOUSE_PATH.as_ptr() as usize,
            0x80000, // VFS_O_CLOEXEC
            0,
        )
    };
    if fd >= 0 {
        return fd as i32;
    }
    let fd = unsafe {
        raw_sys::syscall3(
            raw_sys::SYS_OPEN,
            MOUSE_FALLBACK.as_ptr() as usize,
            0x80000, // VFS_O_CLOEXEC
            0,
        )
    };
    if fd >= 0 {
        fd as i32
    } else {
        -1
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

    // Clock and Date Header above Card
    let mut ts = raw_sys::Timespec::default();
    if unsafe { raw_sys::syscall2(raw_sys::SYS_CLOCK_GETTIME, 0, &mut ts as *mut _ as usize) } == 0 {
        let tz_offset = read_timezone_offset();
        let local_epoch = (ts.tv_sec as i64 + tz_offset).max(0) as u64;
        if let Some((_year, month, day, hour, min, _sec)) = epoch_to_datetime(local_epoch) {
            let mut time_str = [0u8; 8];
            time_str[0] = b'0' + (hour / 10);
            time_str[1] = b'0' + (hour % 10);
            time_str[2] = b':';
            time_str[3] = b'0' + (min / 10);
            time_str[4] = b'0' + (min % 10);
            let time_view = core::str::from_utf8(&time_str[..5]).unwrap_or("");

            let days_since_epoch = local_epoch / 86400;
            let dow_idx = ((days_since_epoch + 4) % 7) as usize;
            let dow = DAYS[dow_idx];
            let mon = MONTHS[(month.saturating_sub(1) as usize).min(11)];

            let mut date_buf = [0u8; 48];
            let mut dlen = 0;
            let dbytes = dow.as_bytes();
            date_buf[dlen..dlen + dbytes.len()].copy_from_slice(dbytes);
            dlen += dbytes.len();
            date_buf[dlen..dlen + 2].copy_from_slice(b", ");
            dlen += 2;
            let mbytes = mon.as_bytes();
            date_buf[dlen..dlen + mbytes.len()].copy_from_slice(mbytes);
            dlen += mbytes.len();
            date_buf[dlen] = b' ';
            dlen += 1;
            if day >= 10 {
                date_buf[dlen] = b'0' + (day / 10);
                dlen += 1;
            }
            date_buf[dlen] = b'0' + (day % 10);
            dlen += 1;
            let date_view = core::str::from_utf8(&date_buf[..dlen]).unwrap_or("");

            if cr.y >= 70 {
                let pill_w = 260.min(screen_w.saturating_sub(20));
                let pill_h = 58;
                let pill_x = (screen_w.saturating_sub(pill_w)) / 2;
                let pill_y = cr.y.saturating_sub(pill_h + 16);

                p.sheet_card(rect(pill_x, pill_y, pill_w, pill_h), 0xdd161b22, 0xff30363d, true);

                let time_w = 5 * 10;
                let tx = pill_x + (pill_w.saturating_sub(time_w)) / 2;
                p.label_bold(tx, pill_y + 8, time_w, 20, time_view, 0xffffffff, 0xdd161b22);

                let date_w = dlen * 8;
                let dx = pill_x + (pill_w.saturating_sub(date_w)) / 2;
                p.label_bg(dx, pill_y + 30, date_w, date_view, theme::MUTED, 0xdd161b22);
            }
        }
    }

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

    // 7. Power & Session Buttons
    p.button(state.restart_btn_rect, "Restart", false, state.focus == 3);
    p.button(state.shutdown_btn_rect, "Shut Down", false, state.focus == 4);
    p.button(state.console_btn_rect, "Console", false, state.focus == 5);
}

fn launch_user_session(_username: &str) -> i32 {
    log_greeter_msg("[greeter] clearing console and releasing display lease for user session\n");
    // Clear text console buffer so session startup doesn't flash old boot/shell text
    raw_sys::write(1, b"\x1b[2J\x1b[H");

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
        if status == 3 {
            log_greeter_msg("[greeter] user requested exit to console, releasing display\n");
            return 3;
        }
        log_greeter_msg("[greeter] user session terminated, returning to greeter\n");
    } else {
        log_greeter_msg("[greeter] failed to spawn desktop session\n");
    }

    // Re-acquire framebuffer for greeter when session ends
    raw_sys::fb_acquire();
    0
}

fn execute_action(action: Action, state: &mut GreeterState) -> Option<i32> {
    match action {
        Action::AttemptLogin => {
            let u = state.username.as_str();
            let p = state.password.as_str();

            if authenticate(u, p) {
                state.error = None;
                state.password.clear();
                if launch_user_session(u) == 3 {
                    raw_sys::write(1, b"\x1b[2J\x1b[H");
                    raw_sys::fb_release();
                    return Some(3);
                }
            } else {
                state.error = Some("Authentication failed");
                state.password.clear();
                raw_sys::sleep_ms(500);
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
        Action::Console => {
            log_greeter_msg("[greeter] console mode requested by user\n");
            raw_sys::write(1, b"\x1b[2J\x1b[H");
            raw_sys::fb_release();
            return Some(3);
        }
        Action::None | Action::Redraw => {}
    }
    None
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

    run_greeter(screen_w, screen_h, pitch, vram)
}

#[inline(never)]
fn run_greeter(screen_w: usize, screen_h: usize, pitch: usize, vram: *mut u32) -> i32 {
    // 2. Double-buffer surface in heap
    let mut back_buf = Vec::new();
    if back_buf.try_reserve_exact(screen_w * screen_h).is_err() {
        raw_sys::fb_release();
        return 1;
    }
    back_buf.resize(screen_w * screen_h, 0);

    let kbd_fd = open_keyboard();
    let mouse_fd = open_mouse();
    let mut state = GreeterState::new(screen_w, screen_h);

    let mut cursor_x = screen_w / 2;
    let mut cursor_y = screen_h / 2;
    let mut last_cursor_x = cursor_x;
    let mut last_cursor_y = cursor_y;
    let mut old_buttons = 0u8;

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
        blit_cursor(vram, pitch, &back_buf, screen_w, screen_h, cursor_x, cursor_y);
    }

    let mut poll_fds = [
        PollFd {
            fd: kbd_fd,
            events: POLLIN,
            revents: 0,
        },
        PollFd {
            fd: mouse_fd,
            events: if mouse_fd >= 0 { POLLIN } else { 0 },
            revents: 0,
        },
    ];
    let poll_count = if mouse_fd >= 0 { 2 } else { 1 };

    let mut last_minute = -1i64;

    loop {
        let mut ts = raw_sys::Timespec::default();
        if unsafe { raw_sys::syscall2(raw_sys::SYS_CLOCK_GETTIME, 0, &mut ts as *mut _ as usize) } == 0 {
            let minute = (ts.tv_sec / 60) as i64;
            if minute != last_minute {
                last_minute = minute;
                if let Some(mut p) = Painter::new(&mut back_buf, screen_w, screen_h, screen_w) {
                    render_greeter(&mut p, &state, screen_w, screen_h);
                    present(&back_buf);
                    blit_cursor(vram, pitch, &back_buf, screen_w, screen_h, cursor_x, cursor_y);
                    last_cursor_x = cursor_x;
                    last_cursor_y = cursor_y;
                }
            }
        }

        let poll_res = unsafe {
            raw_sys::syscall3(
                raw_sys::SYS_POLL,
                poll_fds.as_mut_ptr() as usize,
                poll_count,
                50, // 50ms timeout
            )
        };

        let mut needs_redraw = false;

        // 1. Check Keyboard Events
        if poll_res > 0 && (poll_fds[0].revents & POLLIN != 0) {
            let mut kbuf = [0u8; 8 * 8]; // up to 8 key events
            let kn = raw_sys::read(kbd_fd, &mut kbuf);
            if kn > 0 {
                let valid_bytes = (kn as usize) & !7;
                for chunk in kbuf[..valid_bytes].chunks_exact(8) {
                    let raw: &[u8; 8] = chunk.try_into().unwrap();
                    let ev = KeyEvent::from_bytes(raw);
                    let act = state.handle_key(ev);
                    match act {
                        Action::Redraw => {
                            needs_redraw = true;
                        }
                        Action::None => {}
                        _ => {
                            if let Some(code) = execute_action(act, &mut state) {
                                return code;
                            }
                            needs_redraw = true;
                        }
                    }
                }
            }
        }

        // 2. Check Mouse Events
        if poll_res > 0 && mouse_fd >= 0 && (poll_fds[1].revents & POLLIN != 0) {
            let mut packet = [0u8; 4];
            let n = raw_sys::read(mouse_fd, &mut packet);
            if n >= 3 {
                let buttons = packet[0];
                let mut dx_total = packet[1] as i8 as i32;
                let mut dy_total = packet[2] as i8 as i32;

                // Coalesce rapid packets
                let mut extra = [0u8; 4];
                for _ in 0..16 {
                    if raw_sys::read(mouse_fd, &mut extra) < 3 {
                        break;
                    }
                    if extra[0] != buttons {
                        dx_total += extra[1] as i8 as i32;
                        dy_total += extra[2] as i8 as i32;
                        break;
                    }
                    dx_total += extra[1] as i8 as i32;
                    dy_total += extra[2] as i8 as i32;
                }

                cursor_x = (cursor_x as i64 + dx_total as i64).clamp(0, screen_w.saturating_sub(1) as i64) as usize;
                cursor_y = (cursor_y as i64 + dy_total as i64).clamp(0, screen_h.saturating_sub(1) as i64) as usize;

                let mut click_action = Action::None;

                // Left click detection (rising edge)
                if (buttons & 1 != 0) && (old_buttons & 1 == 0) {
                    if state.user_rect.contains(cursor_x, cursor_y) {
                        if state.focus != 0 {
                            state.focus = 0;
                            needs_redraw = true;
                        }
                    } else if state.pass_rect.contains(cursor_x, cursor_y) {
                        if state.focus != 1 {
                            state.focus = 1;
                            needs_redraw = true;
                        }
                    } else if state.login_btn_rect.contains(cursor_x, cursor_y) {
                        state.focus = 2;
                        needs_redraw = true;
                        click_action = Action::AttemptLogin;
                    } else if state.restart_btn_rect.contains(cursor_x, cursor_y) {
                        state.focus = 3;
                        needs_redraw = true;
                        click_action = Action::Restart;
                    } else if state.shutdown_btn_rect.contains(cursor_x, cursor_y) {
                        state.focus = 4;
                        needs_redraw = true;
                        click_action = Action::Shutdown;
                    } else if state.console_btn_rect.contains(cursor_x, cursor_y) {
                        state.focus = 5;
                        needs_redraw = true;
                        click_action = Action::Console;
                    }
                }
                old_buttons = buttons;

                if !matches!(click_action, Action::None) {
                    if let Some(code) = execute_action(click_action, &mut state) {
                        return code;
                    }
                    needs_redraw = true;
                } else if !needs_redraw && (cursor_x != last_cursor_x || cursor_y != last_cursor_y) {
                    restore_cursor_bg(vram, pitch, &back_buf, screen_w, screen_h, last_cursor_x, last_cursor_y);
                    blit_cursor(vram, pitch, &back_buf, screen_w, screen_h, cursor_x, cursor_y);
                    last_cursor_x = cursor_x;
                    last_cursor_y = cursor_y;
                }
            }
        }

        if needs_redraw {
            if let Some(mut p) = Painter::new(&mut back_buf, screen_w, screen_h, screen_w) {
                render_greeter(&mut p, &state, screen_w, screen_h);
                present(&back_buf);
                blit_cursor(vram, pitch, &back_buf, screen_w, screen_h, cursor_x, cursor_y);
                last_cursor_x = cursor_x;
                last_cursor_y = cursor_y;
            }
        }
    }
}
