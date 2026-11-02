//! Desktop client protocol v1 (mirrored in src/include/desktop_proto.h).
//!
//! The compositor starts each app with:
//!   fd 3  requests  (app writes, compositor reads)
//!   fd 4  events    (compositor writes, app reads)
//!   fd 5  pixels    shared memory, `stride` x screen-height ARGB pixels
//! and /dev/null on 0..2. FortressOS cannot pass descriptors between running
//! processes, so the buffer is created before the app starts and is large
//! enough for any window the app can get (the whole screen).
//!
//! Every message is exactly 64 bytes. The app sends CREATE once, then draws
//! into the buffer at (0,0) with the configured stride and sends DAMAGE for
//! what changed. The compositor copies only damaged pixels, clipped to the
//! window, and never writes into the buffer (it maps it read-only).
#![allow(dead_code)]

pub const MSG_SIZE: usize = 64;
pub const TEXT_MAX: usize = 40;
pub const KEY_EVENT_SIZE: usize = 8;

// App -> compositor.
pub const REQ_CREATE: u32 = 1; // a,b = outer window width/height, text = title
pub const REQ_DAMAGE: u32 = 2; // a,b,c,d = x,y,w,h in content pixels
pub const REQ_TITLE: u32 = 3; // text = title
// Open a file in the desktop's editor. The compositor, not the app, decides
// which program that is (an allowlist of one). Paths exceed one message, so:
pub const REQ_OPEN_BEGIN: u32 = 4; // a = total path length (1..=OPEN_PATH_MAX)
pub const REQ_OPEN_APPEND: u32 = 5; // text = next 1..=40 bytes of the path
pub const REQ_OPEN_COMMIT: u32 = 6; // path complete: open it

// Compositor -> app.
pub const EV_CONFIGURE: u32 = 101; // a,b = content width/height, c = stride (pixels)
// a = ascii byte (0 when non-printable or release; preserved for legacy apps),
// b = Linux keycode, c = modifier bitmask (MOD_*),
// d = state (0 = release, 1 = press, 2 = repeat).
pub const EV_KEY: u32 = 102;
pub const EV_CLOSE: u32 = 103; // window closed: save state and exit
pub const EV_CLICK: u32 = 104; // a,b = x,y in content pixels (left button press)

// Key event state values (mirrors src/include/input_event.h).
pub const KEY_STATE_RELEASE: u8 = 0;
pub const KEY_STATE_PRESS: u8 = 1;
pub const KEY_STATE_REPEAT: u8 = 2;

// Modifier bitmask values (mirrors src/include/input_event.h).
pub const MOD_SHIFT: u16 = 1 << 0;
pub const MOD_CTRL: u16 = 1 << 1;
pub const MOD_ALT: u16 = 1 << 2;
pub const MOD_SUPER: u16 = 1 << 3;
pub const MOD_CAPS: u16 = 1 << 4;
pub const MOD_NUMLOCK: u16 = 1 << 5;
pub const MOD_ALTGR: u16 = 1 << 6;

// Linux input-event-codes keycode constants.
pub const KEY_ESC: u16 = 1;
pub const KEY_1: u16 = 2;
pub const KEY_BACKSPACE: u16 = 14;
pub const KEY_TAB: u16 = 15;
pub const KEY_Q: u16 = 16;
pub const KEY_E: u16 = 18;
pub const KEY_R: u16 = 19;
pub const KEY_T: u16 = 20;
pub const KEY_U: u16 = 22;
pub const KEY_P: u16 = 25;
pub const KEY_ENTER: u16 = 28;
pub const KEY_LEFTCTRL: u16 = 29;
pub const KEY_A: u16 = 30;
pub const KEY_S: u16 = 31;
pub const KEY_D: u16 = 32;
pub const KEY_F: u16 = 33;
pub const KEY_H: u16 = 35;
pub const KEY_J: u16 = 36;
pub const KEY_K: u16 = 37;
pub const KEY_L: u16 = 38;
pub const KEY_LEFTSHIFT: u16 = 42;
pub const KEY_X: u16 = 45;
pub const KEY_C: u16 = 46;
pub const KEY_V: u16 = 47;
pub const KEY_N: u16 = 49;
pub const KEY_M: u16 = 50;
pub const KEY_RIGHTSHIFT: u16 = 54;
pub const KEY_LEFTALT: u16 = 56;
pub const KEY_SPACE: u16 = 57;
pub const KEY_CAPSLOCK: u16 = 58;
pub const KEY_F1: u16 = 59;
pub const KEY_F10: u16 = 68;
pub const KEY_NUMLOCK: u16 = 69;
pub const KEY_F11: u16 = 87;
pub const KEY_F12: u16 = 88;
pub const KEY_KPENTER: u16 = 96;
pub const KEY_RIGHTCTRL: u16 = 97;
pub const KEY_KPSLASH: u16 = 98;
pub const KEY_SYSRQ: u16 = 99;
pub const KEY_RIGHTALT: u16 = 100;
pub const KEY_HOME: u16 = 102;
pub const KEY_UP: u16 = 103;
pub const KEY_PAGEUP: u16 = 104;
pub const KEY_LEFT: u16 = 105;
pub const KEY_RIGHT: u16 = 106;
pub const KEY_END: u16 = 107;
pub const KEY_DOWN: u16 = 108;
pub const KEY_PAGEDOWN: u16 = 109;
pub const KEY_INSERT: u16 = 110;
pub const KEY_DELETE: u16 = 111;
pub const KEY_PAUSE: u16 = 119;
pub const KEY_LEFTMETA: u16 = 125;
pub const KEY_RIGHTMETA: u16 = 126;
pub const KEY_COMPOSE: u16 = 127;

/// 8-byte structured keyboard event read from `/dev/input/keyboard` (`/dev/kbd0`),
/// mirroring `key_event_t` in `src/include/input_event.h`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct KeyEvent {
    pub keycode: u16,
    pub modifiers: u16,
    pub state: u8,
    pub ascii: u8,
    pub reserved: u16,
}
const _: () = assert!(core::mem::size_of::<KeyEvent>() == KEY_EVENT_SIZE);

impl KeyEvent {
    pub const fn new(keycode: u16, modifiers: u16, state: u8, ascii: u8) -> Self {
        Self {
            keycode,
            modifiers,
            state,
            ascii,
            reserved: 0,
        }
    }

    pub fn from_bytes(bytes: &[u8; KEY_EVENT_SIZE]) -> Self {
        Self {
            keycode: u16::from_le_bytes([bytes[0], bytes[1]]),
            modifiers: u16::from_le_bytes([bytes[2], bytes[3]]),
            state: bytes[4],
            ascii: bytes[5],
            reserved: u16::from_le_bytes([bytes[6], bytes[7]]),
        }
    }

    pub fn to_bytes(&self) -> [u8; KEY_EVENT_SIZE] {
        let kc = self.keycode.to_le_bytes();
        let md = self.modifiers.to_le_bytes();
        let rs = self.reserved.to_le_bytes();
        [
            kc[0], kc[1], md[0], md[1], self.state, self.ascii, rs[0], rs[1],
        ]
    }

    /// Synthesizes a press event from a raw ASCII byte (legacy stdin fallback).
    pub const fn from_ascii(ascii: u8) -> Self {
        let (keycode, modifiers) = match ascii {
            27 => (KEY_ESC, 0),
            b'\t' => (KEY_TAB, 0),
            b'\r' | b'\n' => (KEY_ENTER, 0),
            0x08 | 127 => (KEY_BACKSPACE, 0),
            b' ' => (KEY_SPACE, 0),
            3 => (KEY_C, MOD_CTRL),
            18 => (KEY_R, MOD_CTRL),
            19 => (KEY_S, MOD_CTRL),
            22 => (KEY_V, MOD_CTRL),
            24 => (KEY_X, MOD_CTRL),
            b'a' => (KEY_A, 0),
            b'A' => (KEY_A, MOD_SHIFT),
            b'c' => (KEY_C, 0),
            b'C' => (KEY_C, MOD_SHIFT),
            b'd' => (KEY_D, 0),
            b'D' => (KEY_D, MOD_SHIFT),
            b'e' => (KEY_E, 0),
            b'E' => (KEY_E, MOD_SHIFT),
            b'f' => (KEY_F, 0),
            b'F' => (KEY_F, MOD_SHIFT),
            b'h' => (KEY_H, 0),
            b'H' => (KEY_H, MOD_SHIFT),
            b'j' => (KEY_J, 0),
            b'J' => (KEY_J, MOD_SHIFT),
            b'k' => (KEY_K, 0),
            b'K' => (KEY_K, MOD_SHIFT),
            b'l' => (KEY_L, 0),
            b'L' => (KEY_L, MOD_SHIFT),
            b'm' => (KEY_M, 0),
            b'M' => (KEY_M, MOD_SHIFT),
            b'n' => (KEY_N, 0),
            b'N' => (KEY_N, MOD_SHIFT),
            b'p' => (KEY_P, 0),
            b'P' => (KEY_P, MOD_SHIFT),
            b'q' => (KEY_Q, 0),
            b'Q' => (KEY_Q, MOD_SHIFT),
            b'r' => (KEY_R, 0),
            b'R' => (KEY_R, MOD_SHIFT),
            b's' => (KEY_S, 0),
            b'S' => (KEY_S, MOD_SHIFT),
            b't' => (KEY_T, 0),
            b'T' => (KEY_T, MOD_SHIFT),
            b'u' => (KEY_U, 0),
            b'U' => (KEY_U, MOD_SHIFT),
            b'A'..=b'Z' => (0, MOD_SHIFT),
            _ => (0, 0),
        };
        Self {
            keycode,
            modifiers,
            state: KEY_STATE_PRESS,
            ascii,
            reserved: 0,
        }
    }

    /// Encodes this key event as an `EV_KEY` protocol message (`a=ascii, b=keycode, c=modifiers, d=state`).
    pub fn to_msg(&self) -> Msg {
        Msg::new(
            EV_KEY,
            self.ascii as u32,
            self.keycode as u32,
            self.modifiers as u32,
            self.state as u32,
        )
    }

    /// Decodes an `EV_KEY` protocol message into a `KeyEvent`.
    /// Legacy messages with `b == 0 && d == 0 && a != 0` are treated as key presses.
    pub fn from_msg(m: &Msg) -> Option<Self> {
        if m.kind != EV_KEY {
            return None;
        }
        let state = if m.b == 0 && m.d == 0 && m.a != 0 {
            KEY_STATE_PRESS
        } else {
            m.d as u8
        };
        Some(Self {
            keycode: m.b as u16,
            modifiers: m.c as u16,
            state,
            ascii: m.a as u8,
            reserved: 0,
        })
    }

    /// True if this event is a key press or typematic repeat.
    pub const fn is_press_or_repeat(&self) -> bool {
        self.state == KEY_STATE_PRESS || self.state == KEY_STATE_REPEAT
    }

    /// True if this event is the Secure Attention Key (`Ctrl + Alt + Esc` press).
    pub const fn is_sak(&self) -> bool {
        self.state == KEY_STATE_PRESS
            && self.keycode == KEY_ESC
            && (self.modifiers & (MOD_CTRL | MOD_ALT)) == (MOD_CTRL | MOD_ALT)
    }

    /// Synthesizes VT100/ANSI bytes for navigation keys that have `ascii == 0`.
    /// When `MOD_SHIFT` is held, emits lowercase final letters (`a..f`, `h`) so
    /// text editors can extend selection (`Shift + Arrow / Home / End`).
    /// When `MOD_CTRL` is held with `Left` / `Right`, emits `\x1b[Q` / `\x1b[W`
    /// (`\x1b[q` / `\x1b[w` with `MOD_SHIFT`) for word-by-word selection.
    pub const fn vt100_sequence(&self) -> &'static [u8] {
        if !self.is_press_or_repeat() {
            return &[];
        }
        let shifted = (self.modifiers & MOD_SHIFT) != 0;
        let ctrl = (self.modifiers & MOD_CTRL) != 0;
        match self.keycode {
            KEY_UP => if shifted { b"\x1b[a" } else { b"\x1b[A" },
            KEY_DOWN => if shifted { b"\x1b[b" } else { b"\x1b[B" },
            KEY_RIGHT => {
                if ctrl {
                    if shifted { b"\x1b[w" } else { b"\x1b[W" }
                } else if shifted {
                    b"\x1b[c"
                } else {
                    b"\x1b[C"
                }
            }
            KEY_LEFT => {
                if ctrl {
                    if shifted { b"\x1b[q" } else { b"\x1b[Q" }
                } else if shifted {
                    b"\x1b[d"
                } else {
                    b"\x1b[D"
                }
            }
            KEY_HOME => if shifted { b"\x1b[h" } else { b"\x1b[H" },
            KEY_END => if shifted { b"\x1b[f" } else { b"\x1b[F" },
            KEY_INSERT => b"\x1b[2~",
            KEY_DELETE => b"\x1b[3~",
            KEY_PAGEUP => b"\x1b[5~",
            KEY_PAGEDOWN => b"\x1b[6~",
            _ => &[],
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Msg {
    pub kind: u32,
    pub a: u32,
    pub b: u32,
    pub c: u32,
    pub d: u32,
    pub len: u32,
    pub text: [u8; TEXT_MAX],
}
const _: () = assert!(core::mem::size_of::<Msg>() == MSG_SIZE);

impl Default for Msg {
    fn default() -> Self {
        Self {
            kind: 0,
            a: 0,
            b: 0,
            c: 0,
            d: 0,
            len: 0,
            text: [0; TEXT_MAX],
        }
    }
}

impl Msg {
    pub fn new(kind: u32, a: u32, b: u32, c: u32, d: u32) -> Self {
        Self {
            kind,
            a,
            b,
            c,
            d,
            ..Default::default()
        }
    }

    pub fn to_bytes(&self) -> [u8; MSG_SIZE] {
        let mut out = [0u8; MSG_SIZE];
        for (i, v) in [self.kind, self.a, self.b, self.c, self.d, self.len]
            .iter()
            .enumerate()
        {
            out[i * 4..i * 4 + 4].copy_from_slice(&v.to_le_bytes());
        }
        out[24..].copy_from_slice(&self.text);
        out
    }

    pub fn from_bytes(bytes: &[u8; MSG_SIZE]) -> Self {
        let word = |i: usize| u32::from_le_bytes(bytes[i * 4..i * 4 + 4].try_into().unwrap());
        let mut text = [0u8; TEXT_MAX];
        text.copy_from_slice(&bytes[24..]);
        Self {
            kind: word(0),
            a: word(1),
            b: word(2),
            c: word(3),
            d: word(4),
            len: word(5),
            text,
        }
    }

    /// The text field as a title: printable ASCII only, else None.
    pub fn title(&self) -> Option<&str> {
        let len = self.len as usize;
        if len == 0 || len > TEXT_MAX {
            return None;
        }
        let text = &self.text[..len];
        if !text.iter().all(|&c| (0x20..0x7f).contains(&c)) {
            return None;
        }
        core::str::from_utf8(text).ok()
    }
}

/// Clips an app-supplied damage rectangle to the content area. All
/// arithmetic is on untrusted values, so nothing may overflow.
pub fn clip_damage(
    m: &Msg,
    content_w: usize,
    content_h: usize,
) -> Option<(usize, usize, usize, usize)> {
    let (x, y) = (m.a as usize, m.b as usize);
    if x >= content_w || y >= content_h {
        return None;
    }
    let w = (m.c as usize).min(content_w - x);
    let h = (m.d as usize).min(content_h - y);
    (w > 0 && h > 0).then_some((x, y, w, h))
}

pub const OPEN_PATH_MAX: usize = 127;

/// Assembles one REQ_OPEN_* sequence. Every misuse (nested BEGIN, APPEND or
/// COMMIT without BEGIN, overflow, non-printable bytes, an incomplete or
/// relative path) is a protocol violation: the caller drops the app.
pub struct OpenRequest {
    buf: [u8; OPEN_PATH_MAX + 1],
    expected: usize,
    have: usize,
    active: bool,
}

impl Default for OpenRequest {
    fn default() -> Self {
        Self {
            buf: [0; OPEN_PATH_MAX + 1],
            expected: 0,
            have: 0,
            active: false,
        }
    }
}

impl OpenRequest {
    pub fn begin(&mut self, m: &Msg) -> bool {
        let len = m.a as usize;
        if self.active || len == 0 || len > OPEN_PATH_MAX {
            return false;
        }
        *self = Self {
            expected: len,
            active: true,
            ..Default::default()
        };
        true
    }

    pub fn append(&mut self, m: &Msg) -> bool {
        let len = m.len as usize;
        if !self.active || len == 0 || len > TEXT_MAX || len > self.expected - self.have {
            return false;
        }
        let chunk = &m.text[..len];
        if !chunk.iter().all(|&c| (0x20..0x7f).contains(&c)) {
            return false;
        }
        self.buf[self.have..self.have + len].copy_from_slice(chunk);
        self.have += len;
        true
    }

    /// The finished path (NUL-terminated in the buffer), or None.
    pub fn commit(&mut self) -> Option<&core::ffi::CStr> {
        let complete = self.active && self.have == self.expected && self.buf[0] == b'/';
        self.active = false;
        if !complete {
            return None;
        }
        self.buf[self.have] = 0;
        core::ffi::CStr::from_bytes_until_nul(&self.buf).ok()
    }
}
