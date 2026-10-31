//! /bin/terminal: the desktop terminal as its own process. Started by the
//! compositor (protocol: proto.rs) as the session user, with no display,
//! input device or terminal of its own. It creates a PTY, runs /bin/sh in a
//! new session on it and parses the output, which is untrusted.
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
#[path = "../terminal.rs"]
mod terminal;
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

use app::{App, Event, Wait};
use terminal::Terminal;

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
const EAGAIN: isize = -21;

fn winsize(cols: usize, rows: usize) -> sys::WinSize {
    sys::WinSize {
        ws_row: rows as u16,
        ws_col: cols as u16,
        ws_xpixel: (cols * 8) as u16,
        ws_ypixel: (rows * 16) as u16,
    }
}

/// /bin/sh in a new session with the PTY slave as stdin/stdout/stderr and
/// as its controlling terminal. Both PTY ends are close-on-exec: the shell
/// holds only the slave, so closing our master hangs it up (SIGHUP).
fn start_shell(cols: usize, rows: usize) -> Option<i32> {
    let mut pty = [-1i32; 2];
    if sys::pty_pair(&mut pty, sys::VFS_O_NONBLOCK | sys::VFS_O_CLOEXEC as u32) != 0 {
        return None;
    }
    let (master, slave) = (pty[0], pty[1]);
    sys::set_winsize(master, &winsize(cols, rows));
    let dup = |dst| sys::SpawnFdAction {
        action_type: 2,
        dst_fd: dst,
        src_fd: slave,
        ..Default::default()
    };
    let actions = [dup(0), dup(1), dup(2)];
    let argv: [*const u8; 2] = [c"/bin/sh".as_ptr() as *const u8, core::ptr::null()];
    let opts = sys::SpawnOpts {
        size: core::mem::size_of::<sys::SpawnOpts>() as u32,
        version: 2,
        flags: sys::SPAWN_SETSID | sys::SPAWN_STAGED,
        argv: argv.as_ptr() as u64,
        fd_actions: actions.as_ptr() as u64,
        action_count: actions.len() as u32,
        ..Default::default()
    };
    let pid = sys::spawn_ext("/bin/sh", &opts);
    if pid <= 0 {
        sys::close(master);
        sys::close(slave);
        return None;
    }
    // Bind the controlling terminal before the staged shell may run.
    if sys::pty_set_session(master, pid as u64) != 0 {
        sys::group_release(pid as i64, sys::GROUP_CANCEL);
        sys::close(master);
        sys::close(slave);
        return None;
    }
    sys::group_release(pid as i64, sys::GROUP_RELEASE);
    sys::close(slave);
    Some(master)
}

fn resize(app: &mut App, term: &mut Terminal, master: i32) {
    let (cols, rows) = Terminal::grid_for(app.width, app.height);
    sys::set_winsize(master, &winsize(cols, rows));
    term.set_dimensions(cols, rows);
    term.render(&mut app.canvas());
    app.damage_all();
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_main(_argc: usize, _argv: *const *const u8) -> i32 {
    if !allocator::init_heap_bytes(HEAP_BYTES) {
        return 1;
    }
    let Some(mut app) = App::connect(520, 280, "Terminal - /bin/sh") else {
        return 2;
    };
    let (cols, rows) = Terminal::grid_for(app.width, app.height);
    let Some(master) = start_shell(cols, rows) else {
        return 3;
    };
    let mut term = Terminal::new();
    resize(&mut app, &mut term, master);
    let mut buf = [0u8; 4096];
    loop {
        match app.wait(master) {
            Wait::Event(Event::Configure) => resize(&mut app, &mut term, master),
            // Raw bytes, unchanged: Ctrl+C, escape sequences and UTF-8 are
            // the shell's (and the PTY line discipline's) business.
            Wait::Event(Event::Key(b)) => {
                let _ = sys::write(master, &[b]);
            }
            Wait::Event(Event::Click(..)) => {}
            Wait::Event(Event::Close) => break, // closing master hangs up the shell
            Wait::Hangup => break,
            Wait::Ready => {
                // Bounded per wakeup so desktop events stay responsive.
                let mut total = 0;
                let mut eof = false;
                while total < 16 * 1024 {
                    let n = sys::read(master, &mut buf);
                    if n == EAGAIN || n == sys::EINTR {
                        break;
                    }
                    if n <= 0 {
                        eof = total == 0;
                        break;
                    }
                    term.write_bytes(&buf[..n as usize]);
                    total += n as usize;
                }
                if let Some((y, h)) = term.render_dirty(&mut app.canvas()) {
                    app.damage(0, y, app.width, h);
                }
                if eof {
                    break; // shell exited: our exit closes the window
                }
            }
        }
    }
    sys::close(master);
    0
}
