//! FortressOS Ring 3 Syscall Interface

pub const SYS_EXIT: usize = 0;
pub const SYS_WRITE: usize = 1;
pub const SYS_OPEN: usize = 2;
pub const SYS_CLOSE: usize = 3;
pub const SYS_READ: usize = 4;
pub const SYS_STAT: usize = 5;
pub const SYS_SPAWN_EXT: usize = 20;
pub const SYS_GROUP_RELEASE: usize = 35;
pub const SYS_TERMATTR: usize = 34;
pub const SYS_STAT_EXT: usize = 57;
pub const SYS_CLOCK_GETTIME: usize = 74;
pub const SYS_NANOSLEEP: usize = 75;
pub const SYS_PTY_PAIR: usize = 80;
pub const SYS_LIST_DIR: usize = 82;
pub const SYS_POLL: usize = 88;
pub const SYS_FBINFO: usize = 96;
pub const SYS_FB_ACQUIRE: usize = 97;
pub const SYS_FB_RELEASE: usize = 98;
pub const SYS_MMAP: usize = 99;
pub const EINTR: isize = -22;
pub const EBUSY: isize = -36;

pub const VFS_O_RDONLY: u32 = 0x0001;
pub const VFS_O_WRONLY: u32 = 0x0002;
pub const VFS_O_RDWR: u32 = 0x0003;
pub const VFS_O_CREAT: u32 = 0x0040;
pub const VFS_O_TRUNC: u32 = 0x0200;
pub const VFS_O_NONBLOCK: u32 = 0x800;
pub const TERM_SET_WINSIZE: usize = 3;
pub const PTY_SET_SESSION: usize = 5;

pub const GROUP_RELEASE: usize = 0;
pub const GROUP_CANCEL: usize = 1;

pub const SPAWN_STAGED: u32 = 2;
pub const SPAWN_SETSID: u32 = 4;

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

#[repr(C)]
#[derive(Copy, Clone, Debug, Default)]
pub struct SpawnFdAction {
    pub action_type: u32,
    pub dst_fd: i32,
    pub src_fd: i32,
    pub flags: u32,
    pub mode: u32,
    pub reserved: u32,
    pub path: u64,
}

#[repr(C)]
#[derive(Copy, Clone, Debug, Default)]
pub struct SpawnOpts {
    pub size: u32,
    pub version: u32,
    pub flags: u32,
    pub reserved0: u32,
    pub argv: u64,
    pub envp: u64,
    pub cwd: u64,
    pub fd_actions: u64,
    pub action_count: u32,
    pub reserved1: u32,
    pub reserved2: u64,
}

#[repr(C)]
#[derive(Copy, Clone, Debug, Default)]
pub struct WinSize {
    pub ws_row: u16,
    pub ws_col: u16,
    pub ws_xpixel: u16,
    pub ws_ypixel: u16,
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

#[inline(always)]
pub unsafe fn syscall4(nr: usize, arg1: usize, arg2: usize, arg3: usize, arg4: usize) -> isize {
    let ret: isize;
    unsafe {
        core::arch::asm!(
            "syscall",
            in("rax") nr,
            in("rdi") arg1,
            in("rsi") arg2,
            in("rdx") arg3,
            in("r10") arg4,
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

pub fn nanosleep(req: &Timespec, rem: Option<&mut Timespec>) -> isize {
    let rem_ptr = match rem {
        Some(r) => r as *mut _ as usize,
        None => 0,
    };
    unsafe { syscall2(SYS_NANOSLEEP, req as *const _ as usize, rem_ptr) }
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

pub fn pty_pair(fds: &mut [i32; 2], flags: u32) -> isize {
    unsafe { syscall2(SYS_PTY_PAIR, fds.as_mut_ptr() as usize, flags as usize) }
}

pub fn set_winsize(fd: i32, ws: &WinSize) -> isize {
    unsafe {
        syscall4(
            SYS_TERMATTR,
            fd as usize,
            TERM_SET_WINSIZE,
            ws as *const _ as usize,
            core::mem::size_of::<WinSize>(),
        )
    }
}

pub fn spawn_ext(path: &str, opts: &SpawnOpts) -> isize {
    let mut null_terminated = [0u8; 128];
    if path.len() >= null_terminated.len() {
        return -1;
    }
    null_terminated[..path.len()].copy_from_slice(path.as_bytes());
    null_terminated[path.len()] = 0;
    unsafe {
        syscall3(
            SYS_SPAWN_EXT,
            null_terminated.as_ptr() as usize,
            opts as *const _ as usize,
            opts.size as usize,
        )
    }
}

pub fn pty_set_session(master_fd: i32, pid: u64) -> isize {
    unsafe {
        syscall4(
            SYS_TERMATTR,
            master_fd as usize,
            PTY_SET_SESSION,
            &pid as *const _ as usize,
            core::mem::size_of::<u64>(),
        )
    }
}

pub fn group_release(pgid: i64, action: usize) -> isize {
    unsafe { syscall2(SYS_GROUP_RELEASE, pgid as usize, action) }
}

#[repr(C)]
#[derive(Copy, Clone, Debug, Default)]
pub struct StatExtV1 {
    pub size: u32,
    pub version: u32,
    pub file_size: u64,
    pub file_type: u32,
    pub mode: u32,
    pub uid: u32,
    pub gid: u32,
    pub mnt_flags: u32,
    pub reserved: u32,
}

pub fn list_dir(path: &str, names: &mut [[u8; 64]], max_entries: usize) -> isize {
    let mut null_path = [0u8; 256];
    if path.len() >= null_path.len() {
        return -1;
    }
    null_path[..path.len()].copy_from_slice(path.as_bytes());
    null_path[path.len()] = 0;
    unsafe {
        syscall3(
            SYS_LIST_DIR,
            null_path.as_ptr() as usize,
            names.as_mut_ptr() as usize,
            max_entries,
        )
    }
}

pub fn stat_ext(path: &str, st: &mut StatExtV1) -> isize {
    let mut null_path = [0u8; 256];
    if path.len() >= null_path.len() {
        return -1;
    }
    null_path[..path.len()].copy_from_slice(path.as_bytes());
    null_path[path.len()] = 0;
    st.size = core::mem::size_of::<StatExtV1>() as u32;
    st.version = 1;
    unsafe {
        syscall4(
            SYS_STAT_EXT,
            null_path.as_ptr() as usize,
            st as *mut _ as usize,
            core::mem::size_of::<StatExtV1>(),
            1,
        )
    }
}
