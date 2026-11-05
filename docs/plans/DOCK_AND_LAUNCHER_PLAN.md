# Desktop Modernization Plan: Dock and Application Launcher

## Overview
This document specifies the architecture, implementation, and verification of the modernized macOS-inspired desktop dock and searchable application launcher for FortressOS.

## Implementation Stages

### Stage 1: Application Manifest Registry & Client-Window Association
- **Metadata Registry** (`user/compositor/src/app_registry.rs`):
  - Declares typed `AppId` enum (`Terminal`, `Files`, `Notepad`, `Calc`, `Vault`, `Settings`, `SysInfo`, `Network`, `DateTime`, `Power`).
  - Declares `AppManifest` struct with `is_available()` probing `sys::stat_ext` and zero-allocation `matches_query()`.
  - Static `APP_CATALOG` ordering primary apps consistently.
  - Path-to-application resolution helper `resolve_from_path(path: &CStr) -> Option<AppId>`.
- **Client & Window Ownership**:
  - Bound `app_id: Option<AppId>` directly to `Window` in `user/compositor/src/window.rs` and `Client` in `user/compositor/src/clients.rs`.
  - Process creation via `Clients::launch` tags client ownership, inherited by windows on `REQ_CREATE`.
  - Added query helpers: `app_window_count`, `app_is_running`, `app_is_focused`, `app_frontmost_window`, `focus_app`, `cycle_app_windows`.
- **Verification**: `make test-compositor-host` (82/82 PASS).
- **Commit**: `f64d19d` (`ui: implement application manifest registry and client-window association`).

### Stage 2: Predictable Dock Appearance & Window Lifecycle
- **Smart Dock Interaction**:
  - If app not running: spawns a new instance.
  - If app running in background or minimized: focuses and restores the frontmost window without launching duplicate processes.
  - If active app with multiple windows is clicked: cycles focus sequentially through all windows belonging to that application.
  - Shift+Click: explicitly spawns a new window even if already running.
- **Running & Active Indicators**:
  - Active focused app: 16px accent pill (`24..=39` px) in `theme::ACCENT`.
  - Background app with multiple windows: dual dots (`26..=29` and `34..=37` px) in `theme::MUTED`.
  - Background app with single window: centered dot (`28..=35` px) in `theme::MUTED`.
- **Verification**: `make test-compositor-host` (83/83 PASS).
- **Commit**: `bf8bb01` (`ui: implement predictable dock focus, restore, multi-window cycling, and running indicators`).

### Stage 3: Searchable Application Launcher & Session Controls
- **Searchable Launcher State**:
  - Integrated `launcher_search: TextField<32>` with real-time text query filtering.
  - Dynamic `matching_launcher_items()` returns filtered applications matching query by name, category, or keywords.
  - Empty state displays clear message ("No applications found", "Check spelling or search terms").
- **Keyboard Navigation & Dismissal**:
  - Up / Down arrow navigation with wrap-around across matching results.
  - Enter key activates the selected application or session action via `take_pending_action()`.
  - Escape key dismisses the launcher and clears the search query.
- **Separated Session Actions**:
  - Pinned footer with horizontal divider separates session actions (`Exit to Console`, `Restart`, `Lock Screen`) from applications.
  - Mouse hover and keyboard selection are synchronized (`launcher_selected` <-> `menu_hover`).
- **Verification**: `make test-compositor-host` (84/84 PASS).
- **Commit**: `b81c42b` (`ui: implement searchable application launcher, keyboard navigation, and session controls`).

### Stage 4: Damage Envelopes, Metrics & Scaling
- **Expanded Damage Envelopes**:
  - `start_menu_damage_rect` adds a safe margin (+12 * scale px) around launcher bounds to prevent border/shadow clipping.
  - `dock_damage` expands vertically and horizontally (+16 * scale px) to cover tooltip and magnification protrusion.
  - 1080p (Scale 1) and 4K UHD (Scale 2) verified with proportional geometry.
- **Performance Benchmark**:
  - Incremental damage repair ensures frame redraw is bounded (< 10% of screen area).
  - Average incremental damage repair latency measured well within the 60 FPS frame budget (< 16.6 ms).
- **Verification**: `make test-compositor-host` (85/85 PASS), `make build/compositor.elf` (0 warnings, 0 errors).
- **Commit**: `47ceef7` (`ui: implement expanded damage envelopes, 1080p/4K scaling, and benchmark harness`).
