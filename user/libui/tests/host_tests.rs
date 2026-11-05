use libui::gfx::{blend_coverage, rect, rounded_coverage};
use libui::proto::{Msg, clip_damage};
use libui::state::{FocusRing, Grid, TextField};
use libui::style::SurfaceStyle;

#[test]
fn test_rect_operations() {
    let r1 = rect(10, 10, 50, 50);
    let r2 = rect(30, 30, 50, 50);

    let inter = r1.intersection(r2).unwrap();
    assert_eq!(inter, rect(30, 30, 30, 30));

    let u = r1.union(r2);
    assert_eq!(u, rect(10, 10, 70, 70));

    assert!(r1.contains(10, 10));
    assert!(r1.contains(59, 59));
    assert!(!r1.contains(60, 60));

    let inset = r1.inset(5);
    assert_eq!(inset, rect(15, 15, 40, 40));
}

#[test]
fn test_rounded_coverage_symmetry() {
    for x in 0..100 {
        for y in 0..40 {
            let c1 = rounded_coverage(x, y, 100, 40, 8);
            let c2 = rounded_coverage(99 - x, y, 100, 40, 8);
            let c3 = rounded_coverage(x, 39 - y, 100, 40, 8);
            assert_eq!(c1, c2);
            assert_eq!(c1, c3);
            assert!(c1 <= 16);
        }
    }
}

#[test]
fn test_blend_coverage() {
    let fg = 0xffffffff;
    let bg = 0xff000000;
    assert_eq!(blend_coverage(fg, bg, 0), bg);
    assert_eq!(blend_coverage(fg, bg, 16), fg);
}

#[test]
fn test_surface_style() {
    let s = SurfaceStyle::card(0xff222222, 0xff444444);
    assert_eq!(s.radius, 8);
    // Interior point must be fill
    let c = s.shade(10, 10, 100, 50);
    assert_eq!(c, 0xff222222);
}

#[test]
fn test_text_field() {
    let mut tf = TextField::<32>::new();
    assert_eq!(tf.as_str(), "");
    assert!(tf.push('H'));
    assert!(tf.push('i'));
    assert_eq!(tf.as_str(), "Hi");
    assert_eq!(tf.len(), 2);

    assert!(tf.handle_key(b'!'));
    assert_eq!(tf.as_str(), "Hi!");

    assert!(tf.handle_key(0x08)); // Backspace
    assert_eq!(tf.as_str(), "Hi");

    tf.set_text("Fortress");
    assert_eq!(tf.as_str(), "Fortress");

    tf.clear();
    assert_eq!(tf.as_str(), "");
}

#[test]
fn test_focus_ring() {
    let mut ring = FocusRing::<4>::new(4);
    assert_eq!(ring.current(), 0);
    assert!(ring.is_focused(0));

    ring.next();
    assert_eq!(ring.current(), 1);

    ring.prev();
    assert_eq!(ring.current(), 0);

    ring.prev();
    assert_eq!(ring.current(), 3);

    ring.next();
    assert_eq!(ring.current(), 0);
}

#[test]
fn test_grid() {
    let grid = Grid::new(10, 20, 3, 40, 30, 5);
    // cell 0: col 0, row 0 -> (10, 20, 40, 30)
    assert_eq!(grid.cell(0), rect(10, 20, 40, 30));
    // cell 1: col 1, row 0 -> (10 + 45, 20, 40, 30)
    assert_eq!(grid.cell(1), rect(55, 20, 40, 30));
    // cell 3: col 0, row 1 -> (10, 20 + 35, 40, 30)
    assert_eq!(grid.cell(3), rect(10, 55, 40, 30));

    // Span 2 cols
    let span = grid.cell_span(0, 2);
    assert_eq!(span, rect(10, 20, 85, 30));
}

#[test]
fn test_clip_damage() {
    let m = Msg::new(2, 5, 10, 100, 80);
    let clipped = clip_damage(&m, 60, 40);
    assert_eq!(clipped, Some((5, 10, 55, 30)));
}

#[test]
fn test_theme_tokens() {
    use libui::theme::*;
    assert_eq!(BG_APP, PANEL);
    assert_eq!(BG_SURFACE, SURFACE);
    assert_eq!(BORDER_CARD, BORDER);
    assert_eq!(TEXT_BODY, TEXT);
    assert_eq!(BORDER_FOCUS, ACCENT);
    assert_eq!(ACCENT, ACCENT_CYAN);
    assert_eq!(PANEL, OBSIDIAN_PANEL);
    assert_eq!(RADIUS_CARD, 8);
    assert_eq!(RADIUS_BUTTON, 6);
    assert_eq!(WIDTH_SIDEBAR, 180);
    assert_eq!(BG_SIDEBAR, 0xff0d1117);
    assert_eq!(BORDER_DIVIDER, 0xff1e293b);
    assert_eq!(BORDER_SUBTLE, 0xff1e293b);
}

#[test]
fn test_surface_style_extensions() {
    use libui::style::SurfaceStyle;
    use libui::theme::*;

    let s1 = SurfaceStyle::card_on(BG_SIDEBAR, BG_CARD, BORDER_CARD);
    assert_eq!(s1.outer_bg, BG_SIDEBAR);
    assert_eq!(s1.radius, RADIUS_CARD);
    assert_eq!(s1.fill, BG_CARD);

    let s2 = s1.with_radius(12).with_fill(0xff00ff00).with_border(0xff0000ff);
    assert_eq!(s2.radius, 12);
    assert_eq!(s2.fill, 0xff00ff00);
    assert_eq!(s2.border, 0xff0000ff);

    let pill = SurfaceStyle::pill(RADIUS_PILL, BG_CARD, BORDER_SUBTLE, PANEL);
    assert_eq!(pill.radius, RADIUS_PILL);
    assert_eq!(pill.fill, BG_CARD);
}

#[test]
fn test_card_shadow_redraw_cost_benchmark() {
    use libui::gfx::rect;
    use libui::painter::Painter;
    use libui::theme::*;
    use std::time::Instant;

    let width = 600usize;
    let height = 200usize;
    let mut buf = vec![PANEL; width * height];
    let card_r = rect(20, 20, 540, 76);

    const ITERS: usize = 300;

    // 1. Measure card_flat (elevation 0)
    let t0 = Instant::now();
    for _ in 0..ITERS {
        let mut p = Painter::new(&mut buf, width, height, width).unwrap().with_damage();
        p.card_flat(card_r, BG_CARD, BORDER_CARD, PANEL);
        let _ = p.finish();
    }
    let elapsed_flat = t0.elapsed();

    // 2. Measure card_elevated (elevation 2)
    let t0 = Instant::now();
    for _ in 0..ITERS {
        let mut p = Painter::new(&mut buf, width, height, width).unwrap().with_damage();
        p.card_elevated(card_r, BG_CARD, BORDER_CARD, 2, PANEL);
        let _ = p.finish();
    }
    let elapsed_elev2 = t0.elapsed();

    // 3. Measure card_elevated (elevation 3, default)
    let t0 = Instant::now();
    for _ in 0..ITERS {
        let mut p = Painter::new(&mut buf, width, height, width).unwrap().with_damage();
        p.card_elevated(card_r, BG_CARD, BORDER_CARD, 3, PANEL);
        let _ = p.finish();
    }
    let elapsed_elev3 = t0.elapsed();

    let us_flat = elapsed_flat.as_micros() as f64 / ITERS as f64;
    let us_elev2 = elapsed_elev2.as_micros() as f64 / ITERS as f64;
    let us_elev3 = elapsed_elev3.as_micros() as f64 / ITERS as f64;

    std::println!("\n=== Card Shadow Redraw Cost Benchmark (540x76 Hero Card) ===");
    std::println!("Flat card (elevation 0):     {:.2} us/frame", us_flat);
    std::println!("Elevated card (elevation 2): {:.2} us/frame (+{:.1}%)", us_elev2, ((us_elev2 - us_flat) / us_flat) * 100.0);
    std::println!("Elevated card (elevation 3): {:.2} us/frame (+{:.1}%)", us_elev3, ((us_elev3 - us_flat) / us_flat) * 100.0);

    assert!(us_flat > 0.0);
    assert!(us_elev2 > 0.0);
    assert!(us_elev3 > 0.0);
}

#[test]
fn test_multi_size_typography() {
    use libui::font::{
        line_height, text_pixel, text_pixel_bold, text_pixel_bold_sized, text_pixel_sized,
        text_width, text_width_sized, FontSize, BODY_LINE_HEIGHT, CAPTION_LINE_HEIGHT,
        TITLE_LINE_HEIGHT,
    };

    assert_eq!(line_height(FontSize::Title), 22);
    assert_eq!(line_height(FontSize::Body), 18);
    assert_eq!(line_height(FontSize::Caption), 16);
    assert_eq!(TITLE_LINE_HEIGHT, 22);
    assert_eq!(BODY_LINE_HEIGHT, 18);
    assert_eq!(CAPTION_LINE_HEIGHT, 16);

    let sample = "FortressOS Settings & Typography";
    let w_title = text_width_sized(FontSize::Title, sample);
    let w_body = text_width_sized(FontSize::Body, sample);
    let w_caption = text_width_sized(FontSize::Caption, sample);

    // Title 18px must be wider than Body 14px, which must be wider than Caption 12px
    assert!(w_title > w_body, "w_title ({}) > w_body ({})", w_title, w_body);
    assert!(w_body > w_caption, "w_body ({}) > w_caption ({})", w_body, w_caption);

    // Backward compatibility: legacy text_width matches Body exactly
    assert_eq!(text_width(sample), w_body);

    // Empty text width is 0
    assert_eq!(text_width_sized(FontSize::Title, ""), 0);
    assert_eq!(text_width_sized(FontSize::Body, ""), 0);
    assert_eq!(text_width_sized(FontSize::Caption, ""), 0);

    // Out-of-bounds y returns background
    let bg = 0xff1e1e1e;
    let fg = 0xffffffff;
    assert_eq!(text_pixel_sized(FontSize::Title, sample, 0, 22, fg, bg), bg);
    assert_eq!(text_pixel_sized(FontSize::Body, sample, 0, 18, fg, bg), bg);
    assert_eq!(text_pixel_sized(FontSize::Caption, sample, 0, 16, fg, bg), bg);

    // Backward compatibility: text_pixel matches Body exactly
    for y in 0..18 {
        for x in 0..15 {
            assert_eq!(
                text_pixel(sample, x, y, fg, bg),
                text_pixel_sized(FontSize::Body, sample, x, y, fg, bg)
            );
            assert_eq!(
                text_pixel_bold(sample, x, y, fg, bg),
                text_pixel_bold_sized(FontSize::Body, sample, x, y, fg, bg)
            );
        }
    }
}

#[test]
fn test_glass_blur_vs_tint_cost_benchmark() {
    let surfaces = [
        ("Taskbar/Dock", 1024, 48),
        ("Start Menu", 280, 300),
        ("Panel/Modal", 500, 350),
    ];

    std::println!("\n=== Glass & Blur Performance Benchmark ===");
    for &(name, w, h) in &surfaces {
        let stride = 1920;
        let bg_buf = vec![0xff202430u32; stride * 1080];
        let mut out_buf = vec![0u32; stride * 1080];

        // 1. Translucent Tint (Alpha Blend: read under, blend tint, write)
        let t0 = std::time::Instant::now();
        let iters = 50;
        let tint = 0xff1e293b;
        for _ in 0..iters {
            for y in 0..h {
                let row_offset = y * stride;
                for x in 0..w {
                    let under = bg_buf[row_offset + x];
                    out_buf[row_offset + x] = blend_coverage(tint, under, 12);
                }
            }
        }
        let elapsed_tint = t0.elapsed();
        let us_tint = elapsed_tint.as_micros() as f64 / iters as f64;

        // 2. 3x3 Box Blur + Tint (sample 9 neighbors, average, blend tint, write)
        let t0 = std::time::Instant::now();
        for _ in 0..iters {
            for y in 1..(h - 1) {
                let row_offset = y * stride;
                for x in 1..(w - 1) {
                    let mut r_acc = 0u32;
                    let mut g_acc = 0u32;
                    let mut b_acc = 0u32;
                    for dy in 0..3 {
                        let sample_row = (y + dy - 1) * stride;
                        for dx in 0..3 {
                            let p = bg_buf[sample_row + (x + dx - 1)];
                            r_acc += (p >> 16) & 0xff;
                            g_acc += (p >> 8) & 0xff;
                            b_acc += p & 0xff;
                        }
                    }
                    let blurred = 0xff000000
                        | (((r_acc / 9) & 0xff) << 16)
                        | (((g_acc / 9) & 0xff) << 8)
                        | ((b_acc / 9) & 0xff);
                    out_buf[row_offset + x] = blend_coverage(tint, blurred, 12);
                }
            }
        }
        let elapsed_blur3 = t0.elapsed();
        let us_blur3 = elapsed_blur3.as_micros() as f64 / iters as f64;

        std::println!(
            "{:<14} ({}x{} = {} px): Tint: {:>8.2} us | 3x3 Blur: {:>9.2} us (+{:.1}%)",
            name,
            w,
            h,
            w * h,
            us_tint,
            us_blur3,
            ((us_blur3 - us_tint) / us_tint) * 100.0
        );

        assert!(us_tint > 0.0);
        assert!(us_blur3 > 0.0);
    }
}

#[test]
fn test_greeter_date_metrics_no_clipping() {
    use libui::font::{text_width_sized, FontSize};

    let sample_date = "Wednesday, September 24";
    let dlen = sample_date.len();
    let old_flawed_w = dlen * 8; // 23 * 8 = 184 px
    let real_w = text_width_sized(FontSize::Body, sample_date); // proportional font: 215 px!

    // Verify the old flawed calculation was smaller than real width (clipped by 31 px!)
    assert_eq!(old_flawed_w, 184);
    assert_eq!(real_w, 215);
    assert!(real_w > old_flawed_w, "Proportional text must be wider than 8px-monospace assumption");

    // Dynamic clock/date pill calculation
    let time_view = "12:34";
    let time_w = text_width_sized(FontSize::Body, time_view);
    let needed_w = real_w.max(time_w) + 40;
    let pill_w = needed_w.max(260);

    assert!(pill_w >= 260);
    assert!(pill_w > real_w + 32, "Pill must fully contain the date without clipping");
}

#[test]
fn test_label_transparent_rendering_no_background_patch() {
    use libui::painter::Painter;
    use libui::theme;
    use libui::font::{glyph_alpha_sized, FontSize};

    let width = 300;
    let height = 100;
    let mut pixels = vec![0u32; width * height];

    // Fill with a non-panel gradient (e.g. blue/cyan gradient: 0xff203050..0xff305080)
    for y in 0..height {
        for x in 0..width {
            let r = 0x20 + (x * 16 / width) as u32;
            let g = 0x30 + (y * 32 / height) as u32;
            let b = 0x50 + ((x + y) * 24 / (width + height)) as u32;
            pixels[y * width + x] = 0xff000000 | (r << 16) | (g << 8) | b;
        }
    }
    let original = pixels.clone();

    let mut p = Painter::new(&mut pixels, width, height, width).unwrap();
    let text = "Sign in to FortressOS";
    p.label(20, 20, 250, text, 0xffffffff);

    // Verify that pixels where glyph_alpha == 0 are IDENTICAL to original gradient
    let text_w = libui::font::text_width(text);
    for dy in 0..22 {
        for dx in 0..250 {
            let px = 20 + dx;
            let py = 20 + dy;
            let idx = py * width + px;

            // In our label(), text is at dy in 2..20 (since line_height is 18 and offset is 2)
            let alpha = if (2..20).contains(&dy) && dx < text_w {
                glyph_alpha_sized(FontSize::Body, text, dx, dy - 2)
            } else {
                0
            };

            if alpha == 0 {
                // MUST NOT have been overwritten with theme::PANEL or any background rectangle!
                assert_eq!(
                    pixels[idx], original[idx],
                    "Pixel at ({}, {}) outside glyph must match original gradient without background patch",
                    px, py
                );
                assert_ne!(
                    pixels[idx], theme::PANEL,
                    "Pixel at ({}, {}) must not be corrupted by theme::PANEL",
                    px, py
                );
            } else if alpha == 255 {
                // Solid glyph interior is white
                assert_eq!(pixels[idx], 0xffffffff);
            } else {
                // Antialiased edge is blended over original, not over theme::PANEL
                assert_ne!(pixels[idx], original[idx]);
                assert_ne!(pixels[idx], 0xffffffff);
            }
        }
    }
}

#[test]
fn test_sheet_card_no_dark_halos_and_repeated_redraw_idempotence() {
    use libui::painter::Painter;
    use libui::theme;

    let width = 320;
    let height = 200;
    let mut bg = vec![0u32; width * height];

    let make_wallpaper = |buf: &mut [u32]| {
        for y in 0..height {
            for x in 0..width {
                let r = 14 + (x * 20 / width) as u32;
                let g = 18 + (y * 30 / height) as u32;
                let b = 38 + ((x + y) * 50 / (width + height)) as u32;
                buf[y * width + x] = 0xff000000 | (r << 16) | (g << 8) | b;
            }
        }
    };
    make_wallpaper(&mut bg);

    let mut pixels = bg.clone();
    let card_r = rect(50, 50, 200, 80);

    // Frame 1
    {
        let mut p = Painter::new(&mut pixels, width, height, width).unwrap();
        p.sheet_card(card_r, 0xff161b22, 0xff30363d, true);
    }
    let frame1 = pixels.clone();

    // Verify outer corners outside card and shadow are untouched (NOT theme::PANEL)
    // sheet_card bounding rect is rx = 46..254, ry = 48..137
    // Top-left corner of expanded bounding rect: (46, 48)
    assert_eq!(
        pixels[48 * width + 46], bg[48 * width + 46],
        "Corner pixel (46, 48) must match wallpaper (no dark halo patch)"
    );
    assert_ne!(pixels[48 * width + 46], theme::PANEL);

    // Bottom-right corner of expanded bounding rect: (253, 136)
    assert_eq!(
        pixels[136 * width + 253], bg[136 * width + 253],
        "Corner pixel (253, 136) must match wallpaper (no dark halo patch)"
    );

    // Center of card is solid card_bg
    assert_eq!(pixels[90 * width + 150], 0xff161b22);

    // Repeated redraw test: Simulate 10 frames where background is restored and card is redrawn
    for _ in 2..=10 {
        make_wallpaper(&mut pixels);
        let mut p = Painter::new(&mut pixels, width, height, width).unwrap();
        p.sheet_card(card_r, 0xff161b22, 0xff30363d, true);
    }

    // Frame 10 must be 100% bit-identical to Frame 1 (no accumulated darkening)
    assert_eq!(pixels, frame1, "10 consecutive frames must be bit-identical without accumulated darkening");
}

#[test]
fn test_painter_scale_and_clipping() {
    use libui::painter::Painter;

    let width = 400;
    let height = 300;
    let mut pixels = vec![0xff000000u32; width * height];

    // Test Scale 1 (1080p native)
    {
        let mut p = Painter::new(&mut pixels, width, height, width).unwrap().with_scale(1);
        p.sheet_card(rect(10, 10, 100, 60), 0xff161b22, 0xff30363d, false);
        p.label(15, 15, 90, "Scale 1", 0xffffffff);
    }

    // Test Scale 2 (2x UHD)
    {
        let mut p = Painter::new(&mut pixels, width, height, width).unwrap().with_scale(2);
        p.sheet_card(rect(70, 70, 100, 60), 0xff161b22, 0xff30363d, false);
        p.label(75, 75, 90, "Scale 2", 0xffffffff);
    }

    // Extreme coordinate clipping test: must not panic or cause memory error
    {
        let mut p = Painter::new(&mut pixels, width, height, width).unwrap();
        p.sheet_card(rect(380, 280, 200, 100), 0xff161b22, 0xff30363d, true);
        p.label(390, 290, 100, "Clipped", 0xffffffff);
    }
}

