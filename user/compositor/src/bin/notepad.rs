//! /bin/notepad: the desktop text editor as its own process. Started by the
//! compositor (protocol: proto.rs) as the session user, with no display,
//! input device or terminal; `notepad [ABSOLUTE_PATH]` edits that file
//! (default /tmp/notes.txt).
#![no_std]
#![no_main]
#![allow(dead_code)] // shares modules with the compositor

extern crate alloc;

#[path = "../allocator.rs"]
mod allocator;
#[path = "../app.rs"]
mod app;
#[path = "../canvas.rs"]
mod canvas;
#[path = "../font.rs"]
mod font;
#[path = "../notepad.rs"]
mod notepad;
#[path = "../panic.rs"]
mod panic;
#[path = "../preferences.rs"]
mod preferences;
#[path = "../proto.rs"]
mod proto;
#[path = "../sys.rs"]
mod sys;
#[path = "../theme.rs"]
mod theme;

use alloc::string::String;
use app::{App, Event};
use notepad::Notepad;

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

/// Text plus editing state stays far below this; SYS_MMAP backs it eagerly.
const HEAP_BYTES: usize = 4 << 20;

fn argument(argc: usize, argv: *const *const u8) -> Option<&'static str> {
    if argc < 2 || argv.is_null() {
        return None;
    }
    let ptr = unsafe { *argv.add(1) };
    if ptr.is_null() {
        return None;
    }
    let mut len = 0;
    while len < 128 && unsafe { *ptr.add(len) } != 0 {
        len += 1;
    }
    let bytes = unsafe { core::slice::from_raw_parts(ptr, len) };
    core::str::from_utf8(bytes)
        .ok()
        .filter(|p| p.starts_with('/') && p.len() < 128)
}

fn redraw(app: &mut App, np: &Notepad) {
    np.render(&mut app.canvas());
    app.damage_all();
}

fn title(np: &Notepad) -> String {
    let mut t = String::from(if np.modified {
        "* Notes - "
    } else {
        "Notes - "
    });
    t.push_str(&np.path);
    t
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_main(argc: usize, argv: *const *const u8) -> i32 {
    if !allocator::init_heap_bytes(HEAP_BYTES) {
        return 1;
    }
    let path = argument(argc, argv).unwrap_or("/tmp/notes.txt");
    let mut np = Notepad::new(path);
    let Some(mut app) = App::connect(480, 280, &title(&np)) else {
        return 2;
    };
    redraw(&mut app, &np);
    let mut was_modified = np.modified;
    loop {
        let changed = match app.next_event() {
            Event::Configure => true,
            Event::Key(b) => np.handle_key(b),
            Event::Click(x, y) => np.handle_click(x, y),
            Event::Close => return 0,
        };
        if changed {
            redraw(&mut app, &np);
        }
        if np.modified != was_modified {
            was_modified = np.modified;
            app.set_title(&title(&np));
        }
    }
}
