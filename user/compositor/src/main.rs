#![no_std]
#![no_main]

extern crate alloc;

mod allocator;
mod panic;
#[allow(dead_code)]
mod sys;

mod cursor;
mod font;
mod surface;
mod window;
use cursor::Cursor;
use surface::Surface;
use window::WindowManager;

// 16-byte stack aligned entry point satisfying System V ABI & FortressOS loader
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

#[unsafe(no_mangle)]
pub extern "C" fn rust_main() -> i32 {
    let acquired = sys::fb_acquire();
    if acquired != 0 {
        sys::write(
            2,
            if acquired == sys::EBUSY {
                b"[compositor] Display is owned by another process\n"
            } else {
                b"[compositor] Display acquisition failed\n"
            },
        );
        return 1;
    }
    if !allocator::init_heap() {
        sys::write(2, b"[COMPOSITOR] [FAIL] Anonymous heap allocation\n");
        sys::fb_release();
        return 1;
    }
    let status = run();
    if sys::fb_release() != 0 {
        return 1;
    }
    status
}

fn run() -> i32 {
    sys::write(1, b"[COMPOSITOR] Phase 4 starting (right click to exit)\n");
    let fb = match sys::get_fb_info(0) {
        Ok(fb) => fb,
        Err(_) => {
            sys::write(2, b"[COMPOSITOR] [FAIL] SYS_FBINFO\n");
            return 1;
        }
    };
    let mut scene = match Surface::new(fb.width as usize, fb.height as usize) {
        Some(s) => s,
        None => {
            sys::write(2, b"[COMPOSITOR] [FAIL] Backbuffer exceeds bounded heap\n");
            return 1;
        }
    };
    scene.fill(window::DESKTOP);
    let mut windows = match WindowManager::demo() {
        Some(w) => w,
        None => {
            sys::write(2, b"[COMPOSITOR] [FAIL] Window allocation\n");
            return 1;
        }
    };
    let mut cursor = match Cursor::new(scene.width / 2, scene.height / 2) {
        Some(c) => c,
        None => {
            sys::write(2, b"[COMPOSITOR] [FAIL] Cursor allocation\n");
            return 1;
        }
    };
    let mut fd = sys::open("/dev/input/mouse", 0);
    if fd < 0 {
        fd = sys::open("/dev/mouse0", 0);
    }
    if fd < 0 {
        sys::write(2, b"[COMPOSITOR] [FAIL] Mouse open (root required)\n");
        return 1;
    }
    // Console writes share VRAM: finish the startup diagnostic before painting.
    sys::write(1, b"[COMPOSITOR] [OK] Window server ready; polling\n");
    let mut ts = sys::Timespec::default();
    if sys::clock_gettime(0, &mut ts) == 0 {
        windows.update_clock(&mut scene, ts.tv_sec);
    }
    windows.monitor(
        &mut scene,
        fb.pitch as usize,
        allocator::usage(),
        (cursor.x, cursor.y),
    );
    windows.render(&mut scene);
    cursor.show(&mut scene);
    if !unsafe { scene.present(&fb) } {
        sys::close(fd as i32);
        return 1;
    }
    let mut fds = [
        sys::PollFd {
            fd: fd as i32,
            events: sys::POLLIN,
            revents: 0,
        },
        sys::PollFd {
            fd: 0,
            events: sys::POLLIN,
            revents: 0,
        },
    ];
    let mut pending_packet: Option<[u8; 4]> = None;
    let mut frame_count: u32 = 0;
    let status = 'event_loop: loop {
        let mut had_mouse = false;
        let mut packet = [0u8; 4];

        if let Some(p) = pending_packet.take() {
            packet = p;
            had_mouse = true;
        } else {
            let ready = sys::poll(&mut fds, -1);
            if ready == sys::EINTR {
                continue;
            }
            if ready < 0 || fds[0].revents & (sys::POLLERR | 0x10 | 0x20) != 0 {
                break 1;
            }

            // Keyboard input from stdin (/dev/tty)
            if fds[1].revents & sys::POLLIN != 0 {
                let mut kbuf = [0u8; 16];
                let kn = sys::read(0, &mut kbuf);
                if kn > 0 {
                    for &b in &kbuf[..kn as usize] {
                        if b == b'q' || b == b'Q' || b == 27 || b == 3 {
                            break 'event_loop 0;
                        } else if b == b'\t' {
                            windows.cycle_focus(&mut scene);
                        }
                    }
                }
            }

            if fds[0].revents & sys::POLLIN != 0 {
                let n = sys::read(fd as i32, &mut packet);
                if n == 4 {
                    had_mouse = true;
                } else if n != sys::EINTR && n != 0 {
                    break 1;
                }
            }
        }

        if had_mouse {
            if packet[0] & 2 != 0 {
                break 0;
            }

            let buttons = packet[0];
            let mut dx_total: i32 = packet[1] as i8 as i32;
            let mut dy_total: i32 = packet[2] as i8 as i32;

            // Drain pending motion packets from kernel FIFO to coalesce rapid movement
            let mut extra = [0u8; 4];
            while sys::read(fd as i32, &mut extra) == 4 {
                if extra[0] & 2 != 0 {
                    pending_packet = Some(extra);
                    break;
                }
                if extra[0] != buttons {
                    pending_packet = Some(extra);
                    break;
                }
                dx_total += extra[1] as i8 as i32;
                dy_total += extra[2] as i8 as i32;
            }

            cursor.advance(&mut scene, dx_total, dy_total);
            windows.mouse(cursor.x, cursor.y, buttons, &mut scene);
            if frame_count % 8 == 0 {
                windows.monitor_mouse(&mut scene, (cursor.x, cursor.y));
            }
        }

        if frame_count % 8 == 0 {
            if sys::clock_gettime(0, &mut ts) == 0 {
                windows.update_clock(&mut scene, ts.tv_sec);
            }
        }

        windows.render(&mut scene);
        cursor.show(&mut scene);
        if !unsafe { scene.present(&fb) } {
            break 1;
        }
        frame_count = frame_count.wrapping_add(1);

        // Frame pacing / micro-sleep: space out blits to prevent tearing and display controller bus stuttering
        if pending_packet.is_none() {
            let p_ts = sys::Timespec {
                tv_sec: 0,
                tv_nsec: 1_000_000,
            };
            sys::nanosleep(&p_ts, None);
        }
    };
    cursor.hide(&mut scene);
    unsafe {
        scene.present(&fb);
    }
    sys::close(fd as i32);
    sys::write(1, b"[COMPOSITOR] Event loop stopped\n");
    status
}
