//! /bin/desktop-vault: FortressOS Credential Vault Desktop Application.
//! Administrator-only graphical interface for managing encrypted system secrets,
//! Wi-Fi credentials, SSH keys, and application tokens.
#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
use libui::client::{App, Args, Event};
use libui::gfx::rect;
use libui::state::{hit, TextField};
use libui::theme;

mod vault_backend;
use vault_backend::{delete_secret, get_secret, init_vault, list_records, rekey_vault, set_secret, VaultRecord};

libui::app_main!(main, heap = 4 << 20);

const APP_W: usize = 640;
const APP_H: usize = 420;

#[derive(Clone, Copy, PartialEq, Eq)]
enum CategoryFilter {
    All,
    WiFi,
    SSH,
    Tokens,
    Passwords,
}

#[derive(PartialEq, Eq)]
enum View {
    Locked,
    Manager,
    AddModal,
    RekeyModal,
}

struct VaultAppState {
    view: View,
    passphrase: TextField<64>,
    master_key: String, // Kept in memory while unlocked, wiped on lock
    error_msg: Option<&'static str>,
    info_msg: Option<&'static str>,

    // Manager View State
    records: Vec<VaultRecord>,
    filter: CategoryFilter,
    search: TextField<32>,
    selected_idx: Option<usize>,
    revealed_secret: Option<String>,
    is_revealed: bool,
    copied_timer: usize,

    // Add Modal State
    add_key: TextField<32>,
    add_type: usize, // 0 = password, 1 = ssh_key, 2 = token
    add_val: TextField<128>,
    add_meta: TextField<64>,
    add_focus: usize, // 0=key, 1=type, 2=val, 3=meta

    // Rekey Modal State
    rekey_old: TextField<64>,
    rekey_new: TextField<64>,
    rekey_focus: usize, // 0=old, 1=new
}

impl VaultAppState {
    fn new() -> Self {
        Self {
            view: View::Locked,
            passphrase: TextField::new(),
            master_key: String::new(),
            error_msg: None,
            info_msg: None,

            records: Vec::new(),
            filter: CategoryFilter::All,
            search: TextField::new(),
            selected_idx: None,
            revealed_secret: None,
            is_revealed: false,
            copied_timer: 0,

            add_key: TextField::new(),
            add_type: 0,
            add_val: TextField::new(),
            add_meta: TextField::new(),
            add_focus: 0,

            rekey_old: TextField::new(),
            rekey_new: TextField::new(),
            rekey_focus: 0,
        }
    }

    fn lock(&mut self) {
        self.master_key.clear();
        self.passphrase.clear();
        self.records.clear();
        self.revealed_secret = None;
        self.is_revealed = false;
        self.selected_idx = None;
        self.view = View::Locked;
        self.error_msg = None;
        self.info_msg = Some("Vault locked");
    }

    fn unlock(&mut self) {
        let pass = self.passphrase.as_str();
        if pass.is_empty() {
            self.error_msg = Some("Master passphrase cannot be empty");
            return;
        }

        match list_records(pass) {
            Ok(recs) => {
                self.master_key = String::from(pass);
                self.records = recs;
                self.passphrase.clear();
                self.view = View::Manager;
                self.error_msg = None;
                self.info_msg = None;
                if !self.records.is_empty() {
                    self.select_record(0);
                }
            }
            Err(e) => {
                self.error_msg = Some(e);
            }
        }
    }

    fn initialize_fresh(&mut self) {
        let pass = self.passphrase.as_str();
        if pass.is_empty() {
            self.error_msg = Some("Passphrase required to initialize vault");
            return;
        }

        match init_vault(pass) {
            Ok(_) => {
                self.master_key = String::from(pass);
                self.passphrase.clear();
                self.records.clear();
                self.view = View::Manager;
                self.error_msg = None;
                self.info_msg = Some("New vault initialized");
            }
            Err(e) => {
                self.error_msg = Some(e);
            }
        }
    }

    fn refresh_records(&mut self) {
        if self.master_key.is_empty() {
            return;
        }
        if let Ok(recs) = list_records(&self.master_key) {
            self.records = recs;
            if let Some(idx) = self.selected_idx {
                if idx >= self.records.len() {
                    self.selected_idx = self.records.len().checked_sub(1);
                    self.revealed_secret = None;
                    self.is_revealed = false;
                }
            }
        }
    }

    fn select_record(&mut self, idx: usize) {
        if idx < self.records.len() {
            self.selected_idx = Some(idx);
            self.revealed_secret = None;
            self.is_revealed = false;
        }
    }

    fn load_selected_secret(&mut self) {
        let Some(idx) = self.selected_idx else { return; };
        let Some(rec) = self.records.get(idx) else { return; };
        if let Ok(sec) = get_secret(&rec.key, &self.master_key) {
            self.revealed_secret = Some(sec);
        }
    }

    fn filtered_record_indices(&self) -> Vec<usize> {
        let q = self.search.as_str();
        let mut list = Vec::new();
        for (i, r) in self.records.iter().enumerate() {
            let match_cat = match self.filter {
                CategoryFilter::All => true,
                CategoryFilter::WiFi => r.key.starts_with("wifi.") || r.secret_type == "wifi",
                CategoryFilter::SSH => r.key.starts_with("ssh.") || r.secret_type == "ssh_key",
                CategoryFilter::Tokens => r.key.starts_with("token.") || r.secret_type == "token",
                CategoryFilter::Passwords => r.secret_type == "password" && !r.key.starts_with("wifi.") && !r.key.starts_with("ssh."),
            };
            if !match_cat {
                continue;
            }
            if !q.is_empty() && !r.key.contains(q) && !r.meta.contains(q) {
                continue;
            }
            list.push(i);
        }
        list
    }
}

fn copy_to_clipboard(text: &str) {
    let path = b"/run/clipboard/current\0";
    let fallback = b"/tmp/.clipboard\0";
    unsafe {
        let fd = libui::client::raw_sys::syscall3(
            libui::client::raw_sys::SYS_OPEN,
            path.as_ptr() as usize,
            0x0040 | 0x0001 | 0x0200, // VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC
            0o600,
        );
        let active_fd = if fd >= 0 { fd } else {
            libui::client::raw_sys::syscall3(
                libui::client::raw_sys::SYS_OPEN,
                fallback.as_ptr() as usize,
                0x0040 | 0x0001 | 0x0200,
                0o600,
            )
        };
        if active_fd >= 0 {
            let _ = libui::client::raw_sys::syscall3(
                libui::client::raw_sys::SYS_WRITE,
                active_fd as usize,
                text.as_bytes().as_ptr() as usize,
                text.len(),
            );
            let _ = libui::client::raw_sys::syscall2(libui::client::raw_sys::SYS_CLOSE, active_fd as usize, 0);
        }
    }
}

fn render(app: &mut App, state: &VaultAppState) {
    let damage = {
        let mut p = app.painter().with_damage();
        p.fill(theme::PANEL);

        match state.view {
            View::Locked => {
                // Centered Lock Card
                let card_w = 400;
                let card_h = 240;
                let cx = (APP_W.saturating_sub(card_w)) / 2;
                let cy = (APP_H.saturating_sub(card_h)) / 2;
                let card_rect = rect(cx, cy, card_w, card_h);
                p.card(card_rect, theme::SURFACE, theme::BORDER);

                // Header
                p.label_bold(cx + 20, cy + 24, card_w - 40, 24, "Fortress Credential Vault", theme::WHITE, theme::SURFACE);
                p.label(cx + 20, cy + 50, card_w - 40, "Administrator authentication required", theme::MUTED);

                // Passphrase Input
                let input_rect = rect(cx + 20, cy + 85, card_w - 40, 36);
                let masked = "•".repeat(state.passphrase.len());
                let display_text = if state.passphrase.is_empty() { "Enter Master Passphrase..." } else { &masked };
                p.text_input(input_rect, display_text, true);

                // Buttons
                let unlock_rect = rect(cx + 20, cy + 140, (card_w - 50) / 2, 34);
                let init_rect = rect(cx + 20 + (card_w - 50) / 2 + 10, cy + 140, (card_w - 50) / 2, 34);
                p.button_accent(unlock_rect, "Unlock Vault", false);
                p.button(init_rect, "Init New Vault", false, false);

                // Error / Info banners
                if let Some(err) = state.error_msg {
                    p.label(cx + 20, cy + 195, card_w - 40, err, theme::RED);
                } else if let Some(info) = state.info_msg {
                    p.label(cx + 20, cy + 195, card_w - 40, info, 0xff98c379);
                }
            }

            View::Manager => {
                // 1. Top Bar
                p.fill_rect(rect(0, 0, APP_W, 44), 0xff161b22);
                p.label_bold(14, 12, 180, 22, "Fortress Vault", theme::WHITE, 0xff161b22);
                p.badge(rect(140, 12, 60, 20), "Admin", 0xff98c379, 0xff1e2d24, 0xff2e4f3a);

                // Search box
                let search_rect = rect(210, 8, 160, 28);
                let q_disp = if state.search.is_empty() { "Search..." } else { state.search.as_str() };
                p.text_input(search_rect, q_disp, false);

                // Top Actions
                p.button_accent(rect(380, 8, 70, 28), "+ New", false);
                p.button(rect(460, 8, 80, 28), "Rekey", false, false);
                p.button(rect(550, 8, 76, 28), "Lock", false, false);

                // 2. Category Sidebar
                let sb_w = 140;
                let sb_rect = rect(0, 44, sb_w, APP_H - 44);
                p.fill_rect(sb_rect, 0xff161b22);

                let cats = [
                    (CategoryFilter::All, "All Items"),
                    (CategoryFilter::WiFi, "Wi-Fi"),
                    (CategoryFilter::SSH, "SSH Keys"),
                    (CategoryFilter::Tokens, "Tokens"),
                    (CategoryFilter::Passwords, "Passwords"),
                ];

                for (idx, (cat, name)) in cats.iter().enumerate() {
                    let r = rect(8, 54 + idx * 34, sb_w - 16, 28);
                    let sel = state.filter == *cat;
                    if sel {
                        p.button_accent(r, name, false);
                    } else {
                        p.button(r, name, false, false);
                    }
                }

                // 3. Records List (Middle Pane)
                let list_x = sb_w;
                let list_w = 210;
                let list_rect = rect(list_x, 44, list_w, APP_H - 44);
                p.fill_rect(list_rect, 0xff0d1117);

                let matching = state.filtered_record_indices();
                if matching.is_empty() {
                    p.label(list_x + 16, 70, list_w - 32, "No matching keys", theme::MUTED);
                } else {
                    for (row, &orig_idx) in matching.iter().take(10).enumerate() {
                        let rec = &state.records[orig_idx];
                        let r = rect(list_x + 6, 50 + row * 34, list_w - 12, 30);
                        let is_sel = state.selected_idx == Some(orig_idx);
                        if is_sel {
                            p.fill_rect(r, 0xff1f6feb);
                        } else {
                            p.fill_rect(r, 0xff161b22);
                        }
                        p.label_bold(r.x + 8, r.y + 4, r.w - 16, 16, &rec.key, theme::WHITE, if is_sel { 0xff1f6feb } else { 0xff161b22 });
                        p.label(r.x + 8, r.y + 18, r.w - 16, &rec.secret_type, theme::MUTED);
                    }
                }

                // 4. Detail Panel (Right Pane)
                let det_x = list_x + list_w + 10;
                let det_w = APP_W - det_x - 10;
                let det_rect = rect(det_x, 50, det_w, APP_H - 60);
                p.card(det_rect, theme::SURFACE, theme::BORDER);

                if let Some(idx) = state.selected_idx {
                    if let Some(rec) = state.records.get(idx) {
                        p.label_bold(det_x + 16, 66, det_w - 32, 22, &rec.key, theme::WHITE, theme::SURFACE);
                        p.badge(rect(det_x + 16, 92, 70, 20), &rec.secret_type, 0xff58a6ff, 0xff162030, 0xff28456c);

                        if !rec.meta.is_empty() {
                            p.label(det_x + 16, 120, det_w - 32, &rec.meta, theme::MUTED);
                        }

                        // Secret field box
                        let val_rect = rect(det_x + 16, 150, det_w - 32, 36);
                        p.fill_rect(val_rect, 0xff0d1117);

                        let val_str = if state.is_revealed {
                            state.revealed_secret.as_deref().unwrap_or("••••••••••••••••")
                        } else {
                            "••••••••••••••••"
                        };
                        p.label_bold(val_rect.x + 10, val_rect.y + 8, val_rect.w - 20, 20, val_str, theme::WHITE, 0xff0d1117);

                        // Reveal & Copy Buttons
                        let rev_rect = rect(det_x + 16, 200, 84, 30);
                        let copy_rect = rect(det_x + 108, 200, 84, 30);
                        p.button(rev_rect, if state.is_revealed { "Hide" } else { "Reveal" }, false, false);
                        let copy_lbl = if state.copied_timer > 0 { "Copied!" } else { "Copy" };
                        p.button_accent(copy_rect, copy_lbl, false);

                        // Delete Button
                        let del_rect = rect(det_x + 16, 300, 96, 30);
                        p.button(del_rect, "Delete Key", false, false);
                    }
                } else {
                    p.label(det_x + 16, 70, det_w - 32, "Select a credential to view", theme::MUTED);
                }
            }

            View::AddModal => {
                // Dim background
                p.fill(0xcc000000);

                let mw = 440;
                let mh = 320;
                let mx = (APP_W - mw) / 2;
                let my = (APP_H - mh) / 2;
                let mrect = rect(mx, my, mw, mh);
                p.card(mrect, theme::SURFACE, theme::BORDER);

                p.label_bold(mx + 20, my + 16, mw - 40, 22, "Add New Secret", theme::WHITE, theme::SURFACE);

                // Key Name
                p.label(mx + 20, my + 48, 100, "Key Identifier:", theme::MUTED);
                let k_disp = if state.add_key.is_empty() { "e.g. wifi.office or ssh.server" } else { state.add_key.as_str() };
                p.text_input(rect(mx + 20, my + 68, mw - 40, 30), k_disp, state.add_focus == 0);

                // Type Select
                p.label(mx + 20, my + 106, 100, "Secret Type:", theme::MUTED);
                let type_labels = ["password", "ssh_key", "token"];
                for (ti, tl) in type_labels.iter().enumerate() {
                    let tr = rect(mx + 20 + ti * 100, my + 126, 90, 26);
                    let sel = state.add_type == ti;
                    if sel {
                        p.button_accent(tr, tl, false);
                    } else {
                        p.button(tr, tl, false, false);
                    }
                }

                // Secret Value
                p.label(mx + 20, my + 160, 100, "Secret Value:", theme::MUTED);
                let v_disp = if state.add_val.is_empty() { "Enter password or private key..." } else { state.add_val.as_str() };
                p.text_input(rect(mx + 20, my + 180, mw - 40, 30), v_disp, state.add_focus == 2);

                // Metadata
                p.label(mx + 20, my + 218, 100, "Description / Notes:", theme::MUTED);
                let m_disp = if state.add_meta.is_empty() { "Optional notes..." } else { state.add_meta.as_str() };
                p.text_input(rect(mx + 20, my + 238, mw - 40, 30), m_disp, state.add_focus == 3);

                // Modal Buttons
                let save_rect = rect(mx + 20, my + 276, 100, 30);
                let cancel_rect = rect(mx + 130, my + 276, 100, 30);
                p.button_accent(save_rect, "Save", false);
                p.button(cancel_rect, "Cancel", false, false);
            }

            View::RekeyModal => {
                p.fill(0xcc000000);

                let mw = 400;
                let mh = 240;
                let mx = (APP_W - mw) / 2;
                let my = (APP_H - mh) / 2;
                let mrect = rect(mx, my, mw, mh);
                p.card(mrect, theme::SURFACE, theme::BORDER);

                p.label_bold(mx + 20, my + 16, mw - 40, 22, "Rotate Master Passphrase", theme::WHITE, theme::SURFACE);

                p.label(mx + 20, my + 48, mw - 40, "Current Master Passphrase:", theme::MUTED);
                let o_disp = if state.rekey_old.is_empty() { "Current passphrase..." } else { "••••••••" };
                p.text_input(rect(mx + 20, my + 68, mw - 40, 30), o_disp, state.rekey_focus == 0);

                p.label(mx + 20, my + 108, mw - 40, "New Master Passphrase:", theme::MUTED);
                let n_disp = if state.rekey_new.is_empty() { "New passphrase..." } else { "••••••••" };
                p.text_input(rect(mx + 20, my + 128, mw - 40, 30), n_disp, state.rekey_focus == 1);

                let rekey_btn = rect(mx + 20, my + 180, 110, 32);
                let cancel_btn = rect(mx + 140, my + 180, 100, 32);
                p.button_accent(rekey_btn, "Rotate Key", false);
                p.button(cancel_btn, "Cancel", false, false);
            }
        }

        p.finish()
    };

    app.present(damage);
}

fn main(_args: Args) -> i32 {
    let Some(mut app) = App::connect(APP_W as u32, APP_H as u32, "Fortress Vault") else {
        return 1;
    };

    let mut state = VaultAppState::new();
    render(&mut app, &state);

    loop {
        match app.next_event() {
            Event::Configure => {
                render(&mut app, &state);
            }
            Event::Close => return 0,
            Event::Key(b) => {
                match state.view {
                    View::Locked => {
                        if b == b'\r' || b == b'\n' {
                            state.unlock();
                            render(&mut app, &state);
                        } else if b == 8 || b == 127 {
                            state.passphrase.backspace();
                            render(&mut app, &state);
                        } else if b >= 32 && b <= 126 {
                            state.passphrase.push(b as char);
                            render(&mut app, &state);
                        }
                    }
                    View::Manager => {
                        if b == 27 { // Esc to lock
                            state.lock();
                            render(&mut app, &state);
                        } else if b == 8 || b == 127 {
                            state.search.backspace();
                            render(&mut app, &state);
                        } else if b >= 32 && b <= 126 {
                            state.search.push(b as char);
                            render(&mut app, &state);
                        }
                    }
                    View::AddModal => {
                        if b == b'\t' {
                            state.add_focus = (state.add_focus + 1) % 4;
                            render(&mut app, &state);
                        } else if b == 8 || b == 127 {
                            match state.add_focus {
                                0 => { state.add_key.backspace(); }
                                2 => { state.add_val.backspace(); }
                                3 => { state.add_meta.backspace(); }
                                _ => {}
                            }
                            render(&mut app, &state);
                        } else if b >= 32 && b <= 126 {
                            let ch = b as char;
                            match state.add_focus {
                                0 => { state.add_key.push(ch); }
                                2 => { state.add_val.push(ch); }
                                3 => { state.add_meta.push(ch); }
                                _ => {}
                            }
                            render(&mut app, &state);
                        }
                    }
                    View::RekeyModal => {
                        if b == b'\t' {
                            state.rekey_focus = (state.rekey_focus + 1) % 2;
                            render(&mut app, &state);
                        } else if b == 8 || b == 127 {
                            if state.rekey_focus == 0 { state.rekey_old.backspace(); }
                            else { state.rekey_new.backspace(); }
                            render(&mut app, &state);
                        } else if b >= 32 && b <= 126 {
                            let ch = b as char;
                            if state.rekey_focus == 0 { state.rekey_old.push(ch); }
                            else { state.rekey_new.push(ch); }
                            render(&mut app, &state);
                        }
                    }
                }
            }
            Event::Click(x, y) => {
                match state.view {
                    View::Locked => {
                        let card_w = 400;
                        let card_h = 240;
                        let cx = (APP_W.saturating_sub(card_w)) / 2;
                        let cy = (APP_H.saturating_sub(card_h)) / 2;
                        let unlock_rect = rect(cx + 20, cy + 140, (card_w - 50) / 2, 34);
                        let init_rect = rect(cx + 20 + (card_w - 50) / 2 + 10, cy + 140, (card_w - 50) / 2, 34);

                        if hit(unlock_rect, x, y) {
                            state.unlock();
                            render(&mut app, &state);
                        } else if hit(init_rect, x, y) {
                            state.initialize_fresh();
                            render(&mut app, &state);
                        }
                    }
                    View::Manager => {
                        // Top bar buttons
                        if hit(rect(380, 8, 70, 28), x, y) { // + New
                            state.add_key.clear();
                            state.add_val.clear();
                            state.add_meta.clear();
                            state.add_focus = 0;
                            state.view = View::AddModal;
                            render(&mut app, &state);
                            continue;
                        }
                        if hit(rect(460, 8, 80, 28), x, y) { // Rekey
                            state.rekey_old.clear();
                            state.rekey_new.clear();
                            state.rekey_focus = 0;
                            state.view = View::RekeyModal;
                            render(&mut app, &state);
                            continue;
                        }
                        if hit(rect(550, 8, 76, 28), x, y) { // Lock
                            state.lock();
                            render(&mut app, &state);
                            continue;
                        }

                        // Category sidebar
                        let cats = [
                            CategoryFilter::All,
                            CategoryFilter::WiFi,
                            CategoryFilter::SSH,
                            CategoryFilter::Tokens,
                            CategoryFilter::Passwords,
                        ];
                        for (idx, cat) in cats.iter().enumerate() {
                            let r = rect(8, 54 + idx * 34, 140 - 16, 28);
                            if hit(r, x, y) {
                                state.filter = *cat;
                                state.selected_idx = None;
                                state.revealed_secret = None;
                                state.is_revealed = false;
                                render(&mut app, &state);
                                break;
                            }
                        }

                        // Middle records list
                        let matching = state.filtered_record_indices();
                        for (row, &orig_idx) in matching.iter().take(10).enumerate() {
                            let r = rect(140 + 6, 50 + row * 34, 210 - 12, 30);
                            if hit(r, x, y) {
                                state.select_record(orig_idx);
                                render(&mut app, &state);
                                break;
                            }
                        }

                        // Detail card actions
                        let det_x = 140 + 210 + 10;
                        let _det_w = APP_W - det_x - 10;

                        // Reveal / Hide
                        let rev_rect = rect(det_x + 16, 200, 84, 30);
                        if hit(rev_rect, x, y) {
                            if !state.is_revealed {
                                state.load_selected_secret();
                                state.is_revealed = true;
                            } else {
                                state.is_revealed = false;
                            }
                            render(&mut app, &state);
                        }

                        // Copy
                        let copy_rect = rect(det_x + 108, 200, 84, 30);
                        if hit(copy_rect, x, y) {
                            state.load_selected_secret();
                            if let Some(ref sec) = state.revealed_secret {
                                copy_to_clipboard(sec);
                                state.copied_timer = 5;
                                render(&mut app, &state);
                            }
                        }

                        // Delete
                        let del_rect = rect(det_x + 16, 300, 96, 30);
                        if hit(del_rect, x, y) {
                            if let Some(idx) = state.selected_idx {
                                if let Some(rec) = state.records.get(idx) {
                                    let key_to_del = rec.key.clone();
                                    let _ = delete_secret(&key_to_del, &state.master_key);
                                    state.refresh_records();
                                    render(&mut app, &state);
                                }
                            }
                        }
                    }
                    View::AddModal => {
                        let mw = 440;
                        let mh = 320;
                        let mx = (APP_W - mw) / 2;
                        let my = (APP_H - mh) / 2;

                        // Save Button
                        let save_rect = rect(mx + 20, my + 276, 100, 30);
                        let cancel_rect = rect(mx + 130, my + 276, 100, 30);

                        if hit(save_rect, x, y) {
                            let k = state.add_key.as_str();
                            let v = state.add_val.as_str();
                            let m = state.add_meta.as_str();
                            let t = match state.add_type {
                                1 => "ssh_key",
                                2 => "token",
                                _ => "password",
                            };
                            if !k.is_empty() && !v.is_empty() {
                                let _ = set_secret(k, t, v, m, &state.master_key);
                                state.refresh_records();
                                state.view = View::Manager;
                                render(&mut app, &state);
                            }
                        } else if hit(cancel_rect, x, y) {
                            state.view = View::Manager;
                            render(&mut app, &state);
                        }

                        // Type buttons
                        for ti in 0..3 {
                            let tr = rect(mx + 20 + ti * 100, my + 126, 90, 26);
                            if hit(tr, x, y) {
                                state.add_type = ti;
                                render(&mut app, &state);
                                break;
                            }
                        }
                    }
                    View::RekeyModal => {
                        let mw = 400;
                        let mh = 240;
                        let mx = (APP_W - mw) / 2;
                        let my = (APP_H - mh) / 2;

                        let rekey_btn = rect(mx + 20, my + 180, 110, 32);
                        let cancel_btn = rect(mx + 140, my + 180, 100, 32);

                        if hit(rekey_btn, x, y) {
                            let old = state.rekey_old.as_str();
                            let new = state.rekey_new.as_str();
                            if !old.is_empty() && !new.is_empty() {
                                if let Ok(_) = rekey_vault(old, new) {
                                    state.master_key = String::from(new);
                                    state.view = View::Manager;
                                    render(&mut app, &state);
                                }
                            }
                        } else if hit(cancel_btn, x, y) {
                            state.view = View::Manager;
                            render(&mut app, &state);
                        }
                    }
                }
            }
            _ => {}
        }
    }
}
