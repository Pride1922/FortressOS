use spin::Mutex;
use talc::{ErrOnOom, Span, Talc, Talck};

// Leave room for code/data below the loader's 20 MiB ELF image cap.
// Larger GOP modes fail allocation cleanly; no kernel ownership changes.
const HEAP_SIZE: usize = 16 * 1024 * 1024;

#[unsafe(link_section = ".bss")]
static mut HEAP_ARENA: [u8; HEAP_SIZE] = [0; HEAP_SIZE];

#[global_allocator]
static ALLOCATOR: Talck<Mutex<()>, ErrOnOom> = Talc::new(ErrOnOom).lock();

pub fn usage() -> (usize, usize) {
    (ALLOCATOR.lock().get_counters().allocated_bytes, HEAP_SIZE)
}

pub fn init_heap() {
    unsafe {
        let span = Span::from_array(&raw mut HEAP_ARENA);
        ALLOCATOR
            .lock()
            .claim(span)
            .expect("Fatal: unable to claim compositor heap arena");
    }
}
