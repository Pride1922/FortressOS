#![no_std]

pub mod font;
pub mod gfx;
pub mod glyphs;
pub mod painter;
pub mod proto;
pub mod state;
pub mod style;
pub mod theme;
pub mod widgets;

#[cfg(feature = "client")]
pub mod client;

pub use font::{
    BODY_LINE_HEIGHT, CAPTION_LINE_HEIGHT, FontSize, LINE_HEIGHT, TITLE_LINE_HEIGHT,
    line_height, text_pixel, text_pixel_bold, text_pixel_bold_sized, text_pixel_sized,
    text_width, text_width_sized,
};
pub use gfx::{Rect, blend_coverage, rect, rounded_coverage};
pub use painter::Painter;
pub use style::SurfaceStyle;
