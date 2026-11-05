//! Out-of-process desktop apps (protocol: proto.rs).
//!
//! Each app is a separate process with the session user's identity, no
//! display, no input device and no terminal: it only gets its two pipes and
//! its own pixel buffer. The compositor maps that buffer read-only and copies
//! damaged pixels into the window's content, so the rest of the window
//! system (occlusion, caching, decorations) treats it like any window.
//! Any protocol violation, an unresponsive event pipe or the app's exit
//! closes its window; a closed window gives the app 2 s to exit, then SIGKILL.
extern crate alloc;
use crate::proto::{self, MSG_SIZE, Msg};
use crate::surface::{Rect, Surface};
use crate::sys;
use crate::window::{Window, WindowManager};
use alloc::string::String;

pub const MAX_CLIENTS: usize = 4;
pub const FIRST_WINDOW_ID: u32 = 100;
const PIXELS_BASE: usize = 0x70_0000_0000;
const PIXELS_SPACING: usize = 0x400_0000; // 64 MiB per slot, buffers are <= 32 MiB
const KILL_GRACE_SECONDS: i64 = 2;

/// NUL-terminated path the `h` shortcut launches (--app=PATH).
static mut APP_PATH: [u8; 128] = default_app_path();

const fn default_app_path() -> [u8; 128] {
    let src = b"/bin/hello-window";
    let mut out = [0u8; 128];
    let mut i = 0;
    while i < src.len() {
        out[i] = src[i];
        i += 1;
    }
    out
}

/// Single-threaded: called during argument parsing only.
pub fn set_app_path(path: &[u8]) -> bool {
    if path.is_empty() || path.len() >= 128 || path.contains(&0) {
        return false;
    }
    unsafe {
        let buf = &mut *core::ptr::addr_of_mut!(APP_PATH);
        buf.fill(0);
        buf[..path.len()].copy_from_slice(path);
    }
    true
}

pub fn app_path() -> &'static core::ffi::CStr {
    let buf = unsafe { &*core::ptr::addr_of!(APP_PATH) };
    core::ffi::CStr::from_bytes_until_nul(buf).unwrap_or(c"/bin/hello-window")
}

struct Client {
    window_id: u32,
    app_id: Option<crate::window::AppId>,
    pid: isize,
    requests: i32,
    events: i32,
    pixels: usize, // read-only mapping of the app's buffer
    stride: usize,
    rows: usize,
    created: bool,
    rx: [u8; MSG_SIZE],
    rx_len: usize,
    open: proto::OpenRequest,
    last_open_ns: Option<u64>,
    held_keys: [u16; 16],
    held_count: usize,
    last_focused: bool,
}

/// A process whose window is gone, waiting to exit (or to be killed).
#[derive(Clone, Copy)]
struct Exiting {
    pid: isize,
    since: i64,
    killed: bool,
}

static mut SESSION_ENVP: *const *const u8 = core::ptr::null();

pub fn set_session_envp(envp: *const *const u8) {
    unsafe { SESSION_ENVP = envp; }
}

pub struct Clients {
    slots: [Option<Client>; MAX_CLIENTS],
    exiting: [Option<Exiting>; 8],
    next_window_id: u32,
    /// A validated REQ_OPEN_COMMIT path, launched after the request batch.
    pending_open: Option<[u8; proto::OPEN_PATH_MAX + 1]>,
    envp: *const *const u8,
}

impl Clients {
    /// Restart is refused while an app or its teardown is still active.
    pub fn restart_ready(&self) -> bool {
        self.slots.iter().all(Option::is_none) && self.exiting.iter().all(Option::is_none)
    }
}

impl Default for Clients {
    fn default() -> Self {
        Self {
            slots: Default::default(),
            exiting: [None; 8],
            next_window_id: FIRST_WINDOW_ID,
            pending_open: None,
            envp: unsafe { SESSION_ENVP },
        }
    }
}

fn now() -> i64 {
    sys::monotonic_seconds().unwrap_or(0)
}

impl Clients {
    pub fn owns(&self, window_id: u32) -> bool {
        self.slots
            .iter()
            .flatten()
            .any(|c| c.window_id == window_id)
    }

    /// Starts `path` (an absolute path) as an app with up to 3 arguments.
    /// Returns its pid.
    pub fn launch(
        &mut self,
        path: &core::ffi::CStr,
        args: &[&core::ffi::CStr],
        scene: &Surface,
    ) -> Result<isize, isize> {
        if args.len() > 3 {
            return Err(sys::EINVAL);
        }
        let slot = self
            .slots
            .iter()
            .position(Option::is_none)
            .ok_or(sys::EBUSY)?;
        if self.exiting.iter().all(Option::is_some) {
            return Err(sys::EBUSY);
        }
        // Whole screen, capped at the 32 MiB object limit (4K panels).
        let stride = scene.width;
        let rows = scene.height.min((32 << 20) / (stride * 4));
        let pixels = PIXELS_BASE + slot * PIXELS_SPACING;
        let shm = sys::shm_create(stride * rows * 4, sys::VFS_O_CLOEXEC);
        if shm < 0 {
            return Err(shm);
        }
        let shm = shm as i32;
        let mapped = sys::shm_map(shm, pixels, sys::SHM_MAP_READONLY);
        let (mut req, mut ev) = ([-1i32; 2], [-1i32; 2]);
        let piped = mapped >= 0
            && sys::pipe(&mut req, sys::VFS_O_CLOEXEC) == 0
            && sys::pipe(&mut ev, sys::VFS_O_CLOEXEC) == 0;
        let pid = if piped {
            spawn(path, args, req[1], ev[0], shm, self.envp)
        } else {
            sys::EINVAL
        };
        // The app holds its own ends and buffer fd; ours stay close-on-exec.
        for fd in [shm, req[1], ev[0]] {
            if fd >= 0 {
                sys::close(fd);
            }
        }
        if pid <= 0 {
            for fd in [req[0], ev[1]] {
                if fd >= 0 {
                    sys::close(fd);
                }
            }
            if mapped >= 0 {
                sys::shm_unmap(pixels);
            }
            return Err(if pid < 0 { pid } else { sys::EINVAL });
        }
        let app_id = crate::window::app_registry::resolve_from_path(path);
        self.slots[slot] = Some(Client {
            window_id: 0,
            app_id,
            pid,
            requests: req[0],
            events: ev[1],
            pixels,
            stride,
            rows,
            created: false,
            rx: [0; MSG_SIZE],
            rx_len: 0,
            open: proto::OpenRequest::default(),
            last_open_ns: None,
            held_keys: [0; 16],
            held_count: 0,
            last_focused: false,
        });
        Ok(pid)
    }

    /// Request pipes to poll, as (slot, fd).
    pub fn poll_set(&self, out: &mut [sys::PollFd; MAX_CLIENTS]) {
        for (slot, entry) in out.iter_mut().enumerate() {
            *entry = sys::PollFd {
                fd: self.slots[slot].as_ref().map_or(-1, |c| c.requests),
                events: sys::POLLIN,
                revents: 0,
            };
        }
    }

    /// Handles readiness of one slot's request pipe.
    pub fn ready(
        &mut self,
        slot: usize,
        revents: i16,
        windows: &mut WindowManager,
        scene: &mut Surface,
    ) {
        if self.slots[slot].is_none() || revents == 0 {
            return;
        }
        let mut alive = revents & sys::POLLIN != 0 || revents & (0x08 | 0x10 | 0x20) == 0;
        if revents & sys::POLLIN != 0 {
            let mut buf = [0u8; 16 * MSG_SIZE];
            let fd = self.slots[slot].as_ref().map_or(-1, |c| c.requests);
            let n = sys::read(fd, &mut buf);
            if n <= 0 {
                alive = false;
            } else {
                for &byte in &buf[..n as usize] {
                    let client = self.slots[slot].as_mut().unwrap();
                    client.rx[client.rx_len] = byte;
                    client.rx_len += 1;
                    if client.rx_len == MSG_SIZE {
                        client.rx_len = 0;
                        let msg = Msg::from_bytes(&client.rx);
                        if !self.handle(slot, &msg, windows, scene) {
                            alive = false;
                            break;
                        }
                    }
                }
            }
        }
        if !alive {
            self.drop_client(slot, windows, scene);
        }
        if let Some(path) = self.pending_open.take() {
            if let Ok(arg) = core::ffi::CStr::from_bytes_until_nul(&path) {
                let editor = installed(c"/usr/bin/notepad", c"/bin/notepad");
                match self.launch(editor, &[arg], scene) {
                    Ok(pid) => {
                        crate::privilege::report(1, "[OK]", "Editor opened for app", Some(pid))
                    }
                    Err(e) => {
                        crate::privilege::report(2, "[WARN]", "Editor launch failed", Some(e))
                    }
                }
            }
        }
    }

    /// One request; false ends the app (violation or failure).
    fn handle(
        &mut self,
        slot: usize,
        msg: &Msg,
        windows: &mut WindowManager,
        scene: &mut Surface,
    ) -> bool {
        let next_id = self.next_window_id;
        let client = self.slots[slot].as_mut().unwrap();
        match msg.kind {
            proto::REQ_CREATE if !client.created => {
                let w = (msg.a as usize).clamp(160, scene.width);
                let h = (msg.b as usize).clamp(120, scene.height.min(client.rows + 26));
                let cascade = slot * 32;
                let rect = Rect {
                    x: (scene.width.saturating_sub(w) / 2 + cascade).min(scene.width - w),
                    y: (scene.height.saturating_sub(h) / 2 + cascade)
                        .min(scene.height - h)
                        .max(24),
                    w,
                    h,
                };
                let Some(mut win) = Window::new(next_id, msg.title().unwrap_or("Application"), rect)
                else {
                    return false;
                };
                if let Some(app) = client.app_id {
                    win.app_id = Some(app);
                }
                client.window_id = next_id;
                client.created = true;
                self.next_window_id = next_id.wrapping_add(1).max(FIRST_WINDOW_ID);
                windows.windows.push(win);
                windows.raise(next_id, scene);
                self.configure(slot, windows)
            }
            proto::REQ_DAMAGE if client.created => {
                let Some(win) = windows
                    .windows
                    .iter_mut()
                    .find(|w| w.id == client.window_id)
                else {
                    return true; // closing; the cleanup pass ends the app
                };
                let cw = win.rect.w - 2;
                let ch = win.content.len() / cw;
                let Some((x, y, w, h)) =
                    proto::clip_damage(msg, cw.min(client.stride), ch.min(client.rows))
                else {
                    return true; // empty after clipping: nothing to do
                };
                for row in y..y + h {
                    // The app may scribble concurrently: these are only pixels,
                    // forced opaque so the window core stays opaque.
                    let src = (client.pixels as *const u32).wrapping_add(row * client.stride + x);
                    let dst = &mut win.content[row * cw + x..row * cw + x + w];
                    for (i, d) in dst.iter_mut().enumerate() {
                        *d = unsafe { core::ptr::read_volatile(src.wrapping_add(i)) } | 0xff00_0000;
                    }
                }
                if !win.is_minimized {
                    scene.damage(Rect {
                        x: win.rect.x + 1 + x,
                        y: win.rect.y + 25 + y,
                        w,
                        h,
                    });
                }
                true
            }
            proto::REQ_OPEN_BEGIN if client.created => client.open.begin(msg),
            proto::REQ_OPEN_APPEND if client.created => client.open.append(msg),
            proto::REQ_OPEN_COMMIT if client.created => {
                let mut path = [0u8; proto::OPEN_PATH_MAX + 1];
                match client.open.commit() {
                    Some(p) => {
                        let bytes = p.to_bytes_with_nul();
                        path[..bytes.len()].copy_from_slice(bytes);
                    }
                    None => return false,
                }
                // At most one launch per app per second (nanosecond clock, so
                // a burst cannot straddle a second boundary); extra requests
                // are ignored, and the app limit bounds the total anyway.
                let t = crate::perf::now_ns();
                if client
                    .last_open_ns
                    .is_some_and(|last| t.saturating_sub(last) < 1_000_000_000)
                {
                    return true;
                }
                client.last_open_ns = Some(t);
                self.pending_open = Some(path);
                true
            }
            proto::REQ_TITLE if client.created => {
                let Some(title) = msg.title() else {
                    return false;
                };
                if let Some(win) = windows
                    .windows
                    .iter_mut()
                    .find(|w| w.id == client.window_id)
                {
                    win.title = String::from(title);
                    scene.damage(Rect {
                        x: win.rect.x,
                        y: win.rect.y,
                        w: win.rect.w,
                        h: 25,
                    });
                }
                true
            }
            _ => false,
        }
    }

    fn send(&mut self, slot: usize, msg: &Msg) -> bool {
        let Some(client) = self.slots[slot].as_ref() else {
            return false;
        };
        // Never block on a stuck or dead app: it must have room for a frame.
        let mut fds = [sys::PollFd {
            fd: client.events,
            events: sys::POLLOUT,
            revents: 0,
        }];
        if sys::poll(&mut fds, 0) != 1 || (fds[0].revents & sys::POLLOUT) == 0 {
            return false;
        }
        sys::write_all(client.events, &msg.to_bytes())
    }

    fn configure(&mut self, slot: usize, windows: &WindowManager) -> bool {
        let Some(client) = self.slots[slot].as_ref() else {
            return false;
        };
        let Some(win) = windows.windows.iter().find(|w| w.id == client.window_id) else {
            return false;
        };
        let cw = (win.rect.w - 2).min(client.stride);
        let ch = (win.content.len() / (win.rect.w - 2)).min(client.rows);
        let msg = Msg::new(
            proto::EV_CONFIGURE,
            cw as u32,
            ch as u32,
            client.stride as u32,
            0,
        );
        self.send(slot, &msg)
    }

    /// Routes a structured key event to the app owning `window_id`. False if not an app window.
    pub fn key(
        &mut self,
        window_id: u32,
        event: proto::KeyEvent,
        windows: &mut WindowManager,
        scene: &mut Surface,
    ) -> bool {
        let Some(slot) = self.slot_of(window_id) else {
            return false;
        };
        let client = self.slots[slot].as_mut().unwrap();
        if event.state == proto::KEY_STATE_PRESS {
            if !client.held_keys[..client.held_count].contains(&event.keycode) {
                if client.held_count < client.held_keys.len() {
                    client.held_keys[client.held_count] = event.keycode;
                    client.held_count += 1;
                }
            }
        } else if event.state == proto::KEY_STATE_RELEASE {
            if let Some(pos) = client.held_keys[..client.held_count]
                .iter()
                .position(|&k| k == event.keycode)
            {
                client.held_count -= 1;
                client.held_keys[pos] = client.held_keys[client.held_count];
                client.held_keys[client.held_count] = 0;
            }
        }
        if !self.send(slot, &event.to_msg()) {
            self.drop_client(slot, windows, scene);
        }
        true
    }

    /// Synchronizes focus state across all clients, notifying them of focus gain/loss
    /// and synthesizing key release events for any held keys when focus leaves a client.
    pub fn sync_focus(
        &mut self,
        focused_window_id: Option<u32>,
        windows: &mut WindowManager,
        scene: &mut Surface,
    ) {
        for slot in 0..MAX_CLIENTS {
            let (is_client, window_id, last_focused, held_count, held_keys) = match self.slots[slot].as_ref() {
                Some(c) if c.created => (true, c.window_id, c.last_focused, c.held_count, c.held_keys),
                _ => (false, 0, false, 0, [0; 16]),
            };
            if !is_client { continue; }
            let is_focused = Some(window_id) == focused_window_id;
            if last_focused != is_focused {
                if let Some(c) = self.slots[slot].as_mut() {
                    c.last_focused = is_focused;
                }
                let msg = Msg::new(proto::EV_FOCUS, if is_focused { 1 } else { 0 }, 0, 0, 0);
                let _ = self.send(slot, &msg);
            }
            if !is_focused && held_count > 0 {
                if let Some(c) = self.slots[slot].as_mut() {
                    c.held_count = 0;
                    c.held_keys.fill(0);
                }
                for &k in &held_keys[..held_count] {
                    let rel_ev = proto::KeyEvent::new(k, 0, proto::KEY_STATE_RELEASE, 0);
                    if !self.send(slot, &rel_ev.to_msg()) {
                        self.drop_client(slot, windows, scene);
                        break;
                    }
                }
            }
        }
    }

    /// A left-button press in an app window's content area.
    pub fn click(
        &mut self,
        window_id: u32,
        x: usize,
        y: usize,
        windows: &mut WindowManager,
        scene: &mut Surface,
    ) -> bool {
        let Some(slot) = self.slot_of(window_id) else {
            return false;
        };
        if !self.send(slot, &Msg::new(proto::EV_CLICK, x as u32, y as u32, 0, 0)) {
            self.drop_client(slot, windows, scene);
        }
        true
    }

    /// A left-button drag motion in an app window's content area (`c = 1`).
    pub fn drag(&mut self, window_id: u32, x: usize, y: usize) -> bool {
        let Some(slot) = self.slot_of(window_id) else {
            return false;
        };
        let _ = self.send(slot, &Msg::new(proto::EV_CLICK, x as u32, y as u32, 1, 0));
        true
    }

    /// After a resize or maximize: the app redraws at the new size.
    pub fn resized(
        &mut self,
        window_id: u32,
        windows: &mut WindowManager,
        scene: &mut Surface,
    ) -> bool {
        let Some(slot) = self.slot_of(window_id) else {
            return false;
        };
        if !self.configure(slot, windows) {
            self.drop_client(slot, windows, scene);
        }
        true
    }

    fn slot_of(&self, window_id: u32) -> Option<usize> {
        self.slots.iter().position(|c| {
            c.as_ref()
                .is_some_and(|c| c.created && c.window_id == window_id)
        })
    }

    /// Ends the app in `slot`: close its window, tell it to exit, release
    /// pipes and buffer, and track the process until it is gone.
    fn drop_client(&mut self, slot: usize, windows: &mut WindowManager, scene: &mut Surface) {
        let _ = self.send(slot, &Msg::new(proto::EV_CLOSE, 0, 0, 0, 0));
        let Some(client) = self.slots[slot].take() else {
            return;
        };
        if client.created {
            windows.close_window(client.window_id, scene);
        }
        sys::close(client.requests);
        sys::close(client.events);
        sys::shm_unmap(client.pixels);
        let entry = Exiting {
            pid: client.pid,
            since: now(),
            killed: false,
        };
        match self.exiting.iter_mut().find(|e| e.is_none()) {
            Some(free) => *free = Some(entry),
            None => {
                sys::kill(client.pid, sys::SIGKILL); // no room to wait politely
            }
        }
    }

    /// Once per loop: end apps whose window was closed with [X], reap exited
    /// processes, and SIGKILL any that ignore EV_CLOSE past the grace period.
    pub fn maintain(&mut self, windows: &mut WindowManager, scene: &mut Surface) {
        let focused_id = windows.focused_window_id();
        self.sync_focus(focused_id, windows, scene);
        for slot in 0..MAX_CLIENTS {
            let gone = self.slots[slot]
                .as_ref()
                .is_some_and(|c| c.created && !windows.windows.iter().any(|w| w.id == c.window_id));
            if gone {
                self.drop_client(slot, windows, scene);
            }
        }
        let t = now();
        for entry in self.exiting.iter_mut() {
            let Some(e) = entry.as_mut() else {
                continue;
            };
            if sys::child_status(e.pid).is_some() {
                *entry = None;
            } else if !e.killed && t - e.since >= KILL_GRACE_SECONDS {
                sys::kill(e.pid, sys::SIGKILL);
                e.killed = true;
            }
        }
    }

    /// On desktop exit: end every app and wait briefly for each.
    pub fn shutdown(&mut self, windows: &mut WindowManager, scene: &mut Surface) {
        for slot in 0..MAX_CLIENTS {
            if self.slots[slot].is_some() {
                self.drop_client(slot, windows, scene);
            }
        }
        for entry in self.exiting.iter_mut().flatten() {
            sys::kill(entry.pid, sys::SIGKILL);
            for _ in 0..50 {
                if sys::child_status(entry.pid).is_some() {
                    break;
                }
                sys::nanosleep(
                    &sys::Timespec {
                        tv_sec: 0,
                        tv_nsec: 10_000_000,
                    },
                    None,
                );
            }
        }
        self.exiting = [None; 8];
    }
}

#[allow(dead_code)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ExternalApp {
    pub name: &'static str,
    pub packaged: &'static core::ffi::CStr,
    pub image: &'static core::ffi::CStr,
}

#[allow(dead_code)]
pub const EXTERNAL_APPS: [ExternalApp; 5] = [
    ExternalApp {
        name: "Terminal",
        packaged: c"/usr/bin/terminal",
        image: c"/bin/terminal",
    },
    ExternalApp {
        name: "Files",
        packaged: c"/usr/bin/files",
        image: c"/bin/files",
    },
    ExternalApp {
        name: "Notepad",
        packaged: c"/usr/bin/notepad",
        image: c"/bin/notepad",
    },
    ExternalApp {
        name: "Calculator",
        packaged: c"/usr/bin/calc",
        image: c"/bin/calc",
    },
    ExternalApp {
        name: "Vault",
        packaged: c"/usr/bin/desktop-vault",
        image: c"/bin/desktop-vault",
    },
];

#[allow(dead_code)]
pub fn get_app(name: &str) -> Option<&'static ExternalApp> {
    EXTERNAL_APPS.iter().find(|app| app.name == name)
}

/// A desktop app's binary: the package-installed copy in /usr/bin when one
/// exists (PKG-1 installs executables there), otherwise the image's /bin.
pub fn installed(
    packaged: &'static core::ffi::CStr,
    image: &'static core::ffi::CStr,
) -> &'static core::ffi::CStr {
    let mut st = sys::StatExtV1::default();
    let regular = packaged
        .to_str()
        .is_ok_and(|p| sys::stat_ext(p, &mut st) == 0 && st.file_type == 1);
    if regular { packaged } else { image }
}

fn spawn(
    path: &core::ffi::CStr,
    args: &[&core::ffi::CStr],
    requests: i32,
    events: i32,
    pixels: i32,
    envp: *const *const u8,
) -> isize {
    let null = c"/dev/null".as_ptr() as u64;
    let open = |fd| sys::SpawnFdAction {
        action_type: 1,
        dst_fd: fd,
        flags: sys::VFS_O_RDWR,
        path: null,
        ..Default::default()
    };
    let dup = |src, dst| sys::SpawnFdAction {
        action_type: 2,
        dst_fd: dst,
        src_fd: src,
        ..Default::default()
    };
    // The sources may themselves be numbered 3..5 in the compositor, so a
    // direct dup2 onto 3..5 could overwrite a source still needed. Stage
    // through the top of the table first (STAGE..STAGE+2 must be free and
    // above every source), then place, then close the staging copies.
    const STAGE: i32 = 29; // MAX_PROCESS_FDS is 32
    if [requests, events, pixels]
        .iter()
        .any(|&fd| fd < 0 || fd >= STAGE)
    {
        return sys::EINVAL;
    }
    let close = |fd| sys::SpawnFdAction {
        action_type: 3,
        dst_fd: fd,
        ..Default::default()
    };
    // No terminal: stdin/stdout/stderr are /dev/null, so an app can neither
    // read the compositor's keyboard nor write to its console.
    let actions = [
        dup(requests, STAGE),
        dup(events, STAGE + 1),
        dup(pixels, STAGE + 2),
        open(0),
        open(1),
        open(2),
        dup(STAGE, 3),
        dup(STAGE + 1, 4),
        dup(STAGE + 2, 5),
        close(STAGE),
        close(STAGE + 1),
        close(STAGE + 2),
    ];
    let mut argv = [core::ptr::null(); 5];
    argv[0] = path.as_ptr();
    for (slot, arg) in argv[1..].iter_mut().zip(args) {
        *slot = arg.as_ptr();
    }
    let opts = sys::SpawnOpts {
        size: core::mem::size_of::<sys::SpawnOpts>() as u32,
        version: 2,
        argv: argv.as_ptr() as u64,
        envp: envp as u64,
        fd_actions: actions.as_ptr() as u64,
        action_count: actions.len() as u32,
        ..Default::default()
    };
    match path.to_str() {
        Ok(p) => sys::spawn_ext(p, &opts),
        Err(_) => sys::EINVAL,
    }
}

#[cfg(test)]
mod restart_tests {
    use super::*;
    #[test]
    fn restart_waits_for_client_teardown() {
        let mut clients=Clients::default();
        assert!(clients.restart_ready());
        clients.slots[0]=Some(Client{window_id:100,app_id:None,pid:42,requests:-1,events:-1,pixels:0,
            stride:0,rows:0,created:false,rx:[0;MSG_SIZE],rx_len:0,
            open:proto::OpenRequest::default(),last_open_ns:None,
            held_keys:[0; 16],held_count:0,last_focused:false});
        assert!(!clients.restart_ready());
        clients.slots[0]=None;
        clients.exiting[0]=Some(Exiting{pid:42,since:0,killed:false});
        assert!(!clients.restart_ready());
    }

    #[test]
    fn focus_loss_synthesizes_held_key_releases() {
        let mut clients = Clients::default();
        let (mut req, mut ev) = ([-1i32; 2], [-1i32; 2]);
        assert_eq!(sys::pipe(&mut req, 0), 0);
        assert_eq!(sys::pipe(&mut ev, 0), 0);
        clients.slots[0] = Some(Client {
            window_id: 100,
            app_id: None,
            pid: 42,
            requests: req[0],
            events: ev[1],
            pixels: 0,
            stride: 0,
            rows: 0,
            created: true,
            rx: [0; MSG_SIZE],
            rx_len: 0,
            open: proto::OpenRequest::default(),
            last_open_ns: None,
            held_keys: [103, 29, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
            held_count: 2,
            last_focused: true,
        });

        let mut windows = WindowManager::desktop();
        let mut scene = Surface::new(800, 600).unwrap();

        clients.sync_focus(Some(101), &mut windows, &mut scene);

        let mut buf = [0u8; MSG_SIZE * 3];
        let n = sys::read(ev[0], &mut buf);
        assert!(n >= (MSG_SIZE * 3) as isize);

        let msg0 = Msg::from_bytes(buf[0..MSG_SIZE].try_into().unwrap());
        assert_eq!(msg0.kind, proto::EV_FOCUS);
        assert_eq!(msg0.a, 0);

        let msg1 = Msg::from_bytes(buf[MSG_SIZE..MSG_SIZE * 2].try_into().unwrap());
        assert_eq!(msg1.kind, proto::EV_KEY);
        assert_eq!(msg1.b, 103);
        assert_eq!(msg1.d, proto::KEY_STATE_RELEASE as u32);

        let msg2 = Msg::from_bytes(buf[MSG_SIZE * 2..MSG_SIZE * 3].try_into().unwrap());
        assert_eq!(msg2.kind, proto::EV_KEY);
        assert_eq!(msg2.b, 29);
        assert_eq!(msg2.d, proto::KEY_STATE_RELEASE as u32);

        assert_eq!(clients.slots[0].as_ref().unwrap().held_count, 0);

        sys::close(req[0]); sys::close(req[1]);
        sys::close(ev[0]); sys::close(ev[1]);
    }
}
