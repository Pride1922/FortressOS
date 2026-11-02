//! Desktop & Watchtower Clipboard Service (`clipboard.service` / `/run/clipboard`).
//!
//! Provides a bounded 4 KiB clipboard with an 8-slot history ring.
//! On FortressOS, clipboard state is persisted in `/run/clipboard/current`
//! (managed by `/sbin/clipboardd` under Watchtower) with `/tmp/.clipboard` fallback,
//! and mirrored in an in-process ring for zero-syscall unit testing on the host.
#![allow(dead_code)]

use alloc::string::String;
use core::cell::UnsafeCell;
use core::sync::atomic::{AtomicBool, Ordering};

pub const CLIPBOARD_MAX: usize = 4096;
pub const CLIPBOARD_HISTORY_MAX: usize = 8;
pub const CLIPBOARD_DIR: &str = "/run/clipboard";
pub const CLIPBOARD_CURRENT: &str = "/run/clipboard/current";
pub const CLIPBOARD_FALLBACK: &str = "/tmp/.clipboard";

struct ClipboardRing {
    slots: [String; CLIPBOARD_HISTORY_MAX],
    count: usize,
}

impl ClipboardRing {
    const fn new() -> Self {
        Self {
            slots: [
                String::new(),
                String::new(),
                String::new(),
                String::new(),
                String::new(),
                String::new(),
                String::new(),
                String::new(),
            ],
            count: 0,
        }
    }

    fn push(&mut self, text: &str) {
        if text.is_empty() {
            return;
        }
        if self.count > 0 && self.slots[0].as_str() == text {
            return;
        }
        let limit = self.count.min(CLIPBOARD_HISTORY_MAX - 1);
        for i in (0..limit).rev() {
            self.slots[i + 1] = self.slots[i].clone();
        }
        let mut s = String::new();
        if s.try_reserve_exact(text.len()).is_ok() {
            s.push_str(text);
            self.slots[0] = s;
            if self.count < CLIPBOARD_HISTORY_MAX {
                self.count += 1;
            }
        }
    }

    fn clear(&mut self) {
        for slot in &mut self.slots {
            slot.clear();
        }
        self.count = 0;
    }
}

struct SpinRing {
    locked: AtomicBool,
    inner: UnsafeCell<ClipboardRing>,
}

unsafe impl Sync for SpinRing {}

impl SpinRing {
    const fn new() -> Self {
        Self {
            locked: AtomicBool::new(false),
            inner: UnsafeCell::new(ClipboardRing::new()),
        }
    }

    fn with<R>(&self, f: impl FnOnce(&mut ClipboardRing) -> R) -> R {
        while self
            .locked
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            core::hint::spin_loop();
        }
        let res = f(unsafe { &mut *self.inner.get() });
        self.locked.store(false, Ordering::Release);
        res
    }
}

static RING: SpinRing = SpinRing::new();

/// Sanitizes clipboard text to bounded printable ASCII + newlines/tabs (<= 4096 bytes).
pub fn sanitize(text: &str) -> String {
    let mut out = String::new();
    let cap = text.len().min(CLIPBOARD_MAX);
    if out.try_reserve_exact(cap).is_err() {
        return out;
    }
    for &b in text.as_bytes().iter().take(CLIPBOARD_MAX) {
        if (0x20..=0x7e).contains(&b) || b == b'\n' || b == b'\t' {
            out.push(b as char);
        }
    }
    out
}

/// Copies `text` into the clipboard and records it in the 8-slot history ring.
pub fn set(text: &str) -> bool {
    let clean = sanitize(text);
    if clean.is_empty() {
        return false;
    }
    RING.with(|r| r.push(&clean));
    #[cfg(not(test))]
    {
        let _ = crate::sys::mkdir(CLIPBOARD_DIR, 0o777);
        let _ = crate::sys::clipboard_write(CLIPBOARD_CURRENT, clean.as_bytes());
        let _ = crate::sys::clipboard_write(CLIPBOARD_FALLBACK, clean.as_bytes());
    }
    true
}

/// Reads the current clipboard text (up to 4096 bytes).
pub fn get() -> Option<String> {
    #[cfg(not(test))]
    {
        let mut buf = [0u8; CLIPBOARD_MAX + 1];
        for path in [CLIPBOARD_CURRENT, CLIPBOARD_FALLBACK] {
            if let Ok(n) = crate::sys::read_file(path, &mut buf) {
                if n > 0 {
                    if let Ok(s) = core::str::from_utf8(&buf[..n.min(CLIPBOARD_MAX)]) {
                        let clean = sanitize(s);
                        if !clean.is_empty() {
                            RING.with(|r| r.push(&clean));
                            return Some(clean);
                        }
                    }
                }
            }
        }
    }
    RING.with(|ring| {
        if ring.count > 0 && !ring.slots[0].is_empty() {
            Some(ring.slots[0].clone())
        } else {
            None
        }
    })
}

/// Clears the clipboard and history ring.
pub fn clear() {
    RING.with(|r| r.clear());
    #[cfg(not(test))]
    {
        let _ = crate::sys::clipboard_write(CLIPBOARD_CURRENT, b"");
        let _ = crate::sys::unlink(CLIPBOARD_FALLBACK);
    }
}

/// Returns the number of entries in the in-memory clipboard history ring.
pub fn history_count() -> usize {
    RING.with(|r| r.count)
}

/// Returns the `index`-th entry (0 = most recent) from the clipboard history ring.
pub fn history_get(index: usize) -> Option<String> {
    RING.with(|ring| {
        if index < ring.count && !ring.slots[index].is_empty() {
            Some(ring.slots[index].clone())
        } else {
            None
        }
    })
}
