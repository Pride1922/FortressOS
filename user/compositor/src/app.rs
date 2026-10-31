//! Client side of the desktop protocol (proto.rs) for Rust apps.
//!
//!     let mut app = App::connect(480, 280, "Title")?;   // waits for CONFIGURE
//!     loop {
//!         match app.next_event() {
//!             Event::Configure => { draw(&mut app.canvas()); app.damage_all(); }
//!             Event::Key(b) => ...,
//!             Event::Click(x, y) => ...,
//!             Event::Close => break,
//!         }
//!     }
use crate::canvas::Canvas;
use crate::proto::{self, MSG_SIZE, Msg};
use crate::sys;

const FD_REQUESTS: i32 = 3;
const FD_EVENTS: i32 = 4;
const FD_PIXELS: i32 = 5;
const PIXELS_VA: usize = 0x70_0000_0000;

pub enum Event {
    /// Size changed (also the first event): redraw everything.
    Configure,
    Key(u8),
    Click(usize, usize),
    /// Window closed or compositor gone: save if needed and exit.
    Close,
}

/// Result of App::wait: a desktop event, or the extra descriptor is ready.
pub enum Wait {
    Event(Event),
    /// `extra` is readable (or at EOF: the next read returns 0).
    Ready,
    /// `extra` hung up or failed.
    Hangup,
}

pub struct App {
    pixels: *mut u32,
    capacity: usize, // pixels in the buffer
    pub width: usize,
    pub height: usize,
    pub stride: usize,
}

impl App {
    /// Maps the window buffer, asks for an `outer_w` x `outer_h` window and
    /// waits for its size. None if not started by the compositor.
    pub fn connect(outer_w: u32, outer_h: u32, title: &str) -> Option<Self> {
        let bytes = sys::fstat_size(FD_PIXELS)? as usize;
        let va = sys::shm_map(FD_PIXELS, PIXELS_VA, 0);
        if va < 0 {
            return None;
        }
        let mut app = Self {
            pixels: va as *mut u32,
            capacity: bytes / 4,
            width: 0,
            height: 0,
            stride: 0,
        };
        let mut create = Msg::new(proto::REQ_CREATE, outer_w, outer_h, 0, 0);
        set_text(&mut create, title);
        if !app.send(&create) {
            return None;
        }
        loop {
            match app.next_event() {
                Event::Configure => return Some(app),
                Event::Close => return None,
                _ => {}
            }
        }
    }

    /// Blocks for the next event.
    pub fn next_event(&mut self) -> Event {
        loop {
            let mut bytes = [0u8; MSG_SIZE];
            if !sys::read_exact(FD_EVENTS, &mut bytes) {
                return Event::Close;
            }
            let m = Msg::from_bytes(&bytes);
            match m.kind {
                proto::EV_CONFIGURE => {
                    let (w, h, stride) = (m.a as usize, m.b as usize, m.c as usize);
                    // Never trust a size beyond the buffer we really mapped.
                    let fits = stride.checked_mul(h).is_some_and(|n| n <= self.capacity);
                    if w == 0 || h == 0 || w > stride || !fits {
                        return Event::Close;
                    }
                    (self.width, self.height, self.stride) = (w, h, stride);
                    return Event::Configure;
                }
                proto::EV_KEY => return Event::Key(m.a as u8),
                proto::EV_CLICK => return Event::Click(m.a as usize, m.b as usize),
                proto::EV_CLOSE => return Event::Close,
                _ => {} // newer compositor: ignore unknown events
            }
        }
    }

    /// Waits for a desktop event or for `extra` (e.g. a PTY master) to have
    /// input. Desktop events win when both are pending, so a close request
    /// is never starved by a busy `extra`.
    pub fn wait(&mut self, extra: i32) -> Wait {
        loop {
            let mut fds = [
                sys::PollFd {
                    fd: FD_EVENTS,
                    events: sys::POLLIN,
                    revents: 0,
                },
                sys::PollFd {
                    fd: extra,
                    events: sys::POLLIN,
                    revents: 0,
                },
            ];
            let n = sys::poll(&mut fds, -1);
            if n == sys::EINTR {
                continue;
            }
            if n < 0 {
                return Wait::Event(Event::Close);
            }
            if fds[0].revents != 0 {
                return Wait::Event(self.next_event());
            }
            if fds[1].revents & sys::POLLIN != 0 {
                return Wait::Ready;
            }
            if fds[1].revents != 0 {
                return Wait::Hangup;
            }
        }
    }

    pub fn canvas(&mut self) -> Canvas<'_> {
        let len = self.stride * self.height;
        let pixels = unsafe { core::slice::from_raw_parts_mut(self.pixels, len) };
        Canvas::new(pixels, self.width, self.height, self.stride).expect("validated in next_event")
    }

    pub fn damage(&self, x: usize, y: usize, w: usize, h: usize) -> bool {
        self.send(&Msg::new(
            proto::REQ_DAMAGE,
            x as u32,
            y as u32,
            w as u32,
            h as u32,
        ))
    }

    pub fn damage_all(&self) -> bool {
        self.damage(0, 0, self.width, self.height)
    }

    pub fn set_title(&self, title: &str) -> bool {
        let mut m = Msg::new(proto::REQ_TITLE, 0, 0, 0, 0);
        set_text(&mut m, title);
        self.send(&m)
    }

    /// Asks the desktop to open `path` (absolute, printable ASCII, at most
    /// proto::OPEN_PATH_MAX bytes) in its editor. The desktop chooses the
    /// program; repeated requests within a second are ignored.
    pub fn open_in_editor(&self, path: &str) -> bool {
        let bytes = path.as_bytes();
        if !path.starts_with('/')
            || bytes.len() > proto::OPEN_PATH_MAX
            || !bytes.iter().all(|&c| (0x20..0x7f).contains(&c))
        {
            return false;
        }
        if !self.send(&Msg::new(
            proto::REQ_OPEN_BEGIN,
            bytes.len() as u32,
            0,
            0,
            0,
        )) {
            return false;
        }
        for chunk in bytes.chunks(proto::TEXT_MAX) {
            let mut m = Msg::new(proto::REQ_OPEN_APPEND, 0, 0, 0, 0);
            m.text[..chunk.len()].copy_from_slice(chunk);
            m.len = chunk.len() as u32;
            if !self.send(&m) {
                return false;
            }
        }
        self.send(&Msg::new(proto::REQ_OPEN_COMMIT, 0, 0, 0, 0))
    }

    fn send(&self, m: &Msg) -> bool {
        sys::write_all(FD_REQUESTS, &m.to_bytes())
    }
}

/// Printable ASCII only (the compositor drops anything else), truncated
/// with "..." to the 40-byte field.
fn set_text(m: &mut Msg, text: &str) {
    let bytes = text.as_bytes();
    let n = bytes.len().min(proto::TEXT_MAX);
    for (i, &b) in bytes[..n].iter().enumerate() {
        m.text[i] = if (0x20..0x7f).contains(&b) { b } else { b'?' };
    }
    if bytes.len() > proto::TEXT_MAX {
        m.text[proto::TEXT_MAX - 3..].copy_from_slice(b"...");
    }
    m.len = n.max(1) as u32;
    if n == 0 {
        m.text[0] = b' ';
    }
}
