extern crate alloc;
#[path = "../user/compositor/src/cursor.rs"]
mod cursor;
#[path = "../user/compositor/src/font.rs"]
mod font;
#[path = "../user/compositor/src/surface.rs"]
mod surface;
#[path = "../user/compositor/src/sys.rs"]
#[allow(dead_code)]
mod sys;
#[path = "../user/compositor/src/window.rs"]
mod window;
use surface::{Rect, Surface};

#[test]
fn cursor_restores_text_and_clips_every_edge() {
    let mut s = Surface::new(40, 40).unwrap();
    s.fill(0xff123456);
    s.text(0, 0, "A?\nZ", 0xffffffff, 0xff123456);
    let original = s.pixels.clone();
    let mut c = cursor::Cursor::new(0, 0).unwrap();
    c.show(&mut s);
    assert_ne!(s.pixels, original);
    for (dx, dy) in [(1, 1), (127, 127), (-128, 127), (127, -128), (-128, -128)] {
        c.motion(&mut s, dx, dy);
        assert!(c.x < 40 && c.y < 40);
        c.hide(&mut s);
        assert_eq!(s.pixels, original);
        c.show(&mut s);
    }
    c.hide(&mut s);
    let mut tiny = Surface::new(1, 1).unwrap();
    c.motion(&mut tiny, -128, -128);
    assert_eq!((c.x, c.y), (0, 0));
}

#[test]
fn font_pixels_and_clipped_strings() {
    let mut s = Surface::new(9, 17).unwrap();
    s.text(0, 0, "A", 123, 456);
    for y in 0..16 {
        for x in 0..8 {
            assert_eq!(
                s.pixels[y * 9 + x],
                if font::GLYPHS[33][y] & (128 >> x) != 0 {
                    123
                } else {
                    456
                }
            );
        }
    }
    s.text(8, 16, "?\n\u{00e9}", 1, 2);
    s.text(usize::MAX, usize::MAX, "overflow", 1, 2);
    assert!(Surface::new(usize::MAX, 2).is_none());
    assert!(Surface::new(0, 1).is_none());
}

#[test]
fn damage_pitch_padding_and_invalid_metadata() {
    let mut s = Surface::new(7, 4).unwrap();
    let mut vram = vec![0xdeadbeefu32; 9 * 4];
    let mut fb = sys::FbInfoV1 {
        version: 1,
        width: 7,
        height: 4,
        pitch: 36,
        bpp: 32,
        fb_user_vaddr: vram.as_mut_ptr() as u64,
        fb_size: 144,
        ..Default::default()
    };
    s.pixels.fill(42);
    s.damage(Rect {
        x: 2,
        y: 1,
        w: 3,
        h: 2,
    });
    assert!(unsafe { s.present(&fb) });
    for y in 0..4 {
        for x in 0..9 {
            assert_eq!(
                vram[y * 9 + x],
                if (1..3).contains(&y) && (2..5).contains(&x) {
                    42
                } else {
                    0xdeadbeef
                }
            );
        }
    }
    s.pixels.fill(99);
    assert!(unsafe { s.present(&fb) }); // no damage means no writes
    assert_eq!(vram[11], 42);
    for _ in 0..33 {
        s.damage(Rect {
            x: 0,
            y: 0,
            w: 1,
            h: 1,
        });
    }
    fb.fb_size = 1;
    assert!(!unsafe { s.present(&fb) });
    fb.fb_size = 144;
    assert!(unsafe { s.present(&fb) });
    assert_eq!(vram[3 * 9 + 6], 99);
    assert_eq!(vram[3 * 9 + 7], 0xdeadbeef);
}

fn flush(s: &mut Surface) {
    let mut pixels = vec![0u32; s.width * s.height];
    let fb = sys::FbInfoV1 {
        version: 1,
        width: s.width as u32,
        height: s.height as u32,
        pitch: (s.width * 4) as u32,
        bpp: 32,
        fb_user_vaddr: pixels.as_mut_ptr() as u64,
        fb_size: (pixels.len() * 4) as u64,
        ..Default::default()
    };
    assert!(unsafe { s.present(&fb) });
}

#[test]
fn window_hit_boundaries_and_raise_capture() {
    use window::{Hit, WindowManager};
    let mut s = Surface::new(640, 480).unwrap();
    let mut wm = WindowManager::demo().unwrap();
    assert_eq!(wm.hit_test(300, 260), Some((2, Hit::Client)));
    assert_eq!(wm.hit_test(80, 120), Some((1, Hit::Border)));
    assert_eq!(wm.hit_test(100, 144), Some((1, Hit::Title)));
    assert_eq!(wm.hit_test(100, 145), Some((1, Hit::Client)));
    assert_eq!(wm.hit_test(539, 220), Some((2, Hit::Border)));
    assert_eq!(wm.hit_test(540, 220), None);
    assert_eq!(wm.hit_test(515, 225), Some((2, Hit::Close)));
    assert_eq!(wm.hit_test(491, 225), Some((2, Hit::Minimize)));
    wm.mouse(100, 130, 1, &mut s);
    assert_eq!(
        wm.windows.iter().map(|w| w.id).collect::<Vec<_>>(),
        vec![2, 1]
    );
    assert_eq!(wm.windows.iter().filter(|w| w.is_focused).count(), 1);
    assert!(wm.windows[1].is_dragging);
    assert_eq!(wm.windows[1].drag_offset, (20, 10));
    wm.mouse(340, 240, 1, &mut s);
    assert_eq!((wm.windows[1].rect.x, wm.windows[1].rect.y), (320, 230));
    assert_eq!(wm.hit_test(350, 260), Some((1, Hit::Client)));
    wm.mouse(350, 250, 0, &mut s); // final release motion is honored
    let released = wm.windows[1].rect;
    assert_eq!((released.x, released.y), (330, 240));
    wm.mouse(600, 450, 0, &mut s);
    assert_eq!(wm.windows[1].rect, released);
    assert!(!wm.windows[1].is_dragging);
}

#[test]
fn window_controls_hover_minimize_restore_close_and_button_edges() {
    use window::{HOVER_CLOSE, HOVER_MIN, INACTIVE, WindowManager};
    let mut s = Surface::new(640, 480).unwrap();
    s.fill(window::DESKTOP);
    let mut wm = WindowManager::demo().unwrap();
    wm.mouse(520, 222, 0, &mut s);
    wm.render(&mut s);
    assert_eq!(s.pixels[222 * 640 + 520], HOVER_CLOSE);
    wm.mouse(500, 222, 0, &mut s);
    wm.render(&mut s);
    assert_eq!(s.pixels[222 * 640 + 500], HOVER_MIN);
    wm.mouse(500, 222, 1, &mut s);
    assert!(wm.windows.iter().find(|w| w.id == 2).unwrap().is_minimized);
    assert_eq!(wm.hit_test(500, 222), None);
    assert!(wm.windows.iter().find(|w| w.id == 1).unwrap().is_focused);
    wm.render(&mut s);
    assert_eq!(s.pixels[453 * 640 + 180], INACTIVE); // task button
    wm.mouse(180, 453, 0, &mut s);
    wm.mouse(180, 453, 1, &mut s);
    assert_eq!(wm.windows.last().unwrap().id, 2);
    assert!(!wm.windows.last().unwrap().is_minimized);
    assert!(wm.windows.last().unwrap().is_focused);
    assert!(!wm.windows.last().unwrap().is_dragging);
    wm.mouse(520, 222, 0, &mut s);
    wm.mouse(520, 222, 1, &mut s);
    assert_eq!(wm.windows.len(), 1);
    assert_eq!(wm.windows[0].id, 1);
    assert!(wm.windows[0].is_focused);
    wm.mouse(380, 122, 1, &mut s); // holding never re-triggers close
    assert_eq!(wm.windows.len(), 1);
    wm.mouse(380, 122, 0, &mut s);
    wm.mouse(380, 122, 1, &mut s);
    assert!(wm.windows.is_empty());
    wm.render(&mut s); // empty desktop remains usable
}

#[test]
fn window_damage_repair_matches_full_composition_with_occlusion_and_clipping() {
    let mut s = Surface::new(600, 440).unwrap();
    let mut wm = window::WindowManager::demo().unwrap();
    s.fill(window::DESKTOP);
    wm.render(&mut s);
    flush(&mut s);
    // Includes raise, drag over another client, off-screen clipping, hover,
    // release and reversal over the exposed desktop.
    let events = [
        (100, 130, 1),
        (240, 230, 1),
        (590, 430, 1),
        (599, 439, 0),
        (280, 230, 1),
        (30, 20, 1),
        (0, 0, 1),
        (20, 10, 0),
        (550, 400, 0),
    ];
    for (x, y, buttons) in events {
        let before = s.pixels.clone();
        wm.mouse(x, y, buttons, &mut s);
        wm.monitor(&mut s, 2400, (123456, 16 * 1024 * 1024), (x, y));
        wm.render(&mut s);
        let mut full = Surface::new(s.width, s.height).unwrap();
        full.fill(window::DESKTOP);
        wm.render(&mut full);
        assert_eq!(s.pixels, full.pixels, "partial repair differs at {x},{y}");
        for (i, (&old, &new)) in before.iter().zip(&s.pixels).enumerate() {
            if old != new {
                assert!(
                    (0..16)
                        .filter_map(|n| s.damage_rect(n))
                        .any(|r| r.contains(i % s.width, i / s.width))
                );
            }
        }
        for r in (0..16).filter_map(|n| s.damage_rect(n)) {
            assert!(r.x + r.w <= s.width && r.y + r.h <= s.height);
        }
        flush(&mut s);
    }
    assert_eq!(
        Rect {
            x: usize::MAX,
            y: 0,
            w: 8,
            h: 8
        }
        .intersection(Rect {
            x: 0,
            y: 0,
            w: 600,
            h: 440
        }),
        None
    );
    assert!(
        window::Window::new(
            3,
            "invalid",
            Rect {
                x: 0,
                y: 0,
                w: 1,
                h: 1
            }
        )
        .is_none()
    );
    assert!(
        window::Window::new(
            3,
            "overflow",
            Rect {
                x: 0,
                y: 0,
                w: usize::MAX,
                h: usize::MAX
            }
        )
        .is_none()
    );
}

#[test]
fn monitor_mouse_bounded_damage_and_coalesced_deltas() {
    let mut s = Surface::new(640, 480).unwrap();
    let mut wm = window::WindowManager::demo().unwrap();
    s.fill(window::DESKTOP);
    wm.render(&mut s);
    flush(&mut s);

    // Initial full monitor damage is 320x200 = 64,000 pixels
    wm.monitor(&mut s, 2560, (1024, 4096), (100, 100));
    assert_eq!(s.damage_rect(0), Some(Rect { x: 80, y: 120, w: 320, h: 200 }));
    flush(&mut s);

    // Optimized monitor_mouse damage is bounded to the text sub-rectangle (<= 220x16 = 3,520 pixels)
    wm.monitor_mouse(&mut s, (150, 250));
    let damaged = s.damage_rect(0).expect("damage rect must exist");
    assert_eq!(damaged, Rect { x: 93, y: 229, w: 220, h: 16 });
    assert!(damaged.w * damaged.h <= 3520);
    assert_eq!(s.damage_rect(1), None);
    flush(&mut s);

    // Verify coalesced large motion deltas (> 127) clamp cleanly
    let mut c = cursor::Cursor::new(100, 100).unwrap();
    c.show(&mut s);
    c.motion(&mut s, 1000, 1000);
    assert_eq!((c.x, c.y), (639, 479));
    c.motion(&mut s, -2000, -2000);
    assert_eq!((c.x, c.y), (0, 0));
}
