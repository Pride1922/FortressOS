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
    assert_eq!(TEXT_PRIMARY, WHITE);
    assert_eq!(TEXT_BODY, TEXT);
    assert_eq!(BORDER_FOCUS, ACCENT);
    assert_eq!(RADIUS_CARD, 8);
    assert_eq!(RADIUS_BUTTON, 6);
    assert_eq!(WIDTH_SIDEBAR, 180);
    assert_eq!(BG_SIDEBAR, 0xff111620);
    assert_eq!(BORDER_DIVIDER, 0xff263040);
    assert_eq!(BORDER_SUBTLE, 0xff2c313c);
}
