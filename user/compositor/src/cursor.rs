use crate::surface::{Rect, Surface};
use alloc::vec::Vec;

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

pub struct Cursor {
    pub x: usize,
    pub y: usize,
    saved: Vec<u32>,
    visible: bool,
}

fn blend(dst: u32, src: u32, alpha: u32) -> u32 {
    let mut out = 0xff000000;
    for shift in [0, 8, 16] {
        out |= ((((src >> shift) & 255) * alpha + ((dst >> shift) & 255) * (255 - alpha) + 127)
            / 255)
            << shift;
    }
    out
}

impl Cursor {
    pub fn new(x: usize, y: usize) -> Option<Self> {
        let mut saved = Vec::new();
        saved.try_reserve_exact(256).ok()?;
        saved.resize(256, 0);
        Some(Self {
            x,
            y,
            saved,
            visible: false,
        })
    }
    fn rect(&self) -> Rect {
        Rect {
            x: self.x,
            y: self.y,
            w: 16,
            h: 16,
        }
    }
    // Restore/draw only RAM, then present both damaged regions together.
    // Scene changes beneath a visible cursor must be bracketed by hide/show.
    pub fn hide(&mut self, s: &mut Surface) {
        if !self.visible {
            return;
        }
        for y in 0..16.min(s.height - self.y) {
            for x in 0..16.min(s.width - self.x) {
                s.pixels[(self.y + y) * s.width + self.x + x] = self.saved[y * 16 + x];
            }
        }
        s.damage(self.rect());
        self.visible = false;
    }
    pub fn show(&mut self, s: &mut Surface) {
        if self.visible {
            return;
        }
        self.x = self.x.min(s.width - 1);
        self.y = self.y.min(s.height - 1);
        for y in 0..16.min(s.height - self.y) {
            for x in 0..16.min(s.width - self.x) {
                let i = (self.y + y) * s.width + self.x + x;
                self.saved[y * 16 + x] = s.pixels[i];
                s.pixels[i] = match ARROW[y][x] {
                    b'#' => 0xff101010,
                    b'o' => 0xffffffff,
                    b'+' => blend(s.pixels[i], 0xff101010, 128),
                    _ => s.pixels[i],
                };
            }
        }
        s.damage(self.rect());
        self.visible = true;
    }
    /// Restore save-under before scene mutation; caller renders then calls show.
    pub fn advance(&mut self, s: &mut Surface, dx: i8, dy: i8) {
        self.hide(s);
        self.x = (self.x as i64 + dx as i64).clamp(0, s.width as i64 - 1) as usize;
        self.y = (self.y as i64 + dy as i64).clamp(0, s.height as i64 - 1) as usize;
    }
    #[cfg(test)]
    pub fn motion(&mut self, s: &mut Surface, dx: i8, dy: i8) {
        self.advance(s, dx, dy);
        self.show(s);
    }
}
