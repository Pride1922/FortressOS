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
    allocator::init_heap();
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
    let mut fds = [sys::PollFd {
        fd: fd as i32,
        events: sys::POLLIN,
        revents: 0,
    }];
    let status = loop {
        let ready = sys::poll(&mut fds, -1);
        if ready == -4 {
            continue;
        } // EINTR
        if ready < 0 || fds[0].revents & (sys::POLLERR | 0x10 | 0x20) != 0 {
            break 1;
        }
        if fds[0].revents & sys::POLLIN == 0 {
            continue;
        }
        let mut packet = [0u8; 4];
        let n = sys::read(fd as i32, &mut packet);
        if n == -4 || n == 0 {
            continue;
        }
        if n != 4 {
            break 1;
        }
        if packet[0] & 2 != 0 {
            break 0;
        }
        cursor.advance(&mut scene, packet[1] as i8, packet[2] as i8);
        windows.mouse(cursor.x, cursor.y, packet[0], &mut scene);
        windows.monitor(
            &mut scene,
            fb.pitch as usize,
            allocator::usage(),
            (cursor.x, cursor.y),
        );
        windows.render(&mut scene);
        cursor.show(&mut scene);
        if !unsafe { scene.present(&fb) } {
            break 1;
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
