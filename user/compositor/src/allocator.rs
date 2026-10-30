use spin::Mutex;
use talc::{ErrOnOom, Span, Talc, Talck};

// Process-owned anonymous RAM; ELF image remains below the 4 MiB ceiling.
// 1080p scene + maximized clients and reallocations exceed the old 16 MiB
// arena. Stay within the existing SYS_MMAP process-private 32 MiB cap.
const HEAP_SIZE: usize = 32 * 1024 * 1024;

#[global_allocator]
static ALLOCATOR: Talck<Mutex<()>, ErrOnOom> = Talc::new(ErrOnOom).lock();

static CLAIMED: core::sync::atomic::AtomicUsize = core::sync::atomic::AtomicUsize::new(0);

pub fn usage() -> (usize, usize) {
    (
        ALLOCATOR.lock().get_counters().allocated_bytes,
        CLAIMED.load(core::sync::atomic::Ordering::Relaxed),
    )
}

pub fn init_heap() -> bool {
    init_heap_bytes(HEAP_SIZE)
}

/// SYS_MMAP backs every page up front, so apps ask only for what they need.
pub fn init_heap_bytes(bytes: usize) -> bool {
    let arena = crate::sys::mmap(bytes);
    if arena < 0 {
        return false;
    }
    let ok = unsafe {
        ALLOCATOR
            .lock()
            .claim(Span::from_base_size(arena as *mut u8, bytes))
            .is_ok()
    };
    if ok {
        CLAIMED.store(bytes, core::sync::atomic::Ordering::Relaxed);
    }
    ok
}
