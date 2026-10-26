use crate::sys;
use core::panic::PanicInfo;

#[panic_handler]
fn panic(info: &PanicInfo) -> ! {
    let msg = b"\n[FORTRESS RUST PANIC] Unrecoverable fault in compositor\n";
    sys::write(2, msg);

    if let Some(loc) = info.location() {
        sys::write(2, b"  Location: ");
        sys::write(2, loc.file().as_bytes());
        sys::write(2, b"\n");
    }

    sys::exit(1);
}
