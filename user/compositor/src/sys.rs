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
pub const SYS_SYSINFO: usize = 37;
pub const SYS_NETCTL: usize = 42;
pub const SYS_WIFI: usize = 87;
pub const SYS_PIPE: usize = 24;
pub const SYS_GETRESUID: usize = 62;
pub const SYS_GETRESGID: usize = 63;
pub const SYS_SETRESUID: usize = 66;
pub const SYS_SETRESGID: usize = 67;
pub const SYS_SETGROUPS: usize = 68;
pub const SYS_CAPGET: usize = 70;
pub const VFS_O_CLOEXEC: i32 = 0x80000;

#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct NetworkInfo {
    pub version: u32,
    pub reserved: u32,
    pub mac: [u8; 6],
    pub reserved2: u16,
    pub ip: u32,
    pub netmask: u32,
    pub gateway: u32,
    pub mtu: u32,
    pub link: u32,
    pub reserved3: u32,
    pub rx: u64,
    pub tx: u64,
    pub reserved4: [u32; 2],
}
const _: () = assert!(core::mem::size_of::<NetworkInfo>() == 64);
pub fn network_info() -> Option<NetworkInfo> {
    let mut result = NetworkInfo {
        version: 1,
        ..Default::default()
    };
    (unsafe { syscall3(SYS_NETCTL, 2, &mut result as *mut _ as usize, 64) } == 0).then_some(result)
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct WirelessInfo {
    pub state: u32,
    pub rssi: i8,
    pub channel: u8,
    pub bssid: [u8; 6],
    pub ssid: [u8; 33],
    pub mac: [u8; 6],
    pub online: u16,
    pub reserved: u32,
}
impl Default for WirelessInfo {
    fn default() -> Self {
        Self {
            state: 0,
            rssi: 0,
            channel: 0,
            bssid: [0; 6],
            ssid: [0; 33],
            mac: [0; 6],
            online: 0,
            reserved: 0,
        }
    }
}
const _: () = assert!(core::mem::size_of::<WirelessInfo>() == 60);
pub fn wireless_info() -> Option<WirelessInfo> {
    let mut result = WirelessInfo::default();
    (unsafe { syscall3(SYS_WIFI, 4, &mut result as *mut _ as usize, 60) } == 0
        && result.mac.iter().any(|b| *b != 0))
    .then_some(result)
}
pub fn wireless_connect(ssid: &str, password: &[u8], bssid: [u8; 6]) -> isize {
    if ssid.is_empty()
        || ssid.len() > 32
        || password.len() > 64
        || ssid.as_bytes().contains(&0)
        || password.contains(&0)
    {
        return -1;
    }
    let mut request = [0u8; 110];
    request[..ssid.len()].copy_from_slice(ssid.as_bytes());
    request[33..33 + password.len()].copy_from_slice(password);
    request[98..104].copy_from_slice(&bssid);
    let result = unsafe { syscall3(SYS_WIFI, 2, request.as_ptr() as usize, request.len()) };
    for b in &mut request {
        unsafe {
            core::ptr::write_volatile(b, 0);
        }
    }
    result
}

/// Use the CLI's bounded firmware decoder/bootstrap without credentials or
/// profiles in argv. The caller polls this child instead of blocking painting.
#[cfg(not(test))]
pub fn wireless_prepare() -> isize {
    let argv = [c"/bin/wifi".as_ptr(), c"init".as_ptr(), core::ptr::null()];
    let actions = [0, 1, 2].map(|fd| SpawnFdAction {
        action_type: 1, // OPEN
        dst_fd: fd,
        flags: VFS_O_RDWR,
        path: c"/dev/null".as_ptr() as u64,
        ..Default::default()
    });
    let opts = SpawnOpts {
        size: core::mem::size_of::<SpawnOpts>() as u32,
        version: 2,
        argv: argv.as_ptr() as u64,
        fd_actions: actions.as_ptr() as u64,
        action_count: 3,
        ..Default::default()
    };
    spawn_ext("/bin/wifi", &opts)
}
#[cfg(test)]
pub fn wireless_prepare() -> isize {
    -1
}

pub fn child_status(pid: isize) -> Option<Result<(), isize>> {
    let mut status = 0u64;
    let rc = unsafe { syscall3(33, pid as usize, &mut status as *mut _ as usize, 1) };
    if rc == 0 || rc == -4 {
        None
    } else if rc < 0 {
        Some(Err(rc))
    } else if status == 0 {
        Some(Ok(()))
    } else {
        Some(Err(-5))
    }
}

#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct Calendar {
    pub year: i32,
    pub month: i32,
    pub day: i32,
    pub hour: i32,
    pub minute: i32,
    pub second: i32,
    pub weekday: i32,
}
#[cfg(not(test))]
unsafe extern "C" {
    fn desktop_clock(seconds: i64, local: *mut i64, calendar: *mut Calendar) -> bool;
    fn desktop_clock_invalidate();
    fn desktop_zone_valid(name: *const u8, seconds: i64) -> bool;
}
pub fn local_clock(seconds: i64) -> Option<(i64, Calendar)> {
    #[cfg(not(test))]
    {
        let mut local = 0;
        let mut calendar = Calendar::default();
        return unsafe { desktop_clock(seconds, &mut local, &mut calendar) }
            .then_some((local, calendar));
    }
    #[cfg(test)]
    {
        let _ = seconds;
        None
    }
}
pub fn invalidate_clock() {
    #[cfg(not(test))]
    unsafe {
        desktop_clock_invalidate();
    }
}
pub fn zone_valid(name: &str, seconds: i64) -> bool {
    if name.is_empty() || name.len() > 95 || name.as_bytes().contains(&0) {
        return false;
    }
    let mut buffer = [0u8; 96];
    buffer[..name.len()].copy_from_slice(name.as_bytes());
    #[cfg(not(test))]
    {
        return unsafe { desktop_zone_valid(buffer.as_ptr(), seconds) };
    }
    #[cfg(test)]
    {
        let _ = (buffer, seconds);
        false
    }
}

pub fn read_file(path: &str, buffer: &mut [u8]) -> Result<usize, isize> {
    let fd = open(path, (VFS_O_RDONLY | 0x80000) as i32);
    if fd < 0 {
        return Err(fd);
    }
    let mut used = 0;
    let result = loop {
        if used == buffer.len() {
            break Err(-12);
        }
        let n = read(fd as i32, &mut buffer[used..]);
        if n < 0 {
            break Err(n);
        }
        if n == 0 {
            break Ok(used);
        }
        if n as usize > buffer.len() - used {
            break Err(-9);
        }
        used += n as usize;
    };
    close(fd as i32);
    result
}

fn path_call(nr: usize, first: &str, second: Option<&str>, mode: usize) -> isize {
    if first.len() >= 128 || first.as_bytes().contains(&0) {
        return -1;
    }
    let mut a = [0u8; 128];
    a[..first.len()].copy_from_slice(first.as_bytes());
    let mut b = [0u8; 128];
    if let Some(other) = second {
        if other.len() >= 128 || other.as_bytes().contains(&0) {
            return -1;
        }
        b[..other.len()].copy_from_slice(other.as_bytes());
        unsafe { syscall3(nr, a.as_ptr() as usize, b.as_ptr() as usize, mode) }
    } else {
        unsafe { syscall3(nr, a.as_ptr() as usize, mode, 0) }
    }
}

/// Temp names are exclusive; failure never truncates the previous setting.
pub fn atomic_write(path: &str, temporary: &str, bytes: &[u8]) -> Result<(), isize> {
    let fd = path_call(
        SYS_OPEN,
        temporary,
        None,
        (VFS_O_WRONLY | VFS_O_CREAT | 0x80 | 0x80000) as usize,
    );
    // SYS_OPEN uses path/flags; creation permissions follow the VFS policy.
    if fd < 0 {
        return Err(fd);
    }
    let mut at = 0;
    while at < bytes.len() {
        let n = write(fd as i32, &bytes[at..]);
        if n <= 0 || n as usize > bytes.len() - at {
            close(fd as i32);
            path_call(12, temporary, None, 0);
            return Err(if n < 0 { n } else { -9 });
        }
        at += n as usize;
    }
    let sync = unsafe { syscall1(86, fd as usize) };
    let closed = close(fd as i32);
    // Volatile live /etc has no device durability operation; ext4 does.
    if (sync < 0 && sync != -14) || closed < 0 {
        path_call(12, temporary, None, 0);
        return Err(if closed < 0 { closed } else { sync });
    }
    let mut renamed = path_call(84, temporary, Some(path), 0);
    if renamed == -5 {
        renamed = path_call(13, temporary, Some(path), 0);
    }
    if renamed < 0 {
        path_call(12, temporary, None, 0);
        Err(renamed)
    } else {
        Ok(())
    }
}

/// System files the unprivileged desktop may change, only through the root
/// settings helper (`compositor --settings-broker`) once privileges dropped.
pub const SETTING_PREFERENCES: u8 = 1;
pub const SETTING_TIMEZONE: u8 = 2;
/// Largest request payload; preferences::MAX_BYTES and timezone names fit.
pub const SETTING_MAX: usize = 512;

/// (target, exclusive temporary) for each setting.
pub fn setting_paths(kind: u8) -> Option<(&'static str, &'static str)> {
    match kind {
        SETTING_PREFERENCES => Some((crate::preferences::PATH, "/etc/.desktop.conf.new")),
        SETTING_TIMEZONE => Some(("/etc/timezone", "/etc/.desktop-timezone.new")),
        _ => None,
    }
}

/// Request pipe (write end) and reply pipe (read end) to the settings
/// helper; -1 while the compositor still runs with its launch identity.
static BROKER_FDS: [core::sync::atomic::AtomicI32; 2] = [
    core::sync::atomic::AtomicI32::new(-1),
    core::sync::atomic::AtomicI32::new(-1),
];

pub fn set_settings_broker(request: i32, reply: i32) {
    use core::sync::atomic::Ordering::Release;
    BROKER_FDS[0].store(request, Release);
    BROKER_FDS[1].store(reply, Release);
}

/// Writes one system setting: directly while still privileged (or when no
/// helper exists), otherwise as one request to the helper, which validates
/// and writes it. Errors are negative errno values as before.
pub fn write_setting(kind: u8, bytes: &[u8]) -> Result<(), isize> {
    use core::sync::atomic::Ordering::Acquire;
    let (request, reply) = (BROKER_FDS[0].load(Acquire), BROKER_FDS[1].load(Acquire));
    if request < 0 || reply < 0 {
        let (path, temporary) = setting_paths(kind).ok_or(EINVAL)?;
        return atomic_write(path, temporary, bytes);
    }
    if bytes.len() > SETTING_MAX {
        return Err(EINVAL);
    }
    let mut message = [0u8; 3 + SETTING_MAX];
    message[0] = kind;
    message[1..3].copy_from_slice(&(bytes.len() as u16).to_le_bytes());
    message[3..3 + bytes.len()].copy_from_slice(bytes);
    if !write_all(request, &message[..3 + bytes.len()]) {
        return Err(EPIPE); // helper gone
    }
    let mut status = [0u8; 8];
    if !read_exact(reply, &mut status) {
        return Err(EPIPE);
    }
    match i64::from_le_bytes(status) {
        0 => Ok(()),
        error => Err(error as isize),
    }
}

pub fn write_all(fd: i32, mut bytes: &[u8]) -> bool {
    while !bytes.is_empty() {
        let n = write(fd, bytes);
        if n == EINTR {
            continue;
        }
        if n <= 0 || n as usize > bytes.len() {
            return false;
        }
        bytes = &bytes[n as usize..];
    }
    true
}

/// False on EOF or error before `buffer` is full.
pub fn read_exact(fd: i32, buffer: &mut [u8]) -> bool {
    let mut at = 0;
    while at < buffer.len() {
        let n = read(fd, &mut buffer[at..]);
        if n == EINTR {
            continue;
        }
        if n <= 0 || n as usize > buffer.len() - at {
            return false;
        }
        at += n as usize;
    }
    true
}

pub fn pipe(fds: &mut [i32; 2], flags: i32) -> isize {
    unsafe { syscall2(SYS_PIPE, fds.as_mut_ptr() as usize, flags as usize) }
}

/// (real, effective, saved) user or group ids.
pub fn getres(group: bool) -> Option<(u32, u32, u32)> {
    let (mut r, mut e, mut s) = (u32::MAX, u32::MAX, u32::MAX);
    let nr = if group { SYS_GETRESGID } else { SYS_GETRESUID };
    let rc = unsafe {
        syscall3(
            nr,
            &mut r as *mut u32 as usize,
            &mut e as *mut u32 as usize,
            &mut s as *mut u32 as usize,
        )
    };
    (rc == 0).then_some((r, e, s))
}

/// Sets real, effective and saved ids to `id` (no keep flags).
pub fn setres(group: bool, id: u32) -> isize {
    let nr = if group { SYS_SETRESGID } else { SYS_SETRESUID };
    unsafe { syscall4(nr, id as usize, id as usize, id as usize, 0) }
}

pub fn setgroups(groups: &[u32]) -> isize {
    unsafe { syscall2(SYS_SETGROUPS, groups.len(), groups.as_ptr() as usize) }
}

pub const SYS_KILL: usize = 27;
pub const SYS_SIGACTION: usize = 28;
pub const SYS_SHM_CREATE: usize = 100;
pub const SYS_SHM_MAP: usize = 101;
pub const SYS_SHM_UNMAP: usize = 102;
pub const SHM_MAP_READONLY: u32 = 1;
pub const SIGKILL: usize = 9;
pub const SIGPIPE: usize = 13;

/// Ignore SIGPIPE: a pipe write to a dead peer (app or settings helper)
/// must return EPIPE, not kill the compositor.
pub fn ignore_sigpipe() -> isize {
    let action: [u64; 4] = [1, 0, 0, 0]; // SIG_IGN, mask, flags, reserved
    unsafe { syscall3(SYS_SIGACTION, SIGPIPE, action.as_ptr() as usize, 0) }
}

pub fn kill(pid: isize, signal: usize) -> isize {
    unsafe { syscall2(SYS_KILL, pid as usize, signal) }
}

pub fn shm_create(bytes: usize, flags: i32) -> isize {
    unsafe { syscall2(SYS_SHM_CREATE, bytes, flags as usize) }
}

pub fn shm_map(fd: i32, va: usize, flags: u32) -> isize {
    unsafe { syscall3(SYS_SHM_MAP, fd as usize, va, flags as usize) }
}

pub fn shm_unmap(va: usize) -> isize {
    unsafe { syscall1(SYS_SHM_UNMAP, va) }
}

pub fn capget() -> isize {
    unsafe { syscall0(SYS_CAPGET) }
}

/// Fixed 72-byte public sysinfo_t, src/include/syscall_abi.h.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct SystemInfo {
    pub total_ram_bytes: u64,
    pub free_ram_bytes: u64,
    pub uptime_ticks: u64,
    pub tick_hz: u64,
    pub cpu_count: u32,
    pub task_count: u32,
    pub tsc_hz: u64,
    pub kernel_heap_used: u64,
    pub kernel_heap_total: u64,
    pub thread_count: u32,
    pub reserved: u32,
}
const _: () = assert!(core::mem::size_of::<SystemInfo>() == 72);
const _: () = assert!(core::mem::offset_of!(SystemInfo, cpu_count) == 32);
const _: () = assert!(core::mem::offset_of!(SystemInfo, thread_count) == 64);

pub fn system_info() -> Option<SystemInfo> {
    let mut info = SystemInfo::default();
    if unsafe { syscall1(SYS_SYSINFO, &mut info as *mut _ as usize) } == 0 {
        Some(info)
    } else {
        None
    }
}
pub const EINTR: isize = -22;
pub const EINVAL: isize = -1;
pub const EPIPE: isize = -20;
pub const EPERM: isize = -24;
pub const EBUSY: isize = -36;

// Exact access-mode values from src/fs/vfs.h (not bit flags).
pub const VFS_O_RDONLY: u32 = 0;
pub const VFS_O_WRONLY: u32 = 1;
pub const VFS_O_RDWR: u32 = 2;
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
pub fn monotonic_seconds() -> Option<i64> {
    let mut time = Timespec::default();
    (clock_gettime(1, &mut time) == 0).then_some(time.tv_sec)
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
