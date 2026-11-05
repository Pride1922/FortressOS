//! The FortressOS desktop palette and design system tokens (0xAARRGGBB, always opaque).

// --- Core Palette (Canonical Tokens) ---
pub const PANEL: u32 = 0xff1e1e1e;
pub const SURFACE: u32 = 0xff2d2d2d;
pub const BORDER: u32 = 0xff3e3e3e;
pub const TEXT: u32 = 0xffe0e0e0;
pub const MUTED: u32 = 0xff989898;
pub const SUBTEXT: u32 = 0xff7e8796;
pub const ACCENT: u32 = 0xff007aff;
pub const ACCENT_BLUE: u32 = 0xff007aff;
pub const AMBER: u32 = 0xfff59e0b;
pub const RED: u32 = 0xffef4444;
pub const YELLOW: u32 = 0xffeab308;
pub const WHITE: u32 = 0xffffffff;

/// Background behind recessed text inputs created with `entry()`.
pub const INPUT_OUTER: u32 = 0xff282828;
/// Selected-button fill.
pub const SELECTED: u32 = 0xff244b6e;

// --- Semantic Background Surfaces ---
pub const BG_APP: u32 = PANEL;
pub const BG_SURFACE: u32 = SURFACE;
pub const BG_SIDEBAR: u32 = 0xff111620;
pub const BG_CARD: u32 = 0xff282828;
pub const BG_CARD_HOVER: u32 = 0xff323232;
pub const BG_INPUT_WELL: u32 = 0xff12151c;
pub const BG_NAV_ROW: u32 = 0xff1e293b;
pub const BG_NAV_SELECTED: u32 = 0xff334155;
pub const BG_BADGE_SUCCESS: u32 = 0xff133223;
pub const BG_BADGE_WARNING: u32 = 0xff332b14;
pub const BG_BADGE_DANGER: u32 = 0xff361818;
pub const BG_BADGE_INFO: u32 = 0xff1e2d40;
pub const BG_BADGE_MUTED: u32 = 0xff1f2733;

// --- Semantic Borders & Dividers ---
pub const BORDER_CARD: u32 = BORDER;
pub const BORDER_SUBTLE: u32 = 0xff2c313c;
pub const BORDER_DIVIDER: u32 = 0xff263040;
pub const BORDER_DIVIDER_DARK: u32 = 0xff1c2533;
pub const BORDER_PROFILE_DIVIDER: u32 = 0xff222d3d;
pub const BORDER_FOCUS: u32 = ACCENT;
pub const BORDER_ACCENT_LIGHT: u32 = 0xff38bdf8;
pub const BORDER_ACCENT_BLUE: u32 = 0xff3b82f6;
pub const BORDER_BADGE_SUCCESS: u32 = 0xff22c55e;
pub const BORDER_BADGE_WARNING: u32 = 0xffeab308;
pub const BORDER_BADGE_DANGER: u32 = 0xffef4444;
pub const BORDER_BADGE_MUTED: u32 = 0xff3a485c;

// --- Semantic Typography Colors ---
pub const TEXT_PRIMARY: u32 = WHITE;
pub const TEXT_BODY: u32 = TEXT;
pub const TEXT_MUTED: u32 = MUTED;
pub const TEXT_SUBTEXT: u32 = SUBTEXT;
pub const TEXT_NAV_UNSELECTED: u32 = 0xff9fb0c7;
pub const TEXT_SLATE: u32 = 0xff64748b;
pub const TEXT_SUCCESS: u32 = 0xff4ade80;
pub const TEXT_WARNING: u32 = 0xfffacc15;
pub const TEXT_DANGER: u32 = 0xfff87171;

// --- Elevation & Bevel Highlights ---
pub const TOP_HIGHLIGHT_DEFAULT: u32 = 0xff4c4c52;
pub const TOP_HIGHLIGHT_ACCENT: u32 = 0xff5cc8ff;
pub const TOP_HIGHLIGHT_SELECTED: u32 = 0xff4f94f8;
pub const TOP_HIGHLIGHT_SURFACE: u32 = 0xff4a4a4a;
pub const TOP_HIGHLIGHT_NAV: u32 = 0xff475973;
pub const TOP_HIGHLIGHT_NAV_FOCUSED: u32 = 0xff60a5fa;
pub const SHADOW_AMBIENT: u32 = 0xff08090c;
pub const SHADOW_CORE: u32 = 0xff0b0d12;

// --- Visual Geometry & Layout Metrics (logical pixels) ---
pub const RADIUS_CARD: usize = 8;
pub const RADIUS_BUTTON: usize = 6;
pub const RADIUS_BADGE: usize = 6;
pub const RADIUS_PILL: usize = 10;
pub const RADIUS_INPUT: usize = 6;

pub const HEIGHT_BUTTON: usize = 28;
pub const HEIGHT_INPUT: usize = 30;
pub const HEIGHT_NAV_ITEM: usize = 32;
pub const HEIGHT_HERO_CARD: usize = 76;
pub const WIDTH_SIDEBAR: usize = 180;
