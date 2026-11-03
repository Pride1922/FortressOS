//! Built-in app host v1. Apps use shared widgets and existing syscall services;
//! no app-specific code belongs in the compositor's pointer dispatch loop.
use crate::{
    preferences::{Preferences, Text},
    surface::{Rect, Surface},
    sys,
    ui::{self, AppDescriptor, Canvas, DesktopApp},
    window::{Window, WindowManager, theme},
};
use alloc::{boxed::Box, vec::Vec};
use core::fmt::Write;

pub const VERSION: &str = match option_env!("CARGO_PKG_VERSION") {
    Some(version) => version,
    None => "0.2.0",
};

#[derive(Clone, Copy)]
pub struct Builtin {
    pub kind: usize,
    pub focus: usize,
    size: (usize, usize),
    scroll: usize,
}
#[derive(Clone, Copy)]
struct Control {
    id: u16,
    rect: Rect,
}
const fn control(id: u16, x: usize, y: usize, w: usize, h: usize) -> Control {
    Control {
        id,
        rect: Rect { x, y, w, h },
    }
}
const NAV: [Control; 4] = [
    control(100, 6, 56, 168, 34),
    control(101, 6, 94, 168, 34),
    control(102, 6, 132, 168, 34),
    control(103, 6, 170, 168, 34),
];
const POWER: [Control; 2] = [
    control(1, 196, 210, 160, 32),
    control(2, 366, 210, 160, 32),
];
const APPEARANCE: [Control; 9] = [
    control(1, 196, 96, 96, 28),
    control(2, 300, 96, 96, 28),
    control(3, 404, 96, 96, 28),
    control(4, 196, 178, 140, 28),
    control(5, 344, 178, 140, 28),
    control(6, 196, 262, 110, 28),
    control(7, 314, 262, 110, 28),
    control(8, 196, 306, 130, 32),
    control(9, 334, 306, 130, 32),
];
const NETWORK: [Control; 12] = [
    control(1, 486, 145, 96, 28),
    control(2, 588, 145, 138, 28),
    control(3, 562, 186, 84, 26),
    control(4, 652, 186, 84, 26),
    control(5, 206, 350, 240, 28),
    control(6, 206, 402, 196, 28),
    control(7, 476, 402, 124, 28),
    control(8, 408, 402, 62, 28),
    control(20, 196, 216, 540, 24),
    control(21, 196, 242, 540, 24),
    control(22, 196, 268, 540, 24),
    control(23, 196, 294, 540, 24),
];
const CLOCK: [Control; 8] = [
    control(1, 196, 152, 270, 28),
    control(2, 474, 152, 140, 28),
    control(3, 196, 206, 70, 26),
    control(4, 274, 206, 96, 26),
    control(5, 378, 206, 96, 26),
    control(6, 482, 206, 70, 26),
    control(7, 196, 286, 180, 28),
    control(8, 624, 64, 102, 28),
];
impl Builtin {
    #[cfg(test)]
    pub fn test_instance(kind: usize) -> Self {
        Self {
            kind,
            focus: 0,
            size: (0, 0),
            scroll: 0,
        }
    }
    fn controls(&self) -> &'static [Control] {
        match self.kind {
            0 => &APPEARANCE,
            1 => &NETWORK,
            2 => &CLOCK,
            _ => &POWER,
        }
    }
}
impl DesktopApp for Builtin {
    fn descriptor(&self) -> AppDescriptor {
        ui::APPS[self.kind]
    }
    fn pointer(&mut self, x: usize, y: usize) -> Option<u16> {
        if let Some(c) = NAV.iter().find(|c| c.rect.contains(x, y)) {
            return Some(c.id);
        }
        let index = self.controls().iter().position(|c| c.rect.contains(x, y))?;
        self.focus = index;
        Some(self.controls()[index].id)
    }
    fn key(&mut self, byte: u8) -> Option<u16> {
        if byte == b'\t' {
            self.focus = (self.focus + 1) % self.controls().len();
            return Some(0);
        }
        let id = self.controls()[self.focus].id;
        if byte == b'\r' || byte == b'\n' {
            return Some(if self.kind == 1 && (id == 5 || id == 6) {
                7
            } else if self.kind == 2 && id == 1 {
                2
            } else {
                id
            });
        }
        Some(0x100 | byte as u16)
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AccessPoint {
    pub ssid: Text<32>,
    pub bssid: [u8; 6],
    pub rssi: i8,
    pub security: u8,
}
pub fn parse_scan(bytes: &[u8]) -> Option<Vec<AccessPoint>> {
    if bytes.len() != 776 {
        return None;
    }
    let count = u32::from_le_bytes(bytes[..4].try_into().ok()?) as usize;
    if count > 16 {
        return None;
    }
    let mut points = Vec::new();
    points.try_reserve_exact(count).ok()?;
    for i in 0..count {
        let e = &bytes[8 + i * 48..8 + (i + 1) * 48];
        let len = e[8] as usize;
        if len > 32 || e[9] > 3 || e[10] != 0 || e[11] != 0 {
            return None;
        }
        let Ok(text) = core::str::from_utf8(&e[12..12 + len]) else {
            continue;
        };
        if text.bytes().any(|b| !(32..=126).contains(&b)) {
            continue;
        }
        points.push(AccessPoint {
            ssid: Text::from_str(text),
            bssid: e[..6].try_into().ok()?,
            rssi: e[7] as i8,
            security: e[9],
        });
    }
    Some(points)
}

pub struct AppHost {
    pub preferences: Preferences,
    pub draft: Preferences,
    pub apps: [Builtin; 4],
    pub status: Text<112>,
    messages: [Text<112>; 4],
    message_page: usize,
    pub ssid: Text<32>,
    password: Text<64>,
    pub zone: Text<95>,
    pub network: Option<sys::NetworkInfo>,
    pub wireless: Option<sys::WirelessInfo>,
    pub calendar: Option<sys::Calendar>,
    pub local_seconds: Option<i64>,
    points: Vec<AccessPoint>,
    selected: usize,
    utc: i64,
    last_tick: i64,
    pub changed: bool,
    pub layout_changed: bool,
    pub wallpaper_changed: bool,
    scan_after: Option<i64>,
    page: usize,
    show_password: bool,
    connecting_until: Option<i64>,
    wifi_prepare_pid: Option<isize>,
    wifi_prepared: bool,
    pub clock_sync_in_progress: bool,
    pub clock_sync_until: Option<i64>,
    pub clock_synced: bool,
    pub battery: Option<sys::BatteryStatus>,
    pub show_power_menu: bool,
    pub power_profile: u8,
}
pub fn normalize_timezone(raw: &str) -> &'static str {
    let s = raw.trim();
    if s.is_empty() {
        return "";
    }
    // Brussels / Belgium / CET / CEST (covers common misspellings like "Brussles")
    if s.eq_ignore_ascii_case("brussels")
        || s.eq_ignore_ascii_case("europe/brussels")
        || s.eq_ignore_ascii_case("europe brussels")
        || s.eq_ignore_ascii_case("europe/brussles")
        || s.eq_ignore_ascii_case("europe brussles")
        || s.eq_ignore_ascii_case("brussles")
        || s.eq_ignore_ascii_case("belgium")
        || s.eq_ignore_ascii_case("cet")
        || s.eq_ignore_ascii_case("cest")
    {
        return "Europe/Brussels";
    }
    // UTC / GMT / Zulu
    if s.eq_ignore_ascii_case("utc")
        || s.eq_ignore_ascii_case("gmt")
        || s.eq_ignore_ascii_case("zulu")
    {
        return "UTC";
    }
    // New York / Eastern Time
    if s.eq_ignore_ascii_case("new york")
        || s.eq_ignore_ascii_case("america/new_york")
        || s.eq_ignore_ascii_case("america new_york")
        || s.eq_ignore_ascii_case("america/new york")
        || s.eq_ignore_ascii_case("america new york")
        || s.eq_ignore_ascii_case("nyc")
        || s.eq_ignore_ascii_case("est")
        || s.eq_ignore_ascii_case("edt")
    {
        return "America/New_York";
    }
    // Tokyo / Japan / JST
    if s.eq_ignore_ascii_case("tokyo")
        || s.eq_ignore_ascii_case("asia/tokyo")
        || s.eq_ignore_ascii_case("asia tokyo")
        || s.eq_ignore_ascii_case("japan")
        || s.eq_ignore_ascii_case("jst")
    {
        return "Asia/Tokyo";
    }
    // London / UK
    if s.eq_ignore_ascii_case("london")
        || s.eq_ignore_ascii_case("europe/london")
        || s.eq_ignore_ascii_case("europe london")
        || s.eq_ignore_ascii_case("uk")
        || s.eq_ignore_ascii_case("bst")
    {
        return "Europe/London";
    }
    // Paris / France
    if s.eq_ignore_ascii_case("paris")
        || s.eq_ignore_ascii_case("europe/paris")
        || s.eq_ignore_ascii_case("europe paris")
    {
        return "Europe/Paris";
    }
    // Berlin / Germany
    if s.eq_ignore_ascii_case("berlin")
        || s.eq_ignore_ascii_case("europe/berlin")
        || s.eq_ignore_ascii_case("europe berlin")
    {
        return "Europe/Berlin";
    }
    // Rome / Italy
    if s.eq_ignore_ascii_case("rome")
        || s.eq_ignore_ascii_case("europe/rome")
        || s.eq_ignore_ascii_case("europe rome")
    {
        return "Europe/Rome";
    }
    // Madrid / Spain
    if s.eq_ignore_ascii_case("madrid")
        || s.eq_ignore_ascii_case("europe/madrid")
        || s.eq_ignore_ascii_case("europe madrid")
    {
        return "Europe/Madrid";
    }
    // Amsterdam / Netherlands
    if s.eq_ignore_ascii_case("amsterdam")
        || s.eq_ignore_ascii_case("europe/amsterdam")
        || s.eq_ignore_ascii_case("europe amsterdam")
    {
        return "Europe/Amsterdam";
    }
    // Los Angeles / Pacific Time
    if s.eq_ignore_ascii_case("los angeles")
        || s.eq_ignore_ascii_case("america/los_angeles")
        || s.eq_ignore_ascii_case("america los_angeles")
        || s.eq_ignore_ascii_case("la")
        || s.eq_ignore_ascii_case("pst")
        || s.eq_ignore_ascii_case("pdt")
    {
        return "America/Los_Angeles";
    }
    // Chicago / Central Time
    if s.eq_ignore_ascii_case("chicago")
        || s.eq_ignore_ascii_case("america/chicago")
        || s.eq_ignore_ascii_case("america chicago")
        || s.eq_ignore_ascii_case("cst")
        || s.eq_ignore_ascii_case("cdt")
    {
        return "America/Chicago";
    }
    // Sao Paulo / Brazil
    if s.eq_ignore_ascii_case("sao paulo")
        || s.eq_ignore_ascii_case("america/sao_paulo")
        || s.eq_ignore_ascii_case("america sao_paulo")
    {
        return "America/Sao_Paulo";
    }
    ""
}

impl AppHost {
    pub fn new(preferences: Preferences) -> Self {
        Self {
            preferences,
            draft: preferences,
            apps: [
                Builtin {
                    kind: 0,
                    focus: 0,
                    size: (0, 0),
                    scroll: 0,
                },
                Builtin {
                    kind: 1,
                    focus: 0,
                    size: (0, 0),
                    scroll: 0,
                },
                Builtin {
                    kind: 2,
                    focus: 0,
                    size: (0, 0),
                    scroll: 0,
                },
                Builtin {
                    kind: 3,
                    focus: 0,
                    size: (0, 0),
                    scroll: 0,
                },
            ],
            status: Text::from_str(
                "Preferences persist on installed roots across reboots.",
            ),
            messages: [
                Text::from_str(
                    "Preferences persist on installed roots across reboots.",
                ),
                Text::from_str("Wi-Fi credentials are not stored by the desktop."),
                Text::from_str("Choose an installed timezone and save to apply it."),
                Text::from_str("Configure system power management and energy saving profile."),
            ],
            message_page: 0,
            ssid: Text::default(),
            password: Text::default(),
            zone: Text::from_str("UTC"),
            network: None,
            wireless: None,
            calendar: None,
            local_seconds: None,
            points: Vec::new(),
            selected: usize::MAX,
            utc: 0,
            last_tick: -1,
            changed: false,
            layout_changed: false,
            wallpaper_changed: false,
            scan_after: None,
            page: 0,
            show_password: false,
            connecting_until: None,
            wifi_prepare_pid: None,
            wifi_prepared: false,
            clock_sync_in_progress: false,
            clock_sync_until: None,
            clock_synced: false,
            battery: None,
            show_power_menu: false,
            power_profile: 0,
        }
    }
    #[inline(never)]
    pub fn load(is_test: bool) -> Option<Box<Self>> {
        let (preferences, valid) = if is_test {
            (Preferences::default(), true)
        } else {
            load_preferences()
        };
        let layout = alloc::alloc::Layout::new::<Self>();
        let ptr = unsafe { alloc::alloc::alloc(layout) } as *mut Self;
        if ptr.is_null() {
            return None;
        }
        unsafe {
            ptr.write(Self::new(preferences));
        }
        let mut host = unsafe { Box::from_raw(ptr) };
        if !valid {
            host.message("Invalid desktop.conf; defaults active. Save to replace it.");
        }
        if !is_test {
            let mut text = [0u8; 97];
            if let Ok(n) = sys::read_file("/etc/timezone", &mut text) {
                if let Ok(zone) = core::str::from_utf8(&text[..n]) {
                    host.zone = Text::from_str(zone.trim());
                }
            }
            theme::set_scale(preferences.scale);
        }
        Some(host)
    }
    fn message(&mut self, message: &str) {
        self.status = Text::from_str(message);
        self.messages[self.message_page] = self.status.clone();
    }
    fn error(&mut self, message: &str, error: isize) {
        self.status.clear();
        let reason = match error {
            -5 => "file not found",
            -6 => "too many open files",
            -9 => "storage or device error",
            -10 => "not enough memory",
            -11 => "read-only filesystem",
            -12 => "file is too large",
            -13 => "no free storage space",
            -14 => "operation unavailable",
            -15 => "temporary settings file already exists",
            -22 => "operation interrupted",
            -24 => "permission denied",
            _ => "system operation failed",
        };
        let _ = write!(self.status, "{}: {}", message, reason);
        self.messages[self.message_page] = self.status.clone();
    }
    pub fn open(&mut self, id: u32, wm: &mut WindowManager, s: &mut Surface) {
        let Some(index) = ui::APPS.iter().position(|a| a.id == id) else {
            return;
        };
        // If any built-in settings window (7, 8, or 9) is already open,
        // reuse the open window and switch its page in-place!
        if let Some(open_id) = [7, 8, 9, 10].iter().copied().find(|&oid| wm.windows.iter().any(|w| w.id == oid)) {
            let open_idx = ui::APPS.iter().position(|a| a.id == open_id).unwrap_or(0);
            self.apps[open_idx].kind = index;
            self.apps[open_idx].focus = 0;
            self.apps[open_idx].scroll = 0;
            self.apps[open_idx].size = (0, 0);
            self.message_page = index;
            wm.unminimize_and_raise(open_id, s);
            #[cfg(not(test))]
            if index == 1 {
                self.wireless = sys::wireless_info();
                if self.scan_after.is_none() && self.connecting_until.is_none() {
                    self.scan();
                }
            }
            self.render(open_id, wm, s);
            return;
        }
        let descriptor = self.apps[index].descriptor();
        self.message_page = index;
        if !wm.windows.iter().any(|w| w.id == id) {
            let scale = theme::scale(s.height);
            let rect = Rect {
                x: 64,
                y: 24 * scale + 24,
                w: (descriptor.width * scale).min(s.width.saturating_sub(96)),
                h: (descriptor.height * scale).min(s.height.saturating_sub(90 * scale + 48)),
            };
            let Some(window) = Window::new(id, descriptor.name, rect) else {
                self.message("Not enough desktop memory to open the app.");
                return;
            };
            if wm.windows.try_reserve(1).is_err() {
                self.message("Not enough desktop memory.");
                return;
            }
            wm.windows.push(window);
            self.apps[index].size = (0, 0);
            self.apps[index].scroll = 0;
        }
        wm.unminimize_and_raise(id, s);
        #[cfg(not(test))]
        if index == 1 {
            self.wireless = sys::wireless_info();
            if self.scan_after.is_none() && self.connecting_until.is_none() {
                self.scan();
            }
        }
        self.render(id, wm, s);
    }
    pub fn pointer(
        &mut self,
        id: u32,
        x: usize,
        y: usize,
        wm: &mut WindowManager,
        s: &mut Surface,
    ) {
        let Some(index) = ui::APPS.iter().position(|a| a.id == id) else {
            return;
        };
        let scale = theme::scale(s.height);
        let logical_w = wm
            .windows
            .iter()
            .find(|w| w.id == id || [7, 8, 9].contains(&w.id))
            .map(|w| (w.rect.w - 2) / scale)
            .unwrap_or(0);
        let x_off = ui::content_offset(logical_w);
        let lx = x / scale;
        let adj_x = if lx > 180 { lx.saturating_sub(x_off) } else { lx };
        if let Some(command) =
            self.apps[index].pointer(adj_x, y / scale + self.apps[index].scroll)
        {
            self.command(index, command, wm, s);
            if self.apps[index].kind == 1 && (20..=23).contains(&command) {
                self.reveal_focus(index, wm, s);
            }
        }
        self.render(id, wm, s);
    }
    pub fn key(&mut self, id: u32, byte: u8, wm: &mut WindowManager, s: &mut Surface) {
        let Some(index) = ui::APPS.iter().position(|a| a.id == id) else {
            return;
        };
        if byte == 27 {
            if self.apps[index].kind == 1 {
                self.password.clear();
                self.show_password = false;
            }
            wm.close_window(id, s);
            return;
        }
        if byte == 21 || byte == 4 {
            self.scroll(id, if byte == 21 { -1 } else { 1 }, wm, s);
            return;
        }
        if let Some(command) = self.apps[index].key(byte) {
            self.command(index, command, wm, s);
            if byte == b'\t' {
                self.reveal_focus(index, wm, s);
            }
        }
        self.render(id, wm, s);
    }
    fn reveal_focus(&mut self, index: usize, wm: &WindowManager, s: &Surface) {
        let Some(w) = wm.windows.iter().find(|w| w.id == ui::APPS[index].id) else {
            return;
        };
        let visible = (w.rect.h - 26) / theme::scale(s.height);
        let r = self.apps[index].controls()[self.apps[index].focus].rect;
        let scroll = self.apps[index].scroll;
        let target = if r.y < scroll {
            r.y.saturating_sub(8)
        } else if r.y + r.h > scroll + visible {
            (r.y + r.h + 8).saturating_sub(visible)
        } else {
            scroll
        };
        let maximum = ui::APPS[index]
            .height
            .saturating_sub(26)
            .saturating_sub(visible);
        self.apps[index].scroll = target.min(maximum);
        if self.apps[index].scroll != scroll {
            self.apps[index].size = (0, 0);
        }
    }
    pub fn scroll(&mut self, id: u32, steps: i32, wm: &mut WindowManager, s: &mut Surface) {
        let Some(index) = ui::APPS.iter().position(|a| a.id == id) else {
            return;
        };
        let Some(w) = wm.windows.iter().find(|w| w.id == id) else {
            return;
        };
        let visible = (w.rect.h - 26) / theme::scale(s.height);
        let maximum = ui::APPS[index]
            .height
            .saturating_sub(26)
            .saturating_sub(visible);
        let offset =
            (self.apps[index].scroll as i64 + steps as i64 * 32).clamp(0, maximum as i64) as usize;
        if offset != self.apps[index].scroll {
            self.apps[index].scroll = offset;
            self.apps[index].size = (0, 0);
            self.render(id, wm, s);
        }
    }
    fn command(&mut self, index: usize, command: u16, wm: &mut WindowManager, s: &mut Surface) {
        let kind = self.apps[index].kind;
        let id = ui::APPS[index].id;
        if (100..104).contains(&command) {
            let target = (command - 100) as usize;
            if self.apps[index].kind != target {
                self.apps[index].kind = target;
                self.apps[index].focus = 0;
                self.apps[index].scroll = 0;
                self.apps[index].size = (0, 0);
                if target == 1 {
                    #[cfg(not(test))]
                    {
                        self.wireless = sys::wireless_info();
                        if self.scan_after.is_none() && self.connecting_until.is_none() {
                            self.scan();
                        }
                    }
                }
                self.render(id, wm, s);
            }
            return;
        }
        self.message_page = kind;
        if kind == 1 && self.wifi_prepare_pid.is_some() { return; }
        if kind == 1 && self.connecting_until.is_some() && command != 2 {
            return;
        }
        if kind == 1 && matches!(command, 1 | 2 | 7) && self.wireless.is_none() {
            self.password.clear();
            self.message("No Wi-Fi adapter is available.");
            return;
        }
        if command & 0x100 != 0 {
            let byte = command as u8;
            let focus = self.apps[index].controls()[self.apps[index].focus].id;
            if kind == 1 && self.wireless.is_none() {
                return;
            }
            match (kind, focus) {
                (1, 5) => {
                    if self.ssid.edit(byte) {
                        self.selected = usize::MAX;
                        self.password.clear();
                        self.show_password = false;
                    }
                }
                (1, 6) => {
                    self.password.edit_secret(byte);
                }
                (2, 1) => {
                    self.zone.edit(byte);
                }
                _ => (),
            }
            return;
        }
        match kind {
            0 => match command {
                1..=3 => self.draft.scale = (command - 1) as u8,
                4 => {
                    self.draft.wallpaper = true;
                    self.wallpaper_changed |= self.preferences.wallpaper != self.draft.wallpaper;
                    self.changed |= self.preferences != self.draft;
                    self.preferences.wallpaper = true;
                    self.save_preferences();
                }
                5 => {
                    self.draft.wallpaper = false;
                    self.wallpaper_changed |= self.preferences.wallpaper != self.draft.wallpaper;
                    self.changed |= self.preferences != self.draft;
                    self.preferences.wallpaper = false;
                    self.save_preferences();
                }
                6 => {
                    self.draft.clock_24h = true;
                    self.changed |= self.preferences.clock_24h != self.draft.clock_24h;
                    self.preferences.clock_24h = true;
                    self.save_preferences();
                }
                7 => {
                    self.draft.clock_24h = false;
                    self.changed |= self.preferences.clock_24h != self.draft.clock_24h;
                    self.preferences.clock_24h = false;
                    self.save_preferences();
                }
                8 => self.save_preferences(),
                9 => {
                    self.draft = Preferences::default();
                    self.save_preferences();
                    self.message("Defaults applied.");
                }
                _ => (),
            },
            1 => match command {
                1 => self.scan(),
                2 => {
                    let result = unsafe { sys::syscall3(sys::SYS_WIFI, 3, 0, 0) };
                    self.password.clear();
                    self.connecting_until = None;
                    self.show_password = false;
                    if result < 0 {
                        self.error("Disconnect unavailable", result);
                    } else {
                        self.message("Wi-Fi disconnected.");
                    }
                }
                3 | 4 => {
                    self.page = if command == 3 {
                        self.page.saturating_sub(1)
                    } else {
                        (self.page + 1).min(self.points.len().saturating_sub(1) / 4)
                    };
                }
                7 => self.connect(),
                8 => self.show_password = !self.show_password,
                20..=23 => self.select_network(self.page * 4 + (command - 20) as usize),
                _ => (),
            },
            2 => match command {
                2 => self.save_zone(),
                3 => {
                    self.zone = Text::from_str("UTC");
                    self.save_zone();
                }
                4 => {
                    self.zone = Text::from_str("Europe/Brussels");
                    self.save_zone();
                }
                5 => {
                    self.zone = Text::from_str("America/New_York");
                    self.save_zone();
                }
                6 => {
                    self.zone = Text::from_str("Asia/Tokyo");
                    self.save_zone();
                }
                7 => {
                    self.draft.clock_24h = !self.draft.clock_24h;
                    self.save_preferences();
                }
                8 => self.sync_clock(),
                _ => (),
            },
            3 => match command {
                1 => {
                    self.power_profile = 0;
                    wm.power_profile = 0;
                    self.message("Power profile: Balanced / Battery Saver");
                    s.damage(WindowManager::power_menu_rect(s.width, s.height));
                }
                2 => {
                    self.power_profile = 1;
                    wm.power_profile = 1;
                    self.message("Power profile: High Performance");
                    s.damage(WindowManager::power_menu_rect(s.width, s.height));
                }
                _ => (),
            },
            _ => (),
        }
    }
    #[inline(never)]
    pub fn sync_clock(&mut self) {
        if self.clock_sync_in_progress {
            self.message_page = 2;
            self.message("Clock synchronization is already in progress.");
            return;
        }
        match sys::clock_sync_start("pool.ntp.org") {
            Ok(()) => {
                self.clock_sync_in_progress = true;
                self.clock_sync_until = sys::monotonic_seconds().map(|s| s.saturating_add(8));
                self.message_page = 2;
                self.message("Synchronizing clock with pool.ntp.org via NTP...");
            }
            Err(e) => {
                self.message_page = 2;
                self.error("Clock sync failed to start", e);
            }
        }
    }
    #[inline(never)]
    fn save_preferences(&mut self) {
        let mut text = Text::<512>::default();
        if self.draft.encode(&mut text).is_err() {
            self.message("Invalid preferences.");
            return;
        }
        match sys::write_setting(sys::SETTING_PREFERENCES, text.as_bytes()) {
            Ok(()) => {
                self.layout_changed |= self.preferences.scale != self.draft.scale;
                self.wallpaper_changed |= self.preferences.wallpaper != self.draft.wallpaper;
                self.changed |= self.preferences != self.draft;
                self.preferences = self.draft;
                theme::set_scale(self.preferences.scale);
                self.message("Preferences saved. Live-media changes last until reboot.");
            }
            Err(error) => {
                #[cfg(not(test))]
                self.error("Preferences were not saved", error);
                #[cfg(test)]
                {
                    let _ = error;
                }
            }
        }
    }

    #[inline(never)]
    fn save_zone(&mut self) {
        let norm = normalize_timezone(self.zone.as_str());
        if !norm.is_empty() {
            self.zone = Text::from_str(norm);
        }
        if !sys::zone_valid(self.zone.as_str(), self.utc) {
            self.message("Unknown timezone; enter an installed Region/City name.");
            return;
        }
        let mut text = Text::<128>::default();
        let _ = writeln!(text, "{}", self.zone.as_str());
        match sys::write_setting(sys::SETTING_TIMEZONE, text.as_bytes()) {
            Ok(()) => {
                sys::invalidate_clock();
                self.last_tick = -1;
                if let Some((local, calendar)) = sys::local_clock(self.utc) {
                    self.local_seconds = Some(local);
                    self.calendar = Some(calendar);
                } else if self.utc > 0 {
                    self.local_seconds = Some(self.utc);
                }
                self.message("System timezone saved; daylight-saving rules apply automatically.");
            }
            Err(error) => self.error("Timezone was not saved", error),
        }
    }
    #[inline(never)]
    fn scan(&mut self) {
        if self.scan_after.is_some() || self.connecting_until.is_some() {
            return;
        }
        if self.wireless.is_some_and(|w| (2..=4).contains(&w.state)) {
            self.message("A Wi-Fi connection is already in progress. Please wait.");
            return;
        }
        if self.wireless.is_none() {
            self.message("No Wi-Fi adapter is available.");
            return;
        }
        if !self.wifi_prepared {
            if self.wifi_prepare_pid.is_none() {
                let pid = sys::wireless_prepare();
                if pid > 0 {
                    self.wifi_prepare_pid = Some(pid);
                    self.message("Preparing Wi-Fi. Scanning will start automatically...");
                } else {
                    self.error("Could not start Wi-Fi preparation", pid);
                }
            }
            return;
        }
        let mut bytes = Vec::new();
        if bytes.try_reserve_exact(776).is_err() {
            self.message("Not enough scan memory.");
            return;
        }
        bytes.resize(776, 0);
        let result =
            unsafe { sys::syscall3(sys::SYS_WIFI, 1, bytes.as_mut_ptr() as usize, bytes.len()) };
        if result < 0 {
            self.error("Wi-Fi scan unavailable", result);
            return;
        }
        let Some(now) = sys::monotonic_seconds() else {
            self.message("Cannot schedule scan result collection.");
            return;
        };
        self.scan_after = Some(now.saturating_add(2));
        self.message("Scanning. Results will update without blocking the desktop.");
    }
    fn collect_scan(&mut self) {
        let mut bytes = Vec::new();
        if bytes.try_reserve_exact(776).is_err() {
            self.message("Not enough scan memory.");
            return;
        }
        bytes.resize(776, 0);
        let result = sys::wireless_scan_results(&mut bytes);
        if result < 0 {
            self.error("Scan results unavailable", result);
            return;
        }
        let Some(points) = parse_scan(&bytes) else {
            self.message("Invalid Wi-Fi scan response.");
            return;
        };
        self.set_networks(points);
    }
    fn set_networks(&mut self, mut points: Vec<AccessPoint>) {
        // At most sixteen entries: insertion sort avoids the generic sort's
        // large temporary stack buffer in this freestanding process.
        for i in 1..points.len() {
            let mut j = i;
            while j > 0 && points[j].rssi > points[j - 1].rssi {
                points.swap(j, j - 1);
                j -= 1;
            }
        }
        let mut unique: Vec<AccessPoint> = Vec::new();
        if unique.try_reserve_exact(points.len()).is_err() {
            self.message("Not enough memory to show nearby networks.");
            return;
        }
        for point in points {
            if point.ssid.len() != 0
                && !unique
                    .iter()
                    .any(|p| p.ssid == point.ssid && p.security == point.security)
            {
                unique.push(point);
            }
        }
        self.points = unique;
        self.page = 0;
        self.selected = usize::MAX;
        self.message(if self.points.is_empty() {
            "No networks found. Move closer to your router and try Refresh."
        } else {
            "Choose a network below, then enter its password to join."
        });
    }
    fn select_network(&mut self, index: usize) {
        let Some(point) = self.points.get(index) else {
            return;
        };
        self.ssid = point.ssid.clone();
        self.selected = index;
        self.password.clear();
        self.show_password = false;
        let next_focus = if point.security == 0 { 6 } else { 5 };
        for app in &mut self.apps {
            if app.kind == 1 {
                app.focus = next_focus;
            }
        }
        if point.security == 0 {
            self.message("Open network selected. Press Enter or click Connect to join.");
        } else {
            self.message("Enter the Wi-Fi password, then press Enter to join.");
        }
    }
    fn connection_feedback(&mut self, now: i64) {
        let Some(deadline) = self.connecting_until else {
            return;
        };
        let state = self.wireless.map(|w| (w.state, w.online));
        let message = match state {
            Some((5, online)) if online != 0 => "Connected to Wi-Fi.",
            Some((6, _)) => "Could not connect. Check the password and signal, then try again.",
            _ if now >= deadline => "Connection timed out. Check your router and try again.",
            Some((2, _)) => "Connecting: authenticating with the network...",
            Some((3, _)) => "Connecting: joining the network...",
            Some((4, _)) => "Connecting: checking the Wi-Fi password...",
            _ => "Connecting to your Wi-Fi network...",
        };
        if state.is_some_and(|(state, online)| state == 6 || (state == 5 && online != 0))
            || now >= deadline
        {
            self.connecting_until = None;
        }
        self.messages[1] = Text::from_str(message);
    }
    fn connect(&mut self) {
        if self.connecting_until.is_some() || self.scan_after.is_some() {
            return;
        }
        if self.wireless.is_none() {
            self.password.clear();
            self.message("No Wi-Fi adapter is available.");
            return;
        }
        if self.ssid.len() == 0 {
            self.message("Choose a network or enter its name first.");
            return;
        }
        let is_open = self
            .points
            .get(self.selected)
            .filter(|p| p.ssid == self.ssid)
            .is_some_and(|p| p.security == 0)
            || (self.selected == usize::MAX
                && self.password.len() == 0
                && self
                    .points
                    .iter()
                    .any(|p| p.ssid == self.ssid && p.security == 0));
        if !is_open && self.password.len() < 8 {
            self.message("Enter a Wi-Fi password of 8 to 64 characters.");
            for app in &mut self.apps {
                if app.kind == 1 {
                    app.focus = 5;
                }
            }
            return;
        }
        if is_open {
            self.password.clear();
        }
        if !self.wifi_prepared {
            self.message("Connecting to open or secured network after Wi-Fi preparation...");
            self.scan();
            return;
        }
        let bssid = self
            .points
            .get(self.selected)
            .filter(|p| p.ssid == self.ssid)
            .map(|p| p.bssid)
            .unwrap_or([0; 6]);
        let _ = sys::wireless_save_network(self.ssid.as_str(), self.password.as_bytes());
        let result = sys::wireless_connect(self.ssid.as_str(), self.password.as_bytes(), bssid);
        self.password.clear();
        self.show_password = false;
        if result < 0 {
            self.error("Wi-Fi connection request failed", result);
        } else {
            self.connecting_until = sys::monotonic_seconds().map(|n| n.saturating_add(20));
            self.message("Connecting to your Wi-Fi network...");
        }
    }
    fn finish_wifi_preparation(&mut self, result: Result<(), isize>) -> bool {
        self.wifi_prepare_pid = None;
        self.wifi_prepared = result.is_ok();
        self.message_page = 1;
        if let Err(error) = result {
            self.error("Wi-Fi preparation failed; try Refresh", error);
        }
        self.wifi_prepared
    }
    fn wifi_window_open(&self, wm: &WindowManager, unminimized_only: bool) -> bool {
        wm.windows.iter().any(|w| {
            (!unminimized_only || !w.is_minimized)
                && (w.id == 8
                    || ui::APPS
                        .iter()
                        .position(|a| a.id == w.id)
                        .is_some_and(|idx| self.apps[idx].kind == 1))
        })
    }
    pub fn refresh(&mut self, mut utc: i64, wm: &mut WindowManager, s: &mut Surface) {
        if !self.wifi_window_open(wm, false) {
            self.password.clear();
            self.show_password = false;
            self.scan_after = None;
        }
        if self.last_tick == utc && !self.clock_sync_in_progress {
            return;
        }
        self.last_tick = utc;

        // Poll battery status once per second and damage bounded rect on change
        let new_battery = sys::battery_get().ok();
        if new_battery != self.battery {
            let old_pct = self.battery.map(|b| b.capacity_percent);
            let new_pct = new_battery.map(|b| b.capacity_percent);
            let old_state = self.battery.map(|b| b.state);
            let new_state = new_battery.map(|b| b.state);
            self.battery = new_battery;
            wm.battery_status = new_battery;
            if old_pct != new_pct || old_state != new_state {
                s.damage(WindowManager::battery_rect(s.width, s.height));
                if wm.show_power_menu {
                    s.damage(WindowManager::power_menu_rect(s.width, s.height));
                }
            }
        }
        self.show_power_menu = wm.show_power_menu;
        if self.clock_sync_in_progress {
            let status = sys::clock_sync_status();
            let now = sys::monotonic_seconds().unwrap_or(0);
            let timed_out = self.clock_sync_until.is_some_and(|until| now >= until);
            if status == 0 {
                self.clock_sync_in_progress = false;
                self.clock_sync_until = None;
                self.clock_synced = true;
                sys::invalidate_clock();
                let mut now_ts = sys::Timespec::default();
                let effective_utc = if sys::clock_gettime(0, &mut now_ts) == 0 {
                    now_ts.tv_sec
                } else {
                    utc
                };
                utc = effective_utc;
                self.last_tick = effective_utc;
                wm.force_clock_damage(s);
                self.message_page = 2;
                self.message("Clock synchronized successfully via NTP (pool.ntp.org).");
            } else if status < 0 || timed_out {
                self.clock_sync_in_progress = false;
                self.clock_sync_until = None;
                self.message_page = 2;
                self.message("NTP sync failed: check network connection or NTP server.");
            }
        }
        if let Some(pid) = self.wifi_prepare_pid {
            if let Some(result) = sys::child_status(pid) {
                if self.finish_wifi_preparation(result) && self.wifi_window_open(wm, false) {
                    self.wireless = sys::wireless_info();
                    self.scan();
                }
            }
        }
        self.utc = utc;
        if let Some((local, calendar)) = sys::local_clock(utc) {
            self.local_seconds = Some(local);
            self.calendar = Some(calendar);
        } else if utc > 0 {
            self.local_seconds = Some(utc);
            let mut cal = sys::Calendar::default();
            let s_mod = utc.rem_euclid(86400) as i32;
            cal.second = s_mod % 60;
            cal.minute = (s_mod / 60) % 60;
            cal.hour = s_mod / 3600;
            self.calendar = Some(cal);
        } else {
            self.local_seconds = None;
            self.calendar = None;
        }
        self.network = sys::network_info();
        if self.wifi_window_open(wm, true) {
            self.wireless = sys::wireless_info();
            if let Some(now) = sys::monotonic_seconds() {
                self.connection_feedback(now);
            }
        }
        if self
            .scan_after
            .is_some_and(|after| sys::monotonic_seconds().is_some_and(|now| now >= after))
        {
            self.scan_after = None;
            self.message_page = 1;
            self.collect_scan();
        }
        let state = self
            .network
            .map(|n| if n.link == 2 { 2 } else { 1 })
            .unwrap_or(0);
        wm.set_network_state(s, state);
        wm.clock_24h = self.preferences.clock_24h;
        let clock_val = self.local_seconds.unwrap_or(if utc > 0 { utc } else { -1 });
        wm.update_clock(s, clock_val);
        for app in ui::APPS {
            self.render(app.id, wm, s);
        }
    }
    pub fn render(&mut self, id: u32, wm: &mut WindowManager, s: &mut Surface) {
        let Some(index) = ui::APPS.iter().position(|a| a.id == id) else {
            return;
        };
        let Some(window) = wm
            .windows
            .iter_mut()
            .find(|w| w.id == id && !w.is_minimized)
        else {
            return;
        };
        let dimensions = (window.rect.w, window.rect.h);
        let reset = self.apps[index].size != dimensions || window.needs_app_repaint;
        self.apps[index].size = dimensions;
        let visible = (window.rect.h - 26) / theme::scale(s.height);
        let maximum = ui::APPS[index]
            .height
            .saturating_sub(26)
            .saturating_sub(visible);
        self.apps[index].scroll = self.apps[index].scroll.min(maximum);
        let mut canvas = Canvas::scrolled(
            window,
            theme::scale(s.height),
            reset,
            self.apps[index].scroll,
        );
        self.draw(index, &mut canvas);
        let dirty = canvas.finish();
        window.needs_app_repaint = false;
        if let Some(dirty) = dirty {
            s.damage(dirty);
        }
    }
    pub fn invalidate(&mut self) {
        for app in &mut self.apps {
            app.size = (0, 0);
            app.scroll = 0;
        }
    }
    pub fn paint_invalid(&mut self, wm: &mut WindowManager, s: &mut Surface) {
        for index in 0..self.apps.len() {
            let id = self.apps[index].descriptor().id;
            if wm.windows.iter().any(|w| {
                w.id == id && !w.is_minimized
                    && (self.apps[index].size != (w.rect.w, w.rect.h) || w.needs_app_repaint)
            }) {
                self.render(id, wm, s);
            }
        }
    }
    pub fn reflow(&mut self, wm: &mut WindowManager, s: &mut Surface) {
        let scale = theme::scale(s.height);
        let work_w = s.width.saturating_sub(16 * scale);
        let work_h = s.height.saturating_sub(104 * scale);
        for w in &mut wm.windows {
            if w.id < 3 {
                continue;
            }
            let (width, height, left, top) = if w.saved_rect.is_some() {
                let half = w.rect.w < s.width * 3 / 4;
                (
                    if half {
                        (s.width / 2).saturating_sub(12 * scale)
                    } else {
                        work_w
                    },
                    work_h,
                    if half && w.rect.x >= s.width / 2 {
                        s.width / 2 + 4 * scale
                    } else {
                        8 * scale
                    },
                    28 * scale,
                )
            } else {
                let (width, height) = if w.id == 6 {
                    ((520 * scale).min(work_w), (350 * scale).min(work_h))
                } else if let Some(app) = ui::APPS.iter().find(|a| a.id == w.id) {
                    (
                        (app.width * scale).min(s.width.saturating_sub(96)),
                        (app.height * scale).min(s.height.saturating_sub(90 * scale + 48)),
                    )
                } else {
                    (w.rect.w.min(work_w), w.rect.h.min(work_h))
                };
                (
                    width,
                    height,
                    w.rect.x.min(s.width.saturating_sub(width + 8 * scale)),
                    w.rect
                        .y
                        .max(28 * scale)
                        .min(s.height.saturating_sub(height + 76 * scale)),
                )
            };
            if w.resize(width, height) {
                w.rect.x = left;
                w.rect.y = top;
            } else {
                self.message_page = 0;
                self.message("Not enough memory to resize every window. Try native scale.");
            }
        }
        self.invalidate();
        wm.invalidate_panels();
        s.damage(Rect {
            x: 0,
            y: 0,
            w: s.width,
            h: s.height,
        });
    }
    fn draw_sidebar(&self, index: usize, c: &mut Canvas) {
        let h = c.height();
        c.fill_rect(Rect { x: 180, y: 0, w: 1, h }, 0xff263040);

        c.badge_on(
            Rect { x: 14, y: 11, w: 30, h: 30 },
            "O",
            0xffffffff,
            0xff1e385b,
            theme::ACCENT,
            0xff111620,
        );
        c.label_bold(52, 9, 120, 18, "Operator", 0xffffffff, 0xff111620);
        c.label_h(52, 26, 120, 18, "Administrator", theme::SUBTEXT, 0xff111620);

        c.fill_rect(Rect { x: 14, y: 48, w: 152, h: 1 }, 0xff222d3d);

        let nav_items = [
            "Personalization",
            "Network & Wi-Fi",
            "Time & Language",
            "Power & Battery",
        ];
        for (i, control) in NAV.iter().enumerate() {
            c.sidebar_nav(
                control.rect,
                nav_items[i],
                i == index,
                false,
            );
        }

        if h >= 400 {
            c.fill_rect(Rect { x: 14, y: h.saturating_sub(36), w: 152, h: 1 }, 0xff1c2533);
            c.label_h(14, h.saturating_sub(26), 150, 18, "FortressOS v0.2.0", 0xff64748b, 0xff111620);
        }
    }
    fn draw(&self, index: usize, c: &mut Canvas) {
        let kind = self.apps[index].kind;
        self.draw_sidebar(kind, c);
        let focus = self.apps[index].focus;
        let mut text = Text::<112>::default();
        match kind {
            0 => {
                c.label_h(196, 12, 520, 18, "Personalization", theme::TEXT, theme::PANEL);
                c.label_h(196, 32, 520, 18, "Display scaling, wallpaper artwork, and clock format", theme::SUBTEXT, theme::PANEL);

                c.label_bold(196, 56, 520, 18, "Interface Scale", 0xffffffff, theme::PANEL);
                c.label_h(196, 74, 520, 18, "Scale windows and desktop elements for screen density", theme::SUBTEXT, theme::PANEL);
                for i in 0..3 {
                    c.button(
                        APPEARANCE[i].rect,
                        ["Automatic", "Native (1x)", "Large (2x)"][i],
                        self.draft.scale as usize == i,
                        focus == i,
                    );
                }
                c.fill_rect(Rect { x: 196, y: 130, w: 540, h: 1 }, 0xff2c313c);

                c.label_bold(196, 138, 520, 18, "Desktop Background", 0xffffffff, theme::PANEL);
                c.label_h(196, 156, 520, 18, "Choose wallpaper artwork or ambient radial glow", theme::SUBTEXT, theme::PANEL);
                c.wallpaper_card(
                    APPEARANCE[3].rect,
                    "Hello World",
                    true,
                    self.draft.wallpaper,
                    focus == 3,
                );
                c.wallpaper_card(
                    APPEARANCE[4].rect,
                    "Radial Glow",
                    false,
                    !self.draft.wallpaper,
                    focus == 4,
                );
                c.fill_rect(Rect { x: 196, y: 214, w: 540, h: 1 }, 0xff2c313c);

                c.label_bold(196, 222, 520, 18, "Taskbar Clock", 0xffffffff, theme::PANEL);
                c.label_h(196, 240, 520, 18, "Choose between 24-hour and 12-hour clock format", theme::SUBTEXT, theme::PANEL);
                c.button(
                    APPEARANCE[5].rect,
                    "24-Hour",
                    self.draft.clock_24h,
                    focus == 5,
                );
                c.button(
                    APPEARANCE[6].rect,
                    "12-Hour",
                    !self.draft.clock_24h,
                    focus == 6,
                );
                c.fill_rect(Rect { x: 196, y: 298, w: 540, h: 1 }, 0xff2c313c);

                c.button_accent(APPEARANCE[7].rect, "Apply & Save", focus == 7);
                c.button(APPEARANCE[8].rect, "Use Defaults", false, focus == 8);

                c.label_h(196, 350, 540, 18, self.messages[kind].as_str(), theme::MUTED, theme::PANEL);
            }
            1 => self.draw_network(c, focus),
            2 => {
                c.label_h(196, 12, 520, 18, "Time & Language", theme::TEXT, theme::PANEL);
                c.label_h(196, 32, 520, 18, "System clock, time synchronization, and regional timezone", theme::SUBTEXT, theme::PANEL);

                c.card(Rect { x: 196, y: 52, w: 540, h: 52 }, 0xff282828, theme::BORDER);
                c.icon_badge(
                    Rect { x: 204, y: 64, w: 32, h: 28 },
                    2,
                    theme::ACCENT,
                    0xff1e2d40,
                    theme::ACCENT,
                    0xff282828,
                );
                if let Some(t) = self.calendar {
                    let _ = write!(
                        text,
                        "{:04}-{:02}-{:02}    {:02}:{:02}:{:02}",
                        t.year, t.month, t.day, t.hour, t.minute, t.second
                    );
                } else {
                    let _ = write!(text, "Clock awaiting RTC synchronization");
                }
                c.label_bold(246, 58, 360, 18, text.as_str(), 0xffffffff, 0xff282828);
                let (sublabel, subcolor) = if self.clock_sync_in_progress {
                    ("Synchronizing clock via NTP...", 0xfffacc15)
                } else if self.clock_synced {
                    ("Clock synchronized via NTP (pool.ntp.org)", 0xff4ade80)
                } else {
                    ("Hardware RTC clock synchronized", theme::SUBTEXT)
                };
                c.label_h(246, 78, 360, 18, sublabel, subcolor, 0xff282828);
                if self.clock_sync_in_progress {
                    c.button(CLOCK[7].rect, "Syncing...", true, focus == 7);
                } else if self.clock_synced {
                    c.badge_on(
                        CLOCK[7].rect,
                        "NTP Synced",
                        0xff4ade80,
                        0xff133223,
                        0xff22c55e,
                        0xff282828,
                    );
                } else {
                    c.button_accent(CLOCK[7].rect, "Sync Clock", focus == 7);
                }

                c.label_bold(196, 114, 520, 18, "System Timezone", 0xffffffff, theme::PANEL);
                c.label_h(196, 132, 520, 18, "Enter an installed IANA Region/City name", theme::SUBTEXT, theme::PANEL);
                c.entry_on(CLOCK[0].rect, self.zone.as_str(), focus == 0, theme::PANEL);
                c.button_accent(CLOCK[1].rect, "Save Timezone", focus == 1);
                c.label_h(196, 186, 520, 18, "Quick Presets", theme::SUBTEXT, theme::PANEL);
                for (i, label) in ["UTC", "Brussels", "New York", "Tokyo"].iter().enumerate() {
                    c.button(CLOCK[i + 2].rect, label, false, focus == i + 2);
                }
                c.fill_rect(Rect { x: 196, y: 238, w: 540, h: 1 }, 0xff2c313c);

                c.label_bold(196, 246, 520, 18, "Taskbar Clock Format", 0xffffffff, theme::PANEL);
                c.label_h(196, 264, 520, 18, "Toggle between 12-hour and 24-hour display format", theme::SUBTEXT, theme::PANEL);
                c.button(
                    CLOCK[6].rect,
                    if self.preferences.clock_24h {
                        "24-Hour Format"
                    } else {
                        "12-Hour Format"
                    },
                    true,
                    focus == 6,
                );
                c.fill_rect(Rect { x: 196, y: 322, w: 540, h: 1 }, 0xff2c313c);

                c.label(196, 330, 520, self.messages[kind].as_str(), theme::MUTED);
            }
            _ => self.draw_power(c, focus),
        }
    }
    fn draw_power(&self, c: &mut Canvas, focus: usize) {
        c.label_h(196, 12, 520, 18, "Power & Battery", theme::TEXT, theme::PANEL);
        c.label_h(196, 32, 520, 18, "Energy modes, battery charge status, and power efficiency", theme::SUBTEXT, theme::PANEL);

        // Power Status Hero Card (x: 196, y: 56, w: 540, h: 96)
        c.card(Rect { x: 196, y: 56, w: 540, h: 96 }, 0xff282828, theme::BORDER);

        if let Some(ref bat) = self.battery {
            c.battery_icon(212, 70, bat);

            let mut text = Text::<64>::default();
            let cap = bat.capacity_percent.min(100);
            let state_str = match bat.state {
                sys::BATTERY_STATE_CHARGING => "Charging",
                sys::BATTERY_STATE_DISCHARGING => "Discharging",
                sys::BATTERY_STATE_FULL => "Fully Charged",
                _ => "Unknown",
            };
            let _ = write!(text, "{}% - {}", cap, state_str);
            c.label_bold(244, 68, 480, 18, text.as_str(), 0xffffffff, 0xff282828);

            // Progress bar
            c.fill_rect(Rect { x: 212, y: 92, w: 508, h: 6 }, 0xff334155);
            let fill_w = ((cap as usize) * 508) / 100;
            let col = if cap < 10 { theme::RED } else if cap < 20 { theme::AMBER } else { theme::ACCENT_BLUE };
            if fill_w > 0 {
                c.fill_rect(Rect { x: 212, y: 92, w: fill_w, h: 6 }, col);
            }

            let mut sub = Text::<96>::default();
            let ac_str = if bat.ac_online == sys::AC_POWER_ONLINE { "AC Connected" } else { "On Battery" };
            if bat.time_remaining_mins > 0 {
                let h = bat.time_remaining_mins / 60;
                let m = bat.time_remaining_mins % 60;
                let _ = write!(sub, "{}h {:02}m remaining   {}", h, m, ac_str);
            } else {
                let _ = write!(sub, "{}", ac_str);
            }
            if bat.temperature_c > 0 {
                let _ = write!(sub, "   Thermal: {} C", bat.temperature_c);
            }
            c.label_h(212, 106, 508, 18, sub.as_str(), theme::MUTED, 0xff282828);
        } else {
            c.icon_badge(
                Rect { x: 204, y: 64, w: 32, h: 28 },
                0,
                theme::ACCENT,
                0xff1e2d40,
                theme::ACCENT,
                0xff282828,
            );
            c.label_bold(246, 64, 480, 18, "No Battery Detected", 0xffffffff, 0xff282828);
            c.label_h(246, 84, 480, 18, "Desktop Mode: No battery detected or ACPI probe pending.", theme::MUTED, 0xff282828);
            c.label_h(246, 106, 480, 18, "Hardware power states and thermal limits managed by ACPI.", theme::SUBTEXT, 0xff282828);
        }

        // Power Profile Section
        c.fill_rect(Rect { x: 196, y: 168, w: 540, h: 1 }, 0xff2c313c);
        c.label_bold(196, 178, 520, 18, "Power Profile", 0xffffffff, theme::PANEL);
        c.label_h(196, 196, 520, 18, "Balance system responsiveness and processor energy efficiency", theme::SUBTEXT, theme::PANEL);

        c.button(
            POWER[0].rect,
            "Balanced / Saver",
            self.power_profile == 0,
            focus == 0,
        );
        c.button(
            POWER[1].rect,
            "High Performance",
            self.power_profile == 1,
            focus == 1,
        );

        c.fill_rect(Rect { x: 196, y: 260, w: 540, h: 1 }, 0xff2c313c);
        c.label(196, 270, 520, self.messages[3].as_str(), theme::MUTED);
    }
    fn draw_network(&self, c: &mut Canvas, focus: usize) {
        let mut text = Text::<112>::default();

        c.label_h(196, 12, 520, 18, "Network & Internet", theme::TEXT, theme::PANEL);
        c.label_h(196, 32, 520, 18, "Wired Ethernet, wireless Wi-Fi networks, and IPv4 status", theme::SUBTEXT, theme::PANEL);

        let wifi_online = self.wireless.is_some_and(|w| w.online != 0);
        let net_is_wifi = match (self.network, self.wireless) {
            (Some(n), Some(w)) => wifi_online || (w.mac.iter().any(|&b| b != 0) && n.mac == w.mac),
            _ => false,
        };

        // Hero Ethernet Card container (x: 196, y: 52, w: 540, h: 76)
        c.card(Rect { x: 196, y: 52, w: 540, h: 76 }, 0xff282828, theme::BORDER);
        c.icon_badge(
            Rect { x: 204, y: 58, w: 32, h: 28 },
            0,
            theme::ACCENT,
            0xff1e2d40,
            theme::ACCENT,
            0xff282828,
        );

        if let Some(n) = self.network.filter(|_| !net_is_wifi) {
            let (status_text, status_color, pill_label, pill_fg, pill_bg, pill_border) = match n.link {
                2 => ("Connected  (Online)", 0xff4ade80, "Online", 0xff4ade80, 0xff133223, 0xff22c55e),
                1 => ("Waiting for cable...", 0xfffacc15, "Waiting", 0xfffacc15, 0xff332b14, 0xffeab308),
                3 => ("Link Down", 0xfff87171, "Offline", 0xfff87171, 0xff361818, 0xffef4444),
                4 => ("Link Failed", 0xfff87171, "Failed", 0xfff87171, 0xff361818, 0xffef4444),
                _ => ("Uninitialized", theme::SUBTEXT, "Idle", theme::SUBTEXT, 0xff1f2733, 0xff3a485c),
            };
            c.label_h(246, 58, 320, 18, "Ethernet (LAN)", theme::TEXT, 0xff282828);
            c.label_h(246, 76, 320, 18, status_text, status_color, 0xff282828);
            c.badge_on(
                Rect { x: 632, y: 58, w: 92, h: 22 },
                pill_label,
                pill_fg,
                pill_bg,
                pill_border,
                0xff282828,
            );
            let _ = write!(text, "IPv4: {}   Gateway: {}", ipv4(n.ip).as_str(), ipv4(n.gateway).as_str());
            c.label_h(206, 102, 516, 18, text.as_str(), theme::SUBTEXT, 0xff282828);
            text.clear();
        } else if net_is_wifi {
            c.label_h(246, 58, 320, 18, "Ethernet (LAN)", theme::TEXT, 0xff282828);
            c.label_h(246, 76, 320, 18, "Disconnected (Cable unplugged)", theme::SUBTEXT, 0xff282828);
            c.badge_on(
                Rect { x: 632, y: 58, w: 92, h: 22 },
                "Unplugged",
                0xff9fb0c7,
                0xff1f2733,
                0xff334155,
                0xff282828,
            );
            if let Some(n) = self.network.filter(|n| n.ip != 0) {
                let _ = write!(text, "Wi-Fi IPv4: {}   Gateway: {}", ipv4(n.ip).as_str(), ipv4(n.gateway).as_str());
                c.label_h(206, 102, 516, 18, text.as_str(), 0xff4ade80, 0xff282828);
                text.clear();
            } else {
                c.label_h(206, 102, 516, 18, "Active traffic routed over Wi-Fi interface (wlan0)", theme::SUBTEXT, 0xff282828);
            }
        } else {
            c.label_h(246, 58, 320, 18, "Ethernet (LAN)", theme::TEXT, 0xff282828);
            c.label_h(246, 76, 320, 18, "Hardware unavailable", theme::SUBTEXT, 0xff282828);
            c.badge_on(
                Rect { x: 632, y: 58, w: 92, h: 22 },
                "Offline",
                theme::SUBTEXT,
                0xff1f2733,
                0xff334155,
                0xff282828,
            );
        }

        // Wi-Fi Card container (x: 196, y: 136, w: 540, h: 44)
        c.card(Rect { x: 196, y: 136, w: 540, h: 44 }, 0xff282828, theme::BORDER);
        c.icon_badge(
            Rect { x: 204, y: 144, w: 32, h: 28 },
            1,
            theme::ACCENT,
            0xff1e2d40,
            theme::ACCENT,
            0xff282828,
        );

        text.clear();
        let mut wifi_sub = Text::<112>::default();
        if let Some(w) = self.wireless {
            let name = core::str::from_utf8(&w.ssid[..w.ssid.iter().position(|b| *b == 0).unwrap_or(33)]).unwrap_or("?");
            let _ = write!(
                text,
                "{}{}",
                match w.state {
                    1 => "Wi-Fi: Scanning...",
                    2..=4 => "Wi-Fi: Connecting...",
                    5 if w.online != 0 => "Connected: ",
                    5 => "Wi-Fi: Authenticating...",
                    6 => "Wi-Fi: Connection failed",
                    _ => "Wi-Fi: Disconnected",
                },
                if w.state == 5 && w.online != 0 { name } else { "" }
            );
            if let Some(n) = self.network.filter(|n| net_is_wifi && n.ip != 0) {
                if w.channel > 0 {
                    let _ = write!(wifi_sub, "IPv4: {} (Ch {})", ipv4(n.ip).as_str(), w.channel);
                } else {
                    let _ = write!(wifi_sub, "IPv4: {}", ipv4(n.ip).as_str());
                }
            } else {
                let _ = write!(wifi_sub, "802.11 wireless adapter");
            }
        } else {
            let _ = write!(text, "Wi-Fi: No adapter available");
            let _ = write!(wifi_sub, "Wireless hardware not detected");
        }
        c.label_bold(
            246,
            141,
            234,
            18,
            text.as_str(),
            if wifi_online { 0xff4ade80 } else { 0xffffffff },
            0xff282828,
        );
        c.label_h(246, 159, 234, 18, wifi_sub.as_str(), theme::SUBTEXT, 0xff282828);

        let enabled = self.wireless.is_some();
        let busy = self.connecting_until.is_some() || self.wifi_prepare_pid.is_some();
        c.button_enabled(
            NETWORK[0].rect,
            if self.wifi_prepare_pid.is_some() {
                "Preparing"
            } else if self.scan_after.is_some() {
                "Scanning"
            } else {
                "Refresh"
            },
            false,
            focus == 0,
            enabled && !busy && self.scan_after.is_none(),
        );
        c.button_enabled(
            NETWORK[1].rect,
            if busy { "Cancel" } else { "Disconnect" },
            false,
            focus == 1,
            enabled && self.wifi_prepare_pid.is_none()
                && (busy || self.wireless.is_some_and(|w| w.state >= 2 && w.state <= 5)),
        );

        // Available Networks Section
        c.label_bold(196, 189, 176, 18, "Available Networks", 0xffffffff, theme::PANEL);
        text.clear();
        let _ = write!(
            text,
            "{}-{} of {}",
            if self.points.is_empty() { 0 } else { self.page * 4 + 1 },
            ((self.page + 1) * 4).min(self.points.len()),
            self.points.len()
        );
        c.label_h(376, 188, 108, 22, text.as_str(), theme::SUBTEXT, theme::PANEL);
        c.button_enabled(NETWORK[2].rect, "< Back", false, focus == 2, self.page > 0);
        c.button_enabled(NETWORK[3].rect, "More >", false, focus == 3, (self.page + 1) * 4 < self.points.len());

        for row in 0..4 {
            let index = self.page * 4 + row;
            if let Some(point) = self.points.get(index) {
                let connected = self.wireless.is_some_and(|w| w.online != 0 && w.bssid == point.bssid);
                c.wifi_row(
                    NETWORK[8 + row].rect,
                    point.ssid.as_str(),
                    point.rssi,
                    point.security,
                    connected,
                    index == self.selected,
                    focus == 8 + row,
                    enabled && !busy,
                );
            } else {
                let empty_msg = if row == 0 {
                    if !enabled {
                        "No Wi-Fi adapter available"
                    } else if self.scan_after.is_some() {
                        "Looking for nearby networks..."
                    } else {
                        "No networks found. Click Refresh to scan."
                    }
                } else {
                    ""
                };
                c.button_enabled(
                    NETWORK[8 + row].rect,
                    empty_msg,
                    false,
                    focus == 8 + row,
                    false,
                );
            }
        }

        // Elevated Sheet Modal Card for Join Network / Password Prompt (x: 196, y: 326, w: 540, h: 110)
        let selected_open = self
            .points
            .get(self.selected)
            .filter(|p| p.ssid == self.ssid)
            .is_some_and(|p| p.security == 0);
        let sheet_active = self.selected != usize::MAX || (4..=7).contains(&focus);
        let modal_border = if sheet_active {
            theme::ACCENT
        } else {
            0xff323846
        };
        let header_bg = if sheet_active { 0xff1e293b } else { 0xff282828 };
        let header_fg = if sheet_active { 0xffffffff } else { theme::SUBTEXT };
        c.sheet_card(Rect { x: 196, y: 326, w: 540, h: 110 }, 0xff282828, modal_border, sheet_active);
        c.label_bold(
            208,
            330,
            320,
            18,
            if self.selected != usize::MAX {
                "Join Selected Network"
            } else {
                "Manual / Selected Network"
            },
            header_fg,
            header_bg,
        );
        c.entry(NETWORK[4].rect, self.ssid.as_str(), focus == 4 && enabled);
        c.badge_on(
            Rect { x: 458, y: 352, w: 266, h: 24 },
            if selected_open {
                "Open 802.11 (No Password)"
            } else {
                "WPA2 / WPA3 Personal"
            },
            if selected_open { 0xff38bdf8 } else { 0xff9fb0c7 },
            0xff1a2230,
            0xff334155,
            0xff282828,
        );

        c.label_bold(
            208,
            383,
            320,
            18,
            if selected_open {
                "Open Network (No Password)"
            } else {
                "Security Key (Password)"
            },
            0xffffffff,
            0xff282828,
        );
        let stars = [b'*'; 64];
        let masked = core::str::from_utf8(&stars[..self.password.len()]).unwrap_or("");
        c.entry(
            NETWORK[5].rect,
            if selected_open {
                "None (Open Hotspot)"
            } else if self.show_password {
                self.password.as_str()
            } else {
                masked
            },
            focus == 5 && enabled && !selected_open,
        );
        c.button_enabled(
            NETWORK[7].rect,
            if self.show_password { "Hide" } else { "Show" },
            self.show_password,
            focus == 7,
            enabled && !busy && !selected_open,
        );
        c.button_accent(
            NETWORK[6].rect,
            if busy { "Connecting..." } else { "Connect" },
            focus == 6,
        );

        // Status notification card
        let message = self.messages[1].as_str();
        c.card(Rect { x: 196, y: 444, w: 540, h: 24 }, 0xff22252c, 0xff323846);
        c.label_h(208, 447, 516, 18, message, if self.connecting_until.is_some() { theme::ACCENT } else { theme::SUBTEXT }, 0xff22252c);
    }
}
impl Drop for AppHost {
    fn drop(&mut self) {
        self.password.clear();
    }
}

pub fn ipv4(ip: u32) -> Text<32> {
    let b = ip.to_ne_bytes();
    let mut text = Text::default();
    let _ = write!(text, "{}.{}.{}.{}", b[0], b[1], b[2], b[3]);
    text
}
#[inline(never)]
fn load_preferences() -> (Preferences, bool) {
    let mut buffer = Vec::new();
    if buffer.try_reserve_exact(513).is_err() {
        return (Preferences::default(), false);
    }
    buffer.resize(513, 0);
    match sys::read_file(crate::preferences::PATH, &mut buffer) {
        Ok(n) => Preferences::parse(&buffer[..n])
            .map(|p| (p, true))
            .unwrap_or((Preferences::default(), false)),
        Err(-5) => (Preferences::default(), true),
        Err(_) => (Preferences::default(), false),
    }
}

#[allow(dead_code)]
pub fn signal_label(rssi: i8) -> &'static str {
    match rssi {
        -50..=127 => "Excellent",
        -65..=-51 => "Good",
        -75..=-66 => "Fair",
        _ => "Weak",
    }
}

#[cfg(test)]
mod wifi_ux_tests {
    use super::*;
    fn point(name: &str, rssi: i8, security: u8, id: u8) -> AccessPoint {
        AccessPoint {
            ssid: Text::from_str(name),
            rssi,
            security,
            bssid: [id; 6],
        }
    }
    #[test]
    fn nearby_networks_are_ranked_grouped_clickable_and_passwords_are_cleared() {
        let mut h = AppHost::new(Preferences::default());
        h.message_page = 1;
        let mut connected_ssid = [0u8; 33];
        connected_ssid[..4].copy_from_slice(b"Home");
        h.network = Some(sys::NetworkInfo {
            version: 1,
            mac: [1; 6],
            ip: u32::from_ne_bytes([192, 168, 0, 194]),
            gateway: u32::from_ne_bytes([192, 168, 0, 1]),
            link: 2,
            ..Default::default()
        });
        h.wireless = Some(sys::WirelessInfo {
            state: 5,
            online: 1,
            bssid: [3; 6],
            ssid: connected_ssid,
            mac: [1; 6],
            ..Default::default()
        });
        h.set_networks(alloc::vec![
            point("Home", -78, 1, 1),
            point("Cafe", -60, 0, 2),
            point("Home", -40, 1, 3),
            point("", -30, 1, 4),
            point("Guest", -65, 1, 5),
            point("Office", -68, 1, 6),
            point("Other", -80, 1, 7)
        ]);
        assert_eq!(h.points.len(), 5);
        assert_eq!(h.points[0].bssid, [3; 6]);
        let mut wm = WindowManager::desktop();
        let mut s = Surface::new(1280, 800).unwrap();
        h.open(8, &mut wm, &mut s);
        h.pointer(8, 250, 225, &mut wm, &mut s);
        assert_eq!(h.ssid.as_str(), "Home");
        assert_eq!(h.apps[1].focus, 5);
        for b in b"secret123" {
            h.key(8, *b, &mut wm, &mut s);
        }
        assert_eq!(h.password.as_str(), "secret123");
        h.pointer(8, 430, 415, &mut wm, &mut s);
        assert!(h.show_password);
        if let Ok(path) = std::env::var("COMPOSITOR_WIFI_PREVIEW") {
            h.apps[1].focus = 5;
            h.render(8, &mut wm, &mut s);
            s.fill(crate::window::DESKTOP);
            s.damage(Rect { x: 0, y: 0, w: s.width, h: s.height });
            wm.render(&mut s);
            let w = wm.windows.iter().find(|w| w.id == 8).unwrap();
            let rx = w.rect.x;
            let ry = w.rect.y;
            let rw = w.rect.w;
            let rh = w.rect.h;
            let mut image = alloc::format!("P6\n{} {}\n255\n", rw, rh).into_bytes();
            for y in ry..ry + rh {
                for x in rx..rx + rw {
                    let pixel = s.pixels[y * s.width + x];
                    image.extend_from_slice(&[(pixel >> 16) as u8, (pixel >> 8) as u8, pixel as u8]);
                }
            }
            std::fs::write(&path, image).unwrap();
            if let Some(stem) = path.strip_suffix(".ppm") {
                for (tab_cmd, suffix) in [(100u16, "_personalization.ppm"), (102u16, "_time.ppm")] {
                    h.command(1, tab_cmd, &mut wm, &mut s);
                    wm.render(&mut s);
                    let w = wm.windows.iter().find(|w| w.id == 8).unwrap();
                    let mut img = alloc::format!("P6\n{} {}\n255\n", w.rect.w, w.rect.h).into_bytes();
                    for y in w.rect.y..w.rect.y + w.rect.h {
                        for x in w.rect.x..w.rect.x + w.rect.w {
                            let pixel = s.pixels[y * s.width + x];
                            img.extend_from_slice(&[(pixel >> 16) as u8, (pixel >> 8) as u8, pixel as u8]);
                        }
                    }
                    std::fs::write(alloc::format!("{}{}", stem, suffix), img).unwrap();
                }
                h.command(1, 101, &mut wm, &mut s);
                h.apps[1].focus = 5;

                // Also export System Information (id 6) preview
                let mut sys_wm = WindowManager::desktop();
                let mut sys_s = Surface::new(1280, 800).unwrap();
                let mut sys_w = Window::new(6, "System Information", Rect { x: 80, y: 60, w: 520, h: 340 }).unwrap();
                sys_w.is_focused = true;
                sys_wm.windows.push(sys_w);
                sys_wm.system_panel(
                    &mut sys_s,
                    Some(sys::SystemInfo {
                        cpu_count: 8,
                        total_ram_bytes: 16384 * 1024 * 1024,
                        free_ram_bytes: 14210 * 1024 * 1024,
                        task_count: 14,
                        thread_count: 28,
                        uptime_ticks: 412800,
                        tick_hz: 100,
                        ..Default::default()
                    }),
                );
                sys_s.fill(crate::window::DESKTOP);
                sys_s.damage(Rect { x: 0, y: 0, w: 1280, h: 800 });
                sys_wm.render(&mut sys_s);
                let sw = &sys_wm.windows[0];
                let mut sys_img = alloc::format!("P6\n{} {}\n255\n", sw.rect.w, sw.rect.h).into_bytes();
                for y in sw.rect.y..sw.rect.y + sw.rect.h {
                    for x in sw.rect.x..sw.rect.x + sw.rect.w {
                        let pixel = sys_s.pixels[y * sys_s.width + x];
                        sys_img.extend_from_slice(&[(pixel >> 16) as u8, (pixel >> 8) as u8, pixel as u8]);
                    }
                }
                std::fs::write(alloc::format!("{}_sysinfo.ppm", stem), sys_img).unwrap();

                // Export Files (id 4) & Notes (id 5) previews
                let mut app_wm = WindowManager::desktop();
                let mut app_s = Surface::new(1280, 800).unwrap();
                let mut fw = Window::new(4, "Files", Rect { x: 40, y: 40, w: 520, h: 320 }).unwrap();
                {
                    let mut fm = crate::files::FileManager::new("/etc");
                    fm.entries.clear();
                    for (name, is_dir, is_exe, size) in [
                        ("network", true, false, 0),
                        ("ssl", true, false, 0),
                        ("watchtower", true, false, 0),
                        ("desktop-init", false, true, 1840),
                        ("desktop.conf", false, false, 128),
                        ("hostname", false, false, 11),
                        ("passwd", false, false, 412),
                        ("timezone", false, false, 16),
                    ] {
                        fm.entries.push(crate::files::FileEntry {
                            name: alloc::string::String::from(name),
                            is_dir,
                            is_exe,
                            size,
                        });
                    }
                    fm.selected = 4;
                    let cw = fw.rect.w - 2;
                    let ch = fw.rect.h - 26;
                    let mut c = crate::canvas::Canvas::new(&mut fw.content, cw, ch, cw).unwrap();
                    fm.render(&mut c);
                }
                let mut nw = Window::new(5, "Notes - /etc/desktop.conf", Rect { x: 600, y: 40, w: 520, h: 320 }).unwrap();
                {
                    let mut np = crate::notepad::Notepad::new("/etc/desktop.conf");
                    np.text.clear();
                    np.text.extend_from_slice(
                        b"# FortressOS Desktop Configuration\nversion=1\nscale=auto\nwallpaper=hello\nclock=24h\n",
                    );
                    np.cursor = 45;
                    np.modified = true;
                    let cw = nw.rect.w - 2;
                    let ch = nw.rect.h - 26;
                    let mut c = crate::canvas::Canvas::new(&mut nw.content, cw, ch, cw).unwrap();
                    np.render(&mut c);
                }
                nw.is_focused = true;
                fw.is_focused = true;
                app_wm.windows.push(fw);
                app_wm.windows.push(nw);
                app_s.fill(crate::window::DESKTOP);
                app_s.damage(Rect { x: 0, y: 0, w: 1280, h: 800 });
                app_wm.render(&mut app_s);
                for (wid, suffix) in [(4u32, "_files.ppm"), (5u32, "_notes.ppm")] {
                    let aw = app_wm.windows.iter().find(|w| w.id == wid).unwrap();
                    let mut aimg = alloc::format!("P6\n{} {}\n255\n", aw.rect.w, aw.rect.h).into_bytes();
                    for y in aw.rect.y..aw.rect.y + aw.rect.h {
                        for x in aw.rect.x..aw.rect.x + aw.rect.w {
                            let pixel = app_s.pixels[y * app_s.width + x];
                            aimg.extend_from_slice(&[(pixel >> 16) as u8, (pixel >> 8) as u8, pixel as u8]);
                        }
                    }
                    std::fs::write(alloc::format!("{}{}", stem, suffix), aimg).unwrap();
                }
            }
        }
        h.pointer(8, 250, 250, &mut wm, &mut s);
        assert_eq!(h.ssid.as_str(), "Cafe");
        assert_eq!(h.password.len(), 0);
        assert!(!h.show_password);
        assert_eq!(h.apps[1].focus, 6);
        assert!(h.status.as_str().contains("Open network selected"));
        h.pointer(8, 680, 195, &mut wm, &mut s);
        h.pointer(8, 250, 225, &mut wm, &mut s);
        assert_eq!(h.ssid.as_str(), "Other");
        h.key(8, b'\n', &mut wm, &mut s); // Empty password on secured network returns before syscall.
        assert!(h.status.as_str().contains("8 to 64"));
        assert!(
            wm.windows
                .iter_mut()
                .find(|w| w.id == 8)
                .unwrap()
                .resize(680, 360)
        );
        h.key(8, b'\t', &mut wm, &mut s);
        assert!(h.apps[1].scroll > 0, "keyboard focus must scroll into view");
        h.key(8, 27, &mut wm, &mut s);
        assert!(!h.show_password);
    }
    #[test]
    fn connection_progress_success_failure_and_timeout_are_distinct() {
        let mut h = AppHost::new(Preferences::default());
        h.wireless = Some(sys::WirelessInfo {
            state: 4,
            ..Default::default()
        });
        h.connecting_until = Some(20);
        h.connection_feedback(10);
        assert!(h.messages[1].as_str().contains("checking"));
        h.wireless.as_mut().unwrap().state = 5;
        h.wireless.as_mut().unwrap().online = 1;
        h.connection_feedback(11);
        assert!(h.messages[1].as_str().starts_with("Connected"));
        assert!(h.connecting_until.is_none());
        h.connecting_until = Some(30);
        h.wireless.as_mut().unwrap().state = 6;
        h.connection_feedback(12);
        assert!(h.messages[1].as_str().contains("Check the password"));
        assert!(h.connecting_until.is_none());
        h.connecting_until = Some(30);
        h.wireless = None;
        h.connection_feedback(30);
        assert!(h.messages[1].as_str().contains("timed out"));
        assert!(h.connecting_until.is_none());
    }
    #[test]
    fn preparation_is_reaped_before_join_and_failures_can_retry() {
        let mut h = AppHost::new(Preferences::default());
        h.wireless = Some(sys::WirelessInfo::default());
        h.ssid = Text::from_str("Fixture");
        h.password = Text::from_str("fixture-key");
        h.wifi_prepare_pid = Some(42);
        h.connect();
        assert_eq!(h.wifi_prepare_pid, Some(42));
        assert!(h.connecting_until.is_none());
        assert_eq!(h.password.as_str(), "fixture-key");
        assert!(!h.finish_wifi_preparation(Err(-5)));
        assert!(h.wifi_prepare_pid.is_none());
        assert!(h.messages[1].as_str().contains("preparation failed"));
        h.wifi_prepare_pid = Some(43);
        assert!(h.finish_wifi_preparation(Ok(())));
        assert!(h.wifi_prepared && h.wifi_prepare_pid.is_none());
    }
}
