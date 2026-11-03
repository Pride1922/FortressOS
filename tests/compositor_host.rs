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
#[path = "../user/compositor/src/clipboard.rs"]
mod clipboard;
#[path = "../user/compositor/src/file_dialog.rs"]
mod file_dialog;
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
#[path = "libui_golden.rs"]
mod libui_golden;
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
    let mut term = terminal::Terminal::new();
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

    // Render into a 500x260 window's content, in a wider-stride buffer.
    let mut pixels = vec![0u32; 700 * 234];
    let mut c = canvas::Canvas::new(&mut pixels, 498, 234, 700).unwrap();
    term.render(&mut c);
    assert!(pixels.iter().any(|&p| p == terminal::TERM_FG));
    for row in 0..234 {
        assert!(
            pixels[row * 700 + 498..row * 700 + 700]
                .iter()
                .all(|&p| p == 0)
        );
    }
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
    let fm = files::FileManager::new("/");
    assert_eq!(fm.current_path, "/");

    let mut pixels = vec![0u32; 640 * 224];
    let mut c = canvas::Canvas::new(&mut pixels, 398, 224, 640).unwrap();
    fm.render(&mut c);
    assert!(
        pixels
            .iter()
            .any(|&c| c == files::FILES_DIR || c == files::FILES_FG)
    );
    for row in 0..224 {
        assert!(
            pixels[row * 640 + 398..row * 640 + 640]
                .iter()
                .all(|&p| p == 0)
        );
    }
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
    let mut fm = files::FileManager::new("/");
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
    let mut t = terminal::Terminal::new();
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

fn open_msg(kind: u32, a: u32, text: &[u8]) -> proto::Msg {
    let mut m = proto::Msg::new(kind, a, 0, 0, 0);
    m.text[..text.len()].copy_from_slice(text);
    m.len = text.len() as u32;
    m
}

#[test]
fn open_requests_assemble_only_well_formed_absolute_paths() {
    use proto::{OpenRequest, REQ_OPEN_APPEND as A, REQ_OPEN_BEGIN as B};
    // A 100-byte path in three chunks.
    let path: Vec<u8> = std::iter::once(b'/')
        .chain(std::iter::repeat(b'x').take(99))
        .collect();
    let mut r = OpenRequest::default();
    assert!(r.begin(&open_msg(B, 100, b"")));
    for chunk in path.chunks(40) {
        assert!(r.append(&open_msg(A, 0, chunk)));
    }
    assert_eq!(r.commit().unwrap().to_bytes(), &path[..]);
    // Reusable after a commit.
    assert!(r.begin(&open_msg(B, 4, b"")));
    assert!(r.append(&open_msg(A, 0, b"/tmp")));
    assert_eq!(r.commit().unwrap().to_bytes(), b"/tmp");

    let mut r = OpenRequest::default();
    assert!(!r.begin(&open_msg(B, 0, b"")), "empty");
    assert!(!r.begin(&open_msg(B, 128, b"")), "too long");
    assert!(!r.begin(&open_msg(B, u32::MAX, b"")), "huge");
    assert!(!r.append(&open_msg(A, 0, b"/x")), "append without begin");
    assert!(r.commit().is_none(), "commit without begin");
    assert!(r.begin(&open_msg(B, 5, b"")));
    assert!(!r.begin(&open_msg(B, 5, b"")), "nested begin");
    assert!(
        !r.append(&open_msg(A, 0, b"/tmp/x")),
        "overflow past declared length"
    );
    assert!(r.append(&open_msg(A, 0, b"/tm")));
    assert!(r.commit().is_none(), "incomplete");
    for bad in [&b"tmp/x"[..], b"/a\x01b", b"/a\xffb"] {
        let mut r = OpenRequest::default();
        assert!(r.begin(&open_msg(B, bad.len() as u32, b"")));
        let ok = r.append(&open_msg(A, 0, bad)) && r.commit().is_some();
        assert!(!ok, "accepted {bad:?}");
    }
    let mut r = OpenRequest::default();
    assert!(r.begin(&open_msg(B, 3, b"")));
    let mut over = open_msg(A, 0, b"/ab");
    over.len = 41;
    assert!(!r.append(&over), "chunk length beyond the text field");
}

#[test]
fn files_render_hostile_names_inside_the_canvas() {
    let mut fm = files::FileManager::new("/nonexistent-for-test");
    fm.entries.clear();
    let names = [
        String::from("a\u{1}b\u{7f}c\r\n\t"),
        "x".repeat(63),
        String::from("ünïcødé ✓ 名前"),
        String::new(),
        "y".repeat(400),
    ];
    for (i, name) in names.iter().enumerate() {
        fm.entries.push(files::FileEntry {
            name: name.clone(),
            is_dir: i % 2 == 0,
            is_exe: i == 1,
            size: u64::MAX >> i,
        });
    }
    for (w, h) in [(398, 224), (60, 60), (1918, 1054)] {
        let stride = w + 37;
        let mut pixels = vec![0u32; stride * h];
        let mut c = canvas::Canvas::new(&mut pixels, w, h, stride).unwrap();
        for selected in 0..names.len() {
            fm.selected = selected;
            fm.ensure_visible(files::FileManager::rows_for(h));
            fm.render(&mut c);
        }
        for row in 0..h {
            assert!(
                pixels[row * stride + w..row * stride + stride]
                    .iter()
                    .all(|&p| p == 0)
            );
        }
    }
}

/// The shell's output is untrusted: arbitrary bytes (including every
/// escape-sequence prefix) must never panic the emulator or draw outside
/// its canvas, at any size it can be configured to.
#[test]
fn terminal_survives_arbitrary_output() {
    let mut seed = 0x9e3779b97f4a7c15u64;
    let mut next = || {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        seed
    };
    for (w, h) in [(498, 234), (60, 40), (1918, 1054)] {
        let stride = w + 21;
        let mut pixels = vec![0u32; stride * h];
        let mut term = terminal::Terminal::new();
        let (cols, rows) = terminal::Terminal::grid_for(w, h);
        term.set_dimensions(cols, rows);
        for round in 0..200 {
            let mut chunk = [0u8; 512];
            for b in chunk.iter_mut() {
                let r = next();
                // Bias toward ESC, '[', digits, ';' and controls.
                *b = match r % 8 {
                    0 => 0x1b,
                    1 => b'[',
                    2 => b'0' + (r >> 8) as u8 % 10,
                    3 => b';',
                    4 => (r >> 8) as u8 % 32,
                    _ => (r >> 8) as u8,
                };
            }
            term.write_bytes(&chunk);
            let mut c = canvas::Canvas::new(&mut pixels, w, h, stride).unwrap();
            if round % 3 == 0 {
                term.render(&mut c);
            } else if let Some((y, dh)) = term.render_dirty(&mut c) {
                assert!(y + dh <= h);
            }
            assert!(term.cursor_row < term.rows && term.cursor_col <= term.cols);
        }
        for row in 0..h {
            assert!(
                pixels[row * stride + w..row * stride + stride]
                    .iter()
                    .all(|&p| p == 0)
            );
        }
    }
}

/// Regression test: binary output without a trailing newline (e.g. cat of an ELF)
/// followed by the shell's line redraw sequences must not drop or hide echoed
/// characters or corrupt the active row.
#[test]
fn terminal_regression_binary_output_and_shell_redraw() {
    for (w, h) in [(520, 280), (498, 234)] {
        let stride = w + 16;
        let mut pixels = vec![0u32; stride * h];
        let mut c = canvas::Canvas::new(&mut pixels, w, h, stride).unwrap();

        let mut term = terminal::Terminal::new();
        let (cols, rows) = terminal::Terminal::grid_for(w, h);
        term.set_dimensions(cols, rows);
        term.render(&mut c);

        // When clean, render_dirty must return None without clearing or redrawing.
        assert_eq!(term.render_dirty(&mut c), None);

        // 1. Feed binary output that ends without a newline and has arbitrary bytes
        // including ESC, control characters and printable text (simulating `cat /bin/shm-test`).
        if let Ok(elf) = std::fs::read("build/shm-test.elf") {
            term.write_bytes(&elf);
        } else {
            // Standalone synthetic binary output without newline
            let mut fake_elf = vec![0x7fu8, b'E', b'L', b'F'];
            fake_elf.extend_from_slice(&[0x1b, 0x06, 0x00, 0x1b, 0xfd, 0xff, 0xff, b'A']);
            fake_elf.extend_from_slice(b".shstrtab.text.rodata.bss@A!@\0\r\t(p");
            term.write_bytes(&fake_elf);
        }
        let _ = term.render_dirty(&mut c);

        // 2. Shell line-discipline draws the prompt and clears the line tail
        let prompt = b"operator@fortress:/ $ ";
        let plen = prompt.len();
        let mut s = Vec::new();
        s.extend_from_slice(b"\r\x1b[?25l");
        s.extend_from_slice(prompt);
        s.extend_from_slice(b"\x1b[K\r\x1b[");
        s.extend_from_slice(format!("{}C\x1b[?25h", plen).as_bytes());
        term.write_bytes(&s);
        let _ = term.render_dirty(&mut c);

        // 3. User types `echo alive > /tmp/x`: each keystroke invokes the shell's paint()
        let command = b"echo alive > /tmp/x";
        for i in 1..=command.len() {
            let text = &command[..i];
            let mut redraw = Vec::new();
            redraw.extend_from_slice(b"\r\x1b[?25l");
            redraw.extend_from_slice(prompt);
            redraw.extend_from_slice(text);
            redraw.extend_from_slice(b"\x1b[K\r\x1b[");
            redraw.extend_from_slice(format!("{}C\x1b[?25h", plen + text.len()).as_bytes());
            term.write_bytes(&redraw);

            let dirty = term.render_dirty(&mut c);
            assert!(dirty.is_some(), "keystroke {} must produce dirty damage", i);

            // Active row grid must contain both the full prompt and typed command
            let row_bytes = &term.grid[term.cursor_row][..cols];
            let expected_prefix = [&prompt[..], text].concat();
            assert!(
                row_bytes.starts_with(&expected_prefix),
                "keystroke {}: row does not start with expected text! Expected {:?}, got {:?}",
                i,
                String::from_utf8_lossy(&expected_prefix),
                String::from_utf8_lossy(row_bytes)
            );
        }

        // Full typed command must be intact in the grid
        let final_row = &term.grid[term.cursor_row][..cols];
        let full_expected = [&prompt[..], &command[..]].concat();
        assert!(final_row.starts_with(&full_expected));
    }
}

/// Tests FileManager with large directories (>16 entries) covering scrolling,
/// keyboard selection, item click resolution, and bounds-checked canvas rendering.
#[test]
fn files_large_directory_scrolling_and_selection() {
    let mut fm = files::FileManager::new("/test-large-dir");
    fm.entries.clear();

    // Populate with 80 entries (well beyond the legacy 16-slot limit)
    for i in 0..80 {
        fm.entries.push(files::FileEntry {
            name: format!("document_{:03}.txt", i),
            is_dir: i % 5 == 0,
            is_exe: false,
            size: (i as u64 + 1) * 1024,
        });
    }

    assert_eq!(fm.entries.len(), 80);
    assert_eq!(fm.selected, 0);
    assert_eq!(fm.scroll, 0);

    let h = 300;
    let visible_rows = files::FileManager::rows_for(h);
    assert!(visible_rows > 0);

    // Scroll down to entry 50
    for i in 1..=50 {
        fm.selected = i;
        fm.ensure_visible(visible_rows);
    }
    assert_eq!(fm.selected, 50);
    assert!(fm.scroll > 0);
    assert!(fm.selected >= fm.scroll && fm.selected < fm.scroll + visible_rows);

    // Click on item at row 3 within visible viewport: cy = 26 + 3 * 20
    let click_res = fm.handle_click(100, 26 + 3 * 20);
    let expected_idx = fm.scroll + 3;
    assert_eq!(fm.selected, expected_idx);
    if fm.entries[expected_idx].is_dir {
        assert_eq!(click_res, Some(files::FileAction::Refresh));
    } else {
        assert_eq!(
            click_res,
            Some(files::FileAction::OpenPath(format!(
                "/test-large-dir/document_{:03}.txt",
                expected_idx
            )))
        );
    }

    // Render large directory into canvas with pitch padding
    let w = 400;
    let stride = w + 27;
    let mut pixels = vec![0u32; stride * h];
    let mut c = canvas::Canvas::new(&mut pixels, w, h, stride).unwrap();
    fm.render(&mut c);

    for row in 0..h {
        assert!(
            pixels[row * stride + w..row * stride + stride]
                .iter()
                .all(|&p| p == 0),
            "drawing leaked into pitch padding on row {}",
            row
        );
    }
}

/// Tests FileManager handling of long file names (up to and beyond 63/64 bytes)
/// ensuring full path integrity on enter_selected and safe glyph clipping.
#[test]
fn files_long_filenames_handling() {
    let mut fm = files::FileManager::new("/test-long-names");
    fm.entries.clear();

    let name_63 = "a".repeat(63);
    let name_64 = "b".repeat(64);
    let name_128 = "c".repeat(128);
    let name_255 = "d".repeat(255);

    fm.entries.push(files::FileEntry {
        name: name_63.clone(),
        is_dir: false,
        is_exe: false,
        size: 100,
    });
    fm.entries.push(files::FileEntry {
        name: name_64.clone(),
        is_dir: false,
        is_exe: false,
        size: 200,
    });
    fm.entries.push(files::FileEntry {
        name: name_128.clone(),
        is_dir: true,
        is_exe: false,
        size: 0,
    });
    fm.entries.push(files::FileEntry {
        name: name_255.clone(),
        is_dir: false,
        is_exe: false,
        size: 400,
    });

    // 63-byte file open path
    fm.selected = 0;
    assert_eq!(
        fm.enter_selected(),
        Some(format!("/test-long-names/{}", name_63))
    );

    // 64-byte file open path
    fm.selected = 1;
    assert_eq!(
        fm.enter_selected(),
        Some(format!("/test-long-names/{}", name_64))
    );

    // 255-byte file open path
    fm.selected = 3;
    assert_eq!(
        fm.enter_selected(),
        Some(format!("/test-long-names/{}", name_255))
    );

    // 128-byte directory enter (modifies current_path and clears entries via refresh)
    fm.selected = 2;
    assert_eq!(fm.enter_selected(), None);
    assert_eq!(fm.current_path, format!("/test-long-names/{}", name_128));

    // Render with varying widths ensuring clipping
    for (w, h) in [(180, 200), (450, 300)] {
        let stride = w + 19;
        let mut pixels = vec![0u32; stride * h];
        let mut c = canvas::Canvas::new(&mut pixels, w, h, stride).unwrap();
        fm.render(&mut c);

        for row in 0..h {
            assert!(
                pixels[row * stride + w..row * stride + stride]
                    .iter()
                    .all(|&p| p == 0)
            );
        }
    }
}

/// Tests structured key press, repeat, and release event encoding, wire layout,
/// and delivery through EV_KEY protocol messages.
#[test]
fn key_press_and_release_event_delivery() {
    use proto::{
        EV_KEY, KEY_A, KEY_DELETE, KEY_LEFT, KEY_STATE_PRESS, KEY_STATE_RELEASE,
        KEY_STATE_REPEAT, KEY_UP, KeyEvent, MOD_NUMLOCK,
    };

    let press = KeyEvent::new(KEY_A, MOD_NUMLOCK, KEY_STATE_PRESS, b'a');
    let raw = press.to_bytes();
    assert_eq!(raw.len(), 8);
    assert_eq!(KeyEvent::from_bytes(&raw), press);

    let msg_press = press.to_msg();
    assert_eq!(msg_press.kind, EV_KEY);
    assert_eq!(msg_press.a, b'a' as u32);
    assert_eq!(msg_press.b, KEY_A as u32);
    assert_eq!(msg_press.c, MOD_NUMLOCK as u32);
    assert_eq!(msg_press.d, KEY_STATE_PRESS as u32);

    let wire = msg_press.to_bytes();
    let decoded_msg = proto::Msg::from_bytes(&wire);
    assert_eq!(KeyEvent::from_msg(&decoded_msg), Some(press));
    assert!(press.is_press_or_repeat());

    // Typematic repeat event preserves keycode, modifiers, and ascii
    let repeat = KeyEvent::new(KEY_A, MOD_NUMLOCK, KEY_STATE_REPEAT, b'a');
    let msg_repeat = repeat.to_msg();
    assert_eq!(msg_repeat.d, KEY_STATE_REPEAT as u32);
    assert_eq!(
        KeyEvent::from_msg(&proto::Msg::from_bytes(&msg_repeat.to_bytes())),
        Some(repeat)
    );
    assert!(repeat.is_press_or_repeat());

    // Key release event carries state=0 and ascii=0 while preserving keycode and modifiers
    let release = KeyEvent::new(KEY_A, MOD_NUMLOCK, KEY_STATE_RELEASE, 0);
    let msg_release = release.to_msg();
    assert_eq!(msg_release.kind, EV_KEY);
    assert_eq!(msg_release.a, 0);
    assert_eq!(msg_release.b, KEY_A as u32);
    assert_eq!(msg_release.c, MOD_NUMLOCK as u32);
    assert_eq!(msg_release.d, KEY_STATE_RELEASE as u32);
    assert_eq!(
        KeyEvent::from_msg(&proto::Msg::from_bytes(&msg_release.to_bytes())),
        Some(release)
    );
    assert!(!release.is_press_or_repeat());

    // Navigation keys synthesize VT100 sequences on press/repeat and empty on release
    let up_press = KeyEvent::new(KEY_UP, 0, KEY_STATE_PRESS, 0);
    let up_release = KeyEvent::new(KEY_UP, 0, KEY_STATE_RELEASE, 0);
    let left_repeat = KeyEvent::new(KEY_LEFT, 0, KEY_STATE_REPEAT, 0);
    let del_press = KeyEvent::new(KEY_DELETE, 0, KEY_STATE_PRESS, 0);
    assert_eq!(up_press.vt100_sequence(), b"\x1b[A");
    assert_eq!(up_release.vt100_sequence(), b"");
    assert_eq!(left_repeat.vt100_sequence(), b"\x1b[D");
    assert_eq!(del_press.vt100_sequence(), b"\x1b[3~");
}

/// Tests modifier bitmask tracking (Shift, Ctrl, Alt, Super, CapsLock, NumLock, AltGr)
/// across modifier press, chorded key press, and modifier release events.
#[test]
fn key_modifier_bitmask_tracking() {
    use proto::{
        KEY_A, KEY_C, KEY_LEFTALT, KEY_LEFTCTRL, KEY_LEFTMETA, KEY_LEFTSHIFT, KEY_RIGHTALT,
        KEY_STATE_PRESS, KEY_STATE_RELEASE, KeyEvent, MOD_ALT, MOD_ALTGR, MOD_CAPS, MOD_CTRL,
        MOD_NUMLOCK, MOD_SHIFT, MOD_SUPER,
    };

    assert_eq!(MOD_SHIFT, 1);
    assert_eq!(MOD_CTRL, 2);
    assert_eq!(MOD_ALT, 4);
    assert_eq!(MOD_SUPER, 8);
    assert_eq!(MOD_CAPS, 16);
    assert_eq!(MOD_NUMLOCK, 32);
    assert_eq!(MOD_ALTGR, 64);

    let sequence = [
        // 1. Press Left Shift: MOD_SHIFT set, ascii = 0
        KeyEvent::new(KEY_LEFTSHIFT, MOD_SHIFT, KEY_STATE_PRESS, 0),
        // 2. Press 'A' while Shift held: MOD_SHIFT set, ascii = b'A'
        KeyEvent::new(KEY_A, MOD_SHIFT, KEY_STATE_PRESS, b'A'),
        // 3. Release 'A' while Shift still held: MOD_SHIFT still set, ascii = 0
        KeyEvent::new(KEY_A, MOD_SHIFT, KEY_STATE_RELEASE, 0),
        // 4. Press Left Ctrl while Shift held: MOD_SHIFT | MOD_CTRL
        KeyEvent::new(KEY_LEFTCTRL, MOD_SHIFT | MOD_CTRL, KEY_STATE_PRESS, 0),
        // 5. Press Left Alt: MOD_SHIFT | MOD_CTRL | MOD_ALT
        KeyEvent::new(
            KEY_LEFTALT,
            MOD_SHIFT | MOD_CTRL | MOD_ALT,
            KEY_STATE_PRESS,
            0,
        ),
        // 6. Press Right Alt (AltGr) + Super + Caps + NumLock
        KeyEvent::new(
            KEY_RIGHTALT,
            MOD_SHIFT | MOD_CTRL | MOD_ALT | MOD_ALTGR | MOD_SUPER | MOD_CAPS | MOD_NUMLOCK,
            KEY_STATE_PRESS,
            0,
        ),
        // 7. Release Left Shift: MOD_SHIFT cleared, others remain
        KeyEvent::new(
            KEY_LEFTSHIFT,
            MOD_CTRL | MOD_ALT,
            KEY_STATE_RELEASE,
            0,
        ),
        // 8. Release Left Alt: only MOD_CTRL remains
        KeyEvent::new(KEY_LEFTALT, MOD_CTRL, KEY_STATE_RELEASE, 0),
        // 9. Press 'c' under Ctrl: ascii = 3 (ETX), MOD_CTRL set
        KeyEvent::new(KEY_C, MOD_CTRL, KEY_STATE_PRESS, 3),
        // 10. Release Left Ctrl: modifiers = 0
        KeyEvent::new(KEY_LEFTCTRL, 0, KEY_STATE_RELEASE, 0),
        // 11. Press and release Super
        KeyEvent::new(KEY_LEFTMETA, MOD_SUPER, KEY_STATE_PRESS, 0),
        KeyEvent::new(KEY_LEFTMETA, 0, KEY_STATE_RELEASE, 0),
    ];

    for ev in sequence {
        let msg = proto::Msg::from_bytes(&ev.to_msg().to_bytes());
        assert_eq!(msg.c as u16, ev.modifiers);
        let roundtrip = KeyEvent::from_msg(&msg).unwrap();
        assert_eq!(roundtrip, ev);
    }
}

/// Tests SAK (Ctrl + Alt + Esc) interception and global compositor shortcuts
/// (Alt + Tab, Super), verifying that SAK never reaches client windows, releases
/// active capture/focus on the client window, and raises the task manager.
#[test]
fn sak_interception_and_global_shortcuts_prevent_client_delivery() {
    use proto::{
        KEY_A, KEY_ESC, KEY_LEFTMETA, KEY_STATE_PRESS, KEY_STATE_RELEASE, KEY_TAB, KeyEvent,
        MOD_ALT, MOD_CTRL, MOD_SUPER,
    };
    use surface::{Rect, Surface};
    use window::{Window, WindowManager};

    let mut s = Surface::new(1280, 800).unwrap();
    let mut wm = WindowManager::desktop();

    // Add two client windows (ids 100 and 101), with 101 focused and actively dragging
    let w1 = Window::new(
        100,
        "Background Client",
        Rect {
            x: 100,
            y: 100,
            w: 300,
            h: 200,
        },
    )
    .unwrap();
    let mut w2 = Window::new(
        101,
        "Focused Rogue Client",
        Rect {
            x: 200,
            y: 150,
            w: 320,
            h: 220,
        },
    )
    .unwrap();
    w2.is_focused = true;
    w2.is_dragging = true;
    wm.windows.push(w1);
    wm.windows.push(w2);
    wm.start_open = true;
    wm.context_menu = Some((300, 300));

    let mut delivered_to_client: Vec<(u32, KeyEvent)> = Vec::new();
    fn route_event(
        ev: KeyEvent,
        wm: &mut WindowManager,
        s: &mut Surface,
        delivered: &mut Vec<(u32, KeyEvent)>,
    ) {
        if wm.intercept_key(ev, s) {
            return;
        }
        if let Some(id) = wm.focused_window_id() {
            if id >= 100 {
                delivered.push((id, ev));
            }
        }
    }

    // Ordinary key press and release go to focused client 101
    let key_a_down = KeyEvent::new(KEY_A, 0, KEY_STATE_PRESS, b'a');
    let key_a_up = KeyEvent::new(KEY_A, 0, KEY_STATE_RELEASE, 0);
    route_event(key_a_down, &mut wm, &mut s, &mut delivered_to_client);
    route_event(key_a_up, &mut wm, &mut s, &mut delivered_to_client);
    assert_eq!(
        delivered_to_client,
        vec![(101, key_a_down), (101, key_a_up)]
    );

    // Plain Esc or Ctrl+Esc (without Alt) is NOT SAK and reaches focused client 101
    let plain_esc = KeyEvent::new(KEY_ESC, 0, KEY_STATE_PRESS, 27);
    let ctrl_esc = KeyEvent::new(KEY_ESC, MOD_CTRL, KEY_STATE_PRESS, 27);
    assert!(!plain_esc.is_sak());
    assert!(!ctrl_esc.is_sak());
    route_event(plain_esc, &mut wm, &mut s, &mut delivered_to_client);
    route_event(ctrl_esc, &mut wm, &mut s, &mut delivered_to_client);
    assert_eq!(delivered_to_client.len(), 4);

    // Alt + Tab is intercepted by compositor: cycles focus to window 100, not sent to clients
    let alt_tab = KeyEvent::new(KEY_TAB, MOD_ALT, KEY_STATE_PRESS, b'\t');
    route_event(alt_tab, &mut wm, &mut s, &mut delivered_to_client);
    assert_eq!(delivered_to_client.len(), 4);
    assert_eq!(wm.focused_window_id(), Some(100));

    // Alt + Tab again cycles focus back to client 101
    route_event(alt_tab, &mut wm, &mut s, &mut delivered_to_client);
    assert_eq!(wm.focused_window_id(), Some(101));

    // Super key toggles Start menu at compositor level and is never sent to clients
    assert!(wm.start_open);
    let super_press = KeyEvent::new(KEY_LEFTMETA, MOD_SUPER, KEY_STATE_PRESS, 0);
    let super_release = KeyEvent::new(KEY_LEFTMETA, 0, KEY_STATE_RELEASE, 0);
    route_event(super_press, &mut wm, &mut s, &mut delivered_to_client);
    route_event(super_release, &mut wm, &mut s, &mut delivered_to_client);
    assert!(!wm.start_open);
    assert_eq!(delivered_to_client.len(), 4);

    // Re-open start menu with Super press
    route_event(super_press, &mut wm, &mut s, &mut delivered_to_client);
    assert!(wm.start_open);

    // Re-engage dragging on client 101 and trigger SAK (Ctrl + Alt + Esc)
    wm.windows.iter_mut().find(|w| w.id == 101).unwrap().is_dragging = true;
    let sak_press = KeyEvent::new(KEY_ESC, MOD_CTRL | MOD_ALT, KEY_STATE_PRESS, 27);
    let sak_release = KeyEvent::new(KEY_ESC, MOD_CTRL | MOD_ALT, KEY_STATE_RELEASE, 0);
    assert!(sak_press.is_sak());
    route_event(sak_press, &mut wm, &mut s, &mut delivered_to_client);
    route_event(sak_release, &mut wm, &mut s, &mut delivered_to_client);

    // SAK was never delivered to client 101
    assert_eq!(delivered_to_client.len(), 4);

    // Client 101 lost focus and drag capture; menus closed; Task Manager (id=6) raised and focused
    let w101 = wm.windows.iter().find(|w| w.id == 101).unwrap();
    assert!(!w101.is_focused);
    assert!(!w101.is_dragging);
    assert!(!wm.start_open);
    assert!(wm.context_menu.is_none());
    assert_eq!(wm.focused_window_id(), Some(6));

    // Subsequent keys no longer go to client 101
    route_event(key_a_down, &mut wm, &mut s, &mut delivered_to_client);
    assert_eq!(delivered_to_client.len(), 4);

    // Direct emergency_unfocus clears all focus so focused_window_id() is None
    wm.emergency_unfocus(&mut s);
    assert_eq!(wm.focused_window_id(), None);
}

/// Tests backward compatibility: existing apps that only inspect `m.a as u8`
/// continue receiving typed characters unchanged.
#[test]
fn ev_key_backward_compatibility_for_byte_consumers() {
    use proto::{
        EV_KEY, KEY_BACKSPACE, KEY_ENTER, KEY_H, KEY_LEFTSHIFT, KEY_STATE_PRESS,
        KEY_STATE_RELEASE, KeyEvent, MOD_SHIFT, Msg,
    };

    let mut np = notepad::Notepad::new("/tmp/compat.txt");
    np.text.clear();
    np.cursor = 0;

    // Stream of extended key events including modifier press, character presses, and key releases
    let events = [
        KeyEvent::new(KEY_LEFTSHIFT, MOD_SHIFT, KEY_STATE_PRESS, 0),
        KeyEvent::new(KEY_H, MOD_SHIFT, KEY_STATE_PRESS, b'H'),
        KeyEvent::new(KEY_H, MOD_SHIFT, KEY_STATE_RELEASE, 0),
        KeyEvent::new(KEY_LEFTSHIFT, 0, KEY_STATE_RELEASE, 0),
        KeyEvent::new(23, 0, KEY_STATE_PRESS, b'i'),
        KeyEvent::new(23, 0, KEY_STATE_RELEASE, 0),
        KeyEvent::new(KEY_ENTER, 0, KEY_STATE_PRESS, b'\n'),
        KeyEvent::new(KEY_ENTER, 0, KEY_STATE_RELEASE, 0),
        KeyEvent::new(KEY_BACKSPACE, 0, KEY_STATE_PRESS, 0x08),
        KeyEvent::new(KEY_BACKSPACE, 0, KEY_STATE_RELEASE, 0),
    ];

    for ev in events {
        let m = Msg::from_bytes(&ev.to_msg().to_bytes());
        assert_eq!(m.kind, EV_KEY);
        // Legacy byte consumer inspects `m.a as u8` (non-zero on printable/control presses)
        let byte = m.a as u8;
        if byte != 0 {
            np.handle_key(byte);
        }
    }
    assert_eq!(np.text, b"Hi");

    // Legacy Msg::new(EV_KEY, byte, 0, 0, 0) with b=0, c=0, d=0 still decodes as a press
    let legacy_msg = Msg::new(EV_KEY, b'Z' as u32, 0, 0, 0);
    let decoded = KeyEvent::from_msg(&legacy_msg).unwrap();
    assert_eq!(decoded.ascii, b'Z');
    assert_eq!(decoded.state, KEY_STATE_PRESS);
    assert!(decoded.is_press_or_repeat());

    // KeyEvent::from_ascii maps ASCII bytes to press events with matching ascii field
    let from_ascii = KeyEvent::from_ascii(b'Q');
    assert_eq!(from_ascii.ascii, b'Q');
    assert_eq!(from_ascii.keycode, proto::KEY_Q);
    assert_eq!(from_ascii.modifiers, MOD_SHIFT);
    assert_eq!(from_ascii.state, KEY_STATE_PRESS);
}

#[test]
fn clipboard_history_notepad_files_and_settings_integration() {
    clipboard::clear();
    assert_eq!(clipboard::get(), None);
    assert_eq!(clipboard::history_count(), 0);

    // 1. History ring stores up to 8 deduplicated entries
    assert!(clipboard::set("first-entry"));
    assert!(clipboard::set("second-entry"));
    assert!(clipboard::set("second-entry")); // duplicate top is deduplicated
    assert_eq!(clipboard::history_count(), 2);
    assert_eq!(clipboard::get().as_deref(), Some("second-entry"));
    assert_eq!(clipboard::history_get(0).as_deref(), Some("second-entry"));
    assert_eq!(clipboard::history_get(1).as_deref(), Some("first-entry"));

    // 2. Notepad Ctrl+C, Ctrl+X, Ctrl+V, and toolbar Copy/Paste buttons
    let mut np = notepad::Notepad::new("/tmp/clip.txt");
    np.text = b"Alpha Line\nBeta Line\n".to_vec();
    np.cursor = 2; // Inside "Alpha Line"
    assert!(np.handle_key(3)); // Ctrl+C copies "Alpha Line"
    assert_eq!(clipboard::get().as_deref(), Some("Alpha Line"));

    // Move cursor to end and press Ctrl+V (22)
    np.cursor = np.text.len();
    assert!(np.handle_key(22));
    assert_eq!(np.text, b"Alpha Line\nBeta Line\nAlpha Line");

    // Move cursor to "Beta Line" and press Ctrl+X (24)
    np.cursor = 12;
    assert!(np.handle_key(24));
    assert_eq!(clipboard::get().as_deref(), Some("Beta Line"));
    assert_eq!(np.text, b"Alpha Line\nAlpha Line");

    // Toolbar Copy (x=90, y=8) and Paste (x=150, y=8)
    np.cursor = 0;
    assert!(np.handle_click(90, 8)); // Copy "Alpha Line"
    assert_eq!(clipboard::get().as_deref(), Some("Alpha Line"));
    np.cursor = np.text.len();
    assert!(np.handle_click(150, 8)); // Paste at end
    assert_eq!(np.text, b"Alpha Line\nAlpha LineAlpha Line");

    // 3. FileManager copy_selected_path (Ctrl+C)
    let mut fm = files::FileManager::new("/etc");
    fm.entries.push(files::FileEntry {
        name: alloc::string::String::from("desktop.conf"),
        is_dir: false,
        is_exe: false,
        size: 42,
    });
    fm.selected = fm.entries.len() - 1;
    assert!(fm.copy_selected_path());
    assert_eq!(clipboard::get().as_deref(), Some("/etc/desktop.conf"));

    // 4. Settings Ctrl+V paste into focused text controls (Timezone)
    let mut s = Surface::new(800, 600).unwrap();
    let mut wm = WindowManager::new(&mut s);
    let mut host = desktop::AppHost::load(true).unwrap();
    host.open(9, &mut wm, &mut s); // Time & Language (focus=0 -> id=1 zone input)
    assert!(clipboard::set("Europe/Paris"));
    host.zone.clear();
    host.key(9, 22, &mut wm, &mut s); // Ctrl+V pastes into zone via Text::edit
    assert_eq!(host.zone.as_str(), "Europe/Paris");

    // 5. Widget-level Text::edit and Text::edit_secret clipboard routing
    let mut input = preferences::Text::<32>::from_str("Fortress-5G");
    assert!(!input.edit(3)); // Ctrl+C copies without mutating
    assert_eq!(clipboard::get().as_deref(), Some("Fortress-5G"));
    assert!(input.edit(24)); // Ctrl+X cuts and clears field
    assert_eq!(input.as_str(), "");

    let mut secret = preferences::Text::<64>::default();
    assert!(secret.edit_secret(22)); // Ctrl+V pastes into masked secret input
    assert_eq!(secret.as_str(), "Fortress-5G");
    assert!(clipboard::set("public-clip"));
    assert!(!secret.edit_secret(3)); // Ctrl+C blocked on secret input
    assert!(!secret.edit_secret(24)); // Ctrl+X blocked on secret input
    assert_eq!(clipboard::get().as_deref(), Some("public-clip"));
    assert_eq!(secret.as_str(), "Fortress-5G");
}

#[test]
fn alt_tab_window_switcher_hud_lifecycle_and_rendering() {
    use proto::{
        KEY_LEFTALT, KEY_STATE_PRESS, KEY_STATE_RELEASE, KEY_TAB, KeyEvent, MOD_ALT, MOD_SHIFT,
    };

    let mut s = Surface::new(800, 600).unwrap();
    let mut wm = WindowManager::desktop();
    let w1 = window::Window::new(
        3,
        "Terminal - /bin/sh",
        Rect {
            x: 60,
            y: 60,
            w: 360,
            h: 220,
        },
    )
    .unwrap();
    let w2 = window::Window::new(
        4,
        "Notes - /tmp/notes.txt",
        Rect {
            x: 120,
            y: 100,
            w: 360,
            h: 220,
        },
    )
    .unwrap();
    let mut w3 = window::Window::new(
        5,
        "Files - /etc",
        Rect {
            x: 180,
            y: 140,
            w: 360,
            h: 220,
        },
    )
    .unwrap();
    w3.is_focused = true;
    wm.windows.push(w1);
    wm.windows.push(w2);
    wm.windows.push(w3);

    assert!(!wm.switcher_open);
    assert_eq!(wm.focused_window_id(), Some(5));

    // Press Alt+Tab: opens switcher HUD and cycles focus to window 3
    let alt_tab = KeyEvent::new(KEY_TAB, MOD_ALT, KEY_STATE_PRESS, b'\t');
    assert!(wm.intercept_key(alt_tab, &mut s));
    assert!(wm.switcher_open);
    assert_eq!(wm.switcher_selected, 3);
    assert_eq!(wm.focused_window_id(), Some(3));

    // Render switcher HUD and verify center card pixels are drawn
    wm.render(&mut s);
    let card = WindowManager::switcher_card_rect(s.width, s.height, 3);
    assert!(card.w >= 200 && card.h >= 100);

    // Press Alt+Shift+Tab: cycles backwards to window 5
    let alt_shift_tab = KeyEvent::new(KEY_TAB, MOD_ALT | MOD_SHIFT, KEY_STATE_PRESS, b'\t');
    assert!(wm.intercept_key(alt_shift_tab, &mut s));
    assert!(wm.switcher_open);
    assert_eq!(wm.switcher_selected, 5);
    assert_eq!(wm.focused_window_id(), Some(5));

    // Release Left Alt: closes switcher HUD and keeps window 5 focused at top
    let alt_release = KeyEvent::new(KEY_LEFTALT, 0, KEY_STATE_RELEASE, 0);
    assert!(wm.intercept_key(alt_release, &mut s));
    assert!(!wm.switcher_open);
    assert_eq!(wm.focused_window_id(), Some(5));
}

#[test]
fn active_shadow_dock_vibrancy_and_magnification() {
    let mut s = Surface::new(800, 600).unwrap();
    let full = Rect {
        x: 0,
        y: 0,
        w: 800,
        h: 600,
    };
    let mut wm = WindowManager::desktop();

    let w_inactive = window::Window::new(
        3,
        "Inactive",
        Rect {
            x: 60,
            y: 80,
            w: 240,
            h: 160,
        },
    )
    .unwrap();
    let mut w_active = window::Window::new(
        4,
        "Active",
        Rect {
            x: 420,
            y: 80,
            w: 240,
            h: 160,
        },
    )
    .unwrap();
    w_active.is_focused = true;
    wm.windows.push(w_inactive);
    wm.windows.push(w_active);

    s.damage(full);
    wm.render(&mut s);
    flush(&mut s);

    // 1. At distance d=6 below each window (y = 80 + 160 + 2 + 6), the active window casts a
    //    shadow whereas the inactive window's tight 4px shadow has already ended.
    let y_d6 = 80 + 160 + 2 + 6;
    let inactive_d6 = s.pixels[y_d6 * s.width + 180];
    let inactive_bg = window::theme::desktop_pixel(180, y_d6, s.width, s.height);
    assert_eq!(inactive_d6, inactive_bg, "inactive window shadow should stop at 4px");

    let active_d6 = s.pixels[y_d6 * s.width + 540];
    let active_bg = window::theme::desktop_pixel(540, y_d6, s.width, s.height);
    assert_ne!(active_d6, active_bg, "active window should cast an 8px shadow");

    // 2. Hover Dock slot 0 and verify both slot 0 (hovered) and slot 1 (neighbor) magnify,
    //    and incremental hover repair matches full composition.
    let dock = WindowManager::dock_rect(s.width, s.height);
    let slot0_icon_top_y = dock.y + 7;
    let slot0_center_x = dock.x + 32;
    let slot1_neighbor_y = dock.y + 10;
    let slot1_center_x = dock.x + 64 + 32;
    let before_slot0 = s.pixels[slot0_icon_top_y * s.width + slot0_center_x];
    let before_slot1 = s.pixels[slot1_neighbor_y * s.width + slot1_center_x];

    wm.mouse(slot0_center_x, dock.y + 24, 0, &mut s);
    wm.render(&mut s);
    flush(&mut s);

    assert_ne!(
        s.pixels[slot0_icon_top_y * s.width + slot0_center_x],
        before_slot0,
        "hovered dock icon should magnify upward into y=7"
    );
    assert_ne!(
        s.pixels[slot1_neighbor_y * s.width + slot1_center_x],
        before_slot1,
        "adjacent dock icon should magnify upward into y=10"
    );

    let mut full_ref = Surface::new(800, 600).unwrap();
    full_ref.damage(full);
    wm.render(&mut full_ref);
    assert_eq!(s.pixels, full_ref.pixels, "incremental dock magnification must match full render");
}

#[test]
fn notepad_and_terminal_text_selection_copy_cut_paste() {
    let mut np = notepad::Notepad::new("/tmp/sel.txt");
    np.text = b"Hello Fortress World\nSecond Line\n".to_vec();
    np.cursor = 0;

    // 1. Mouse click at col 6 ("Fortress") and drag to col 14
    assert!(np.handle_click(48 + 6 * 8, 28));
    assert_eq!(np.selected_range(), None);
    assert!(np.handle_drag(48 + 14 * 8, 28));
    assert_eq!(np.selected_range(), Some((6, 14)));

    // Copy only the selected range ("Fortress")
    assert!(np.handle_key(3)); // Ctrl+C
    assert_eq!(clipboard::get().as_deref(), Some("Fortress"));

    // 2. Shift+Right (\x1b[c) extends selection by 6 more chars (" World")
    for &b in b"\x1b[c\x1b[c\x1b[c\x1b[c\x1b[c\x1b[c" {
        let _ = np.handle_key(b);
    }
    assert_eq!(np.selected_range(), Some((6, 20)));

    // Cut selected range ("Fortress World")
    assert!(np.handle_key(24)); // Ctrl+X
    assert_eq!(clipboard::get().as_deref(), Some("Fortress World"));
    assert_eq!(np.text, b"Hello \nSecond Line\n");

    // 3. Ctrl+A (1) selects all, and Ctrl+V (22) replaces selection with clipboard
    assert!(np.handle_key(1)); // Ctrl+A
    assert_eq!(np.selected_range(), Some((0, np.text.len())));
    assert!(np.handle_key(22)); // Ctrl+V
    assert_eq!(np.text, b"Fortress World");

    // 4. Ctrl+Right (\x1b[W) and Ctrl+Left (\x1b[Q) word-by-word selection, plus Ctrl+W (23) and double-click
    np.cursor = 0;
    np.selection_anchor = None;
    for &b in b"\x1b[W" {
        let _ = np.handle_key(b);
    }
    assert_eq!(np.selected_range(), Some((0, 8))); // "Fortress"
    assert!(np.handle_key(3)); // Ctrl+C
    assert_eq!(clipboard::get().as_deref(), Some("Fortress"));

    // Double-click on "World" (col 10) selects the entire word "World"
    assert!(np.handle_click(48 + 10 * 8, 28));
    assert!(np.handle_click(48 + 10 * 8, 28));
    assert_eq!(np.selected_range(), Some((9, 14))); // "World"
    assert!(np.handle_key(3)); // Ctrl+C
    assert_eq!(clipboard::get().as_deref(), Some("World"));

    // Ctrl+W (23) at col 3 selects "Fortress"
    np.cursor = 3;
    np.selection_anchor = None;
    assert!(np.handle_key(23));
    assert_eq!(np.selected_range(), Some((0, 8)));

    // 5. Terminal mouse click-and-drag text selection
    let mut term = terminal::Terminal::new();
    term.write_bytes(b"fortress$ uname -a\r\nFortressOS x86_64\r\n");
    assert!(!term.handle_click(10 + 0 * 8, 8 + 1 * 16)); // row 1, col 0 ("FortressOS")
    assert!(term.handle_drag(10 + 9 * 8, 8 + 1 * 16)); // row 1, col 9
    assert_eq!(term.selected_text().as_deref(), Some("FortressOS"));
    term.clear_selection();
    assert_eq!(term.selected_text(), None);
}

#[test]
fn settings_clock_sync_lifecycle_and_status() {
    let mut s = Surface::new(1280, 800).unwrap();
    let mut wm = WindowManager::desktop();
    let mut host = desktop::AppHost::new(preferences::Preferences::default());

    // Open Time & Language (id 9)
    host.open(9, &mut wm, &mut s);
    assert_eq!(wm.focused_window_id(), Some(9));
    assert_eq!(host.apps[2].kind, 2);
    assert!(!host.clock_synced);
    assert!(!host.clock_sync_in_progress);

    // Initial render
    host.render(9, &mut wm, &mut s);
    flush(&mut s);

    // 1. Click "Sync Clock" button (control 8 at x = 650, y = 78)
    host.pointer(9, 650, 78, &mut wm, &mut s);
    assert!(host.clock_sync_in_progress);
    assert!(host.status.as_str().contains("Synchronizing clock"));

    // 2. Refresh while sync is in progress (sys state = 1)
    sys::test_set_clock_sync_state(1);
    host.refresh(1761508800, &mut wm, &mut s);
    assert!(host.clock_sync_in_progress);
    assert!(!host.clock_synced);

    // 3. Refresh when sync completes successfully (sys state = 0)
    sys::test_set_clock_sync_state(0);
    host.refresh(1761508801, &mut wm, &mut s);
    assert!(!host.clock_sync_in_progress);
    assert!(host.clock_synced);
    assert!(host.status.as_str().contains("Clock synchronized successfully"));

    // Render updated card showing green "NTP Synced" badge
    host.render(9, &mut wm, &mut s);
    flush(&mut s);

    // 4. Trigger failure path
    sys::test_set_clock_sync_state(-1);
    host.sync_clock();
    assert!(host.clock_sync_in_progress);
    host.refresh(1761508802, &mut wm, &mut s);
    assert!(!host.clock_sync_in_progress);
    assert!(host.status.as_str().contains("NTP sync failed"));
}

#[test]
fn settings_timezone_preset_autosave_normalization_and_clock_fallback() {
    let mut s = Surface::new(1280, 800).unwrap();
    let mut wm = WindowManager::desktop();
    let mut host = desktop::AppHost::new(preferences::Preferences::default());

    // 1. Verify timezone normalization
    assert_eq!(desktop::normalize_timezone("Europe Brussles"), "Europe/Brussels");
    assert_eq!(desktop::normalize_timezone("Europe/Brussles"), "Europe/Brussels");
    assert_eq!(desktop::normalize_timezone("brussels"), "Europe/Brussels");
    assert_eq!(desktop::normalize_timezone("Brussels"), "Europe/Brussels");
    assert_eq!(desktop::normalize_timezone("belgium"), "Europe/Brussels");
    assert_eq!(desktop::normalize_timezone("CET"), "Europe/Brussels");
    assert_eq!(desktop::normalize_timezone("utc"), "UTC");
    assert_eq!(desktop::normalize_timezone("new york"), "America/New_York");
    assert_eq!(desktop::normalize_timezone("tokyo"), "Asia/Tokyo");

    // 2. Open Time & Language (id 9)
    host.open(9, &mut wm, &mut s);

    // 3. Click Brussels preset (command 4, control 4)
    // Control 4 is at x=274..370, y=206..232
    host.pointer(9, 300, 215, &mut wm, &mut s);
    assert_eq!(host.zone.as_str(), "Europe/Brussels");

    // Click Tokyo preset (command 6, control 6 at x=482..552, y=206..232)
    host.pointer(9, 500, 215, &mut wm, &mut s);
    assert_eq!(host.zone.as_str(), "Asia/Tokyo");

    // Click Brussels preset again
    host.pointer(9, 300, 215, &mut wm, &mut s);
    assert_eq!(host.zone.as_str(), "Europe/Brussels");

    // 4. Clock fallback: when local clock is unavailable (as in test harness),
    // refresh with a valid timestamp (1761508800 = 20:00:00 UTC) must fallback to UTC
    // and NOT leave local_seconds as None or clock_sec as -1.
    host.refresh(1761508800, &mut wm, &mut s);
    assert!(host.local_seconds.is_some());
    assert_eq!(host.local_seconds.unwrap(), 1761508800);
    assert_eq!(wm.clock_sec, 1761508800);
    assert!(host.calendar.is_some());
    let cal = host.calendar.unwrap();
    assert_eq!(cal.hour, 20);
    assert_eq!(cal.minute, 0);
    assert_eq!(cal.second, 0);
}

#[test]
fn desktop_restart_action_and_launcher_marker() {
    assert!(!privilege::desktop_session_from_env(core::ptr::null()));
    let yes=std::ffi::CString::new("SUDO_DESKTOP_SESSION=1").unwrap();
    let no=std::ffi::CString::new("SUDO_DESKTOP_SESSION=0").unwrap();
    let short=std::ffi::CString::new("S").unwrap();
    let vars=[short.as_ptr() as *const u8,yes.as_ptr() as *const u8,core::ptr::null()];
    assert!(privilege::desktop_session_from_env(vars.as_ptr()));
    let vars=[no.as_ptr() as *const u8,core::ptr::null()];
    assert!(!privilege::desktop_session_from_env(vars.as_ptr()));
    let mut scene=surface::Surface::new(1280,800).unwrap();
    let mut wm=window::WindowManager::new(&mut scene);
    wm.toggle_start_menu(&mut scene);
    let menu=window::WindowManager::start_menu_rect(scene.height);
    let scale=if scene.height>=1800 {2} else {1};
    let action=wm.mouse(menu.x+30*scale,menu.y+(24+6*22+10)*scale,1,&mut scene);
    assert_eq!(action,window::Action::Restart);
}

#[test]
fn battery_icon_and_power_menu_lifecycle() {
    let bat_normal = sys::BatteryStatus {
        capacity_percent: 85,
        state: sys::BATTERY_STATE_CHARGING,
        time_remaining_mins: 0,
        voltage_mv: 11400,
        ac_online: sys::AC_POWER_ONLINE,
        temperature_c: 45,
    };
    let bat_low = sys::BatteryStatus {
        capacity_percent: 15,
        state: sys::BATTERY_STATE_DISCHARGING,
        time_remaining_mins: 45,
        voltage_mv: 10800,
        ac_online: sys::AC_POWER_OFFLINE,
        temperature_c: 42,
    };
    let bat_crit = sys::BatteryStatus {
        capacity_percent: 5,
        state: sys::BATTERY_STATE_DISCHARGING,
        time_remaining_mins: 15,
        voltage_mv: 10200,
        ac_online: sys::AC_POWER_OFFLINE,
        temperature_c: 40,
    };
    let bat_none = sys::BatteryStatus {
        capacity_percent: 0,
        state: sys::BATTERY_STATE_UNKNOWN,
        time_remaining_mins: 0,
        voltage_mv: 0,
        ac_online: sys::AC_POWER_UNKNOWN,
        temperature_c: 0,
    };

    // 1. Direct pixel rendering verification
    assert_eq!(ui::render_battery_pixel(20, 4, &bat_normal, 0), Some(0xffcbd5e1));
    assert_eq!(ui::render_battery_pixel(0, 0, &bat_normal, 0x123456), Some(0x123456));
    assert_eq!(ui::render_battery_pixel(0, 5, &bat_normal, 0), Some(0xffcbd5e1));
    assert_eq!(ui::render_battery_pixel(3, 3, &bat_normal, 0), Some(theme::ACCENT_BLUE));
    assert_eq!(ui::render_battery_pixel(10, 4, &bat_normal, 0), Some(theme::YELLOW));
    assert_eq!(ui::render_battery_pixel(3, 3, &bat_low, 0), Some(theme::AMBER));
    assert_eq!(ui::render_battery_pixel(2, 3, &bat_crit, 0), Some(theme::RED));
    assert_eq!(ui::render_battery_pixel(4, 2, &bat_none, 0), Some(theme::RED));

    // 2. Canvas::battery_icon rendering verification
    let mut pixels = [0u32; 100 * 50];
    let mut c = canvas::Canvas::new(&mut pixels, 100, 50, 100).unwrap();
    c.battery_icon(10, 10, &bat_normal);
    assert_eq!(pixels[(10 + 4) * 100 + (10 + 20)], 0xffcbd5e1); // Nub
    assert_eq!(pixels[(10 + 3) * 100 + (10 + 3)], theme::ACCENT_BLUE); // Fill
    assert_eq!(pixels[(10 + 4) * 100 + (10 + 10)], theme::YELLOW); // Lightning bolt

    let mut pixels_low = [0u32; 100 * 50];
    let mut c_low = canvas::Canvas::new(&mut pixels_low, 100, 50, 100).unwrap();
    c_low.battery_icon(10, 10, &bat_low);
    assert_eq!(pixels_low[(10 + 3) * 100 + (10 + 3)], theme::AMBER); // Amber fill

    let mut pixels_crit = [0u32; 100 * 50];
    let mut c_crit = canvas::Canvas::new(&mut pixels_crit, 100, 50, 100).unwrap();
    c_crit.battery_icon(10, 10, &bat_crit);
    assert_eq!(pixels_crit[(10 + 3) * 100 + (10 + 2)], theme::RED); // Red fill

    let mut pixels_none = [0u32; 100 * 50];
    let mut c_none = canvas::Canvas::new(&mut pixels_none, 100, 50, 100).unwrap();
    c_none.battery_icon(10, 10, &bat_none);
    assert_eq!(pixels_none[(10 + 2) * 100 + (10 + 4)], theme::RED); // Red X

    // Also test ui::Canvas::battery_icon
    let mut w = window::Window::new(
        7,
        "BatteryWin",
        surface::Rect { x: 0, y: 0, w: 100, h: 80 },
    ).unwrap();
    let mut ui_c = ui::Canvas::new(&mut w, 0, true);
    ui_c.battery_icon(5, 5, &bat_normal);
    let _ = ui_c.finish();

    // 3. WindowManager TopBar & Power Menu interaction
    let mut scene = surface::Surface::new(1280, 800).unwrap();
    let mut wm = window::WindowManager::new(&mut scene);
    wm.battery_status = Some(bat_normal);
    let bat_rect = window::WindowManager::battery_rect(scene.width, scene.height);

    // Clicking battery_rect opens power menu
    assert!(!wm.show_power_menu);
    let action = wm.mouse(bat_rect.x + 5, bat_rect.y + 5, 1, &mut scene);
    assert_eq!(action, window::Action::None);
    wm.mouse(bat_rect.x + 5, bat_rect.y + 5, 0, &mut scene);
    assert!(wm.show_power_menu);

    // Toggle power profile
    let pwr_rect = window::WindowManager::power_menu_rect(scene.width, scene.height);
    let saver_x = pwr_rect.x + 100;
    let saver_y = pwr_rect.y + 80;
    wm.mouse(saver_x, saver_y, 1, &mut scene);
    wm.mouse(saver_x, saver_y, 0, &mut scene);
    assert_eq!(wm.power_profile, 1);

    let perf_x = pwr_rect.x + 20;
    let perf_y = pwr_rect.y + 80;
    wm.mouse(perf_x, perf_y, 1, &mut scene);
    wm.mouse(perf_x, perf_y, 0, &mut scene);
    assert_eq!(wm.power_profile, 0);

    // Clicking "Power Settings..." inside power menu opens Power & Battery settings app (id 10)
    let settings_link_y = pwr_rect.y + 115;
    let act = wm.mouse(pwr_rect.x + 30, settings_link_y, 1, &mut scene);
    wm.mouse(pwr_rect.x + 30, settings_link_y, 0, &mut scene);
    assert_eq!(act, window::Action::OpenDesktopApp(10));
    assert!(!wm.show_power_menu);

    // 4. Desktop AppHost refresh polling
    sys::set_mock_battery(Some(Ok(bat_normal)));
    let mut host = desktop::AppHost::new(preferences::Preferences::default());
    host.refresh(100, &mut wm, &mut scene);
    assert_eq!(wm.battery_status, Some(bat_normal));

    sys::set_mock_battery(Some(Ok(bat_low)));
    host.refresh(101, &mut wm, &mut scene);
    assert_eq!(wm.battery_status, Some(bat_low));

    sys::set_mock_battery(Some(Err(-35))); // -ENODEV (Desktop PC: no battery)
    host.refresh(102, &mut wm, &mut scene);
    assert_eq!(wm.battery_status, None);

    // 5. Desktop PC mode without battery:
    // Tray is compacted, clicking battery rect does NOT open power menu
    let act_click_bat = wm.mouse(bat_rect.x + 5, bat_rect.y + 5, 1, &mut scene);
    wm.mouse(bat_rect.x + 5, bat_rect.y + 5, 0, &mut scene);
    assert_ne!(act_click_bat, window::Action::None); // Does not toggle power menu
    assert!(!wm.show_power_menu);

    // Clicking power icon in compacted tray directly opens Power & Battery Settings
    let tray_w = 86;
    let tray_x = scene.width - (tray_w + 8);
    let pwr_icon_x = tray_x + 58;
    let act_pwr = wm.mouse(pwr_icon_x, 14, 1, &mut scene);
    wm.mouse(pwr_icon_x, 14, 0, &mut scene);
    assert_eq!(act_pwr, window::Action::OpenDesktopApp(10));

    // 6. Settings App: Power & Battery Page (kind: 3)
    host.open(7, &mut wm, &mut scene); // Opens Settings
    assert!(wm.windows.iter().any(|w| w.id == 7));

    // Switch to Power & Battery tab (command 103)
    host.pointer(7, 50, 180, &mut wm, &mut scene); // Clicks NAV[3] Power & Battery
    assert_eq!(host.apps[0].kind, 3);

    // Click "High Performance" button (command 2)
    host.pointer(7, 400, 225, &mut wm, &mut scene);
    assert_eq!(host.power_profile, 1);
    assert_eq!(wm.power_profile, 1);

    // Click "Balanced / Saver" button (command 1)
    host.pointer(7, 230, 225, &mut wm, &mut scene);
    assert_eq!(host.power_profile, 0);
    assert_eq!(wm.power_profile, 0);

    sys::set_mock_battery(None);
}

#[path = "../user/compositor/src/clients.rs"]
#[allow(dead_code)]
mod clients;

#[path = "../user/compositor/src/perf.rs"]
#[allow(dead_code)]
mod perf;

#[test]
fn file_dialog_navigation_and_directory_traversal() {
    let mut dlg = file_dialog::FileDialog::new(file_dialog::FileDialogMode::Save, "/tmp/notes.txt");
    assert_eq!(dlg.current_dir, "/tmp");
    assert_eq!(dlg.filename, "notes.txt");
    assert_eq!(dlg.full_path(), "/tmp/notes.txt");

    // Quick navigation
    dlg.navigate_to("/");
    assert_eq!(dlg.current_dir, "/");
    assert_eq!(dlg.full_path(), "/notes.txt");

    dlg.navigate_to("/home/operator");
    assert_eq!(dlg.current_dir, "/home/operator");
    assert_eq!(dlg.full_path(), "/home/operator/notes.txt");

    dlg.navigate_up();
    assert_eq!(dlg.current_dir, "/home");

    dlg.navigate_up();
    assert_eq!(dlg.current_dir, "/");

    // At root, navigate_up stays at root
    dlg.navigate_up();
    assert_eq!(dlg.current_dir, "/");
}

#[test]
fn file_dialog_typing_and_file_selection() {
    let mut dlg = file_dialog::FileDialog::new(file_dialog::FileDialogMode::Save, "/tmp/doc.txt");
    dlg.entries.push(file_dialog::FileEntry {
        name: String::from("projects"),
        is_dir: true,
        is_exe: false,
        size: 0,
    });
    dlg.entries.push(file_dialog::FileEntry {
        name: String::from("readme.txt"),
        is_dir: false,
        is_exe: false,
        size: 1024,
    });

    // Test backspace
    dlg.handle_key(0x08);
    dlg.handle_key(0x08);
    assert_eq!(dlg.filename, "doc.t");

    // Test typing
    dlg.handle_key(b'x');
    dlg.handle_key(b't');
    assert_eq!(dlg.filename, "doc.txt");

    // Index 0 is ".. (Parent Dir)", Index 1 is "projects", Index 2 is "readme.txt"
    // Clicking file entry (row 2: cy = 85 + 2 * 20 + 5 = 130) selects it and populates filename
    dlg.handle_click(110, 130, 480, 280);
    assert_eq!(dlg.selected, 2);
    assert_eq!(dlg.filename, "readme.txt");

    // Clicking folder entry (row 1: cy = 85 + 1 * 20 + 5 = 110) enters directory
    dlg.handle_click(110, 110, 480, 280);
    assert_eq!(dlg.current_dir, "/tmp/projects");
}

#[test]
fn file_dialog_save_and_open_actions() {
    let mut dlg = file_dialog::FileDialog::new(file_dialog::FileDialogMode::Save, "/tmp/output.log");

    // Test Enter key on filename confirms Save
    let act = dlg.handle_key(b'\n');
    assert_eq!(act, Some(file_dialog::FileDialogAction::Save(String::from("/tmp/output.log"))));

    // Test Esc key cancels
    let act_esc = dlg.handle_key(0x1b);
    assert_eq!(act_esc, Some(file_dialog::FileDialogAction::Cancel));

    // Test Cancel button click:
    // Canvas 480x280: mw=360, mh=210, mx=60, my=35.
    // btn_y = 35 + 210 - 28 = 217.
    // Cancel button: cx in mx + mw - 74..=mx + mw - 8 => 346..=412.
    let act_cancel = dlg.handle_click(380, 225, 480, 280);
    assert_eq!(act_cancel, Some(file_dialog::FileDialogAction::Cancel));

    // Test Save button click:
    // Save button: cx in mx + mw - 146..=mx + mw - 80 => 274..=340.
    let act_save = dlg.handle_click(300, 225, 480, 280);
    assert_eq!(act_save, Some(file_dialog::FileDialogAction::Save(String::from("/tmp/output.log"))));

    // Test Open mode
    let mut dlg_open = file_dialog::FileDialog::new(file_dialog::FileDialogMode::Open, "/etc/desktop.conf");
    let act_open = dlg_open.handle_key(b'\r');
    assert_eq!(act_open, Some(file_dialog::FileDialogAction::Open(String::from("/etc/desktop.conf"))));
}

#[test]
fn file_dialog_rendering_and_backdrop_dimming() {
    let dlg = file_dialog::FileDialog::new(file_dialog::FileDialogMode::Save, "/tmp/test.txt");
    let mut pixels = vec![0xff4488ccu32; 480 * 280];
    let mut c = canvas::Canvas::new(&mut pixels, 480, 280, 480).unwrap();
    dlg.render(&mut c);

    // Verify backdrop dimming: top-left pixel (0,0) outside modal was dimmed by half
    // Original: 0x44 (68) -> 34, 0x88 (136) -> 68, 0xcc (204) -> 102
    let p0 = pixels[0];
    assert_eq!((p0 >> 16) & 0xff, 34);
    assert_eq!((p0 >> 8) & 0xff, 68);
    assert_eq!(p0 & 0xff, 102);

    // Verify modal was drawn inside center:
    assert!(pixels.iter().any(|&p| p == 0xff3b82f6)); // Accent border
    assert!(pixels.iter().any(|&p| p == 0xff161d28)); // Header bg
}

#[test]
fn notepad_file_dialog_modal_integration() {
    let mut np = notepad::Notepad::new("/tmp/initial.txt");
    np.text.clear();
    np.insert_char(b'A');
    np.insert_char(b'B');
    assert!(np.modified);
    assert!(np.dialog.is_none());

    // Click "Save" toolbar button: cx = 20, cy = 8
    assert!(np.handle_click(20, 8));
    assert!(np.dialog.is_some());

    // When modal is open, typing modifies the filename in the dialog
    let dlg = np.dialog.as_ref().unwrap();
    assert_eq!(dlg.mode, file_dialog::FileDialogMode::Save);
    assert_eq!(dlg.current_dir, "/tmp");

    // Press Esc cancels the dialog
    assert!(np.handle_key(0x1b));
    assert!(np.dialog.is_none());
    assert_eq!(np.path, "/tmp/initial.txt");

    // Click "Save" again to open dialog
    assert!(np.handle_click(20, 8));
    assert!(np.dialog.is_some());

    // Type a new filename: Backspace 15 times, then type "saved.txt"
    for _ in 0..15 {
        np.handle_key(0x08);
    }
    for &ch in b"saved.txt" {
        np.handle_key(ch);
    }
    assert_eq!(np.dialog.as_ref().unwrap().filename, "saved.txt");

    // Press Enter to confirm Save
    assert!(np.handle_key(b'\n'));
    assert!(np.dialog.is_none());
    assert_eq!(np.path, "/tmp/saved.txt");

    // Click "Open" toolbar button: cx = 330, cy = 8
    assert!(np.handle_click(330, 8));
    assert!(np.dialog.is_some());
    assert_eq!(np.dialog.as_ref().unwrap().mode, file_dialog::FileDialogMode::Open);

    // Press Esc to dismiss Open dialog
    assert!(np.handle_key(0x1b));
    assert!(np.dialog.is_none());
}
