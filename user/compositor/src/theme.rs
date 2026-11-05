//! Shared desktop styling. Integer-only, allocation-free raster operations.
#[allow(unused_imports)]
pub use libui::font::{text_pixel, text_pixel_bold, text_width};
#[allow(unused_imports)]
pub use libui::gfx::{blend_coverage, rounded_coverage};
#[allow(unused_imports)]
pub use libui::theme::*;

#[allow(dead_code)]
static SCALE_OVERRIDE: core::sync::atomic::AtomicU8 = core::sync::atomic::AtomicU8::new(0);

#[allow(dead_code)]
pub fn set_scale(mode: u8) {
    SCALE_OVERRIDE.store(
        if mode <= 2 { mode } else { 0 },
        core::sync::atomic::Ordering::Relaxed,
    );
}

// 1080p uses native raster sizes. Only UHD needs the larger chrome preset.
#[allow(dead_code)]
pub fn scale(height: usize) -> usize {
    match SCALE_OVERRIDE.load(core::sync::atomic::Ordering::Relaxed) {
        1 => return 1,
        2 => return 2,
        _ => (),
    }
    if height >= 2160 { 2 } else { 1 }
}

/// Row form of `desktop_pixel()`: the vertical term is computed once.
#[allow(dead_code)]
pub fn desktop_row(x0: usize, y: usize, width: usize, height: usize, out: &mut [u32]) {
    let ny = (y * 1024 / height.max(1)) as i32;
    let dy = ny - 380;
    let dy2 = dy * dy;
    for (i, p) in out.iter_mut().enumerate() {
        let nx = ((x0 + i) * 1024 / width.max(1)) as i32;
        let dx = nx - 580;
        let dist2 = (dx * dx + dy2) / 1024;
        let glow = (900 - dist2).max(0) as u32;
        let r = 11 + glow * 26 / 900;
        let g = 15 + glow * 47 / 900;
        let b = 23 + glow * 82 / 900;
        *p = 0xff000000 | r << 16 | g << 8 | b;
    }
}

/// Per-pixel reference for `desktop_row()`, kept for the host tests.
#[allow(dead_code)]
pub fn desktop_pixel(x: usize, y: usize, width: usize, height: usize) -> u32 {
    let nx = (x * 1024 / width.max(1)) as i32;
    let ny = (y * 1024 / height.max(1)) as i32;
    let dx = nx - 580;
    let dy = ny - 380;
    let dist2 = (dx * dx + dy * dy) / 1024;
    let glow = (900 - dist2).max(0) as u32;
    let r = 11 + glow * 26 / 900;
    let g = 15 + glow * 47 / 900;
    let b = 23 + glow * 82 / 900;
    0xff000000 | r << 16 | g << 8 | b
}

/// Renders a single pixel for the 22x10 battery icon:
/// - 20x10 rounded body outline
/// - 2x4 nub on the right side
/// - Fill level inside based on capacity_percent:
///   ACCENT_BLUE for normal, AMBER for <20%, RED for <10%
/// - Yellow lightning bolt if state == Charging
/// - Red 'X' if no battery detected (-ENODEV)
pub fn render_battery_pixel(dx: usize, dy: usize, status: &crate::sys::BatteryStatus, bg: u32) -> Option<u32> {
    if dx >= 22 || dy >= 10 {
        return None;
    }

    // 2x4 Nub on the right side: dx 20..22, dy 3..7
    if dx >= 20 {
        if (3..7).contains(&dy) {
            return Some(0xffcbd5e1);
        } else {
            return Some(bg);
        }
    }

    // Battery Body: dx 0..20, dy 0..10
    // Rounded corners: omit (0,0), (0,9), (19,0), (19,9)
    let is_corner = (dx == 0 && dy == 0) || (dx == 0 && dy == 9) || (dx == 19 && dy == 0) || (dx == 19 && dy == 9);
    if is_corner {
        return Some(bg);
    }

    // Outline border
    let is_border = dx == 0 || dx == 19 || dy == 0 || dy == 9;
    if is_border {
        return Some(0xffcbd5e1);
    }

    // Interior: dx 1..19, dy 1..9
    let is_inner_border = dx == 1 || dx == 18 || dy == 1 || dy == 8;
    let interior_bg = 0xff1e2430;

    // Check for no battery (e.g. unknown with 0 capacity) -> draw Red 'X'
    let is_no_battery = status.state == crate::sys::BATTERY_STATE_UNKNOWN && status.capacity_percent == 0;
    if is_no_battery {
        if is_inner_border {
            return Some(interior_bg);
        }
        // Red 'X' inside dx 4..16, dy 2..8
        if (4..16).contains(&dx) && (2..8).contains(&dy) {
            let rx = dx - 4; // 0..11
            let ry = dy - 2; // 0..5
            let on_x = (ry * 11 / 5 == rx) || ((5 - ry) * 11 / 5 == rx)
                || ((ry * 11 + 5) / 5 == rx) || (((5 - ry) * 11 + 5) / 5 == rx);
            if on_x {
                return Some(RED);
            }
        }
        return Some(interior_bg);
    }

    // Check for yellow lightning bolt if Charging
    if status.state == crate::sys::BATTERY_STATE_CHARGING && (7..14).contains(&dx) && (2..8).contains(&dy) {
        let lx = dx - 7; // 0..6
        let ly = dy - 2; // 0..5
        let is_bolt = match ly {
            0 => lx == 3 || lx == 4,
            1 => lx == 2 || lx == 3,
            2 => (0..=5).contains(&lx),
            3 => lx == 1 || lx == 2,
            4 => lx == 2 || lx == 3,
            5 => lx == 2,
            _ => false,
        };
        if is_bolt {
            return Some(YELLOW);
        }
    }

    // Fill level: inside width is 16 pixels (dx 2..18)
    let fill_color = if status.capacity_percent < 10 {
        RED
    } else if status.capacity_percent < 20 {
        AMBER
    } else {
        ACCENT_BLUE
    };

    let cap = status.capacity_percent.min(100) as usize;
    let fill_w = if cap > 0 { ((cap * 16) / 100).max(1) } else { 0 };
    if (2..18).contains(&dx) && (2..8).contains(&dy) {
        if dx - 2 < fill_w {
            Some(fill_color)
        } else {
            Some(interior_bg)
        }
    } else {
        Some(interior_bg)
    }
}
