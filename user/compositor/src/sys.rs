//! FortressOS Ring 3 Syscall Interface

pub const SYS_EXIT: usize = 0;
pub const SYS_WRITE: usize = 1;
pub const SYS_OPEN: usize = 2;
pub const SYS_CLOSE: usize = 3;
pub const SYS_READ: usize = 4;
pub const SYS_CLOCK_GETTIME: usize = 74;
pub const SYS_POLL: usize = 88;
pub const SYS_FBINFO: usize = 96;
pub const SYS_FB_ACQUIRE: usize = 97;
pub const SYS_FB_RELEASE: usize = 98;
pub const SYS_MMAP: usize = 99;
pub const EINTR: isize = -22;
pub const EBUSY: isize = -36;

pub const POLLIN: i16 = 0x0001;
pub const POLLOUT: i16 = 0x0004;
pub const POLLERR: i16 = 0x0008;

#[repr(C)]
#[derive(Copy, Clone, Debug, Default)]
pub struct PollFd {
    pub fd: i32,
    pub events: i16,
    pub revents: i16,
}

#[repr(C)]
#[derive(Copy, Clone, Debug, Default)]
pub struct Timespec {
    pub tv_sec: i64,
    pub tv_nsec: i64,
}

#[repr(C)]
#[derive(Copy, Clone, Debug, Default)]
pub struct FbInfoV1 {
    pub version: u32,
    pub width: u32,
    pub height: u32,
    pub pitch: u32,
    pub bpp: u32,
    pub reserved: u32,
    pub fb_user_vaddr: u64,
    pub fb_size: u64,
}

#[inline(always)]
pub unsafe fn syscall0(nr: usize) -> isize {
    let ret: isize;
    unsafe {
        core::arch::asm!(
            "syscall",
            in("rax") nr,
            out("rcx") _,
            out("r11") _,
            lateout("rax") ret,
            options(nostack)
        );
    }
    ret
}

#[inline(always)]
pub unsafe fn syscall1(nr: usize, arg1: usize) -> isize {
    let ret: isize;
    unsafe {
        core::arch::asm!(
            "syscall",
            in("rax") nr,
            in("rdi") arg1,
            out("rcx") _,
            out("r11") _,
            lateout("rax") ret,
            options(nostack)
        );
    }
    ret
}

#[inline(always)]
pub unsafe fn syscall2(nr: usize, arg1: usize, arg2: usize) -> isize {
    let ret: isize;
    unsafe {
        core::arch::asm!(
            "syscall",
            in("rax") nr,
            in("rdi") arg1,
            in("rsi") arg2,
            out("rcx") _,
            out("r11") _,
            lateout("rax") ret,
            options(nostack)
        );
    }
    ret
}

#[inline(always)]
pub unsafe fn syscall3(nr: usize, arg1: usize, arg2: usize, arg3: usize) -> isize {
    let ret: isize;
    unsafe {
        core::arch::asm!(
            "syscall",
            in("rax") nr,
            in("rdi") arg1,
            in("rsi") arg2,
            in("rdx") arg3,
            out("rcx") _,
            out("r11") _,
            lateout("rax") ret,
            options(nostack)
        );
    }
    ret
}

pub fn exit(code: i32) -> ! {
    unsafe {
        syscall1(SYS_EXIT, code as usize);
        loop {
            core::arch::asm!("ud2", options(noreturn));
        }
    }
}

pub fn write(fd: i32, buf: &[u8]) -> isize {
    unsafe { syscall3(SYS_WRITE, fd as usize, buf.as_ptr() as usize, buf.len()) }
}

pub fn read(fd: i32, buf: &mut [u8]) -> isize {
    unsafe { syscall3(SYS_READ, fd as usize, buf.as_mut_ptr() as usize, buf.len()) }
}

pub fn open(path: &str, flags: i32) -> isize {
    let mut null_terminated = [0u8; 128];
    if path.len() >= null_terminated.len() {
        return -1;
    }
    null_terminated[..path.len()].copy_from_slice(path.as_bytes());
    null_terminated[path.len()] = 0;
    unsafe { syscall2(SYS_OPEN, null_terminated.as_ptr() as usize, flags as usize) }
}

pub fn close(fd: i32) -> isize {
    unsafe { syscall1(SYS_CLOSE, fd as usize) }
}

pub fn poll(fds: &mut [PollFd], timeout_ms: i64) -> isize {
    unsafe {
        syscall3(
            SYS_POLL,
            fds.as_mut_ptr() as usize,
            fds.len(),
            timeout_ms as usize,
        )
    }
}

pub fn clock_gettime(clk_id: u32, ts: &mut Timespec) -> isize {
    unsafe { syscall2(SYS_CLOCK_GETTIME, clk_id as usize, ts as *mut _ as usize) }
}

pub fn get_fb_info(req_vaddr: u64) -> Result<FbInfoV1, isize> {
    let mut info = FbInfoV1::default();
    let res = unsafe { syscall2(SYS_FBINFO, &mut info as *mut _ as usize, req_vaddr as usize) };
    if res == 0 { Ok(info) } else { Err(res) }
}

pub fn fb_acquire() -> isize {
    unsafe { syscall0(SYS_FB_ACQUIRE) }
}
pub fn fb_release() -> isize {
    unsafe { syscall0(SYS_FB_RELEASE) }
}
pub fn mmap(bytes: usize) -> isize {
    unsafe { syscall1(SYS_MMAP, bytes) }
}
