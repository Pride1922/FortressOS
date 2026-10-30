extern crate alloc;
#[path = "../user/compositor/src/desktop.rs"]
#[allow(dead_code)]
mod desktop;
#[path = "../user/compositor/src/preferences.rs"]
mod preferences;
#[path = "../user/compositor/src/ui.rs"]
mod ui;

#[test]
fn desktop_preferences_strict_roundtrip_and_bounded_edits() {
    assert_eq!(ui::APP_API_VERSION, 1);
    assert_eq!(
        (sys::VFS_O_RDONLY, sys::VFS_O_WRONLY, sys::VFS_O_RDWR),
        (0, 1, 2)
    );
    use preferences::{Preferences, Text};
    for scale in 0..3 {
        for wallpaper in [false, true] {
            for clock_24h in [false, true] {
                let prefs = Preferences {
                    scale,
                    wallpaper,
                    clock_24h,
                };
                let mut text = Text::<512>::default();
                prefs.encode(&mut text).unwrap();
                assert_eq!(Preferences::parse(text.as_bytes()), Some(prefs));
            }
        }
    }
    for bad in [
        "version=2\nscale=auto\nwallpaper=plain\nclock=24h\n",
        "version=1\nscale=9\nwallpaper=plain\nclock=24h\n",
        "version=1\nscale=auto\nwallpaper=plain\nclock=24h\nscale=native\n",
        "version=1\nscale=auto\nwallpaper=plain\nclock=24h\nextra=yes\n",
        "version=1\0",
    ] {
        assert!(Preferences::parse(bad.as_bytes()).is_none());
    }
    assert!(Preferences::parse(&[b'x'; 513]).is_none());
    let mut text = Text::<4>::default();
    for b in b"secret" {
        text.edit(*b);
    }
    assert_eq!(text.as_str(), "secr");
    text.edit(8);
    assert_eq!(text.as_str(), "sec");
    text.clear();
    assert!(text.as_bytes().is_empty());
}

#[test]
fn widget_viewport_clips_extreme_rectangles_before_rasterizing() {
    let mut w = window::Window::new(
        7,
        "Clip",
        surface::Rect {
            x: 10,
            y: 10,
            w: 100,
            h: 80,
        },
    )
    .unwrap();
    let mut c = ui::Canvas::new(&mut w, 0, true);
    c.button(
        surface::Rect {
            x: 0,
            y: 0,
            w: usize::MAX,
            h: usize::MAX,
        },
        "bounded",
        false,
        false,
    );
    c.button(
        surface::Rect {
            x: usize::MAX,
            y: usize::MAX,
            w: 20,
            h: 20,
        },
        "offscreen",
        false,
        false,
    );
    let r = c.finish().unwrap();
    assert!(r.x >= 11 && r.y >= 35 && r.x + r.w <= 109 && r.y + r.h <= 89);
}

#[test]
fn built_in_apps_share_focus_widgets_and_incremental_damage() {
    use ui::DesktopApp;
    let mut host = desktop::AppHost::new(preferences::Preferences::default());
    let mut wm = window::WindowManager::desktop();
    let mut s = surface::Surface::new(1280, 800).unwrap();
    host.open(7, &mut wm, &mut s);
    assert_eq!(wm.focused_window_id(), Some(7));
    assert_eq!(ui::APPS[0].name, "Settings");
    assert!(ui::registered(7));
    assert!(!ui::registered(77));
    flush(&mut s);
    host.render(7, &mut wm, &mut s);
    assert!(s.damage_rect(0).is_none(), "idle widgets must not repaint");
    host.pointer(7, 320, 105, &mut wm, &mut s);
    assert_eq!(host.draft.scale, 1);
    assert_eq!(host.preferences.scale, 0);
    let area: usize = (0..16)
        .filter_map(|i| s.damage_rect(i))
        .map(|r| r.w * r.h)
        .sum();
    assert!(area < 30_000, "selection repainted entire window: {area}");
    // Interactive wallpaper & clock switching in real-time
    host.pointer(7, 360, 190, &mut wm, &mut s); // Plain Black
    assert!(!host.draft.wallpaper);
    assert!(host.changed);
    host.changed = false;
    host.pointer(7, 220, 190, &mut wm, &mut s); // Hello World
    assert!(host.draft.wallpaper);
    assert!(host.changed);
    host.changed = false;
    host.pointer(7, 330, 275, &mut wm, &mut s); // 12-Hour
    assert!(!host.draft.clock_24h);
    assert!(host.changed);
    host.changed = false;
    host.pointer(7, 220, 275, &mut wm, &mut s); // 24-Hour
    assert!(host.draft.clock_24h);
    assert!(host.changed);
    host.changed = false;
    host.pointer(7, 50, 110, &mut wm, &mut s); // Shared Network navigation.
    assert_eq!(wm.focused_window_id(), Some(7));
    assert_eq!(host.apps[0].kind, 1);
    // Explicit UP/device fixture; no kernel/hardware call is made by typing.
    host.wireless = Some(sys::WirelessInfo {
        mac: [2, 0, 0, 0, 0, 1],
        ..Default::default()
    });
    host.pointer(7, 250, 360, &mut wm, &mut s);
    for b in b"example" {
        host.key(7, *b, &mut wm, &mut s);
    }
    assert_eq!(host.ssid.as_str(), "example");
    host.key(7, 27, &mut wm, &mut s);
    assert!(!wm.windows.iter().any(|w| w.id == 7));
    host.open(7, &mut wm, &mut s);
    flush(&mut s);
    host.render(8, &mut wm, &mut s);
    assert!(
        s.damage_rect(0).is_none(),
        "reopened window must initialize once"
    );
    // The app trait routes keyboard focus independently of global shortcuts.
    let mut app = desktop::Builtin::test_instance(0);
    assert_eq!(app.descriptor().id, 7);
    assert_eq!(app.key(b'\t'), Some(0));
    assert_eq!(app.key(b'\r'), Some(2));
}

#[test]
fn scan_decoder_rejects_malformed_responses_and_preserves_identity() {
    let mut scan = vec![0u8; 776];
    scan[0..4].copy_from_slice(&1u32.to_le_bytes());
    scan[8..14].copy_from_slice(&[1, 2, 3, 4, 5, 6]);
    scan[15] = (-42i8) as u8;
    scan[16] = 4;
    scan[17] = 1;
    scan[20..24].copy_from_slice(b"test");
    let points = desktop::parse_scan(&scan).unwrap();
    assert_eq!(points[0].ssid.as_str(), "test");
    assert_eq!(points[0].rssi, -42);
    assert_eq!(points[0].bssid, [1, 2, 3, 4, 5, 6]);
    scan[16] = 33;
    assert!(desktop::parse_scan(&scan).is_none());
    scan[16] = 4;
    scan[0..4].copy_from_slice(&17u32.to_le_bytes());
    assert!(desktop::parse_scan(&scan).is_none());
    assert_eq!(
        desktop::ipv4(u32::from_ne_bytes([192, 168, 1, 1])).as_str(),
        "192.168.1.1"
    );
}

#[test]
fn desktop_windows_snap_restore_and_scroll_with_clipped_widgets() {
    let mut s = surface::Surface::new(1280, 800).unwrap();
    let mut wm = window::WindowManager::desktop();
    let mut host = desktop::AppHost::new(preferences::Preferences::default());
    host.open(7, &mut wm, &mut s);
    let start = wm.windows[0].rect;
    wm.mouse(start.x + 180, start.y + 12, 1, &mut s);
    wm.mouse(0, 300, 1, &mut s);
    assert_eq!(
        wm.mouse(0, 300, 0, &mut s),
        window::Action::WindowResized(7)
    );
    let snapped = wm.windows[0].rect;
    assert_eq!(snapped.x, 8);
    assert!(snapped.w < 1280 / 2);
    assert!(wm.windows[0].saved_rect.is_some());
    wm.mouse(snapped.x + snapped.w - 54, snapped.y + 12, 1, &mut s);
    wm.mouse(snapped.x + snapped.w - 54, snapped.y + 12, 0, &mut s);
    assert_eq!(wm.windows[0].rect.w, start.w);
    host.render(7, &mut wm, &mut s);
    let expected = wm.windows[0].content.clone();
    wm.windows[0].resize(300, 180);
    host.render(7, &mut wm, &mut s);
    flush(&mut s);
    host.scroll(7, 2, &mut wm, &mut s);
    assert!(s.damage_rect(0).is_some());
    for r in (0..16).filter_map(|i| s.damage_rect(i)) {
        assert!(r.x + r.w <= s.width && r.y + r.h <= s.height);
    }
    host.render(7, &mut wm, &mut s);
    flush(&mut s);
    host.render(7, &mut wm, &mut s);
    assert!(s.damage_rect(0).is_none());
    wm.windows[0].resize(start.w, start.h);
    host.render(7, &mut wm, &mut s);
    assert!(
        wm.windows[0].content == expected,
        "growing the viewport must reveal the full unscrolled page"
    );
    wm.invalidate_panels();
}

#[test]
fn wifi_controls_require_an_adapter_on_every_cpu_count() {
    let mut s = surface::Surface::new(1280, 800).unwrap();
    let mut wm = window::WindowManager::desktop();
    let mut host = desktop::AppHost::new(preferences::Preferences::default());
    host.open(7, &mut wm, &mut s);
    host.pointer(8, 500, 155, &mut wm, &mut s); // Missing adapter returns before syscall.
    assert!(host.status.as_str().contains("No Wi-Fi adapter"));
    host.pointer(8, 590, 155, &mut wm, &mut s); // No adapter.
    assert!(host.status.as_str().contains("No Wi-Fi adapter"));
    host.pointer(8, 500, 415, &mut wm, &mut s); // No adapter.
    assert!(host.status.as_str().contains("No Wi-Fi adapter"));
}

#[test]
fn native_1080p_chrome_and_smooth_dock_edges() {
    use window::theme;
    assert_eq!(theme::scale(1080), 1);
    assert_eq!(theme::scale(2160), 2);
    let mut partial = 0;
    for y in 0..18 {
        for x in 0..18 {
            let c = theme::rounded_coverage(x, y, 256, 58, 18);
            assert_eq!(c, theme::rounded_coverage(255 - x, y, 256, 58, 18));
            assert_eq!(c, theme::rounded_coverage(x, 57 - y, 256, 58, 18));
            assert!(c <= 16);
            if c > 0 && c < 16 {
                partial += 1;
                let blended = theme::blend_coverage(0xffffffff, 0xff000000, c);
                assert!(blended != 0xffffffff && blended != 0xff000000);
            }
        }
    }
    assert!(
        partial > 10,
        "dock outline must contain antialiased edge pixels"
    );
}

#[test]
fn title_controls_repaint_only_their_hover_pixels() {
    for id in [3, 6] {
        for maximized in [false, true] {
            let mut scene = surface::Surface::new(1920, 1080).unwrap();
            scene.fill(window::DESKTOP);
            let mut wm = window::WindowManager::desktop();
            let rect = surface::Rect {
                x: 100,
                y: 100,
                w: if maximized { 1700 } else { 520 },
                h: 700,
            };
            let mut w = window::Window::new(id, "Hover regression", rect).unwrap();
            if maximized {
                w.saved_rect = Some(rect);
            }
            wm.windows.push(w);
            wm.render(&mut scene);
            flush(&mut scene);
            let right = rect.x + rect.w;
            for (x, y) in [
                (right - 90, 112),
                (right - 54, 112),
                (right - 18, 112),
                (rect.x + 200, 112),
                (rect.x + 200, 300),
            ] {
                wm.mouse(x, y, 0, &mut scene);
                let area: usize = (0..16)
                    .filter_map(|i| scene.damage_rect(i))
                    .map(|r| r.w * r.h)
                    .sum();
                assert!(
                    area <= 1800,
                    "hover {id}, maximized={maximized}: {area} pixels"
                );
                wm.render(&mut scene);
                let mut full = surface::Surface::new(1920, 1080).unwrap();
                full.fill(window::DESKTOP);
                wm.render(&mut full);
                assert!(
                    scene.pixels == full.pixels,
                    "hover repair differs from full render"
                );
                flush(&mut scene);
            }
        }
    }
}

#[test]
fn maximized_system_panel_updates_only_changed_fields() {
    let mut scene = surface::Surface::new(1920, 1080).unwrap();
    let mut wm = window::WindowManager::desktop();
    wm.ensure_window_1(&mut scene);
    wm.mouse(125, 132, 1, &mut scene);
    wm.mouse(125, 132, 0, &mut scene);
    let mut info = sys::SystemInfo {
        cpu_count: 8,
        total_ram_bytes: 32 << 30,
        free_ram_bytes: 30 << 30,
        task_count: 9,
        thread_count: 20,
        uptime_ticks: 9900,
        tick_hz: 100,
        ..Default::default()
    };
    wm.system_panel(&mut scene, Some(info));
    wm.render(&mut scene);
    flush(&mut scene);
    wm.system_panel(&mut scene, Some(info));
    assert!(
        scene.damage_rect(0).is_none(),
        "unchanged sample repaints fullscreen"
    );
    // Crossing 99 -> 100 seconds changes the string length. The reverse
    // transition also has to erase old trailing digits.
    for seconds in [100, 9] {
        info.uptime_ticks = seconds * 100;
        wm.system_panel(&mut scene, Some(info));
        let regions: Vec<_> = (0..16).filter_map(|i| scene.damage_rect(i)).collect();
        assert_eq!(regions.len(), 1);
        assert!(
            regions[0].w * regions[0].h < 6_000,
            "one counter change must not repaint the maximized client"
        );
        wm.render(&mut scene);
        let mut fresh = window::WindowManager::desktop();
        fresh
            .windows
            .push(window::Window::new(6, "System Information", wm.windows[0].rect).unwrap());
        fresh.windows[0].saved_rect = wm.windows[0].saved_rect;
        let mut full = surface::Surface::new(1920, 1080).unwrap();
        fresh.system_panel(&mut full, Some(info));
        assert_eq!(wm.windows[0].content, fresh.windows[0].content);
        flush(&mut scene);
    }
    wm.system_panel(&mut scene, None);
    flush(&mut scene);
    wm.system_panel(&mut scene, None);
    assert!(scene.damage_rect(0).is_none());
}

#[test]
fn nested_damage_is_presented_once_without_overflow() {
    let mut s = surface::Surface::new(1920, 1080).unwrap();
    let full = surface::Rect {
        x: 0,
        y: 0,
        w: 1920,
        h: 1080,
    };
    s.damage(full);
    for _ in 0..32 {
        s.damage(surface::Rect {
            x: 100,
            y: 100,
            w: 16,
            h: 16,
        });
    }
    assert_eq!(s.damage_rect(0), Some(full));
    assert!(s.damage_rect(1).is_none());
    flush(&mut s);
    s.damage(surface::Rect {
        x: 100,
        y: 100,
        w: 16,
        h: 16,
    });
    s.damage(full);
    assert_eq!(s.damage_rect(0), Some(full));
    assert!(s.damage_rect(1).is_none());
}

#[test]
fn pointer_motion_over_maximized_client_has_constant_damage() {
    let mut scene = surface::Surface::new(1920, 1080).unwrap();
    scene.fill(window::DESKTOP);
    let mut wm = window::WindowManager::desktop();
    wm.ensure_window_1(&mut scene);
    wm.mouse(125, 132, 1, &mut scene);
    wm.mouse(125, 132, 0, &mut scene);
    wm.system_panel(&mut scene, Some(sys::SystemInfo::default()));
    let mut cursor = cursor::Cursor::new(700, 400).unwrap();
    wm.mouse(700, 400, 0, &mut scene);
    wm.render(&mut scene);
    cursor.show(&mut scene);
    flush(&mut scene);
    for _ in 0..64 {
        cursor.advance(&mut scene, 5, 2);
        wm.mouse(cursor.x, cursor.y, 0, &mut scene);
        wm.render(&mut scene);
        cursor.show(&mut scene);
        let area: usize = (0..16)
            .filter_map(|i| scene.damage_rect(i))
            .map(|r| r.w * r.h)
            .sum();
        assert!(area <= 512, "pointer-only frame damaged {area} pixels");
        flush(&mut scene);
    }
}

#[test]
fn polished_desktop_scaled_hover_repair_and_system_information() {
    for (width, height) in [(1280, 800), (1920, 1080)] {
        let mut s = surface::Surface::new(width, height).unwrap();
        s.fill(window::DESKTOP);
        let mut wm = window::WindowManager::desktop();
        assert!(wm.windows.is_empty());
        wm.update_clock(&mut s, 12 * 3600 + 34 * 60);
        wm.render(&mut s);
        flush(&mut s);
        let dock = window::WindowManager::dock_rect(width, height);
        let scale = window::theme::scale(height);
        assert_eq!(dock.w, 4 * 64 * scale);
        for (x, y, down) in [
            (dock.x + 3 * 64 * scale + 20, dock.y + 20, 0),
            (dock.x + 3 * 64 * scale + 20, dock.y + 20, 1),
            (dock.x + 3 * 64 * scale + 20, dock.y + 20, 0),
            (20, 12, 0),
        ] {
            wm.mouse(x, y, down, &mut s);
            if wm.windows.iter().any(|w| w.id == 6) {
                wm.system_panel(
                    &mut s,
                    Some(sys::SystemInfo {
                        total_ram_bytes: 2 * 1024 * 1024 * 1024,
                        free_ram_bytes: 1536 * 1024 * 1024,
                        cpu_count: 4,
                        task_count: 12,
                        thread_count: 18,
                        uptime_ticks: 123400,
                        tick_hz: 100,
                        ..Default::default()
                    }),
                );
            }
            wm.render(&mut s);
            let mut full = surface::Surface::new(width, height).unwrap();
            full.fill(window::DESKTOP);
            wm.render(&mut full);
            let mismatch = s.pixels.iter().zip(&full.pixels).position(|(a, b)| a != b);
            assert!(
                mismatch.is_none(),
                "scaled hover repair mismatch {width}x{height}: {mismatch:?}"
            );
            flush(&mut s);
        }
        assert_eq!(wm.windows.len(), 1);
        assert_eq!(wm.windows[0].id, 6);
        let restore = wm.windows[0].rect;
        wm.mouse(restore.x + restore.w - 54, restore.y + 12, 1, &mut s);
        wm.mouse(restore.x + restore.w - 54, restore.y + 12, 0, &mut s);
        assert!(wm.windows[0].rect.y >= 24 * scale);
        assert!(wm.windows[0].rect.y + wm.windows[0].rect.h < dock.y);
        let max_w = width.saturating_sub(16 * scale);
        wm.mouse(8 * scale + max_w - 54, 28 * scale + 12, 1, &mut s);
        wm.mouse(8 * scale + max_w - 54, 28 * scale + 12, 0, &mut s);
        assert_eq!(wm.windows[0].rect, restore);
        // Missing telemetry and a zero tick frequency must render safely.
        wm.system_panel(&mut s, Some(sys::SystemInfo::default()));
        wm.system_panel(&mut s, None);
        wm.render(&mut s);
        flush(&mut s);
        wm.update_clock(&mut s, 12 * 3600 + 34 * 60 + 1);
        assert!(
            s.damage_rect(0).is_none(),
            "minute clock must not repaint each second"
        );
    }
}

#[test]
fn ui_font_is_proportional_antialiased_and_bounded() {
    use window::theme;
    assert!(theme::text_width("WWW") > theme::text_width("iii"));
    assert_eq!(theme::text_width("\u{2603}"), theme::text_width("?"));
    let mut antialias = false;
    for y in 0..18 {
        for x in 0..theme::text_width("FortressOS") {
            let p = theme::text_pixel("FortressOS", x, y, 0xffffffff, 0xff000000);
            antialias |= p != 0xffffffff && p != 0xff000000;
        }
    }
    assert!(antialias);
    assert_eq!(theme::text_pixel("x", usize::MAX, 99, 0xffffffff, 42), 42);
}
#[path = "../user/compositor/src/cursor.rs"]
mod cursor;
#[path = "../user/compositor/src/files.rs"]
mod files;
#[path = "../user/compositor/src/font.rs"]
mod font;
#[path = "../user/compositor/src/canvas.rs"]
mod canvas;
#[path = "../user/compositor/src/notepad.rs"]
mod notepad;
#[path = "../user/compositor/src/theme.rs"]
mod theme;
#[path = "../user/compositor/src/surface.rs"]
mod surface;
#[path = "../user/compositor/src/sys.rs"]
#[allow(dead_code)]
mod sys;
#[path = "../user/compositor/src/terminal.rs"]
mod terminal;
#[path = "../user/compositor/src/wallpaper.rs"]
#[allow(dead_code)]
mod wallpaper;
#[path = "../user/compositor/src/window.rs"]
mod window;
use surface::{Rect, Surface};
use window::WindowManager;

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
    // 17 distinct rectangles overflow the 16-entry list. Overflow merges
    // into the cheapest neighbour instead of presenting the whole screen:
    // every damaged pixel still reaches VRAM, row 3 was never damaged.
    for i in 0..17 {
        s.damage(Rect {
            x: i % 7,
            y: i / 7,
            w: 1,
            h: 1,
        });
    }
    fb.fb_size = 1;
    assert!(!unsafe { s.present(&fb) });
    fb.fb_size = 144;
    assert!(unsafe { s.present(&fb) });
    for i in 0..17 {
        assert_eq!(vram[(i / 7) * 9 + i % 7], 99, "damaged pixel {i}");
    }
    assert_eq!(vram[3 * 9 + 6], 0xdeadbeef);
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
        if let Some(i) = s.pixels.iter().zip(&full.pixels).position(|(a, b)| a != b) {
            panic!(
                "partial repair differs after {x},{y} at {},{}: {:08x} != {:08x}",
                i % s.width,
                i / s.width,
                s.pixels[i],
                full.pixels[i]
            );
        }
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
    assert_eq!(
        s.damage_rect(0),
        Some(Rect {
            x: 80,
            y: 120,
            w: 320,
            h: 200
        })
    );
    flush(&mut s);

    // Optimized monitor_mouse damage is bounded to the text sub-rectangle (<= 220x16 = 3,520 pixels)
    wm.monitor_mouse(&mut s, (150, 250));
    let damaged = s.damage_rect(0).expect("damage rect must exist");
    assert_eq!(
        damaged,
        Rect {
            x: 93,
            y: 229,
            w: 220,
            h: 16
        }
    );
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

#[test]
fn taskbar_clock_and_keyboard_cycle_focus() {
    let mut s = Surface::new(640, 480).unwrap();
    let mut wm = window::WindowManager::demo().unwrap();
    s.fill(window::DESKTOP);
    wm.render(&mut s);
    flush(&mut s);

    // Initial state: Window 2 is focused, clock is inactive (-1)
    assert!(wm.windows.iter().find(|w| w.id == 2).unwrap().is_focused);
    assert!(!wm.windows.iter().find(|w| w.id == 1).unwrap().is_focused);
    assert_eq!(wm.clock_sec, -1);

    // Update clock to 1761508800 (20:00:00 UTC)
    wm.update_clock(&mut s, 1761508800);
    assert_eq!(wm.clock_sec, 1761508800);
    let clk_rect = window::WindowManager::clock_rect(s.width, s.height);
    assert_eq!(s.damage_rect(0), Some(clk_rect));
    assert_eq!(s.damage_rect(1), None);
    wm.render(&mut s);
    flush(&mut s);

    // Calling again with the same second must produce zero damage
    wm.update_clock(&mut s, 1761508800);
    assert_eq!(s.damage_rect(0), None);

    // Cycle focus: switches focused window to Window 1
    wm.cycle_focus(&mut s);
    assert!(wm.windows.iter().find(|w| w.id == 1).unwrap().is_focused);
    assert!(!wm.windows.iter().find(|w| w.id == 2).unwrap().is_focused);

    // Cycle focus again: switches back to Window 2
    wm.cycle_focus(&mut s);
    assert!(wm.windows.iter().find(|w| w.id == 2).unwrap().is_focused);
    assert!(!wm.windows.iter().find(|w| w.id == 1).unwrap().is_focused);
}

#[test]
fn terminal_grid_and_scrolling() {
    let mut term = terminal::Terminal::new(3, -1, 100);
    assert_eq!((term.cursor_col, term.cursor_row), (0, 0));

    // Write simple string
    term.write_bytes(b"hello world\n");
    assert_eq!(term.cursor_col, 0);
    assert_eq!(term.cursor_row, 1);
    assert_eq!(&term.grid[0][..11], b"hello world");

    // Backspace handling
    term.write_bytes(b"abc\x08");
    assert_eq!(term.cursor_col, 2);

    // ANSI sequence: \033[K (clear from cursor to end of line)
    term.write_bytes(b"\x1b[K");
    assert_eq!(term.grid[1][2], b' ');

    // ANSI sequence: \033[5C (cursor forward by 5)
    term.write_bytes(b"\x1b[5C");
    assert_eq!(term.cursor_col, 7);

    // ANSI sequence: \033[2J (clear screen) and \033[H (home)
    term.write_bytes(b"\x1b[2J\x1b[H");
    assert_eq!((term.cursor_col, term.cursor_row), (0, 0));
    assert_eq!(term.grid[0][0], b' ');

    // Fill all rows to exercise scrolling
    for _ in 0..terminal::TERM_ROWS + 5 {
        term.write_bytes(b"line\n");
    }
    assert_eq!(term.cursor_row, terminal::TERM_ROWS - 1);

    // Verify rendering into Window content buffer
    let mut win = window::Window::new(
        3,
        "Terminal",
        Rect {
            x: 0,
            y: 0,
            w: 500,
            h: 260,
        },
    )
    .unwrap();
    term.render(&mut win);
    // Non-trivial glyphs must be rendered into content
    assert!(win.content.iter().any(|&c| c == terminal::TERM_FG));
}

#[test]
fn start_menu_toggle_and_actions() {
    let mut wm = WindowManager::demo().unwrap();
    let mut s = Surface::new(640, 480).unwrap();
    assert!(!wm.start_open);

    // Click Top-Left Fortress Menu button (x = 20, y = 12)
    let action = wm.mouse(20, 12, 1, &mut s);
    assert_eq!(action, window::Action::None);
    assert!(wm.start_open);
    wm.mouse(20, 12, 0, &mut s);

    // Hover over item 0 (Terminal) inside menu (menu y = 24; item 0 y = 24 + 24 = 48..70)
    wm.mouse(50, 58, 0, &mut s);
    assert_eq!(wm.menu_hover, Some(0));

    // Click item 0 (Terminal)
    let action = wm.mouse(50, 58, 1, &mut s);
    assert_eq!(action, window::Action::SpawnTerminal);
    assert!(!wm.start_open); // Menu automatically closes on selection
    wm.mouse(50, 58, 0, &mut s);

    // Reopen menu and click item 1 (Notepad) (y = 24 + 24 + 1*22 + 10 = 80)
    let _ = wm.mouse(20, 12, 1, &mut s);
    wm.mouse(20, 12, 0, &mut s);
    assert!(wm.start_open);

    let action = wm.mouse(50, 80, 1, &mut s);
    assert_eq!(action, window::Action::SpawnNotepad);
    assert!(!wm.start_open);
    wm.mouse(50, 80, 0, &mut s);

    // Reopen menu and click item 2 (Files) (y = 24 + 24 + 2*22 + 10 = 102)
    let _ = wm.mouse(20, 12, 1, &mut s);
    wm.mouse(20, 12, 0, &mut s);
    assert!(wm.start_open);

    let action = wm.mouse(50, 102, 1, &mut s);
    assert_eq!(action, window::Action::SpawnFileManager);
    assert!(!wm.start_open);
    wm.mouse(50, 102, 0, &mut s);

    // Reopen menu and click item 5 (Exit); Palette is not a desktop app.
    let _ = wm.mouse(20, 12, 1, &mut s);
    wm.mouse(20, 12, 0, &mut s);
    assert!(wm.start_open);

    let action = wm.mouse(50, 168, 1, &mut s);
    assert_eq!(action, window::Action::Exit);
    assert!(!wm.start_open);
    wm.mouse(50, 168, 0, &mut s);
}

#[test]
fn desktop_right_click_context_menu_actions_and_dismissal() {
    let mut wm = WindowManager::demo().unwrap();
    let mut s = Surface::new(640, 480).unwrap();
    assert!(wm.context_menu.is_none());

    // Right-click on window 1 (x = 100, y = 150) must NOT open desktop context menu
    let act = wm.mouse(100, 150, 2, &mut s);
    assert_eq!(act, window::Action::None);
    assert!(wm.context_menu.is_none());
    wm.mouse(100, 150, 0, &mut s);

    // Right-click on empty desktop (x = 580, y = 100) opens context menu
    let act = wm.mouse(580, 100, 2, &mut s);
    assert_eq!(act, window::Action::None);
    assert!(wm.context_menu.is_some());
    let (mx, my) = wm.context_menu.unwrap();
    assert_eq!(mx, 466); // 580 clamped to 640 - 174 = 466
    assert_eq!(my, 100);
    wm.mouse(580, 100, 0, &mut s);

    // Hover item 2 ("Open in Terminal": py in 47..67, so y = 100 + 55 = 155)
    wm.mouse(mx + 50, my + 55, 0, &mut s);
    assert_eq!(wm.context_hover, Some(2));

    // Left-click item 0 ("New File": y = 100 + 14 = 114)
    let act0 = wm.mouse(mx + 50, my + 14, 1, &mut s);
    assert_eq!(act0, window::Action::SpawnNotepad);
    assert!(wm.context_menu.is_none()); // Menu closed on selection
    wm.mouse(mx + 50, my + 14, 0, &mut s);

    // Reopen and click item 1 ("New Folder": y = 100 + 34 = 134)
    let _ = wm.mouse(580, 100, 2, &mut s);
    wm.mouse(580, 100, 0, &mut s);
    assert!(wm.context_menu.is_some());
    let act1 = wm.mouse(mx + 50, my + 34, 1, &mut s);
    assert_eq!(act1, window::Action::SpawnFileManager);
    assert!(wm.context_menu.is_none());
    wm.mouse(mx + 50, my + 34, 0, &mut s);

    // Reopen and click item 2 ("Open in Terminal": y = 100 + 55 = 155)
    let _ = wm.mouse(580, 100, 2, &mut s);
    wm.mouse(580, 100, 0, &mut s);
    assert!(wm.context_menu.is_some());
    let act2 = wm.mouse(mx + 50, my + 55, 1, &mut s);
    assert_eq!(act2, window::Action::SpawnTerminal);
    assert!(wm.context_menu.is_none());
    wm.mouse(mx + 50, my + 55, 0, &mut s);

    // Reopen and click item 3 ("Display Settings": y = 100 + 80 = 180)
    let _ = wm.mouse(580, 100, 2, &mut s);
    wm.mouse(580, 100, 0, &mut s);
    assert!(wm.context_menu.is_some());
    let act3 = wm.mouse(mx + 50, my + 80, 1, &mut s);
    assert_eq!(act3, window::Action::OpenDesktopApp(7));
    assert!(wm.context_menu.is_none());
    wm.mouse(mx + 50, my + 80, 0, &mut s);

    // Reopen and click item 4 ("Personalize": y = 100 + 100 = 200)
    let _ = wm.mouse(580, 100, 2, &mut s);
    wm.mouse(580, 100, 0, &mut s);
    assert!(wm.context_menu.is_some());
    let act4 = wm.mouse(mx + 50, my + 100, 1, &mut s);
    assert_eq!(act4, window::Action::OpenDesktopApp(7));
    assert!(wm.context_menu.is_none());
    wm.mouse(mx + 50, my + 100, 0, &mut s);

    // Reopen and click outside (e.g. at (10, 100)) dismisses menu
    let _ = wm.mouse(580, 100, 2, &mut s);
    wm.mouse(580, 100, 0, &mut s);
    assert!(wm.context_menu.is_some());
    let act_outside = wm.mouse(10, 100, 1, &mut s);
    assert_eq!(act_outside, window::Action::None);
    assert!(wm.context_menu.is_none());
    wm.mouse(10, 100, 0, &mut s);
}

#[test]
fn window_resizing_and_clamping() {
    let mut wm = WindowManager::demo().unwrap();
    let mut s = Surface::new(640, 480).unwrap();
    // Window 2 is at x = 260, y = 220, w = 280, h = 180.
    // Bottom-right corner is at x = 260 + 280 - 1 = 539, y = 220 + 180 - 1 = 399.
    assert_eq!(wm.hit_test(538, 398), Some((2, window::Hit::Resize)));

    // Click on resize handle
    let _ = wm.mouse(538, 398, 1, &mut s);
    assert!(wm.windows.iter().find(|w| w.id == 2).unwrap().is_resizing);

    // Drag to resize window 2 larger: x = 580, y = 430
    let act = wm.mouse(580, 430, 1, &mut s);
    assert_eq!(act, window::Action::WindowResized(2));
    let w2 = wm.windows.iter().find(|w| w.id == 2).unwrap();
    assert_eq!((w2.rect.w, w2.rect.h), (320, 210));

    // Release mouse
    let _ = wm.mouse(580, 430, 0, &mut s);
    assert!(!wm.windows.iter().find(|w| w.id == 2).unwrap().is_resizing);
}

#[test]
fn window_controls_close_minimize_maximize_and_restore() {
    let mut wm = WindowManager::demo().unwrap();
    let mut s = Surface::new(1280, 800).unwrap();
    // Add modern window (id 3) at (100, 100, 500, 300)
    let win3 = window::Window::new(
        3,
        "Terminal",
        Rect {
            x: 100,
            y: 100,
            w: 500,
            h: 300,
        },
    )
    .unwrap();
    wm.windows.push(win3);

    // Hit test Close button (x = 100 + 500 - 18 = 582, y = 100 + 12 = 112)
    assert_eq!(wm.hit_test(582, 112), Some((3, window::Hit::Close)));

    // Hit test Maximize button (x = 100 + 500 - 54 = 546, y = 100 + 12 = 112)
    assert_eq!(wm.hit_test(546, 112), Some((3, window::Hit::Maximize)));

    // Hit test Minimize button (x = 100 + 500 - 90 = 510, y = 100 + 12 = 112)
    assert_eq!(wm.hit_test(510, 112), Some((3, window::Hit::Minimize)));

    // Click Maximize button: Maximizes window 3!
    let act = wm.mouse(546, 112, 1, &mut s);
    let _ = wm.mouse(546, 112, 0, &mut s);
    assert_eq!(act, window::Action::WindowResized(3));
    let (max_x, max_y, max_w) = {
        let w3 = wm.windows.iter().find(|w| w.id == 3).unwrap();
        assert_eq!(w3.rect.x, 8);
        assert_eq!(w3.rect.y, 28);
        assert_eq!(w3.rect.w, 1280 - 16);
        assert_eq!(w3.rect.h, 800 - 104);
        assert!(w3.saved_rect.is_some());
        (w3.rect.x, w3.rect.y, w3.rect.w)
    };

    // Click Maximize button again: Restores window 3 back to (100, 100, 500, 300)!
    let act2 = wm.mouse(max_x + max_w - 54, max_y + 12, 1, &mut s);
    let _ = wm.mouse(max_x + max_w - 54, max_y + 12, 0, &mut s);
    assert_eq!(act2, window::Action::WindowResized(3));
    let w3_restored = wm.windows.iter().find(|w| w.id == 3).unwrap();
    assert_eq!(
        w3_restored.rect,
        Rect {
            x: 100,
            y: 100,
            w: 500,
            h: 300
        }
    );
    assert!(w3_restored.saved_rect.is_none());

    // Click Minimize button: Minimizes window 3!
    let _ = wm.mouse(510, 112, 1, &mut s);
    let _ = wm.mouse(510, 112, 0, &mut s);
    assert!(wm.windows.iter().find(|w| w.id == 3).unwrap().is_minimized);

    // Unminimize window 3
    wm.unminimize_and_raise(3, &mut s);
    assert!(!wm.windows.iter().find(|w| w.id == 3).unwrap().is_minimized);

    // Click Close button: Closes window 3!
    let _ = wm.mouse(582, 112, 1, &mut s);
    let _ = wm.mouse(582, 112, 0, &mut s);
    assert!(!wm.windows.iter().any(|w| w.id == 3));
}

#[test]
fn notepad_editing_and_rendering() {
    let mut np = notepad::Notepad::new("/tmp/test.txt");
    np.text.clear();
    np.insert_char(b'H');
    np.insert_char(b'i');
    np.insert_char(b'!');
    assert_eq!(&np.text, b"Hi!");
    assert_eq!(np.cursor, 3);
    assert!(np.modified);

    np.delete_back();
    assert_eq!(&np.text, b"Hi");
    assert_eq!(np.cursor, 2);

    // Same geometry as a 400x250 window's content, in a wider-stride buffer
    // (app buffers use the screen width as stride).
    let mut pixels = vec![0u32; 640 * 224];
    let mut c = canvas::Canvas::new(&mut pixels, 398, 224, 640).unwrap();
    np.render(&mut c);
    assert!(pixels.iter().any(|&p| p == notepad::NOTEPAD_FG));
    // Nothing is drawn beyond the canvas width inside the stride.
    for row in 0..224 {
        assert!(
            pixels[row * 640 + 398..row * 640 + 640]
                .iter()
                .all(|&p| p == 0)
        );
    }
}

#[test]
fn files_navigation_and_rendering() {
    let fm = files::FileManager::new(5, "/");
    assert_eq!(fm.current_path, "/");

    let mut win = window::Window::new(
        5,
        "Files",
        Rect {
            x: 0,
            y: 0,
            w: 400,
            h: 250,
        },
    )
    .unwrap();
    fm.render(&mut win);
    // Verified header and toolbar rendered
    assert!(
        win.content
            .iter()
            .any(|&c| c == files::FILES_DIR || c == files::FILES_FG)
    );
}

#[test]
fn notepad_mouse_and_arrow_keys() {
    let mut np = notepad::Notepad::new("/tmp/editor.txt");
    np.text.clear();
    np.cursor = 0;

    // Type text: "Hi\nWorld"
    assert!(np.handle_key(b'H'));
    assert!(np.handle_key(b'i'));
    assert!(np.handle_key(b'\n'));
    assert!(np.handle_key(b'W'));
    assert!(np.handle_key(b'o'));
    assert!(np.handle_key(b'r'));
    assert!(np.handle_key(b'l'));
    assert!(np.handle_key(b'd'));
    assert_eq!(&np.text, b"Hi\nWorld");
    assert_eq!(np.cursor, 8); // at end of "World"

    // ANSI Left arrow: \x1b [ D
    assert!(!np.handle_key(0x1b));
    assert!(!np.handle_key(b'['));
    assert!(np.handle_key(b'D'));
    assert_eq!(np.cursor, 7); // before 'd'

    // ANSI Home: \x1b [ H
    assert!(!np.handle_key(0x1b));
    assert!(!np.handle_key(b'['));
    assert!(np.handle_key(b'H'));
    assert_eq!(np.cursor, 3); // start of second line "World"

    // ANSI Up arrow: \x1b [ A
    assert!(!np.handle_key(0x1b));
    assert!(!np.handle_key(b'['));
    assert!(np.handle_key(b'A'));
    assert_eq!(np.cursor, 0); // start of first line "Hi"

    // ANSI End: \x1b [ F
    assert!(!np.handle_key(0x1b));
    assert!(!np.handle_key(b'['));
    assert!(np.handle_key(b'F'));
    assert_eq!(np.cursor, 2); // end of first line "Hi"

    // ANSI Down arrow: \x1b [ B
    assert!(!np.handle_key(0x1b));
    assert!(!np.handle_key(b'['));
    assert!(np.handle_key(b'B'));
    assert_eq!(np.cursor, 5); // col 2 on second line "Wo"

    // ANSI Delete: \x1b [ 3 ~
    assert!(!np.handle_key(0x1b));
    assert!(!np.handle_key(b'['));
    assert!(!np.handle_key(b'3'));
    assert!(np.handle_key(b'~'));
    assert_eq!(&np.text, b"Hi\nWold"); // 'r' deleted forward

    // Mouse click on text area: line 0, col 1
    // cx = 48 (gutter) + 1 * 8 = 56, cy = 24 (toolbar) + 4 = 28
    assert!(np.handle_click(56, 28));
    assert_eq!(np.cursor, 1);

    // Mouse click on toolbar [S] Save: cx = 20, cy = 8
    assert!(np.handle_click(20, 8));
}

#[test]
fn files_mouse_click_and_navigation() {
    let mut fm = files::FileManager::new(5, "/");
    assert_eq!(fm.current_path, "/");

    // Click [/bin] button on toolbar: cx = 110, cy = 8
    let act1 = fm.handle_click(110, 8);
    assert_eq!(act1, Some(files::FileAction::Refresh));
    assert_eq!(fm.current_path, "/bin");

    // Click [^ Up] button on toolbar: cx = 12, cy = 8
    let act2 = fm.handle_click(12, 8);
    assert_eq!(act2, Some(files::FileAction::Refresh));
    assert_eq!(fm.current_path, "/");

    // Click [/etc] button on toolbar: cx = 170, cy = 8
    let act3 = fm.handle_click(170, 8);
    assert_eq!(act3, Some(files::FileAction::Refresh));
    assert_eq!(fm.current_path, "/etc");

    // Click first item row: cy = 30 (offset from top bar)
    let _ = fm.handle_click(50, 30);
    assert_eq!(fm.selected, 0);
}

#[test]
fn terminal_dynamic_dimensions() {
    let mut t = terminal::Terminal::new(1, -1, -1);
    assert_eq!(t.cols, terminal::TERM_COLS);
    assert_eq!(t.rows, terminal::TERM_ROWS);

    // Resize to 100x30
    t.set_dimensions(100, 30);
    assert_eq!(t.cols, 100);
    assert_eq!(t.rows, 30);

    // Clamp upper bounds to 160x60
    t.set_dimensions(200, 80);
    assert_eq!(t.cols, terminal::TERM_MAX_COLS);
    assert_eq!(t.rows, terminal::TERM_MAX_ROWS);

    // Clamp lower bounds to 20x5
    t.set_dimensions(10, 2);
    assert_eq!(t.cols, 20);
    assert_eq!(t.rows, 5);
}

#[test]
fn dock_mouse_actions() {
    let mut s = Surface::new(1280, 800).unwrap();
    let mut wm = WindowManager::demo().unwrap();

    let dock_r = window::WindowManager::dock_rect(1280, 800);
    assert_eq!(dock_r.y, 800 - 66);
    assert_eq!(dock_r.h, 58);

    let click_y = dock_r.y + 20;

    // Item 0: Files
    let act0 = wm.mouse(dock_r.x + 0 * 64 + 20, click_y, 1, &mut s);
    let _ = wm.mouse(dock_r.x + 0 * 64 + 20, click_y, 0, &mut s);
    assert_eq!(act0, window::Action::SpawnFileManager);

    // Item 1: Term
    let act1 = wm.mouse(dock_r.x + 1 * 64 + 20, click_y, 1, &mut s);
    let _ = wm.mouse(dock_r.x + 1 * 64 + 20, click_y, 0, &mut s);
    assert_eq!(act1, window::Action::SpawnTerminal);

    // Item 2: Notes
    let act2 = wm.mouse(dock_r.x + 2 * 64 + 20, click_y, 1, &mut s);
    let _ = wm.mouse(dock_r.x + 2 * 64 + 20, click_y, 0, &mut s);
    assert_eq!(act2, window::Action::SpawnNotepad);

    // Removed demo-only dock entries cannot activate lock or exit.
    let act5 = wm.mouse(dock_r.x + 5 * 64 + 20, click_y, 1, &mut s);
    let _ = wm.mouse(dock_r.x + 5 * 64 + 20, click_y, 0, &mut s);
    assert_eq!(act5, window::Action::None);

    // Item 6: Exit
    let act6 = wm.mouse(dock_r.x + 6 * 64 + 20, click_y, 1, &mut s);
    let _ = wm.mouse(dock_r.x + 6 * 64 + 20, click_y, 0, &mut s);
    assert_eq!(act6, window::Action::None);
}

#[test]
fn rect_subtract_covers_difference_exactly() {
    use surface::Rect;
    let a = Rect {
        x: 10,
        y: 20,
        w: 100,
        h: 50,
    };
    for b in [
        Rect {
            x: 18,
            y: 22,
            w: 100,
            h: 50,
        },
        Rect {
            x: 0,
            y: 0,
            w: 30,
            h: 30,
        },
        Rect {
            x: 40,
            y: 30,
            w: 10,
            h: 10,
        },
        Rect {
            x: 500,
            y: 500,
            w: 10,
            h: 10,
        },
        a,
    ] {
        let parts = a.subtract(b);
        for y in 0..120 {
            for x in 0..160 {
                let want = a.contains(x, y) && !b.contains(x, y);
                let hits = parts.iter().flatten().filter(|p| p.contains(x, y)).count();
                assert_eq!(hits, want as usize, "({x},{y}) a-{b:?}");
            }
        }
    }
}

#[test]
fn damage_overflow_merges_instead_of_full_screen() {
    let mut s = surface::Surface::new(1920, 1080).unwrap();
    for i in 0..40 {
        s.damage(surface::Rect {
            x: (i % 10) * 150,
            y: (i / 10) * 200,
            w: 20,
            h: 20,
        });
    }
    let rects: Vec<_> = (0..16).filter_map(|i| s.damage_rect(i)).collect();
    let area: usize = rects.iter().map(|r| r.w * r.h).sum();
    assert!(area < 1920 * 1080 / 2, "overflow repainted {area} px");
    for i in 0..40 {
        let (x, y) = ((i % 10) * 150 + 5, (i / 10) * 200 + 5);
        assert!(
            rects.iter().any(|r| r.contains(x, y)),
            "lost damage at ({x},{y})"
        );
    }
}

/// Incremental frames (split drag damage + occlusion culling) must leave
/// exactly the pixels a full repaint of the final state produces.
#[test]
fn incremental_drag_repair_matches_full_repaint() {
    use surface::Rect;
    use window::{Window, WindowManager};
    let (w, h) = (1280, 800);
    let full = Rect { x: 0, y: 0, w, h };
    let mut wm = WindowManager::desktop();
    let mut s = Surface::new(w, h).unwrap();
    for (id, x, y, ww, hh) in [
        (4, 100, 120, 500, 320),
        (5, 300, 200, 520, 360),
        (10, 700, 150, 400, 300),
        (11, 650, 420, 300, 200),
    ] {
        let mut win = Window::new(id, "Window", Rect { x, y, w: ww, h: hh }).unwrap();
        for (i, p) in win.content.iter_mut().enumerate() {
            *p = 0xff000000 | (i as u32).wrapping_mul(2654435761) >> 8;
        }
        wm.windows.push(win);
    }
    s.damage(full);
    wm.render(&mut s);
    flush(&mut s);
    let reference = |wm: &WindowManager| {
        wm.clear_render_caches();
        let mut r = Surface::new(w, h).unwrap();
        r.damage(full);
        wm.render(&mut r);
        r.pixels
    };

    // Drag the bottom window (raised on press) over the others, diagonally,
    // in big jumps, partly off-screen and back.
    let (mut px, mut py) = (140usize, 130usize);
    wm.mouse(px, py, 0, &mut s);
    wm.mouse(px, py, 1, &mut s);
    wm.render(&mut s);
    flush(&mut s);
    for (dx, dy) in [(8i32, 2i32); 40]
        .into_iter()
        .chain([(-3, 7); 30])
        .chain([(97, -41); 6])
        .chain([(-180, 90); 4])
        .chain([(25, 25); 20])
    {
        px = (px as i32 + dx).clamp(0, w as i32 - 1) as usize;
        py = (py as i32 + dy).clamp(30, h as i32 - 1) as usize;
        wm.mouse(px, py, 1, &mut s);
        wm.render(&mut s);
        flush(&mut s);
    }
    wm.mouse(px, py, 0, &mut s);
    wm.render(&mut s);
    flush(&mut s);
    assert!(s.pixels == reference(&wm), "drag left stale pixels");

    // Move a window that sits *under* the others: exposed desktop and the
    // lower window must repair correctly around the occluding cores.
    for step in 0..25 {
        let old = wm.windows[0].damage_rect();
        wm.windows[0].rect.x = 60 + step * 13;
        wm.windows[0].rect.y = 90 + step * 7;
        let new = wm.windows[0].damage_rect();
        s.damage(new);
        for e in old.subtract(new).into_iter().flatten() {
            s.damage(e);
        }
        wm.render(&mut s);
        flush(&mut s);
    }
    assert!(
        s.pixels == reference(&wm),
        "occluded move left stale pixels"
    );
}
#[test]
fn background_rows_match_per_pixel_reference() {
    for (sw, sh, cw, ch) in [
        (1920, 1080, 800, 600),
        (1280, 800, 1280, 800),
        (640, 480, 800, 600),
        (1024, 768, 2000, 100),
    ] {
        let pixels: Vec<u32> = (0..cw * ch)
            .map(|i| 0xff000000 | (i as u32).wrapping_mul(2654435761) >> 8)
            .collect();
        let wp = wallpaper::Wallpaper {
            orig_w: cw as u32,
            orig_h: ch as u32,
            bg_color: 0xff123456,
            crop_w: cw as u32,
            crop_h: ch as u32,
            pixels,
        };
        for y in [0, 1, sh / 3, sh / 2, sh - 1] {
            for (x0, len) in [(0, sw), (7, 1), (sw / 4, sw / 2), (sw - 3, 3)] {
                let mut row = vec![0u32; len];
                wp.fill_row(x0, y, sw, sh, &mut row);
                for (i, &p) in row.iter().enumerate() {
                    assert_eq!(
                        p,
                        wp.pixel(x0 + i, y, sw, sh),
                        "wallpaper {sw}x{sh} ({},{y})",
                        x0 + i
                    );
                }
                window::theme::desktop_row(x0, y, sw, sh, &mut row);
                for (i, &p) in row.iter().enumerate() {
                    assert_eq!(
                        p,
                        window::theme::desktop_pixel(x0 + i, y, sw, sh),
                        "desktop ({},{y})",
                        x0 + i
                    );
                }
            }
        }
    }
}

/// Cached title bars must follow focus, hover, maximize and title changes.
#[test]
fn cached_title_bars_follow_state_changes() {
    use surface::Rect;
    use window::{Window, WindowManager};
    let (w, h) = (1280, 800);
    let full = Rect { x: 0, y: 0, w, h };
    let mut wm = WindowManager::desktop();
    for (id, x) in [(4, 100), (5, 600)] {
        wm.windows.push(
            Window::new(
                id,
                "Cache test",
                Rect {
                    x,
                    y: 150,
                    w: 480,
                    h: 300,
                },
            )
            .unwrap(),
        );
    }
    let mut s = Surface::new(w, h).unwrap();
    s.damage(full);
    wm.render(&mut s);
    flush(&mut s);
    let check = |wm: &WindowManager, s: &Surface, what: &str| {
        wm.clear_render_caches();
        let mut r = Surface::new(w, h).unwrap();
        r.damage(full);
        wm.render(&mut r);
        assert!(s.pixels == r.pixels, "stale title bar after {what}");
    };
    // Hover each title button, click the lower window (focus swap), then
    // maximize and restore it through its title bar.
    for (x, y, b, what) in [
        (1060, 162, 0, "close hover"),
        (1000, 162, 0, "maximize hover"),
        (960, 162, 0, "minimize hover"),
        (300, 300, 0, "leave"),
        (300, 165, 1, "focus press"),
        (300, 165, 0, "focus release"),
    ] {
        wm.mouse(x, y, b, &mut s);
        wm.render(&mut s);
        flush(&mut s);
        check(&wm, &s, what);
    }
    wm.windows.last_mut().unwrap().title = "Renamed window".into();
    let d = wm.windows.last().unwrap().damage_rect();
    s.damage(d);
    wm.render(&mut s);
    flush(&mut s);
    check(&wm, &s, "rename");
}

#[path = "../user/compositor/src/privilege.rs"]
#[allow(dead_code)]
mod privilege;

fn launch(vars: &[&str]) -> Result<privilege::Launch, &'static str> {
    let owned: Vec<std::ffi::CString> = vars
        .iter()
        .map(|v| std::ffi::CString::new(*v).unwrap())
        .collect();
    let mut ptrs: Vec<*const u8> = owned.iter().map(|c| c.as_ptr() as *const u8).collect();
    ptrs.push(std::ptr::null());
    privilege::launch_from_env(ptrs.as_ptr())
}

#[test]
fn sudo_identity_is_parsed_strictly() {
    use privilege::Launch;
    // No sudo variables, or invoked by root: stay root (with a warning).
    assert_eq!(launch(&["PATH=/bin", "USER=root"]), Ok(Launch::StayRoot));
    assert_eq!(launch(&["SUDO_UID=0", "SUDO_GID=0"]), Ok(Launch::StayRoot));
    assert_eq!(
        privilege::launch_from_env(std::ptr::null()),
        Ok(Launch::StayRoot)
    );
    // The normal sudo case.
    match launch(&[
        "HOME=/root",
        "SUDO_UID=1000",
        "SUDO_GID=1000",
        "SUDO_USER=operator",
        "SUDO_GROUPS=1000,10,44,104",
    ]) {
        Ok(Launch::Drop(id)) => {
            assert_eq!((id.uid, id.gid), (1000, 1000));
            assert_eq!(&id.groups[..id.ngroups], &[1000, 10, 44, 104]);
        }
        other => panic!("{other:?}"),
    }
    match launch(&["SUDO_UID=1000", "SUDO_GID=1000"]) {
        Ok(Launch::Drop(id)) => assert_eq!(id.ngroups, 0),
        other => panic!("{other:?}"),
    }
    // Anything malformed or incomplete is refused, never guessed.
    for bad in [
        &["SUDO_UID=1000"][..],
        &["SUDO_UID=", "SUDO_GID=1000"],
        &["SUDO_UID=10x0", "SUDO_GID=1000"],
        &["SUDO_UID=-1", "SUDO_GID=1000"],
        &["SUDO_UID=4294967296", "SUDO_GID=1000"],
        &["SUDO_UID=1000", "SUDO_GID=abc"],
        &["SUDO_UID=1000", "SUDO_GID=0"],
        &["SUDO_UID=1000", "SUDO_GID=1000", "SUDO_GROUPS=10,0"],
        &["SUDO_UID=1000", "SUDO_GID=1000", "SUDO_GROUPS=10,,44"],
        &[
            "SUDO_UID=1000",
            "SUDO_GID=1000",
            "SUDO_GROUPS=1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17",
        ],
    ] {
        assert!(launch(bad).is_err(), "accepted {bad:?}");
    }
}

#[path = "../user/compositor/src/proto.rs"]
mod proto;

#[test]
fn protocol_messages_round_trip_and_reject_bad_titles() {
    let mut m = proto::Msg::new(proto::REQ_DAMAGE, 1, 2, 3, 4);
    m.len = 5;
    m.text[..5].copy_from_slice(b"Hello");
    let bytes = m.to_bytes();
    assert_eq!(bytes.len(), 64);
    assert_eq!(&bytes[..4], &proto::REQ_DAMAGE.to_le_bytes());
    assert_eq!(proto::Msg::from_bytes(&bytes), m);
    assert_eq!(m.title(), Some("Hello"));
    for (len, text) in [
        (0, &b""[..]),
        (41, &b"x"[..]),
        (u32::MAX, &b"x"[..]),
        (3, &b"a\x01b"[..]),
        (2, &b"\xc3\xa9"[..]),
    ] {
        let mut bad = proto::Msg::default();
        bad.len = len;
        bad.text[..text.len()].copy_from_slice(text);
        assert_eq!(bad.title(), None, "accepted len={len} {text:?}");
    }
}

#[test]
fn damage_from_apps_is_clipped_without_overflow() {
    let d =
        |x, y, w, h| proto::clip_damage(&proto::Msg::new(proto::REQ_DAMAGE, x, y, w, h), 400, 300);
    assert_eq!(d(0, 0, 400, 300), Some((0, 0, 400, 300)));
    assert_eq!(d(10, 20, 30, 40), Some((10, 20, 30, 40)));
    assert_eq!(d(390, 290, 100, 100), Some((390, 290, 10, 10)));
    assert_eq!(d(0, 0, u32::MAX, u32::MAX), Some((0, 0, 400, 300)));
    assert_eq!(d(399, 299, u32::MAX, u32::MAX), Some((399, 299, 1, 1)));
    assert_eq!(d(400, 0, 10, 10), None);
    assert_eq!(d(0, 300, 10, 10), None);
    assert_eq!(d(u32::MAX, u32::MAX, u32::MAX, u32::MAX), None);
    assert_eq!(d(5, 5, 0, 10), None);
    assert_eq!(d(5, 5, 10, 0), None);
}

#[test]
fn canvas_rejects_undersized_buffers_and_clips() {
    let mut small = vec![0u32; 99];
    assert!(canvas::Canvas::new(&mut small, 10, 10, 10).is_none());
    let mut wide = vec![0u32; 100];
    assert!(canvas::Canvas::new(&mut wide, 11, 10, 10).is_none());
    let mut pixels = vec![0u32; 16 * 8];
    let mut c = canvas::Canvas::new(&mut pixels, 12, 8, 16).unwrap();
    c.fill_rect(8, 4, usize::MAX, usize::MAX, 7);
    c.glyph(10, 0, b'A', 1, 2); // does not fit: ignored
    c.text(11, 7, "clipped", 1, 2);
    for y in 0..8 {
        for x in 0..16 {
            let inside_rect = x >= 8 && x < 12 && y >= 4;
            if x >= 12 {
                assert_eq!(pixels[y * 16 + x], 0, "drew past width at {x},{y}");
            } else if inside_rect && !(x == 11 && y == 7) {
                assert_eq!(pixels[y * 16 + x], 7);
            }
        }
    }
}
