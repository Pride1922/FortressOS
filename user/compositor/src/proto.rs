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

pub const MSG_SIZE: usize = 64;
pub const TEXT_MAX: usize = 40;

// App -> compositor.
pub const REQ_CREATE: u32 = 1; // a,b = outer window width/height, text = title
pub const REQ_DAMAGE: u32 = 2; // a,b,c,d = x,y,w,h in content pixels
pub const REQ_TITLE: u32 = 3; // text = title

// Compositor -> app.
pub const EV_CONFIGURE: u32 = 101; // a,b = content width/height, c = stride (pixels)
pub const EV_KEY: u32 = 102; // a = byte as typed (UTF-8 bytes arrive one by one)
pub const EV_CLOSE: u32 = 103; // window closed: save state and exit

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
