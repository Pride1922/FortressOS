//! Desktop client runtime (`--features client`): `App`, `Event`, `Wait`,
//! raw syscalls, heap allocator setup, and the `app_main!` macro.
use crate::gfx::Rect;
use crate::painter::Painter;
use crate::proto::{self, Msg, MSG_SIZE};

pub use spin::Mutex as SpinMutex;
pub use talc::{ErrOnOom, Span, Talc, Talck};

pub const FD_REQUESTS: i32 = 3;
pub const FD_EVENTS: i32 = 4;
pub const FD_PIXELS: i32 = 5;
pub const PIXELS_VA: usize = 0x70_0000_0000;

pub mod raw_sys {
    pub const SYS_EXIT: usize = 0;
    pub const SYS_WRITE: usize = 1;
    pub const SYS_CLOSE: usize = 3;
    pub const SYS_READ: usize = 4;
    pub const SYS_FSTAT: usize = 78;
    pub const SYS_POLL: usize = 88;
    pub const SYS_MMAP: usize = 99;
    pub const SYS_SHM_MAP: usize = 101;

    pub const EINTR: isize = -22;
    pub const POLLIN: i16 = 0x0001;

    #[repr(C)]
    #[derive(Copy, Clone, Debug, Default)]
    pub struct PollFd {
        pub fd: i32,
        pub events: i16,
        pub revents: i16,
    }

    #[repr(C)]
    #[derive(Copy, Clone, Debug, Default)]
    pub struct StatExtV1 {
        pub size: u32,
        pub version: u32,
        pub dev: u64,
        pub ino: u64,
        pub mode: u32,
        pub nlink: u32,
        pub uid: u32,
        pub gid: u32,
        pub rdev: u64,
        pub file_size: u64,
        pub blksize: u32,
        pub file_type: u32,
        pub blocks: u64,
        pub atime_sec: i64,
        pub atime_nsec: i64,
        pub mtime_sec: i64,
        pub mtime_nsec: i64,
        pub ctime_sec: i64,
        pub ctime_nsec: i64,
        pub btime_sec: i64,
        pub btime_nsec: i64,
        pub attributes: u64,
        pub reserved: [u64; 4],
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

    pub fn close(fd: i32) -> isize {
        unsafe { syscall1(SYS_CLOSE, fd as usize) }
    }

    pub fn mmap(bytes: usize) -> isize {
        unsafe { syscall1(SYS_MMAP, bytes) }
    }

    pub fn shm_map(fd: i32, va: usize, flags: u32) -> isize {
        unsafe { syscall3(SYS_SHM_MAP, fd as usize, va, flags as usize) }
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

    pub fn fstat_size(fd: i32) -> Option<u64> {
        let mut st = StatExtV1 {
            size: core::mem::size_of::<StatExtV1>() as u32,
            version: 1,
            ..Default::default()
        };
        let rc = unsafe { syscall2(SYS_FSTAT, fd as usize, &mut st as *mut StatExtV1 as usize) };
        (rc == 0).then_some(st.file_size)
    }
}

pub fn init_heap_bytes(
    allocator: &Talck<SpinMutex<()>, ErrOnOom>,
    bytes: usize,
) -> bool {
    let arena = raw_sys::mmap(bytes);
    if arena < 0 {
        return false;
    }
    unsafe {
        allocator
            .lock()
            .claim(Span::from_base_size(arena as *mut u8, bytes))
            .is_ok()
    }
}

pub struct Args {
    argc: usize,
    argv: *const *const u8,
}

impl Args {
    pub const fn from_raw(argc: usize, argv: *const *const u8) -> Self {
        Self { argc, argv }
    }

    pub fn len(&self) -> usize {
        self.argc
    }

    pub fn is_empty(&self) -> bool {
        self.argc == 0
    }

    pub fn get(&self, index: usize) -> Option<&'static str> {
        if index >= self.argc || self.argv.is_null() {
            return None;
        }
        let ptr = unsafe { *self.argv.add(index) };
        if ptr.is_null() {
            return None;
        }
        let mut len = 0;
        while len < 256 && unsafe { *ptr.add(len) } != 0 {
            len += 1;
        }
        let bytes = unsafe { core::slice::from_raw_parts(ptr, len) };
        core::str::from_utf8(bytes).ok()
    }

    pub fn first(&self) -> Option<&'static str> {
        self.get(1)
    }
}

pub enum Event {
    /// Window size or buffer geometry configured. Redraw everything.
    Configure,
    /// Key pressed or repeated.
    Key(u8),
    /// Left mouse button clicked at content coordinates (x, y).
    Click(usize, usize),
    /// Mouse dragged at content coordinates (x, y).
    Drag(usize, usize),
    /// Window was closed by user or desktop exit.
    Close,
}

pub enum Wait {
    Event(Event),
    Ready,
    Hangup,
}

pub struct App {
    pixels: *mut u32,
    capacity: usize,
    pub width: usize,
    pub height: usize,
    pub stride: usize,
    pending_keys: [u8; 4],
    pending_pos: u8,
    pending_len: u8,
}

impl App {
    pub fn connect(outer_w: u32, outer_h: u32, title: &str) -> Option<Self> {
        let bytes = raw_sys::fstat_size(FD_PIXELS)? as usize;
        let va = raw_sys::shm_map(FD_PIXELS, PIXELS_VA, 0);
        if va < 0 {
            return None;
        }
        let mut app = Self {
            pixels: va as *mut u32,
            capacity: bytes / 4,
            width: 0,
            height: 0,
            stride: 0,
            pending_keys: [0; 4],
            pending_pos: 0,
            pending_len: 0,
        };
        let mut create = Msg::new(proto::REQ_CREATE, outer_w, outer_h, 0, 0);
        set_text(&mut create, title);
        if !app.send(&create) {
            return None;
        }
        loop {
            match app.next_event() {
                Event::Configure => return Some(app),
                Event::Close => return None,
                _ => {}
            }
        }
    }

    pub fn scale_hint(&self) -> usize {
        1
    }

    fn read_one_event(&mut self) -> Option<Event> {
        if self.pending_pos < self.pending_len {
            let b = self.pending_keys[self.pending_pos as usize];
            self.pending_pos += 1;
            return Some(Event::Key(b));
        }
        let mut bytes = [0u8; MSG_SIZE];
        if !raw_sys::read_exact(FD_EVENTS, &mut bytes) {
            return Some(Event::Close);
        }
        let m = Msg::from_bytes(&bytes);
        match m.kind {
            proto::EV_CONFIGURE => {
                let (w, h, stride) = (m.a as usize, m.b as usize, m.c as usize);
                let fits = stride.checked_mul(h).is_some_and(|n| n <= self.capacity);
                if w == 0 || h == 0 || w > stride || !fits {
                    return Some(Event::Close);
                }
                (self.width, self.height, self.stride) = (w, h, stride);
                Some(Event::Configure)
            }
            proto::EV_KEY => {
                let ev = proto::KeyEvent::from_msg(&m)?;
                if !ev.is_press_or_repeat() {
                    return None;
                }
                if ev.ascii != 0 {
                    return Some(Event::Key(ev.ascii));
                }
                let seq = ev.vt100_sequence();
                if let Some((&first, rest)) = seq.split_first() {
                    self.pending_keys[..rest.len()].copy_from_slice(rest);
                    self.pending_len = rest.len() as u8;
                    self.pending_pos = 0;
                    return Some(Event::Key(first));
                }
                None
            }
            proto::EV_CLICK => {
                if m.c == 1 {
                    Some(Event::Drag(m.a as usize, m.b as usize))
                } else {
                    Some(Event::Click(m.a as usize, m.b as usize))
                }
            }
            proto::EV_CLOSE => Some(Event::Close),
            _ => None,
        }
    }

    pub fn next_event(&mut self) -> Event {
        loop {
            if let Some(ev) = self.read_one_event() {
                return ev;
            }
        }
    }

    pub fn wait(&mut self, extra: i32) -> Wait {
        loop {
            if self.pending_pos < self.pending_len {
                let b = self.pending_keys[self.pending_pos as usize];
                self.pending_pos += 1;
                return Wait::Event(Event::Key(b));
            }
            let mut fds = [
                raw_sys::PollFd {
                    fd: FD_EVENTS,
                    events: raw_sys::POLLIN,
                    revents: 0,
                },
                raw_sys::PollFd {
                    fd: extra,
                    events: raw_sys::POLLIN,
                    revents: 0,
                },
            ];
            let n = raw_sys::poll(&mut fds, -1);
            if n == raw_sys::EINTR {
                continue;
            }
            if n < 0 {
                return Wait::Event(Event::Close);
            }
            if fds[0].revents != 0 {
                if let Some(ev) = self.read_one_event() {
                    return Wait::Event(ev);
                }
            }
            if fds[1].revents & raw_sys::POLLIN != 0 {
                return Wait::Ready;
            }
            if fds[1].revents != 0 {
                return Wait::Hangup;
            }
        }
    }

    pub fn painter(&mut self) -> Painter<'_> {
        let len = self.stride * self.height;
        let pixels = unsafe { core::slice::from_raw_parts_mut(self.pixels, len) };
        Painter::new(pixels, self.width, self.height, self.stride)
            .expect("validated in next_event")
    }

    pub fn present(&self, damage: Option<Rect>) {
        if let Some(d) = damage {
            self.damage(d.x, d.y, d.w, d.h);
        }
    }

    pub fn damage(&self, x: usize, y: usize, w: usize, h: usize) -> bool {
        self.send(&Msg::new(
            proto::REQ_DAMAGE,
            x as u32,
            y as u32,
            w as u32,
            h as u32,
        ))
    }

    pub fn damage_all(&self) -> bool {
        self.damage(0, 0, self.width, self.height)
    }

    pub fn set_title(&self, title: &str) -> bool {
        let mut m = Msg::new(proto::REQ_TITLE, 0, 0, 0, 0);
        set_text(&mut m, title);
        self.send(&m)
    }

    pub fn open_in_editor(&self, path: &str) -> bool {
        let bytes = path.as_bytes();
        if !path.starts_with('/')
            || bytes.len() > proto::OPEN_PATH_MAX
            || !bytes.iter().all(|&c| (0x20..0x7f).contains(&c))
        {
            return false;
        }
        if !self.send(&Msg::new(
            proto::REQ_OPEN_BEGIN,
            bytes.len() as u32,
            0,
            0,
            0,
        )) {
            return false;
        }
        for chunk in bytes.chunks(proto::TEXT_MAX) {
            let mut m = Msg::new(proto::REQ_OPEN_APPEND, 0, 0, 0, 0);
            m.text[..chunk.len()].copy_from_slice(chunk);
            m.len = chunk.len() as u32;
            if !self.send(&m) {
                return false;
            }
        }
        self.send(&Msg::new(proto::REQ_OPEN_COMMIT, 0, 0, 0, 0))
    }

    fn send(&self, m: &Msg) -> bool {
        raw_sys::write_all(FD_REQUESTS, &m.to_bytes())
    }
}

fn set_text(m: &mut Msg, text: &str) {
    let bytes = text.as_bytes();
    let n = bytes.len().min(proto::TEXT_MAX);
    for (i, &b) in bytes[..n].iter().enumerate() {
        m.text[i] = if (0x20..0x7f).contains(&b) { b } else { b'?' };
    }
    if bytes.len() > proto::TEXT_MAX {
        m.text[proto::TEXT_MAX - 3..].copy_from_slice(b"...");
    }
    m.len = n.max(1) as u32;
    if n == 0 {
        m.text[0] = b' ';
    }
}

#[macro_export]
macro_rules! app_main {
    ($entry:ident, heap = $heap_bytes:expr) => {
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

        #[global_allocator]
        static ALLOCATOR: $crate::client::Talck<$crate::client::SpinMutex<()>, $crate::client::ErrOnOom> =
            $crate::client::Talc::new($crate::client::ErrOnOom).lock();

        #[panic_handler]
        fn panic(info: &core::panic::PanicInfo) -> ! {
            let msg = b"\n[FORTRESS APP PANIC] Unrecoverable fault\n";
            $crate::client::raw_sys::write(2, msg);
            if let Some(loc) = info.location() {
                $crate::client::raw_sys::write(2, b"  Location: ");
                $crate::client::raw_sys::write(2, loc.file().as_bytes());
                $crate::client::raw_sys::write(2, b"\n");
            }
            $crate::client::raw_sys::exit(1);
        }

        #[unsafe(no_mangle)]
        pub extern "C" fn rust_main(argc: usize, argv: *const *const u8) -> i32 {
            if !$crate::client::init_heap_bytes(&ALLOCATOR, $heap_bytes) {
                return 1;
            }
            let args = $crate::client::Args::from_raw(argc, argv);
            $entry(args)
        }
    };
    ($entry:ident) => {
        $crate::app_main!($entry, heap = 4 << 20);
    };
}
