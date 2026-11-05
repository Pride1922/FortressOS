//! Application manifest registry and dynamic discovery.
//!
//! Provides typed metadata for desktop applications (display name, binary paths,
//! icon, category, search keywords) and runtime probing via sys::stat_ext.
//! Associates windows reliably with applications regardless of dynamic window
//! IDs or title bar modifications.

#[derive(Copy, Clone, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum AppId {
    Files,
    Terminal,
    Notepad,
    Calc,
    Vault,
    Settings,
    SysInfo,
    Network,
    DateTime,
    Power,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[allow(dead_code)]
pub struct AppManifest {
    pub id: AppId,
    pub name: &'static str,
    pub exec_packaged: &'static core::ffi::CStr,
    pub exec_fallback: &'static core::ffi::CStr,
    pub icon_kind: usize,
    pub category: &'static str,
    pub keywords: &'static str,
    pub is_builtin: bool,
    pub builtin_id: Option<u32>,
}

#[allow(dead_code)]
impl AppManifest {
    /// Returns true if the application is runnable: built-ins are always runnable,
    /// and external binaries are verified via `sys::stat_ext`.
    pub fn is_available(&self) -> bool {
        if self.is_builtin {
            return true;
        }
        let mut st = crate::sys::StatExtV1::default();
        if let Ok(p) = self.exec_packaged.to_str() {
            if !p.is_empty() && crate::sys::stat_ext(p, &mut st) == 0 && st.file_type == 1 {
                return true;
            }
        }
        if let Ok(p) = self.exec_fallback.to_str() {
            if !p.is_empty() && crate::sys::stat_ext(p, &mut st) == 0 && st.file_type == 1 {
                return true;
            }
        }
        false
    }

    /// Case-insensitive query matching against name, category, and keywords.
    pub fn matches_query(&self, query: &str) -> bool {
        let q = query.trim();
        if q.is_empty() {
            return true;
        }
        contains_ignore_case(self.name, q)
            || contains_ignore_case(self.category, q)
            || contains_ignore_case(self.keywords, q)
    }
}

/// Case-insensitive substring match without dynamic allocation.
#[allow(dead_code)]
pub fn contains_ignore_case(haystack: &str, needle: &str) -> bool {
    let h = haystack.as_bytes();
    let n = needle.as_bytes();
    if n.len() > h.len() {
        return false;
    }
    h.windows(n.len()).any(|window| {
        window.iter().zip(n.iter()).all(|(&a, &b)| {
            a.to_ascii_lowercase() == b.to_ascii_lowercase()
        })
    })
}

/// Static catalog of known FortressOS desktop applications.
#[allow(dead_code)]
pub const APP_CATALOG: &[AppManifest] = &[
    AppManifest {
        id: AppId::Terminal,
        name: "Terminal",
        exec_packaged: c"/usr/bin/gterm",
        exec_fallback: c"/bin/gterm",
        icon_kind: 1,
        category: "Utilities",
        keywords: "console shell bash command prompt cli sh terminal gterm",
        is_builtin: false,
        builtin_id: None,
    },
    AppManifest {
        id: AppId::Files,
        name: "Files",
        exec_packaged: c"/usr/bin/files",
        exec_fallback: c"/bin/files",
        icon_kind: 0,
        category: "Productivity",
        keywords: "folder explorer browse directory disk storage files",
        is_builtin: false,
        builtin_id: None,
    },
    AppManifest {
        id: AppId::Notepad,
        name: "Notepad",
        exec_packaged: c"/usr/bin/notepad",
        exec_fallback: c"/bin/notepad",
        icon_kind: 2,
        category: "Productivity",
        keywords: "editor text notes document write txt notepad",
        is_builtin: false,
        builtin_id: None,
    },
    AppManifest {
        id: AppId::Calc,
        name: "Calculator",
        exec_packaged: c"/usr/bin/calc",
        exec_fallback: c"/bin/calc",
        icon_kind: 3,
        category: "Utilities",
        keywords: "math calculate numbers addition compute calculator",
        is_builtin: false,
        builtin_id: None,
    },
    AppManifest {
        id: AppId::Vault,
        name: "Vault",
        exec_packaged: c"/usr/bin/desktop-vault",
        exec_fallback: c"/bin/desktop-vault",
        icon_kind: 4,
        category: "Utilities",
        keywords: "security credentials passwords keys secrets lock vault",
        is_builtin: false,
        builtin_id: None,
    },
    AppManifest {
        id: AppId::Settings,
        name: "Settings",
        exec_packaged: c"",
        exec_fallback: c"",
        icon_kind: 5,
        category: "System",
        keywords: "configuration preferences personalization options display theme settings",
        is_builtin: true,
        builtin_id: Some(7),
    },
    AppManifest {
        id: AppId::SysInfo,
        name: "System Information",
        exec_packaged: c"",
        exec_fallback: c"",
        icon_kind: 6,
        category: "System",
        keywords: "monitor cpu memory tasks activity sysinfo hardware performance",
        is_builtin: true,
        builtin_id: Some(6),
    },
    AppManifest {
        id: AppId::Network,
        name: "Network & Wi-Fi",
        exec_packaged: c"",
        exec_fallback: c"",
        icon_kind: 7,
        category: "System",
        keywords: "wifi wireless ethernet internet ip address connection network",
        is_builtin: true,
        builtin_id: Some(8),
    },
    AppManifest {
        id: AppId::DateTime,
        name: "Date & Time",
        exec_packaged: c"",
        exec_fallback: c"",
        icon_kind: 8,
        category: "System",
        keywords: "clock timezone calendar utc format time date",
        is_builtin: true,
        builtin_id: Some(9),
    },
    AppManifest {
        id: AppId::Power,
        name: "Power & Battery",
        exec_packaged: c"",
        exec_fallback: c"",
        icon_kind: 9,
        category: "System",
        keywords: "energy battery profile performance charging acpi sleep power",
        is_builtin: true,
        builtin_id: Some(10),
    },
];

/// Returns the manifest for a given AppId.
#[allow(dead_code)]
pub fn find_manifest(id: AppId) -> Option<&'static AppManifest> {
    APP_CATALOG.iter().find(|m| m.id == id)
}

/// Resolves an AppId from an executable path.
#[allow(dead_code)]
pub fn resolve_from_path(path: &core::ffi::CStr) -> Option<AppId> {
    let bytes = path.to_bytes();
    if bytes.ends_with(b"/files") || bytes == b"files" {
        Some(AppId::Files)
    } else if bytes.ends_with(b"/gterm") || bytes.ends_with(b"/terminal") || bytes == b"gterm" || bytes == b"terminal" {
        Some(AppId::Terminal)
    } else if bytes.ends_with(b"/notepad") || bytes == b"notepad" {
        Some(AppId::Notepad)
    } else if bytes.ends_with(b"/calc") || bytes == b"calc" {
        Some(AppId::Calc)
    } else if bytes.ends_with(b"/desktop-vault") || bytes == b"desktop-vault" {
        Some(AppId::Vault)
    } else {
        None
    }
}
