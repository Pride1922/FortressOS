# Permissions Phase 3: database, login and credential syscalls

Phase 5 reruns login 9/9 and expands hostile mutations to 10,000 each for
passwd, group and shadow. See [Phase 5 evidence](permissions-phase5-gates.md).
The historical image-not-rebuilt statement below describes Phase 3 only.

2026-10-10. Complete locally. No GitHub changes or Dell/physical testing.
All changes remain uncommitted. Phase 4 set-ID execution/sudo is not included.

## Delivered behavior

- Credential ABI 66–70: setresuid/setresgid, bounded setgroups, drop-only
  capset and capget. Explicit keep flags preserve full uint32 IDs. Handlers
  validate/copy user input outside G, derive canonical complete values, and
  publish only if the entire old snapshot still matches under G. Stale
  publication returns EAGAIN without changes. UID transitions leaving no zero
  UID clear capabilities; neither capset nor ordinary spawn restores them.
- Build-time root-owned passwd/group/shadow databases. Archive modes are
  0644/0644/0600 independent of host staging modes. Root is locked. The user
  approved passwordless live-media operator login with a visible warning and
  temporary HOME=/run/user/1000. FORTRESS_OPERATOR_HASH_FILE can supply an
  explicit SHA-256-crypt hash instead; no known password/hash ships in source.
  Persistent database mutation and persistent homes remain writable-root work.
- Login reads the immutable databases while privileged, authenticates, checks
  or creates user-owned 0700 runtime storage, then drops supplementary groups,
  all GIDs, and all UIDs before spawning the shell. Unexpected existing owner,
  group, type, mode or mount flags reject without repairing the path. Failure
  waits two seconds on the BSP timebase; incoming input cannot shorten it.
  SYS_INPUT_READ already supplies raw non-echoed input. Password buffers and
  crypt intermediates are wiped; overlong input is drained and rejected.
- The kernel's actual boot launcher in main.c now starts /bin/login. The
  plan's original user/init.asm reference was inaccurate: that file is the
  separate Ring 3 self-test. Boot/entry assembly and subsystem init ordering
  are unchanged. LOGIN_TEST=1 admits login=0 only in explicit test builds.
  Normal kernels ignore login=0. Legacy root shell runners can prepare an
  isolated source snapshot with create_ext4_guest_workspace.py --login-test
  and build it with LOGIN_TEST=1; add login=0 when constructing further test
  ISOs. Existing production images must not be used as root-bypass fixtures.
- The shell imports its existing loader-provided environment before history
  and commands. HOME, USER, LOGNAME, SHELL and PATH survive login and child
  spawn. Login ignores INT/TSTP while waiting in the shell's foreground group;
  the shell owns prompt and command signal dispositions. whoami resolves the
  effective UID; ls -l resolves database owners/groups with numeric fallback.

## Host evidence

Final retained aggregate: build/permissions-phase3/final-host.log.
Executed in WSL Ubuntu-24.04:

```
make test-host test-perm-db-host test-perm-creds-host \
     test-perm-syscalls-host test-perm-runfs-host
python3 scripts/test_runner_host.py
python3 scripts/audit_permissions_wiring.py
```

All PASS. Actual-code ASan/UBSan coverage includes malformed/missing/extra
fields, duplicate names/IDs, embedded NUL/control input, file/ID bounds,
complete parser rollback, 3000 deterministic hostile passwd mutations, a
fixed published SHA-256-crypt vector and 20 comparisons against the host's
glibc-compatible libcrypt crypt API (empty/short/long passwords, varying salt
and explicit/default rounds, altered digests, wrong passwords and locked
accounts). The implementation follows
[Drepper's SHA-crypt specification](https://www.akkadia.org/drepper/SHA-crypt.txt)
using the existing digest.c SHA-256 core.

Login helper adapters check no password echo, interrupted-buffer wiping,
overflow draining, zero-effect unsafe-runtime rejection and the privileged
mkdir/chown/chmod sequence. Staging checks root lock, nine-field shadow, default
passwordless operator, archive modes and unsupported-hash rejection before
staging effects. Actual syscall/registry adapters test input-range denial,
excess groups, full-width ID/keep flags, capability bounds/drop, re-elevation
denial and stale publication. Existing credential value tests retain 70,496
matrix cases; runfs and aggregate shell/pipe/EXT2/stream-tool regressions pass.
Shell host tests cover entry-environment import and invalid/oversize rejection;
runner tests cover named ls ownership and UINT32_MAX numeric fallback.
The source inventory retains 173 classified sites, zero unclassified calls.
Host adapters do not establish IRQ, scheduler or physical correctness.

## QEMU evidence

Final invocation: make test-login.
Retained campaign: build/permissions-phase3/login-jvgudsu8.
Manifest: login-jvgudsu8/manifest.json. 9/9 cases PASS:

| Cases | Assertions |
| --- | --- |
| BIOS/UEFI × SMP=1/4, explicit generated test hash (4) | Login prompt, wrong-password delay/no echo, locked root rejection, uid/euid/suid=1000, gid/egid/sgid=1000, groups 1000/10/44/104, caps=0, /etc/shadow EACCES, re-elevation denied, ordinary child inheritance, runtime 1000:1000 0700, named ls/whoami, minimal environment and cwd, writable temporary home, Ctrl-C recovery, logout/relogin, safe directory reuse and unsafe-mode rejection |
| BIOS/UEFI SMP=1 test-only login=0 (2) | Direct root shell; actual Ring 3 full-ID/keep flags, invalid pointers/groups/caps, drop-only/no-restoration ABI and parent-shell identity preservation |
| BIOS SMP=1 passwordless control (1) | Explicit warning, operator identity and all runtime/environment/lifecycle checks |
| BIOS/UEFI SMP=1 normal kernels requesting login=0 (2) | Escape ignored; normal login prompt and warning, operator identity and runtime/environment/lifecycle checks; test credential syscall absent |

All cases use isolated ISO builds, no data disks, paired OVMF code/disposable
vars, exact guarded argv, bounded UART observation and PID termination/reap.
Injected extra -drive/-blockdev arguments fail preflight. UART logs, stderr,
argv, ISOs, initramfs copies, kernel/probe hashes and original snapshot source
manifests are retained. Generated test passwords are not committed or printed;
the temporary hash file is removed after the campaign. The test ISOs contain
only their disposable generated hash, never the production default.

Test snapshot: .codex-remote-attachments/ext4-phase9/guest-workspace-qbvntmp_.
Normal snapshot: .codex-remote-attachments/ext4-phase9/guest-workspace-jaeuadm7.
The normal kernel ELF contains neither sys_test_setcreds nor the login=0 escape
string. A final hash comparison confirms all 293 runtime/build input paths in
the normal snapshot match the current workspace; see preservation.json for
the exact count. No real AP Ring 3 credential-entry or cross-core socket claim
is inferred from selecting SMP=4; the existing registry exclusion is retained.

## Preservation and limits

build/permissions-phase3/baseline preserves all 838 initial tracked/untracked
source paths and the initial binary tracked diff. preservation.json records
20 intentionally extended existing paths and 818 byte-identical paths; no
pre-existing work was reset. Earlier failed/interrupted test attempts remain
retained and are not final acceptance evidence. git diff --check passes.

Normal bin/fortress.img was not rebuilt or attached; SHA-256 remains
d130bec6d0b396a968135fb2132b9543a8774c8889b24fce464f88a10cd68c28.

The database is intentionally bounded (16 records/file, 8192 bytes/file,
512 bytes/line, 31-byte names, 127-byte canonical paths, 128 printable ASCII
password bytes). SHA-256-crypt accepts 1000–100000 rounds, default 5000;
larger work factors reject rather than clamp. Nine-field shadow aging metadata
is validated, but accounts requiring a password change/calendar expiration
fail closed until a clock/aging policy exists. Parser scratch is process-local
static storage for the current single-user-thread runtime, preserving the
4 KiB user stack; reentrant/shared-user-thread parsing is not supported.
No persistent root/home, set-ID exec, sudo, general PAM/aging support,
hardware/storage durability or Dell acceptance is added by this phase.
