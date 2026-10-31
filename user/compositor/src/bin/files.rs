//! /bin/files: the desktop file manager as its own process. Started by the
//! compositor (protocol: proto.rs) as the session user, with no display,
//! input device or terminal; `files [ABSOLUTE_DIR]` starts there (default /).
//! Opening a file asks the desktop to open it in its editor.
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
#[path = "../files.rs"]
mod files;
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
use files::{FileAction, FileManager};

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

fn redraw(app: &mut App, fm: &mut FileManager) {
    fm.ensure_visible(FileManager::rows_for(app.height));
    fm.render(&mut app.canvas());
    app.damage_all();
}

fn title(fm: &FileManager) -> String {
    let mut t = String::from("Files - ");
    t.push_str(&fm.current_path);
    t
}

/// Enter / click on a file: the desktop opens it in its editor.
fn open(app: &App, path: &str) {
    let _ = app.open_in_editor(path);
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_main(argc: usize, argv: *const *const u8) -> i32 {
    if !allocator::init_heap_bytes(HEAP_BYTES) {
        return 1;
    }
    let mut fm = FileManager::new(argument(argc, argv).unwrap_or("/"));
    let Some(mut app) = App::connect(460, 300, &title(&fm)) else {
        return 2;
    };
    redraw(&mut app, &mut fm);
    let mut shown_path = String::from(fm.current_path.as_str());
    let mut escape = 0u8; // ESC [ X arrow sequences
    loop {
        let changed = match app.next_event() {
            Event::Configure => true,
            Event::Close => return 0,
            Event::Click(x, y) => match fm.handle_click(x, y) {
                Some(FileAction::OpenPath(path)) => {
                    open(&app, &path);
                    true
                }
                Some(FileAction::Refresh) => true,
                None => false,
            },
            Event::Key(b) => {
                let key = match escape {
                    0 if b == 0x1b => {
                        escape = 1;
                        None
                    }
                    1 => {
                        escape = if b == b'[' { 2 } else { 0 };
                        None
                    }
                    2 => {
                        escape = 0;
                        match b {
                            b'A' => Some(b'k'),
                            b'B' => Some(b'j'),
                            b'D' => Some(b'u'),
                            b'C' => Some(b'\r'),
                            _ => None,
                        }
                    }
                    _ => Some(b),
                };
                match key {
                    Some(b'\r' | b'\n') => {
                        if let Some(path) = fm.enter_selected() {
                            open(&app, &path);
                        }
                        true
                    }
                    Some(b'u' | b'U' | 0x08 | 127) => {
                        fm.navigate_up();
                        true
                    }
                    Some(b'j' | b'J' | b's' | b'S') => {
                        if fm.selected + 1 < fm.entries.len() {
                            fm.selected += 1;
                        }
                        true
                    }
                    Some(b'k' | b'K' | b'w' | b'W') => {
                        fm.selected = fm.selected.saturating_sub(1);
                        true
                    }
                    _ => false,
                }
            }
        };
        if changed {
            redraw(&mut app, &mut fm);
        }
        if fm.current_path != shown_path {
            shown_path = String::from(fm.current_path.as_str());
            app.set_title(&title(&fm));
        }
    }
}
