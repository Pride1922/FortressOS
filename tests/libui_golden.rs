//! Golden pixel/damage fingerprints for every shared widget (libui Phase 0).
//!
//! Captured from the pre-extraction compositor widgets. The libui refactor must
//! keep every fingerprint bit-identical: pixels, returned damage rectangles and
//! the "redraw of an unchanged scene reports no damage" property.
//! Run with `LIBUI_GOLDEN_PRINT=1` to print the current table.
use crate::{canvas, surface::Rect, sys, ui, window};

fn fnv(h: &mut u64, v: u64) {
    for b in v.to_le_bytes() {
        *h ^= b as u64;
        *h = h.wrapping_mul(0x100000001b3);
    }
}

fn hash_pixels(pixels: &[u32], dirty: Option<Rect>) -> u64 {
    let mut h = 0xcbf29ce484222325u64;
    for &p in pixels {
        fnv(&mut h, p as u64);
    }
    match dirty {
        Some(r) => {
            for v in [1, r.x, r.y, r.w, r.h] {
                fnv(&mut h, v as u64);
            }
        }
        None => fnv(&mut h, 0),
    }
    h
}

fn r(x: usize, y: usize, w: usize, h: usize) -> Rect {
    Rect { x, y, w, h }
}

fn battery(capacity_percent: u32, state: u32) -> sys::BatteryStatus {
    sys::BatteryStatus {
        capacity_percent,
        state,
        time_remaining_mins: 0,
        voltage_mv: 0,
        ac_online: 0,
        temperature_c: 0,
    }
}

/// Built-in app widgets (ui::Canvas over a compositor Window).
fn draw_builtin(c: &mut ui::Canvas, step: usize) {
    match step {
        0 => c.label(4, 4, 120, "Label text", 0xffe0e0e0),
        1 => c.label_bg(190, 6, 140, "Label bg", 0xffffffff, 0xff244b6e),
        2 => c.label_h(10, 30, 160, 34, "Tall label", 0xff989898, 0xff2d2d2d),
        3 => c.label_bold(200, 30, 150, 26, "Bold label", 0xffffffff, 0xff1e1e1e),
        4 => c.badge(r(12, 70, 70, 22), "WPA2", 0xffb8c7dc, 0xff222f42, 0xff33465e),
        5 => c.badge_on(r(190, 70, 90, 24), "Badge", 0xffffffff, 0xff133223, 0xff22c55e, 0xff2d2d2d),
        6 => c.icon_badge(r(300, 64, 30, 30), 0, 0xff38bdf8, 0xff1e293b, 0xff334155, 0xff1e1e1e),
        7 => c.icon_badge(r(340, 64, 30, 30), 1, 0xff38bdf8, 0xff1e293b, 0xff334155, 0xff1e1e1e),
        8 => c.icon_badge(r(380, 64, 30, 30), 2, 0xff38bdf8, 0xff1e293b, 0xff334155, 0xff1e1e1e),
        9 => c.fill_rect(r(420, 64, 40, 20), 0xff123456),
        10 => c.card(r(10, 100, 160, 60), 0xff252525, 0xff3e3e3e),
        11 => c.card(r(190, 100, 160, 60), 0xff252525, 0xff3b82f6),
        12 => c.sheet_card(r(16, 176, 150, 50), 0xff252525, 0xff3e3e3e, true),
        13 => c.sheet_card(r(196, 176, 150, 50), 0xff252525, 0xff3e3e3e, false),
        14 => c.button_accent(r(10, 240, 90, 28), "Apply", false),
        15 => c.button_accent(r(110, 240, 90, 28), "Apply", true),
        16 => c.sidebar_nav(r(4, 280, 170, 32), "Personalization", true, false),
        17 => c.sidebar_nav(r(4, 316, 170, 32), "Network", false, true),
        18 => c.sidebar_nav(r(4, 352, 170, 32), "Date & Time", false, false),
        19 => c.button(r(190, 240, 90, 28), "Plain", false, false),
        20 => c.button(r(290, 240, 90, 28), "Selected", true, false),
        21 => c.button(r(390, 240, 90, 28), "Focused", false, true),
        22 => c.button_enabled(r(190, 276, 90, 28), "Disabled", false, true, false),
        23 => c.button(r(290, 276, 6, 6), "", false, false),
        24 => c.wallpaper_card(r(190, 312, 200, 28), "Hello", true, true, false),
        25 => c.wallpaper_card(r(190, 344, 200, 28), "Plain", false, false, true),
        26 => c.wifi_row(r(10, 392, 470, 24), "HomeNet", -40, 1, true, true, false, true),
        27 => c.wifi_row(r(10, 420, 470, 24), "Cafe", -70, 0, false, false, true, true),
        28 => c.wifi_row(r(10, 448, 470, 24), "Lab", -90, 2, false, false, false, false),
        29 => c.entry(r(190, 480, 200, 30), "typed text", false),
        30 => c.entry_on(r(190, 520, 200, 30), "focused", true, 0xff1e1e1e),
        31 => c.battery_icon(400, 4, &battery(55, sys::BATTERY_STATE_CHARGING)),
        32 => c.battery_icon(430, 4, &battery(0, sys::BATTERY_STATE_UNKNOWN)),
        _ => {}
    }
}
const BUILTIN_STEPS: usize = 33;

fn builtin_scene(id: u32, w: usize, h: usize, scale: usize, scroll: usize, out: &mut alloc::vec::Vec<u64>) {
    let mut win = window::Window::new(id, "Golden", r(3, 5, w, h)).unwrap();
    // Each widget alone (pixels + damage), then the whole scene, then an
    // identical redraw which must report no damage at all.
    for step in 0..BUILTIN_STEPS {
        let c = ui::Canvas::scrolled(&mut win, scale, true, scroll);
        let _ = c.finish();
        let mut c = ui::Canvas::scrolled(&mut win, scale, false, scroll);
        draw_builtin(&mut c, step);
        let d = c.finish();
        out.push(hash_pixels(&win.content, d));
    }
    let mut c = ui::Canvas::scrolled(&mut win, scale, true, scroll);
    for step in 0..BUILTIN_STEPS {
        draw_builtin(&mut c, step);
    }
    let d = c.finish();
    out.push(hash_pixels(&win.content, d));
    let mut c = ui::Canvas::scrolled(&mut win, scale, false, scroll);
    for step in 0..BUILTIN_STEPS {
        draw_builtin(&mut c, step);
    }
    // Overlapping widgets may legitimately repaint; the damage must still match.
    let d = c.finish();
    out.push(hash_pixels(&win.content, d));
    // Overflowing / off-canvas geometry must clip without panicking.
    let mut c = ui::Canvas::scrolled(&mut win, scale, false, scroll);
    c.button(r(0, 0, usize::MAX, usize::MAX), "bounded", false, false);
    c.card(r(usize::MAX, 3, 40, 40), 0, 0);
    c.entry_on(r(5, usize::MAX - 1, 40, 40), "x", true, 0);
    c.label_bold(usize::MAX / 2, 0, usize::MAX, 30, "x", 0, 0);
    let d = c.finish();
    out.push(hash_pixels(&win.content, d));
}

/// Client widgets (canvas::Canvas over a raw stride buffer).
fn client_scene(width: usize, height: usize, stride: usize, out: &mut alloc::vec::Vec<u64>) {
    let mut pixels = alloc::vec![0xff010203u32; stride * height];
    let mut c = canvas::Canvas::new(&mut pixels, width, height, stride).unwrap();
    c.fill(0xff1e1e1e);
    c.fill_rect(3, 4, 50, 20, 0xff123456);
    c.fill_rect(width - 5, height - 5, 50, 50, 0xff654321);
    for (i, ch) in b"Az~\x01".iter().enumerate() {
        c.glyph(10 + i * 8, 30, *ch, 0xffe0e0e0, 0xff000000);
    }
    c.glyph(width - 4, 30, b'X', 0xffffffff, 0);
    c.text(4, 50, "Proportional text", 0xffe0e0e0, 0xff1e1e1e);
    c.text_bold(4, 70, "Bold text", 0xffffffff, 0xff1e1e1e);
    c.text(width - 20, height - 9, "clipped", 0xffffffff, 0);
    c.rounded_box(4, 92, 120, 40, 8, 0xff1e1e1e, 0xff252525, 0xff3e3e3e);
    c.rounded_box(width - 30, 92, 60, 40, 30, 0xff1e1e1e, 0xff252525, 0xff3e3e3e);
    c.pill_button(130, 92, 90, 28, "Open", 0xffffffff, 0xff007aff, 0xff38bdf8, 0xff1e1e1e);
    c.pill_button(130, 124, 4, 4, "", 0xffffffff, 0xff2d2d2d, 0xff3e3e3e, 0xff1e1e1e);
    for kind in 0..3u8 {
        c.file_icon(10 + kind as usize * 20, 140, kind, 0xff1e1e1e);
    }
    c.battery_icon(80, 140, &battery(15, sys::BATTERY_STATE_DISCHARGING));
    c.battery_icon(width - 10, 140, &battery(100, sys::BATTERY_STATE_FULL));
    out.push(hash_pixels(&pixels, None));
}

pub fn fingerprints() -> alloc::vec::Vec<u64> {
    let mut out = alloc::vec::Vec::new();
    // Registered built-in app (sidebar + centred x_offset) and an ordinary
    // window, at native and 2x scale, with and without scrolling.
    for (id, w, h) in [(7u32, 1100usize, 620usize), (7, 1800, 640), (50, 520, 600)] {
        for scale in [1usize, 2] {
            for scroll in [0usize, 37] {
                builtin_scene(id, w, h, scale, scroll, &mut out);
            }
        }
    }
    client_scene(240, 170, 240, &mut out);
    client_scene(237, 165, 300, &mut out);
    out
}

#[test]
fn libui_widget_golden_fingerprints() {
    let got = fingerprints();
    if std::env::var_os("LIBUI_GOLDEN_PRINT").is_some() {
        std::println!("const GOLDEN: [u64; {}] = [", got.len());
        for v in &got {
            std::println!("    {v:#018x},");
        }
        std::println!("];");
    }
    let expected: &[u64] = &GOLDEN;
    assert_eq!(got.len(), expected.len(), "fingerprint count changed");
    for (i, (g, e)) in got.iter().zip(expected).enumerate() {
        assert_eq!(g, e, "fingerprint {i} changed");
    }
}

include!("libui_golden_table.rs");
