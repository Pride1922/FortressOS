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
const NAV: [Control; 3] = [
    control(100, 10, 56, 160, 34),
    control(101, 10, 94, 160, 34),
    control(102, 10, 132, 160, 34),
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
    control(1, 470, 146, 84, 28),
    control(2, 562, 146, 92, 28),
    control(3, 490, 186, 74, 26),
    control(4, 572, 186, 82, 26),
    control(5, 196, 350, 210, 28),
    control(6, 196, 402, 210, 28),
    control(7, 476, 402, 90, 28),
    control(8, 414, 402, 54, 28),
    control(20, 196, 216, 458, 24),
    control(21, 196, 242, 458, 24),
    control(22, 196, 268, 458, 24),
    control(23, 196, 294, 458, 24),
];
const CLOCK: [Control; 7] = [
    control(1, 196, 152, 270, 28),
    control(2, 474, 152, 140, 28),
    control(3, 196, 206, 70, 26),
    control(4, 274, 206, 96, 26),
    control(5, 378, 206, 96, 26),
    control(6, 482, 206, 70, 26),
    control(7, 196, 286, 180, 28),
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
            _ => &CLOCK,
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
        if len > 32 || e[9] > 1 || e[10] != 0 || e[11] != 0 {
            return None;
        }
        let text = core::str::from_utf8(&e[12..12 + len]).ok()?;
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
    apps: [Builtin; 3],
    pub status: Text<112>,
    messages: [Text<112>; 3],
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
            ],
            status: Text::from_str(
                "Preferences persist on installed roots; live media resets on reboot.",
            ),
            messages: [
                Text::from_str(
                    "Preferences persist on installed roots; live media resets on reboot.",
                ),
                Text::from_str("Wi-Fi credentials are not stored by the desktop."),
                Text::from_str("Choose an installed timezone and save to apply it."),
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
        if let Some(command) =
            self.apps[index].pointer(x / scale, y / scale + self.apps[index].scroll)
        {
            self.command(index, command, wm, s);
            if index == 1 && (20..=23).contains(&command) {
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
            if index == 1 {
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
        if (100..103).contains(&command) {
            self.open(ui::APPS[command as usize - 100].id, wm, s);
            return;
        }
        self.message_page = index;
        if index == 1 && self.wifi_prepare_pid.is_some() { return; }
        if index == 1 && self.connecting_until.is_some() && command != 2 {
            return;
        }
        if index == 1 && matches!(command, 1 | 2 | 7) && self.wireless.is_none() {
            self.password.clear();
            self.message("No Wi-Fi adapter is available.");
            return;
        }
        if command & 0x100 != 0 {
            let byte = command as u8;
            let focus = self.apps[index].controls()[self.apps[index].focus].id;
            if index == 1 && self.wireless.is_none() {
                return;
            }
            match (index, focus) {
                (1, 5) => {
                    if self.ssid.edit(byte) {
                        self.selected = usize::MAX;
                        self.password.clear();
                        self.show_password = false;
                    }
                }
                (1, 6) => {
                    self.password.edit(byte);
                }
                (2, 1) => {
                    self.zone.edit(byte);
                }
                _ => (),
            }
            return;
        }
        match index {
            0 => match command {
                1..=3 => self.draft.scale = (command - 1) as u8,
                4 => self.draft.wallpaper = true,
                5 => self.draft.wallpaper = false,
                6 => self.draft.clock_24h = true,
                7 => self.draft.clock_24h = false,
                8 => self.save_preferences(),
                9 => {
                    self.draft = Preferences::default();
                    self.message("Defaults selected. Apply to save.");
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
            _ => match command {
                2 => self.save_zone(),
                3 => self.zone = Text::from_str("UTC"),
                4 => self.zone = Text::from_str("Europe/Brussels"),
                5 => self.zone = Text::from_str("America/New_York"),
                6 => self.zone = Text::from_str("Asia/Tokyo"),
                7 => {
                    self.draft.clock_24h = !self.draft.clock_24h;
                    self.save_preferences();
                }
                _ => (),
            },
        }
    }
    #[inline(never)]
    fn save_preferences(&mut self) {
        let mut text = Text::<512>::default();
        if self.draft.encode(&mut text).is_err() {
            self.message("Invalid preferences.");
            return;
        }
        match sys::atomic_write(
            crate::preferences::PATH,
            "/etc/.desktop.conf.new",
            text.as_bytes(),
        ) {
            Ok(()) => {
                self.layout_changed |= self.preferences.scale != self.draft.scale;
                self.wallpaper_changed |= self.preferences.wallpaper != self.draft.wallpaper;
                self.changed |= self.preferences != self.draft;
                self.preferences = self.draft;
                theme::set_scale(self.preferences.scale);
                self.message("Preferences saved. Live-media changes last until reboot.");
            }
            Err(error) => self.error("Preferences were not saved", error),
        }
    }
    #[inline(never)]
    fn save_zone(&mut self) {
        if !sys::zone_valid(self.zone.as_str(), self.utc) {
            self.message("Unknown timezone; enter an installed Region/City name.");
            return;
        }
        let mut text = Text::<128>::default();
        let _ = writeln!(text, "{}", self.zone.as_str());
        match sys::atomic_write(
            "/etc/timezone",
            "/etc/.desktop-timezone.new",
            text.as_bytes(),
        ) {
            Ok(()) => {
                sys::invalidate_clock();
                self.last_tick = -1;
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
        bytes[4..8].copy_from_slice(&16u32.to_le_bytes());
        let result =
            unsafe { sys::syscall3(sys::SYS_WIFI, 1, bytes.as_mut_ptr() as usize, bytes.len()) };
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
        self.apps[1].focus = 5;
        self.message(if point.security == 0 {
            "Open networks are not supported by the current connection service."
        } else {
            "Enter the Wi-Fi password, then press Enter to join."
        });
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
        if self
            .points
            .get(self.selected)
            .is_some_and(|p| p.security == 0)
        {
            self.message("Open networks are not supported by the current connection service.");
            return;
        }
        if self.password.len() < 8 {
            self.message("Enter a Wi-Fi password of 8 to 64 characters.");
            self.apps[1].focus = 5;
            return;
        }
        if !self.wifi_prepared { self.scan(); return; }
        let bssid = self
            .points
            .get(self.selected)
            .filter(|p| p.ssid == self.ssid)
            .map(|p| p.bssid)
            .unwrap_or([0; 6]);
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
    pub fn refresh(&mut self, utc: i64, wm: &mut WindowManager, s: &mut Surface) {
        if !wm.windows.iter().any(|w| w.id == 8) {
            self.password.clear();
            self.show_password = false;
            self.scan_after = None;
        }
        if self.last_tick == utc {
            return;
        }
        self.last_tick = utc;
        if let Some(pid) = self.wifi_prepare_pid {
            if let Some(result) = sys::child_status(pid) {
                if self.finish_wifi_preparation(result) && wm.windows.iter().any(|w| w.id == 8) {
                    self.scan();
                }
            }
        }
        self.utc = utc;
        if let Some((local, calendar)) = sys::local_clock(utc) {
            self.local_seconds = Some(local);
            self.calendar = Some(calendar);
        } else {
            self.local_seconds = None;
            self.calendar = None;
        }
        self.network = sys::network_info();
        if wm.windows.iter().any(|w| w.id == 8 && !w.is_minimized) {
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
        wm.update_clock(s, self.local_seconds.unwrap_or(-1));
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
        let reset = self.apps[index].size != dimensions;
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
        if let Some(dirty) = canvas.finish() {
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
                w.id == id && !w.is_minimized && self.apps[index].size != (w.rect.w, w.rect.h)
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
        c.fill_rect(Rect { x: 180, y: 0, w: 1, h }, 0xff253040);

        c.badge(Rect { x: 16, y: 12, w: 28, h: 28 }, "O", 0xffffffff, 0xff1f3d5c, 0xff3b82f6);
        c.label_h(52, 10, 120, 18, "Operator", theme::TEXT, 0xff111620);
        c.label_h(52, 28, 120, 18, "Administrator", theme::MUTED, 0xff111620);

        c.fill_rect(Rect { x: 12, y: 48, w: 156, h: 1 }, 0xff1e2634);

        let nav_items = [
            ("P", "Personalization"),
            ("N", "Network & Wi-Fi"),
            ("T", "Date & Time"),
        ];
        for (i, control) in NAV.iter().enumerate() {
            c.sidebar_nav(
                control.rect,
                nav_items[i].0,
                nav_items[i].1,
                i == index,
                false,
            );
        }

        if h >= 400 {
            c.label_h(14, h.saturating_sub(26), 150, 18, "FortressOS v0.2.0", 0xff5a6b82, 0xff111620);
        }
    }
    fn draw(&self, index: usize, c: &mut Canvas) {
        self.draw_sidebar(index, c);
        let focus = self.apps[index].focus;
        let mut text = Text::<112>::default();
        match index {
            0 => {
                c.label_h(196, 12, 450, 20, "Personalization", theme::TEXT, theme::PANEL);
                c.label_h(196, 32, 450, 18, "Display scaling, wallpaper artwork, and clock format", theme::MUTED, theme::PANEL);

                c.label_h(196, 56, 450, 18, "Interface Scale", theme::TEXT, theme::PANEL);
                c.label_h(196, 74, 450, 18, "Scale windows and desktop elements for screen density", theme::MUTED, theme::PANEL);
                for i in 0..3 {
                    c.button(
                        APPEARANCE[i].rect,
                        ["Automatic", "Native (1x)", "Large (2x)"][i],
                        self.draft.scale as usize == i,
                        focus == i,
                    );
                }
                c.fill_rect(Rect { x: 196, y: 130, w: 468, h: 1 }, 0xff253040);

                c.label_h(196, 138, 450, 18, "Desktop Background", theme::TEXT, theme::PANEL);
                c.label_h(196, 156, 450, 18, "Choose wallpaper image or solid minimalist background", theme::MUTED, theme::PANEL);
                c.button(
                    APPEARANCE[3].rect,
                    "Hello World",
                    self.draft.wallpaper,
                    focus == 3,
                );
                c.button(
                    APPEARANCE[4].rect,
                    "Plain Black",
                    !self.draft.wallpaper,
                    focus == 4,
                );
                c.fill_rect(Rect { x: 196, y: 214, w: 468, h: 1 }, 0xff253040);

                c.label_h(196, 222, 450, 18, "Taskbar Clock", theme::TEXT, theme::PANEL);
                c.label_h(196, 240, 450, 18, "Choose between 24-hour and 12-hour clock format", theme::MUTED, theme::PANEL);
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
                c.fill_rect(Rect { x: 196, y: 298, w: 468, h: 1 }, 0xff253040);

                c.button_accent(APPEARANCE[7].rect, "Apply & Save", focus == 7);
                c.button(APPEARANCE[8].rect, "Use Defaults", false, focus == 8);

                c.label_h(196, 350, 450, 18, self.messages[index].as_str(), theme::MUTED, theme::PANEL);
                c.label_h(196, 370, 450, 18, "Preferences persist on installed roots across reboots.", theme::MUTED, theme::PANEL);
            }
            1 => self.draw_network(c, focus),
            _ => {
                c.label_h(196, 12, 450, 20, "Time & Language", theme::TEXT, theme::PANEL);
                c.label_h(196, 32, 450, 18, "System clock, time synchronization, and regional timezone", theme::MUTED, theme::PANEL);

                if let Some(t) = self.calendar {
                    let _ = write!(
                        text,
                        "{:04}-{:02}-{:02}    {:02}:{:02}:{:02}",
                        t.year, t.month, t.day, t.hour, t.minute, t.second
                    );
                } else {
                    let _ = write!(text, "Clock unavailable: check timezone configuration");
                }
                c.label_h(200, 56, 450, 22, text.as_str(), 0xffffffff, theme::PANEL);
                c.label_h(200, 80, 450, 18, "Hardware RTC clock synchronized", theme::MUTED, theme::PANEL);
                c.fill_rect(Rect { x: 196, y: 104, w: 468, h: 1 }, 0xff253040);

                c.label_h(196, 112, 450, 18, "System Timezone", theme::TEXT, theme::PANEL);
                c.label_h(196, 130, 450, 18, "Enter an installed IANA Region/City name", theme::MUTED, theme::PANEL);
                c.entry(CLOCK[0].rect, self.zone.as_str(), focus == 0);
                c.button_accent(CLOCK[1].rect, "Save Timezone", focus == 1);
                c.label_h(196, 186, 450, 18, "Quick Presets", theme::MUTED, theme::PANEL);
                for (i, label) in ["UTC", "Brussels", "New York", "Tokyo"].iter().enumerate() {
                    c.button(CLOCK[i + 2].rect, label, false, focus == i + 2);
                }
                c.fill_rect(Rect { x: 196, y: 238, w: 468, h: 1 }, 0xff253040);

                c.label_h(196, 246, 450, 18, "Taskbar Clock Format", theme::TEXT, theme::PANEL);
                c.label_h(196, 264, 450, 18, "Toggle between 12-hour and 24-hour display format", theme::MUTED, theme::PANEL);
                c.button(
                    CLOCK[6].rect,
                    if self.preferences.clock_24h {
                        "Switch to 12-Hour"
                    } else {
                        "Switch to 24-Hour"
                    },
                    false,
                    focus == 6,
                );
                c.fill_rect(Rect { x: 196, y: 322, w: 468, h: 1 }, 0xff253040);

                c.label_h(196, 330, 450, 22, self.messages[index].as_str(), theme::MUTED, theme::PANEL);
            }
        }
    }
    fn draw_network(&self, c: &mut Canvas, focus: usize) {
        let mut text = Text::<112>::default();

        c.label_h(196, 12, 450, 20, "Network & Internet", theme::TEXT, theme::PANEL);
        c.label_h(196, 32, 450, 18, "Ethernet, wireless Wi-Fi, and network status", theme::MUTED, theme::PANEL);

        // Ethernet badge
        c.badge(Rect { x: 200, y: 56, w: 34, h: 34 }, "ETH", 0xff69b7ff, 0xff1f3d5c, 0xff3b82f6);

        if let Some(n) = self.network {
            let (status_text, status_color) = match n.link {
                2 => ("Connected  (Online)", 0xff4ade80),
                1 => ("Waiting for cable...", 0xfffacc15),
                3 => ("Link Down", 0xfff87171),
                4 => ("Link Failed", 0xfff87171),
                _ => ("Uninitialized", theme::MUTED),
            };
            c.label_h(244, 56, 400, 18, "Ethernet", theme::TEXT, theme::PANEL);
            c.label_h(244, 74, 400, 18, status_text, status_color, theme::PANEL);
            let _ = write!(text, "IPv4 {}   Gateway {}", ipv4(n.ip).as_str(), ipv4(n.gateway).as_str());
            c.label_h(200, 96, 450, 18, text.as_str(), theme::MUTED, theme::PANEL);
            text.clear();
            let _ = write!(text, "RX {} pkts    TX {} pkts    MTU {}", n.rx, n.tx, n.mtu);
            c.label_h(200, 114, 450, 18, text.as_str(), theme::MUTED, theme::PANEL);
        } else {
            c.label_h(244, 56, 400, 18, "Ethernet", theme::TEXT, theme::PANEL);
            c.label_h(244, 74, 400, 18, "Hardware unavailable", theme::MUTED, theme::PANEL);
        }
        c.fill_rect(Rect { x: 196, y: 136, w: 468, h: 1 }, 0xff253040);

        text.clear();
        if let Some(w) = self.wireless {
            let name = core::str::from_utf8(&w.ssid[..w.ssid.iter().position(|b| *b == 0).unwrap_or(33)]).unwrap_or("?");
            let _ = write!(
                text,
                "Wi-Fi: {} {}",
                match w.state {
                    1 => "Scanning...",
                    2..=4 => "Connecting...",
                    5 if w.online != 0 => "Connected to",
                    5 => "Authenticating...",
                    6 => "Failed to connect",
                    _ => "Disconnected",
                },
                if w.state == 5 { name } else { "" }
            );
        } else {
            let _ = write!(text, "Wi-Fi: No adapter available");
        }
        c.label_h(196, 142, 250, 18, text.as_str(), theme::TEXT, theme::PANEL);
        c.label_h(196, 160, 250, 18, "WPA2-PSK 802.11 wireless network", theme::MUTED, theme::PANEL);

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
        c.fill_rect(Rect { x: 196, y: 180, w: 468, h: 1 }, 0xff253040);

        c.label_h(196, 188, 160, 22, "Available Networks", theme::TEXT, theme::PANEL);
        text.clear();
        let _ = write!(
            text,
            "{}-{} of {}",
            if self.points.is_empty() { 0 } else { self.page * 4 + 1 },
            ((self.page + 1) * 4).min(self.points.len()),
            self.points.len()
        );
        c.label_h(370, 188, 100, 22, text.as_str(), theme::MUTED, theme::PANEL);
        c.button_enabled(NETWORK[2].rect, "< Back", false, focus == 2, self.page > 0);
        c.button_enabled(NETWORK[3].rect, "More >", false, focus == 3, (self.page + 1) * 4 < self.points.len());

        for row in 0..4 {
            let index = self.page * 4 + row;
            text.clear();
            if let Some(point) = self.points.get(index) {
                let connected = self.wireless.is_some_and(|w| w.online != 0 && w.bssid == point.bssid);
                let _ = write!(
                    text,
                    "{}    {}    {}{}",
                    point.ssid.as_str(),
                    signal_label(point.rssi),
                    if point.security == 0 { "Open" } else { "Secured" },
                    if connected { "    Connected" } else { "" }
                );
            } else if row == 0 {
                let _ = write!(
                    text,
                    "{}",
                    if !enabled {
                        "No Wi-Fi adapter available"
                    } else if self.scan_after.is_some() {
                        "Looking for nearby networks..."
                    } else {
                        "No networks found. Click Refresh to scan."
                    }
                );
            }
            c.button_enabled(
                NETWORK[8 + row].rect,
                text.as_str(),
                index == self.selected,
                focus == 8 + row,
                enabled && !busy && index < self.points.len(),
            );
        }
        c.fill_rect(Rect { x: 196, y: 324, w: 468, h: 1 }, 0xff253040);

        c.label_h(196, 330, 200, 18, "Network SSID", theme::MUTED, theme::PANEL);
        c.label_h(196, 382, 200, 18, "Security Key", theme::MUTED, theme::PANEL);
        c.entry(NETWORK[4].rect, self.ssid.as_str(), focus == 4 && enabled);
        let stars = [b'*'; 64];
        let masked = core::str::from_utf8(&stars[..self.password.len()]).unwrap_or("");
        c.entry(
            NETWORK[5].rect,
            if self.show_password {
                self.password.as_str()
            } else {
                masked
            },
            focus == 5 && enabled,
        );
        c.button_enabled(
            NETWORK[7].rect,
            if self.show_password { "Hide" } else { "Show" },
            self.show_password,
            focus == 7,
            enabled && !busy,
        );
        c.button_accent(
            NETWORK[6].rect,
            if busy { "Connecting..." } else { "Connect" },
            focus == 6,
        );
        c.label_h(414, 382, 200, 18, "Held in memory only", theme::MUTED, theme::PANEL);
        c.fill_rect(Rect { x: 196, y: 436, w: 468, h: 1 }, 0xff253040);

        let message = self.messages[1].as_str();
        c.label_h(196, 442, 460, 22, message, theme::MUTED, theme::PANEL);
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

pub fn signal_label(rssi: i8) -> &'static str {
    match rssi {
        -55..=127 => "Excellent",
        -67..=-56 => "Good",
        -75..=-68 => "Fair",
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
        h.wireless = Some(sys::WirelessInfo {
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
            let w = wm.windows.iter().find(|w| w.id == 8).unwrap();
            let mut image =
                alloc::format!("P6\n{} {}\n255\n", w.rect.w - 2, w.rect.h - 26).into_bytes();
            for pixel in &w.content {
                image.extend_from_slice(&[(pixel >> 16) as u8, (pixel >> 8) as u8, *pixel as u8]);
            }
            std::fs::write(path, image).unwrap();
        }
        h.pointer(8, 250, 250, &mut wm, &mut s);
        assert_eq!(h.ssid.as_str(), "Cafe");
        assert_eq!(h.password.len(), 0);
        assert!(!h.show_password);
        h.connect(); // Open networks return before any system call.
        assert!(h.status.as_str().contains("not supported"));
        h.pointer(8, 600, 195, &mut wm, &mut s);
        h.pointer(8, 250, 225, &mut wm, &mut s);
        assert_eq!(h.ssid.as_str(), "Other");
        h.key(8, b'\n', &mut wm, &mut s); // Empty password returns before syscall.
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
