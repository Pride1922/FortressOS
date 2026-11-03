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

pub use font::{LINE_HEIGHT, text_pixel, text_pixel_bold, text_width};
pub use gfx::{Rect, blend_coverage, rect, rounded_coverage};
pub use painter::Painter;
pub use style::SurfaceStyle;
