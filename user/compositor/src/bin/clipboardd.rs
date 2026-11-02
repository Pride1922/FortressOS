//! `/sbin/clipboardd` & `/bin/clipboard` — FortressOS System Clipboard Daemon & CLI.
//!
//! Supervised by Watchtower (`clipboard.service`, `Type=simple`, `Order=25`).
//! Maintains `/run/clipboard/current` (mode `0666`, 4096-byte cap) with `/tmp/.clipboard`
//! fallback and an in-memory 8-slot history ring for future clipboard history & screenshot
//! integration. Zero heap allocation (`#![no_std]`, static BSS buffers only).
#![no_std]
#![no_main]
#![allow(dead_code)]

#[path = "../panic.rs"]
mod panic;
mod preferences {
    pub const PATH: &str = "/etc/desktop.conf";
}
#[path = "../sys.rs"]
mod sys;

use spin::Mutex;

const CLIPBOARD_MAX: usize = 4096;
const HISTORY_SLOTS: usize = 8;
const CLIPBOARD_DIR: &str = "/run/clipboard";
const CLIPBOARD_CURRENT: &str = "/run/clipboard/current";
const CLIPBOARD_FALLBACK: &str = "/tmp/.clipboard";

struct DaemonHistory {
    slots: [[u8; CLIPBOARD_MAX]; HISTORY_SLOTS],
    lens: [usize; HISTORY_SLOTS],
    count: usize,
}

impl DaemonHistory {
    const fn new() -> Self {
        Self {
            slots: [[0u8; CLIPBOARD_MAX]; HISTORY_SLOTS],
            lens: [0usize; HISTORY_SLOTS],
            count: 0,
        }
    }

    fn push(&mut self, data: &[u8]) -> bool {
        if data.is_empty() {
            return false;
        }
        let len = data.len().min(CLIPBOARD_MAX);
        if self.count > 0 && self.lens[0] == len && &self.slots[0][..len] == &data[..len] {
            return false;
        }
        let limit = self.count.min(HISTORY_SLOTS - 1);
        for i in (0..limit).rev() {
            let l = self.lens[i];
            let (left, right) = self.slots.split_at_mut(i + 1);
            right[0][..l].copy_from_slice(&left[i][..l]);
            self.lens[i + 1] = l;
        }
        self.slots[0][..len].copy_from_slice(&data[..len]);
        self.lens[0] = len;
        if self.count < HISTORY_SLOTS {
            self.count += 1;
        }
        true
    }
}

static HISTORY: Mutex<DaemonHistory> = Mutex::new(DaemonHistory::new());

core::arch::global_asm!(
    r#"
    .global _start
    .section .text
    _start:
        and rsp, -16
        xor rbp, rbp
        call rust_main
        mov rdi, rax
        xor eax, eax
        syscall
        ud2
    "#
);

fn arg_str(argc: usize, argv: *const *const u8, index: usize) -> Option<&'static str> {
    if index >= argc || argv.is_null() {
        return None;
    }
    let ptr = unsafe { *argv.add(index) };
    if ptr.is_null() {
        return None;
    }
    let mut len = 0;
    while len < CLIPBOARD_MAX && unsafe { *ptr.add(len) } != 0 {
        len += 1;
    }
    let bytes = unsafe { core::slice::from_raw_parts(ptr, len) };
    core::str::from_utf8(bytes).ok()
}

fn sanitize_into(src: &[u8], dst: &mut [u8; CLIPBOARD_MAX]) -> usize {
    let mut out = 0;
    for &b in src.iter().take(CLIPBOARD_MAX) {
        if (0x20..=0x7e).contains(&b) || b == b'\n' || b == b'\t' {
            dst[out] = b;
            out += 1;
        }
    }
    out
}

fn ensure_dir_and_file() {
    let _ = sys::mkdir(CLIPBOARD_DIR, 0o777);
    let _ = sys::chmod(CLIPBOARD_DIR, 0o777);
    let mut probe = [0u8; 4];
    if sys::read_file(CLIPBOARD_CURRENT, &mut probe).is_err() {
        let _ = sys::clipboard_write(CLIPBOARD_CURRENT, b"");
    }
    let _ = sys::chmod(CLIPBOARD_CURRENT, 0o666);
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_main(argc: usize, argv: *const *const u8) -> i32 {
    let _ = sys::ignore_sigpipe();
    ensure_dir_and_file();

    if let Some(cmd) = arg_str(argc, argv, 1) {
        match cmd {
            "--get" | "get" => {
                let mut buf = [0u8; CLIPBOARD_MAX];
                for path in [CLIPBOARD_CURRENT, CLIPBOARD_FALLBACK] {
                    if let Ok(n) = sys::read_file(path, &mut buf) {
                        if n > 0 {
                            let _ = sys::write_all(1, &buf[..n.min(CLIPBOARD_MAX)]);
                            return 0;
                        }
                    }
                }
                return 0;
            }
            "--set" | "set" => {
                let Some(val) = arg_str(argc, argv, 2) else {
                    return 1;
                };
                let mut clean = [0u8; CLIPBOARD_MAX];
                let len = sanitize_into(val.as_bytes(), &mut clean);
                if sys::clipboard_write(CLIPBOARD_CURRENT, &clean[..len]).is_err() {
                    let _ = sys::clipboard_write(CLIPBOARD_FALLBACK, &clean[..len]);
                }
                let _ = sys::chmod(CLIPBOARD_CURRENT, 0o666);
                return 0;
            }
            "--clear" | "clear" => {
                let _ = sys::clipboard_write(CLIPBOARD_CURRENT, b"");
                let _ = sys::chmod(CLIPBOARD_CURRENT, 0o666);
                let _ = sys::unlink(CLIPBOARD_FALLBACK);
                return 0;
            }
            "--status" | "status" => {
                let _ = sys::write_all(1, b"[clipboard] active (/run/clipboard/current, 4096B cap)\n");
                return 0;
            }
            _ => {}
        }
    }

    let _ = sys::write_all(
        1,
        b"[clipboardd] Clipboard service ready (/run/clipboard, 4096B cap, 8-slot history)\n",
    );

    let sleep_interval = sys::Timespec {
        tv_sec: 1,
        tv_nsec: 0,
    };
    let mut raw = [0u8; CLIPBOARD_MAX];
    let mut clean = [0u8; CLIPBOARD_MAX];

    loop {
        ensure_dir_and_file();
        let mut len = sys::read_file(CLIPBOARD_CURRENT, &mut raw).unwrap_or(0);
        if len == 0 {
            if let Ok(fb_len) = sys::read_file(CLIPBOARD_FALLBACK, &mut raw) {
                if fb_len > 0 {
                    len = fb_len;
                    let _ = sys::clipboard_write(CLIPBOARD_CURRENT, &raw[..len.min(CLIPBOARD_MAX)]);
                    let _ = sys::chmod(CLIPBOARD_CURRENT, 0o666);
                }
            }
        }
        if len > 0 {
            let clean_len = sanitize_into(&raw[..len.min(CLIPBOARD_MAX)], &mut clean);
            if clean_len != len {
                let _ = sys::clipboard_write(CLIPBOARD_CURRENT, &clean[..clean_len]);
                let _ = sys::chmod(CLIPBOARD_CURRENT, 0o666);
            }
            let _ = HISTORY.lock().push(&clean[..clean_len]);
        }
        let _ = sys::nanosleep(&sleep_interval, None);
    }
}
