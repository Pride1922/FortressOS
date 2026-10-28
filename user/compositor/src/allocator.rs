use spin::Mutex;
use talc::{ErrOnOom, Span, Talc, Talck};

// Process-owned anonymous RAM; ELF image remains below the 4 MiB ceiling.
// 1080p scene + maximized clients and reallocations exceed the old 16 MiB
// arena. Stay within the existing SYS_MMAP process-private 32 MiB cap.
const HEAP_SIZE: usize = 32 * 1024 * 1024;

#[global_allocator]
static ALLOCATOR: Talck<Mutex<()>, ErrOnOom> = Talc::new(ErrOnOom).lock();

pub fn usage() -> (usize, usize) {
    (ALLOCATOR.lock().get_counters().allocated_bytes, HEAP_SIZE)
}

pub fn init_heap() -> bool {
    let arena = crate::sys::mmap(HEAP_SIZE);
    if arena < 0 {
        return false;
    }
    unsafe {
        ALLOCATOR
            .lock()
            .claim(Span::from_base_size(arena as *mut u8, HEAP_SIZE))
            .is_ok()
    }
}
