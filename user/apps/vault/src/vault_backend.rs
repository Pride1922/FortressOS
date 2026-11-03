//! vault_backend.rs - Client interface executing `/sbin/vault` with secure pipes.
extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
use libui::client::raw_sys::{syscall2, syscall3, SYS_CLOSE, SYS_PIPE, SYS_READ, SYS_SPAWN_EXT, SYS_WAITPID};

#[repr(C)]
#[derive(Copy, Clone, Debug, Default)]
pub struct SpawnFdAction {
    pub action_type: u32,
    pub dst_fd: i32,
    pub src_fd: i32,
    pub flags: u32,
    pub mode: u32,
    pub reserved: u32,
    pub path: u64,
}

#[repr(C)]
#[derive(Copy, Clone, Debug, Default)]
pub struct SpawnOpts {
    pub size: u32,
    pub version: u32,
    pub flags: u32,
    pub reserved0: u32,
    pub argv: u64,
    pub envp: u64,
    pub cwd: u64,
    pub fd_actions: u64,
    pub action_count: u32,
    pub reserved1: u32,
    pub reserved2: u64,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct VaultRecord {
    pub key: String,
    pub secret_type: String,
    pub meta: String,
}

pub fn run_vault_cmd(args: &[&str]) -> Result<(i32, Vec<u8>), &'static str> {
    let mut fds = [-1i32; 2];
    let rc = unsafe { syscall2(SYS_PIPE, fds.as_mut_ptr() as usize, 0x80000) }; // VFS_O_CLOEXEC
    if rc < 0 {
        return Err("failed to create pipe");
    }

    let actions = [
        SpawnFdAction {
            action_type: 2, // dup
            dst_fd: 1,
            src_fd: fds[1],
            ..Default::default()
        },
        SpawnFdAction {
            action_type: 2, // dup
            dst_fd: 2,
            src_fd: fds[1],
            ..Default::default()
        },
        SpawnFdAction {
            action_type: 3, // close
            dst_fd: fds[0],
            ..Default::default()
        },
        SpawnFdAction {
            action_type: 3, // close
            dst_fd: fds[1],
            ..Default::default()
        },
    ];

    // Build null-terminated argv strings
    let mut arg_storage: Vec<Vec<u8>> = Vec::new();
    let prog_name = Vec::from(b"/sbin/vault\0".as_slice());
    arg_storage.push(prog_name);

    for &a in args {
        let mut s = Vec::from(a.as_bytes());
        s.push(0);
        arg_storage.push(s);
    }

    let mut argv_ptrs: Vec<*const u8> = Vec::new();
    for s in &arg_storage {
        argv_ptrs.push(s.as_ptr());
    }
    argv_ptrs.push(core::ptr::null());

    let opts = SpawnOpts {
        size: core::mem::size_of::<SpawnOpts>() as u32,
        version: 2,
        argv: argv_ptrs.as_ptr() as u64,
        fd_actions: actions.as_ptr() as u64,
        action_count: actions.len() as u32,
        ..Default::default()
    };

    let bin_path = b"/sbin/vault\0";
    let pid = unsafe {
        syscall3(
            SYS_SPAWN_EXT,
            bin_path.as_ptr() as usize,
            &opts as *const _ as usize,
            opts.size as usize,
        )
    };

    // Close write end in parent
    unsafe { syscall2(SYS_CLOSE, fds[1] as usize, 0); }

    if pid <= 0 {
        unsafe { syscall2(SYS_CLOSE, fds[0] as usize, 0); }
        return Err("failed to spawn /sbin/vault");
    }

    // Read all output
    let mut output = Vec::new();
    let mut buf = [0u8; 1024];
    loop {
        let n = unsafe { syscall3(SYS_READ, fds[0] as usize, buf.as_mut_ptr() as usize, buf.len()) };
        if n <= 0 {
            break;
        }
        output.extend_from_slice(&buf[..n as usize]);
        if output.len() > 65536 {
            break;
        }
    }
    unsafe { syscall2(SYS_CLOSE, fds[0] as usize, 0); }

    let mut status: i32 = 0;
    unsafe { syscall3(SYS_WAITPID, pid as usize, &mut status as *mut _ as usize, 0); }

    Ok((status, output))
}

pub fn list_records(pass: &str) -> Result<Vec<VaultRecord>, &'static str> {
    let (status, out) = run_vault_cmd(&["list", "-f", "/etc/vault.db", "-p", pass])?;
    if status != 0 {
        let s = core::str::from_utf8(&out).unwrap_or("");
        if s.contains("permission denied") {
            return Err("Permission denied: administrator access required");
        }
        if s.contains("authentication failed") {
            return Err("Incorrect master passphrase");
        }
        return Err("Failed to unlock vault database");
    }

    let text = core::str::from_utf8(&out).map_err(|_| "invalid utf8 output")?;
    let mut records = Vec::new();

    for line in text.lines() {
        let trimmed = line.trim();
        if !trimmed.starts_with("- ") {
            continue;
        }
        // Format: "- <key> [<type>] (<meta>)"
        let rest = &trimmed[2..];
        let mut parts = rest.splitn(2, ' ');
        let key = parts.next().unwrap_or("").trim();
        let remain = parts.next().unwrap_or("").trim();

        let mut secret_type = "password";
        let mut meta = "";

        if let Some(type_start) = remain.find('[') {
            if let Some(type_end) = remain.find(']') {
                secret_type = &remain[type_start + 1..type_end];
            }
        }
        if let Some(meta_start) = remain.find('(') {
            if let Some(meta_end) = remain.rfind(')') {
                meta = &remain[meta_start + 1..meta_end];
            }
        }

        records.push(VaultRecord {
            key: String::from(key),
            secret_type: String::from(secret_type),
            meta: String::from(meta),
        });
    }

    Ok(records)
}

pub fn get_secret(key: &str, pass: &str) -> Result<String, &'static str> {
    let (status, out) = run_vault_cmd(&["get", key, "-f", "/etc/vault.db", "-p", pass, "--raw"])?;
    if status != 0 {
        return Err("failed to retrieve secret");
    }
    String::from_utf8(out).map_err(|_| "invalid secret encoding")
}

pub fn set_secret(key: &str, secret_type: &str, secret: &str, meta: &str, pass: &str) -> Result<(), &'static str> {
    let (status, _) = run_vault_cmd(&[
        "set", key, "-f", "/etc/vault.db", "-p", pass, "-s", secret, "--type", secret_type, "--meta", meta
    ])?;
    if status != 0 {
        return Err("failed to store secret");
    }
    Ok(())
}

pub fn delete_secret(key: &str, pass: &str) -> Result<(), &'static str> {
    let (status, _) = run_vault_cmd(&["delete", key, "-f", "/etc/vault.db", "-p", pass])?;
    if status != 0 {
        return Err("failed to delete secret");
    }
    Ok(())
}

pub fn rekey_vault(old_pass: &str, new_pass: &str) -> Result<(), &'static str> {
    let (status, _) = run_vault_cmd(&["rekey", "-f", "/etc/vault.db", "-p", old_pass, "-n", new_pass])?;
    if status != 0 {
        return Err("failed to rekey vault");
    }
    Ok(())
}

pub fn init_vault(pass: &str) -> Result<(), &'static str> {
    let (status, _) = run_vault_cmd(&["init", "-f", "/etc/vault.db", "-p", pass])?;
    if status != 0 {
        return Err("failed to initialize vault");
    }
    Ok(())
}
