//! Versioned desktop preferences. Persistent paths are outside package payloads.
use core::fmt::{self, Write};

pub const PATH: &str = "/etc/desktop.conf";
pub const MAX_BYTES: usize = 512;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Preferences {
    /// 0=automatic, 1=native, 2=large. Never an arbitrary allocation multiplier.
    pub scale: u8,
    pub wallpaper: bool,
    pub clock_24h: bool,
}

impl Default for Preferences {
    fn default() -> Self {
        Self {
            scale: 0,
            wallpaper: true,
            clock_24h: true,
        }
    }
}

impl Preferences {
    pub fn parse(bytes: &[u8]) -> Option<Self> {
        if bytes.is_empty() || bytes.len() > MAX_BYTES || bytes.contains(&0) {
            return None;
        }
        let text = core::str::from_utf8(bytes).ok()?;
        let mut result = Self::default();
        let mut seen = 0u8;
        for raw in text.lines() {
            let line = raw.trim();
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let (key, value) = line.split_once('=')?;
            let bit = match key {
                "version" if value == "1" => 1,
                "scale" => {
                    result.scale = match value {
                        "auto" => 0,
                        "native" => 1,
                        "large" => 2,
                        _ => return None,
                    };
                    2
                }
                "wallpaper" => {
                    result.wallpaper = match value {
                        "hello-world" => true,
                        "plain" => false,
                        _ => return None,
                    };
                    4
                }
                "clock" => {
                    result.clock_24h = match value {
                        "24h" => true,
                        "12h" => false,
                        _ => return None,
                    };
                    8
                }
                _ => return None,
            };
            if seen & bit != 0 {
                return None;
            }
            seen |= bit;
        }
        (seen == 15).then_some(result)
    }

    pub fn encode(&self, out: &mut Text<MAX_BYTES>) -> fmt::Result {
        out.clear();
        if self.scale > 2 {
            return Err(fmt::Error);
        }
        write!(
            out,
            "version=1\nscale={}\nwallpaper={}\nclock={}\n",
            ["auto", "native", "large"][self.scale as usize],
            if self.wallpaper {
                "hello-world"
            } else {
                "plain"
            },
            if self.clock_24h { "24h" } else { "12h" }
        )
    }
}

/// Bounded UTF-8 text used by labels and edit controls, without format! heaps.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Text<const N: usize> {
    bytes: [u8; N],
    len: usize,
}
impl<const N: usize> Default for Text<N> {
    fn default() -> Self {
        Self {
            bytes: [0; N],
            len: 0,
        }
    }
}
impl<const N: usize> Text<N> {
    pub fn from_str(value: &str) -> Self {
        let mut result = Self::default();
        let _ = result.write_str(value);
        result
    }
    pub fn as_str(&self) -> &str {
        core::str::from_utf8(&self.bytes[..self.len]).unwrap_or("")
    }
    pub fn as_bytes(&self) -> &[u8] {
        &self.bytes[..self.len]
    }
    pub fn len(&self) -> usize {
        self.len
    }
    pub fn clear(&mut self) {
        for byte in &mut self.bytes {
            unsafe {
                core::ptr::write_volatile(byte, 0);
            }
        }
        self.len = 0;
    }
    pub fn edit(&mut self, b: u8) -> bool {
        match b {
            8 | 127 if self.len > 0 => {
                self.len -= 1;
                self.bytes[self.len] = 0;
                true
            }
            32..=126 if self.len < N => {
                self.bytes[self.len] = b;
                self.len += 1;
                true
            }
            _ => false,
        }
    }
}
impl<const N: usize> Write for Text<N> {
    fn write_str(&mut self, value: &str) -> fmt::Result {
        if value.len() > N - self.len {
            return Err(fmt::Error);
        }
        self.bytes[self.len..self.len + value.len()].copy_from_slice(value.as_bytes());
        self.len += value.len();
        Ok(())
    }
}
