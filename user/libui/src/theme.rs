//! The FortressOS desktop palette and design system tokens (0xAARRGGBB, always opaque).
//! Direction A: "Obsidian Slate" — deep obsidian foundation, electric cyan accents,
//! crisp white typography, and subtle slate depth.

// --- Direction A: Obsidian Slate Canonical Tokens ---
pub const OBSIDIAN_BASE: u32 = 0xff090d14;    // Deep midnight slate / desktop background
pub const OBSIDIAN_SLATE: u32 = 0xff0d1117;   // Window frame / sidebar background
pub const OBSIDIAN_PANEL: u32 = 0xff111620;   // Primary container card / panel
pub const OBSIDIAN_SURFACE: u32 = 0xff161d2a; // Elevated card / active surface
pub const OBSIDIAN_ELEVATED: u32 = 0xff1a2230;// Header row / control well
pub const OBSIDIAN_WELL: u32 = 0xff0b0f17;    // Recessed text input well

// --- Direction A Accent & Elevation ---
pub const ACCENT_CYAN: u32 = 0xff38bdf8;      // Electric Sky Blue / Primary Cyan Accent
pub const ACCENT_SAPPHIRE: u32 = 0xff2563eb;  // Active Window Elevation / Sapphire Halo
pub const ACCENT_HOVER: u32 = 0xff7dd3fc;     // Cyan hover
pub const ACCENT_MUTED: u32 = 0xff0284c7;     // Cyan pressed / dark
pub const TEXT_ON_ACCENT: u32 = 0xff090d14;   // High-contrast dark text on cyan buttons

// --- Core Palette (Canonical Tokens) ---
pub const PANEL: u32 = OBSIDIAN_PANEL;
pub const SURFACE: u32 = OBSIDIAN_SURFACE;
pub const BORDER: u32 = 0xff1e293b;
pub const TEXT: u32 = 0xfff0f6fc;
pub const MUTED: u32 = 0xff94a3b8;
pub const SUBTEXT: u32 = 0xff94a3b8;
pub const ACCENT: u32 = ACCENT_CYAN;
pub const ACCENT_BLUE: u32 = 0xff007aff;
pub const AMBER: u32 = 0xfffbbf24;
pub const RED: u32 = 0xfff87171;
pub const YELLOW: u32 = 0xfffbbf24;
pub const WHITE: u32 = 0xffffffff;

/// Background behind recessed text inputs created with `entry()`.
pub const INPUT_OUTER: u32 = OBSIDIAN_PANEL;
/// Selected-button fill.
pub const SELECTED: u32 = 0xff1d3557;

// --- Semantic Background Surfaces ---
pub const BG_APP: u32 = PANEL;
pub const BG_SURFACE: u32 = SURFACE;
pub const BG_SIDEBAR: u32 = OBSIDIAN_SLATE;
pub const BG_CARD: u32 = OBSIDIAN_PANEL;
pub const BG_CARD_HOVER: u32 = OBSIDIAN_SURFACE;
pub const BG_INPUT_WELL: u32 = OBSIDIAN_WELL;
pub const BG_NAV_ROW: u32 = 0xff1e293b;
pub const BG_NAV_SELECTED: u32 = 0xff1d3557;
pub const BG_BADGE_SUCCESS: u32 = 0xff064e3b;
pub const BG_BADGE_WARNING: u32 = 0xff451a03;
pub const BG_BADGE_DANGER: u32 = 0xff4c0519;
pub const BG_BADGE_INFO: u32 = 0xff0c4a6e;
pub const BG_BADGE_MUTED: u32 = 0xff1e293b;

// --- Semantic Borders & Dividers ---
pub const BORDER_CARD: u32 = BORDER;
pub const BORDER_SUBTLE: u32 = 0xff1e293b;
pub const BORDER_DIVIDER: u32 = 0xff1e293b;
pub const BORDER_DIVIDER_DARK: u32 = 0xff131b26;
pub const BORDER_PROFILE_DIVIDER: u32 = 0xff1e293b;
pub const BORDER_FOCUS: u32 = ACCENT_CYAN;
pub const BORDER_HOVER: u32 = 0xff334155;
pub const BORDER_ACCENT_LIGHT: u32 = 0xff38bdf8;
pub const BORDER_ACCENT_BLUE: u32 = 0xff2563eb;
pub const BORDER_BADGE_SUCCESS: u32 = 0xff34d399;
pub const BORDER_BADGE_WARNING: u32 = 0xfffbbf24;
pub const BORDER_BADGE_DANGER: u32 = 0xfff87171;
pub const BORDER_BADGE_MUTED: u32 = 0xff334155;

// --- Semantic Typography Colors ---
pub const TEXT_PRIMARY: u32 = 0xfff0f6fc;
pub const TEXT_BODY: u32 = TEXT;
pub const TEXT_SECONDARY: u32 = 0xff94a3b8;
pub const TEXT_MUTED: u32 = 0xff64748b;
pub const TEXT_SUBTEXT: u32 = 0xff94a3b8;
pub const TEXT_NAV_UNSELECTED: u32 = 0xff94a3b8;
pub const TEXT_SLATE: u32 = 0xff94a3b8;
pub const TEXT_SUCCESS: u32 = 0xff34d399;
pub const TEXT_WARNING: u32 = 0xfffbbf24;
pub const TEXT_DANGER: u32 = 0xfff87171;

// --- Elevation & Bevel Highlights ---
pub const TOP_HIGHLIGHT_DEFAULT: u32 = 0xff334155;
pub const TOP_HIGHLIGHT_ACCENT: u32 = 0xff7dd3fc;
pub const TOP_HIGHLIGHT_SELECTED: u32 = 0xff38bdf8;
pub const TOP_HIGHLIGHT_SURFACE: u32 = 0xff222d3d;
pub const TOP_HIGHLIGHT_NAV: u32 = 0xff334155;
pub const TOP_HIGHLIGHT_NAV_FOCUSED: u32 = 0xff38bdf8;
pub const SHADOW_AMBIENT: u32 = 0xff04070b;
pub const SHADOW_CORE: u32 = 0xff06090e;

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
