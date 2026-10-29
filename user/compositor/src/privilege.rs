//! Privilege separation. The compositor needs root only to take the display
//! and open the mouse; everything after that (terminal shell, file manager,
//! notepad, desktop apps) runs as the user who invoked sudo. The two system
//! files the Settings app may change are written by a small root helper,
//! `compositor --settings-broker`, which accepts nothing else and validates
//! every request.
use crate::preferences::{self, Preferences, Text};
use crate::sys;
use core::fmt::Write;

/// Who the desktop should run as, from sudo's SUDO_UID/SUDO_GID/SUDO_GROUPS.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Identity {
    pub uid: u32,
    pub gid: u32,
    pub groups: [u32; 16],
    pub ngroups: usize,
}

#[derive(Debug, PartialEq, Eq)]
pub enum Launch {
    /// Drop to this identity once the display and mouse are open.
    Drop(Identity),
    /// Started by root itself (no SUDO_UID, or SUDO_UID=0): stay root.
    StayRoot,
}

fn parse_u32(text: &[u8]) -> Option<u32> {
    if text.is_empty() || text.len() > 10 || !text.iter().all(u8::is_ascii_digit) {
        return None;
    }
    let value: u64 = text.iter().fold(0, |v, &d| v * 10 + (d - b'0') as u64);
    u32::try_from(value).ok()
}

/// Reads the sudo variables from a NULL-terminated envp. A present but
/// malformed or incomplete identity is an error: never guess, never stay
/// root by accident.
pub fn launch_from_env(envp: *const *const u8) -> Result<Launch, &'static str> {
    let (mut uid, mut gid, mut groups) = (None, None, None);
    if !envp.is_null() {
        for i in 0..256 {
            let entry = unsafe { *envp.add(i) };
            if entry.is_null() {
                break;
            }
            let mut len = 0;
            while len < 4096 && unsafe { *entry.add(len) } != 0 {
                len += 1;
            }
            let var = unsafe { core::slice::from_raw_parts(entry, len) };
            if let Some(v) = var.strip_prefix(b"SUDO_UID=") {
                uid = Some(v);
            } else if let Some(v) = var.strip_prefix(b"SUDO_GID=") {
                gid = Some(v);
            } else if let Some(v) = var.strip_prefix(b"SUDO_GROUPS=") {
                groups = Some(v);
            }
        }
    }
    let Some(uid) = uid else {
        return Ok(Launch::StayRoot);
    };
    let uid = parse_u32(uid).ok_or("malformed SUDO_UID")?;
    if uid == 0 {
        return Ok(Launch::StayRoot);
    }
    let gid = parse_u32(gid.ok_or("SUDO_GID missing")?).ok_or("malformed SUDO_GID")?;
    let mut id = Identity {
        uid,
        gid,
        groups: [0; 16],
        ngroups: 0,
    };
    if let Some(list) = groups.filter(|l| !l.is_empty()) {
        for item in list.split(|&c| c == b',') {
            if id.ngroups == id.groups.len() {
                return Err("too many SUDO_GROUPS");
            }
            id.groups[id.ngroups] = parse_u32(item).ok_or("malformed SUDO_GROUPS")?;
            id.ngroups += 1;
        }
    }
    if id.groups[..id.ngroups].contains(&0) || gid == 0 {
        return Err("refusing root group for the desktop session");
    }
    Ok(Launch::Drop(id))
}

/// Starts the root settings helper on two close-on-exec pipes, so neither
/// the helper's ends nor ours leak into the terminal shell or other children.
/// Returns the helper's pid.
pub fn spawn_broker() -> Result<isize, isize> {
    let (mut request, mut reply) = ([-1i32; 2], [-1i32; 2]);
    let rc = sys::pipe(&mut request, sys::VFS_O_CLOEXEC);
    if rc < 0 {
        return Err(rc);
    }
    let rc = sys::pipe(&mut reply, sys::VFS_O_CLOEXEC);
    if rc < 0 {
        sys::close(request[0]);
        sys::close(request[1]);
        return Err(rc);
    }
    // DUP2 clears close-on-exec on the helper's stdin/stdout; every other
    // pipe end stays close-on-exec and is gone in the child.
    let actions = [
        sys::SpawnFdAction {
            action_type: 2,
            dst_fd: 0,
            src_fd: request[0],
            ..Default::default()
        },
        sys::SpawnFdAction {
            action_type: 2,
            dst_fd: 1,
            src_fd: reply[1],
            ..Default::default()
        },
    ];
    let argv = [
        c"/bin/compositor".as_ptr(),
        c"--settings-broker".as_ptr(),
        core::ptr::null(),
    ];
    let opts = sys::SpawnOpts {
        size: core::mem::size_of::<sys::SpawnOpts>() as u32,
        version: 2,
        argv: argv.as_ptr() as u64,
        fd_actions: actions.as_ptr() as u64,
        action_count: actions.len() as u32,
        ..Default::default()
    };
    let pid = sys::spawn_ext("/bin/compositor", &opts);
    sys::close(request[0]);
    sys::close(reply[1]);
    if pid <= 0 {
        sys::close(request[1]);
        sys::close(reply[0]);
        return Err(if pid < 0 { pid } else { sys::EINVAL });
    }
    sys::set_settings_broker(request[1], reply[0]);
    Ok(pid)
}

/// Irreversibly becomes `id`: supplementary groups, then gid, then uid (all
/// of real/effective/saved). Then proves it: ids as requested, no
/// capabilities left, and root can no longer be regained.
pub fn drop_to(id: &Identity) -> Result<(), &'static str> {
    if sys::setgroups(&id.groups[..id.ngroups]) < 0 {
        return Err("setgroups failed");
    }
    if sys::setres(true, id.gid) < 0 {
        return Err("setresgid failed");
    }
    if sys::setres(false, id.uid) < 0 {
        return Err("setresuid failed");
    }
    if sys::getres(false) != Some((id.uid, id.uid, id.uid))
        || sys::getres(true) != Some((id.gid, id.gid, id.gid))
    {
        return Err("identity did not change");
    }
    if sys::capget() != 0 {
        return Err("capabilities survived the drop");
    }
    if sys::setres(false, 0) >= 0 {
        return Err("root could be regained");
    }
    Ok(())
}

/// Timezone names as installed: Region/City, ASCII, no absolute or
/// relative path tricks ('.' is not allowed at all).
fn zone_name_ok(name: &[u8]) -> bool {
    !name.is_empty()
        && name.len() <= 95
        && name[0] != b'/'
        && name
            .iter()
            .all(|&c| c.is_ascii_alphanumeric() || b"/_+-".contains(&c))
}

fn handle(kind: u8, payload: &[u8]) -> isize {
    match kind {
        sys::SETTING_PREFERENCES => {
            // Re-encode the parsed value: only canonical bytes reach /etc.
            let Some(prefs) = Preferences::parse(payload) else {
                return sys::EINVAL;
            };
            let mut text = Text::<{ preferences::MAX_BYTES }>::default();
            if prefs.encode(&mut text).is_err() {
                return sys::EINVAL;
            }
            write(kind, text.as_bytes())
        }
        sys::SETTING_TIMEZONE => {
            let Some(name) = payload.strip_suffix(b"\n") else {
                return sys::EINVAL;
            };
            let mut now = sys::Timespec::default();
            let utc = if sys::clock_gettime(0, &mut now) == 0 {
                now.tv_sec
            } else {
                0
            };
            match core::str::from_utf8(name) {
                Ok(zone) if zone_name_ok(name) && sys::zone_valid(zone, utc) => {
                    write(kind, payload)
                }
                _ => sys::EINVAL,
            }
        }
        _ => sys::EINVAL,
    }
}

fn write(kind: u8, bytes: &[u8]) -> isize {
    match sys::setting_paths(kind) {
        Some((path, temporary)) => match sys::atomic_write(path, temporary, bytes) {
            Ok(()) => 0,
            Err(e) => e,
        },
        None => sys::EINVAL,
    }
}

/// `compositor --settings-broker`: serve requests on stdin, answer each
/// with an 8-byte status on stdout, exit when the compositor closes the
/// pipe. Any malformed frame ends the helper.
pub fn broker_main() -> i32 {
    loop {
        let mut header = [0u8; 3];
        if !sys::read_exact(0, &mut header) {
            return 0; // compositor exited
        }
        let len = u16::from_le_bytes([header[1], header[2]]) as usize;
        if len > sys::SETTING_MAX {
            return 1;
        }
        let mut payload = [0u8; sys::SETTING_MAX];
        if !sys::read_exact(0, &mut payload[..len]) {
            return 1;
        }
        let status = handle(header[0], &payload[..len]) as i64;
        if !sys::write_all(1, &status.to_le_bytes()) {
            return 1;
        }
    }
}

/// One line for the serial log / console (`fd` 1 for status, 2 for errors).
pub fn report(fd: i32, prefix: &str, what: &str, value: Option<isize>) {
    let mut line = Text::<160>::default();
    let _ = write!(line, "[COMPOSITOR] {prefix} {what}");
    if let Some(v) = value {
        let _ = write!(line, " ({v})");
    }
    let _ = line.write_str("\n");
    sys::write(fd, line.as_bytes());
}
