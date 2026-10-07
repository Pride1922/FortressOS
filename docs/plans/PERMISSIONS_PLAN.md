# FortressOS permissions implementation plan

Status: PROPOSED (2026-10-05). Not started. Begins after EXT4 Phase 9 acceptance (QEMU crash campaign, then bare metal). Precedes the installer and package manager, which depend on ownership and mode semantics.
Based on the externally supplied "Final Plan: FortressOS Permission System" draft, reconciled with the current `vfs.h`/`vfs.c`, `thread.h`, `syscall.c`, `tarfs.c`, `ext2.c` and `ext4.c` contracts. Target semantics are Linux-flavoured POSIX DAC plus a single 64-bit capability mask.

## 1. Decision and corrections

Implement Unix DAC (owner/group/other mode bits, supplementary groups, umask, setgid directories, sticky bit) enforced in the VFS against resolved nodes, with a kernel capability mask for privileged overrides. Deliver in six phases: node metadata and `/dev` + `/run`, call-site wiring with a permissive stub, enforcement plus metadata syscalls, users and login, setuid and `sudo`, then hardening. No ACLs, xattrs, namespaces, LSM or per-file capabilities in this plan.

Corrections to the source draft:

- **File map.** `vfs_lookup.inc`, `vfs_file.inc`, `vfs_dir.inc`, `process.c`, `exec.c`, `sched.h`, `ext4_layout.inc`, `user/init.c` and `initramfs/etc/` do not exist. VFS logic is in `src/fs/vfs.c`; the TCB is `tcb_t` in `src/kernel/thread.h`; program start is `SYS_SPAWN`/`SYS_SPAWN_EXT` → `process_spawn_from_vfs_group()` → `elf.c`; init is `user/init.asm`; initramfs is staged by the Makefile into `$(BUILD_DIR)/initramfs/{bin,etc,docs}`. There is no fork/exec: credential inheritance and setuid belong in the spawn path.
- **Node shape.** `vfs_node_t` has no `mode`/`uid`/`gid` and no `read_only` field. Type is `vfs_node_type_t` (FILE/DIRECTORY/STREAM); writability is `create == NULL`, `write == NULL` or the `can_write` callback. The draft's struct replacement is not adopted; fields are added alongside existing ones (§3).
- **Errors.** The kernel uses positive `VFS_E*` constants returned negated; `<errno.h>` is unavailable freestanding. Add `VFS_EACCES 13` and `VFS_ENOTDIR 20` (the latter replaces the literal `-8` currently returned by `vfs_create_ext`, which is the wrong value: 8 is ENOEXEC).
- **No device filesystem.** `/dev/tty` and `/dev/null` are `strcmp` special cases in `vfs.c`; there are no block-device nodes. The draft's `/dev/nvme*` `root:disk` gates presuppose a devfs. Phase 0 adds a minimal one.
- **No writable root.** TarFS directories have no `create`, so `/run/user/<uid>` cannot be created. Phase 0 adds a bounded memory-backed `/run`.
- **ext4/ext2 high UID/GID bits.** `l_i_uid_high`/`l_i_gid_high` sit at inode offsets 0x78/0x7A inside `osd2` (0x74–0x7F), i.e. within the base 128 bytes. They are present for ext2 and 128-byte inodes too. Always assemble 32-bit IDs; do not gate on `s_inode_size > 128`. Decode bytes explicitly (EXT4 plan rule), never cast to a packed struct.
- **ext4 currently discards permission bits.** `e4_read_inode` keeps `mode & 0xf000` only, and the ext4 mutator creates inodes as `0x4180`/`0x8180` (`0600` for files *and* directories). Once enforcement is on, every new ext4 directory is untraversable. Phase 0 must retain full `i_mode` and create with requested mode masked by umask. ext2 already creates `0755`/`0644` and preserves uid/gid on rewrite.
- **TarFS must honour header metadata.** Forcing 0755/0644 defaults erases `sudo`'s `04755`. Parse `mode`/`uid`/`gid` from the USTAR header and make the Makefile normalise them (`--owner=0 --group=0 --numeric-owner`, explicit `--mode` overrides) so Windows/WSL staging cannot inject host IDs or modes.
- **Setuid must not depend on read-only state.** The draft's "verify `node->read_only` is false" would block `sudo` itself, which lives on read-only TarFS. Use a per-mount `nosuid` flag: TarFS allows suid; USB mounts default to `nosuid,nodev` (Linux udisks convention).
- **Read-only precedence applies to files, directories and symlinks only.** Linux returns EROFS before DAC for those types, but writes to device nodes on a read-only filesystem are allowed. `/dev` nodes are not on TarFS in this design, but the rule is encoded anyway.
- **Login sequence locks itself out.** `setgroups → setresgid → capset(0) → setresuid(1000)` fails because `setresuid` needs `CAP_SETUID`, which was just dropped. Adopt the Linux rule instead: when a `setres*uid` call leaves no UID equal to 0, `cap_effective` is cleared automatically. `capset` becomes a drop-only refinement.
- **Capability inheritance across spawn was unspecified.** Rule (§4.3): child caps are `~0` iff the resulting euid is 0, else 0. A non-setuid spawn by UID 1000 can never acquire caps.
- **`CAP_DAC_READ_SEARCH`.** The draft only matched `mask == MAY_READ || mask == MAY_EXEC` on directories, so `MAY_READ|MAY_EXEC` slipped through, and Linux also grants file reads. Corrected in §4.2.
- **Password hashing.** A single SHA-256 over `salt||password` is too fast for offline attack resistance. Use sha-crypt `$5$` (SHA-256-crypt, default 5000 rounds, configurable `rounds=`) and the standard 9-field `/etc/shadow` layout, so Linux tools can read and generate FortressOS shadow files. Reuse the SHA-256 core behind `user/tools/digest.h`.
- **Missing user-facing surface.** The draft had no `chmod`, `chown`, `fchmod`, uid/gid in `stat`, `getresuid`/`getresgid`/`getgroups`, or tools (`ls -l`, `id`, `whoami`, `chmod`, `chown`, `umask` builtin). Without them the installer and package manager cannot set ownership. Added to Phase 2.
- **Matrix size.** Owner/group/other bits alone are 512 patterns, before masks × caller identity × caps × node type. The draft's "64-case" matrix is replaced by the exhaustive generated matrix in §7.
- **Static user database.** `/etc` is on read-only TarFS. Until a persistent root exists (installer plan), `passwd`/`useradd` cannot persist. This plan ships a build-time database only; mutation tools are deferred.
- **No committed default password hashes.** The live image must not ship a known password. Root is locked (`!`) by default; the operator hash is generated at build time from an explicit build variable, or the account is passwordless on live media with that fact logged at boot.

## 2. Contracts and integration boundaries

Read PROTECTED.md and AGENTS.md sections 4, 7.1, 7.4, 7.6 and 9 before implementation. Preserve:

- Lock ranks and the no-lock-across-switch rule. Credentials are read by the current thread from its own TCB without a lock; cross-thread reads (e.g. `ps`, signal permission) take the existing process-table lock. Credential mutation happens only on the current TCB, during syscalls.
- `ENABLE_*` raw-write gates, hardware storage exclusions (H6: the internal NVMe is not mounted) and DMA quarantine. **Permissions are an additional layer, never a replacement.** A `root:disk 0660` block node with `CAP_SYS_RAWIO` still cannot write unless the existing gate allows it.
- `-EROFS` (policy) vs `-EIO` (taint) distinction. Permission denial is `-EACCES` (DAC) or `-EPERM` (ownership/capability operations such as `chown`, `setresuid`, sticky-bit unlink), matching Linux.
- ext4 metadata writes (`chmod`, `chown`, create with mode/uid/gid) go through the existing JBD2 transaction engine and must be covered by the Phase 9 crash-injection harness before journaled RW is advertised.
- Kernel-internal lookups (boot init spawn, `usb_mount`, initramfs population, the terminal node) bypass permission checks through explicit `_kernel` entry points, never by passing a fake root credential.

Each user `tcb_t` is one process today (it owns `fd_table` and `cwd`). Credentials live in `tcb_t`. If shared-address-space user threads are introduced, credentials move to a shared process object first; that move is a precondition, not a follow-up.

## 3. Data layout

### 3.1 Credentials (`src/include/creds.h`, new)

```c
#define CREDS_MAX_GROUPS 16

#define CAP_DAC_OVERRIDE    (1ULL << 0)
#define CAP_DAC_READ_SEARCH (1ULL << 1)
#define CAP_FOWNER          (1ULL << 2)  /* chmod/utime/sticky bypass on others' files */
#define CAP_CHOWN           (1ULL << 3)
#define CAP_SETUID          (1ULL << 4)
#define CAP_SETGID          (1ULL << 5)
#define CAP_KILL            (1ULL << 6)
#define CAP_NET_BIND        (1ULL << 7)
#define CAP_SYS_ADMIN       (1ULL << 8)  /* mount/umount, driver control */
#define CAP_SYS_RAWIO       (1ULL << 9)  /* raw block access (still subject to ENABLE_*) */
#define CAP_SYS_BOOT        (1ULL << 10) /* power off / reboot */
#define CAP_ALL             ((1ULL << 11) - 1)

typedef struct creds {
    uint32_t uid, euid, suid;
    uint32_t gid, egid, sgid;
    uint32_t groups[CREDS_MAX_GROUPS];
    uint16_t ngroups;
    uint16_t umask;          /* 0022 default; only low 0777 honoured */
    uint32_t reserved;
    uint64_t cap_effective;
} creds_t;
```

Differences from the draft: adds `CAP_FOWNER`, `CAP_CHOWN`, `CAP_SYS_BOOT` (needed by chmod/chown and existing power syscalls); `CAP_ALL` is the defined-bit mask rather than `~0ULL`, so `capset` can reject unknown bits. Bit numbering is internal (not Linux-numbered); the user ABI exposes `CAP_*` from `syscall_abi.h`. Layout is static-asserted.

`tcb_t` gains `creds_t creds`. Kernel threads hold root/`CAP_ALL`. The boot init process is spawned with root/`CAP_ALL`, umask 0022.

### 3.2 Node metadata (`src/fs/vfs.h`)

Add to `vfs_node_t`:

```c
uint32_t uid, gid;
uint16_t mode;        /* full S_IFMT | 07777 */
uint8_t  mnt_flags;   /* VFS_MNT_RDONLY | VFS_MNT_NOSUID | VFS_MNT_NODEV */
```

`vfs_node_type_t` stays; invariant: `type == VFS_DIRECTORY ⇔ S_ISDIR(mode)`, `type == VFS_FILE ⇔ S_ISREG(mode)`, device and stream nodes carry `S_IFCHR`/`S_IFBLK`/`S_IFIFO`. A debug assertion in the node constructors checks agreement. `S_*` and `MAY_*` constants go in `vfs.h` with `VFS_` prefixes where they could collide with user headers.

`mnt_flags` is copied by the filesystem when it materialises a node (TarFS: `RDONLY`; ext2/ext4 from mount state; USB mounts add `NOSUID|NODEV`; devfs/runfs: 0). `can_write` remains the source of dynamic EROFS/EIO (taint).

`vfs_stat_t` gains `uid`, `gid`; `mode` carries the full mode. This is a user ABI size change: bump through the existing size-checked pattern, never silently.

### 3.3 On-disk mapping

| FS | Read | Write |
| --- | --- | --- |
| ext4 | `mode=u16(0x00)`, `uid=u16(0x02)\|u16(0x78)<<16`, `gid=u16(0x18)\|u16(0x7A)<<16` | Create sets all three; `chmod`/`chown` update in place under a JBD2 handle and recompute the inode checksum |
| ext2 | Same offsets (osd2 is within 128 bytes) | Create stops hardcoding 0755/0644 and uses the masked requested mode; `chmod`/`chown` as above, non-journaled |
| TarFS | USTAR `mode`, `uid`, `gid` octal fields, bounded parse; malformed → boot-time `[FAIL]` like other header errors | n/a (RO) |
| devfs, runfs | Static table / creator credentials | In memory |

ext4 continues to reject inode types it does not support (symlink, device, FIFO) with `-EOPNOTSUPP`; device nodes exist only in devfs.

## 4. Enforcement engine

### 4.1 Entry points (`src/fs/vfs.c`)

```c
int vfs_permission(const vfs_node_t *node, int mask, const creds_t *cr);
int vfs_may_delete(const vfs_node_t *dir, const vfs_node_t *victim, const creds_t *cr);
uint16_t vfs_create_mode(const vfs_node_t *dir, uint16_t requested, const creds_t *cr,
                         uint32_t *gid_out);
```

`vfs_permission` is pure: no locks, no allocation, no I/O. Every user-reachable path resolves `current` credentials once at syscall entry and passes them down.

### 4.2 `vfs_permission` order

1. `MAY_WRITE` on a regular file, directory or symlink with `mnt_flags & VFS_MNT_RDONLY` → `-VFS_EROFS`. Then, if `can_write` is set, propagate its error.
2. `MAY_EXEC` on a regular file with no `x` bit anywhere → `-VFS_EACCES`, even with caps.
3. Pick the class: owner if `euid == node->uid`; else group if `egid` or any supplementary group equals `node->gid`; else other. Linux semantics: **only the selected class is consulted** (an owner with `0077` is denied even though others are allowed).
4. If the class bits grant every requested bit → 0.
5. `CAP_DAC_OVERRIDE` → 0 (step 2 already handled exec).
6. `CAP_DAC_READ_SEARCH`: grant if `mask & ~(MAY_READ | (S_ISDIR ? MAY_EXEC : 0)) == 0`.
7. → `-VFS_EACCES`.

`vfs_may_delete`: requires `MAY_WRITE|MAY_EXEC` on `dir`; if `dir` is sticky, the caller must also be the owner of `dir` or `victim`, or hold `CAP_FOWNER`, else `-VFS_EPERM`.

`vfs_create_mode`: `mode = requested & ~umask & 07777`; if parent is setgid, `gid = parent->gid` and new directories keep `S_ISGID`; otherwise `gid = egid`. uid is `euid`. A non-member creating in a setgid directory gets `S_ISGID` stripped from regular files (Linux rule).

### 4.3 Credential transitions

| Event | Rule |
| --- | --- |
| Spawn, non-setuid image | Child copies parent `creds_t` (including umask and groups); `cap_effective = (euid == 0) ? CAP_ALL : 0` |
| Spawn, `S_ISUID` image, mount allows suid | `euid = suid = node->uid`; caps = `CAP_ALL` iff new euid is 0 |
| Spawn, `S_ISGID` image (with group-x) | `egid = sgid = node->gid` |
| Spawn, setuid/setgid with `nosuid` mount | Bits ignored, spawn succeeds unprivileged (Linux behaviour) |
| `setresuid` | Unprivileged: each new value ∈ {uid, euid, suid}. `CAP_SETUID`: any. After: if none of uid/euid/suid is 0, clear `cap_effective` |
| `setresgid`, `setgroups` | Unprivileged `setresgid` as above; `setgroups` requires `CAP_SETGID`; `ngroups > 16` → `-VFS_EINVAL` |
| `capset(mask)` | `new = mask & cap_effective` only (drop-only); unknown bits → `-VFS_EINVAL` |
| `umask(m)` | Returns old; stores `m & 0777` |

Setuid spawns are "secure" spawns: the kernel closes inherited descriptors above 2 that are not explicitly mapped by `spawn_opts` fd actions, and passes an `AT_SECURE`-style flag in the entry block (see `user-entry-envp.md`) so the user runtime ignores `PATH`-like inputs. Environment filtering itself is done by `sudo` in user space, not the kernel.

### 4.4 Call sites (all in `src/fs/vfs.c` unless noted)

| Path | Check |
| --- | --- |
| Path walk in `vfs_lookup_creds` | `MAY_EXEC` on every traversed directory |
| `vfs_open_ext` | `MAY_READ`/`MAY_WRITE` per `O_ACCMODE`; `O_TRUNC` implies `MAY_WRITE`; `O_CREAT` on a missing name → parent `MAY_WRITE|MAY_EXEC` + `vfs_create_mode`; device nodes on `nodev` mounts → `-VFS_EACCES` |
| `vfs_create_ext`, `vfs_mkdir` | Parent `MAY_WRITE|MAY_EXEC`; `vfs_create_mode`. `create` callback gains `(mode, uid, gid)` |
| `vfs_unlink` (files and directories) | `vfs_may_delete` |
| `vfs_rename` | `vfs_may_delete` on source; parent `MAY_WRITE|MAY_EXEC` on destination, plus `vfs_may_delete` on an existing destination; moving a directory to a new parent also requires `MAY_WRITE` on the moved directory (its `..` changes) |
| `vfs_readdir` via `SYS_READDIR` | `MAY_READ` on the directory |
| Spawn (`process_spawn_from_vfs_group`) | `MAY_EXEC` on the image, regular file only; then §4.3 |
| `SYS_CHDIR` | `MAY_EXEC` on target |
| `SYS_CHMOD`/`FCHMOD` | Caller is owner or `CAP_FOWNER`, else `-VFS_EPERM`; non-member non-`CAP_FSETID` clears `S_ISGID` |
| `SYS_CHOWN` | Changing uid needs `CAP_CHOWN`; owner may change gid to a group they belong to; clears `S_ISUID`/`S_ISGID` on regular files |
| `SYS_KILL` and group signals | Sender `euid`/`uid` matches target `uid`/`suid`, or `CAP_KILL` |
| `SYS_REBOOT` (and power-off paths) | `CAP_SYS_BOOT` |
| `SYS_BIND` to port <1024 | `CAP_NET_BIND` |

## 5. Device and runtime filesystems

**devfs** (`src/fs/devfs.c`, new): a static, boot-lifetime directory replacing the `/dev/tty` and `/dev/null` `strcmp` cases. Initial nodes:

| Node | Owner | Mode |
| --- | --- | --- |
| `/dev/tty` | root:tty (5) | 0666 (controlling-terminal semantics unchanged) |
| `/dev/null` | root:root | 0666 |
| `/dev/console` | root:tty | 0620 |
| Block partition nodes for enumerated GPT partitions (e.g. `/dev/usb0p2`) | root:disk (6) | 0660 |

Block nodes are **read-only in this plan**: their `write` is NULL regardless of capability. Raw write exposure belongs to the installer plan and must go through the existing `ENABLE_*` gates and H6 exclusion. Whether to expose the internal NVMe as a read-only node at all is a Section 9 discussion item; until decided it is omitted. `/dev/fb*`, `/dev/dri/*` and `/dev/input/*` (`root:video`, `root:input`, 0660) are reserved names for the future GUI, not implemented here.

**runfs** (`/run`, new): bounded in-memory directory tree (fixed node pool, fixed per-file byte cap, no file data beyond what login needs). `/run` is `root:root 0755`; `/run/user` is `root:root 0755`; login creates `/run/user/<uid>` as `<uid>:<gid> 0700`. Pool exhaustion → `-VFS_ENOSPC`. No persistence across boot.

## 6. Phases

### Phase 0: node metadata, devfs and runfs (no enforcement)

- `creds.h`; `creds_t` in `tcb_t`; root init credentials; spawn copies credentials.
- `vfs_node_t` fields, `vfs_stat_t` uid/gid/mode, `VFS_EACCES`/`VFS_ENOTDIR`.
- TarFS header parse; Makefile normalises archive ownership and modes.
- ext4: retain full `i_mode`, decode 32-bit uid/gid; create with requested mode/uid/gid (remove `0x4180`/`0x8180`). ext2: decode uid/gid; create with requested mode.
- devfs replaces the special-cased nodes; runfs mounted at `/run`.
- **Gate:** existing `test-host`, `test-shell`, `test-ext2`, ext4 host and QEMU suites, and the Phase 9 crash campaign pass unchanged. New host test: ext4/ext2 inode roundtrip with uid `0x12345678`, gid `0x87654321`, every mode in 07777, checksum valid, `e2fsck -fn` clean on a disposable image. `stat` from Ring 3 reports correct mode/uid/gid for TarFS, ext4, devfs and runfs nodes.

### Phase 1: call-site wiring with a permissive stub

- `vfs_permission` returns 0 but every call site in §4.4 is present and passes real credentials; kernel paths use `_kernel` variants.
- `create` callback signature carries mode/uid/gid end to end.
- **Gate:** all Phase 0 gates; a trace build records every `vfs_permission` call during `test-shell` and shows no path that bypasses it (audit list attached to the roadmap evidence).

### Phase 2: enforcement plus metadata syscalls and tools

- Real `vfs_permission`, `vfs_may_delete`, `vfs_create_mode`.
- Syscalls: `umask`, `chmod`, `fchmod`, `chown`, `getresuid`, `getresgid`, `getgroups`; stat ABI update. Numbers are the next free at implementation time (51 is the current maximum).
- `ls -l` (numeric IDs until Phase 3 provides names), `chmod`, `chown`, `id`, shell `umask` builtin.
- **Gate:** generated host matrix (§7) under ASan/UBSan. QEMU: a test program spawned through a debug-only `SYS_TEST_SETCREDS` (compiled only into test images) as UID 1001, caps 0, asserts `-EACCES` on a root `0600` file, on traversal through a `0700` directory, on writing `/dev/usb0p*`; `-EROFS` on TarFS writes; `-EPERM` on chown and on sticky-directory deletion of another user's file; umask 0027 yields `0640`/`0750`. Root shell workflows unchanged. ext4 `chmod`/`chown` added to the crash-injection inventory and recovered state verified by `e2fsck -fn`.

### Phase 3: user database, login, credential syscalls

- Syscalls: `setresuid`, `setresgid`, `setgroups`, `capset`, plus no-echo terminal read (reuse nano's raw mode path).
- `/etc/passwd` (0644), `/etc/group` (0644), `/etc/shadow` (0600, root:root) staged at build. Groups: `root:0`, `tty:5`, `disk:6`, `wheel:10`, `video:44`, `input:104`, `operator:1000`. Members: `operator` in `wheel,video,input`.
- `user/tools/login.c`: bounded parsers for the three files; sha-crypt `$5$` verification in constant time; failed-attempt delay; sequence `setgroups → setresgid → setresuid` (caps clear automatically); create `/run/user/<uid>` 0700; set `HOME`, `USER`, `LOGNAME`, `SHELL`, `PATH`; spawn the shell.
- `user/init.asm` spawns `/bin/login` instead of the shell. A boot parameter (`login=0`, test images only) keeps direct-shell boot for existing runners.
- `whoami`; `ls -l` resolves names.
- **Gate:** host tests for passwd/group/shadow parsers (malformed, oversize, missing fields, 9-field shadow) and `$5$` against known vectors from glibc's crypt. QEMU: boot to login prompt; operator login gives `uid=1000 euid=1000 caps=0` and groups `1000,10,44,104`; `/run/user/1000` is `1000:1000 0700`; wrong password fails with delay; locked root cannot log in; reading `/etc/shadow` as operator is `-EACCES`.

### Phase 4: setuid and `sudo`

- Spawn applies §4.3 setuid/setgid rules, secure-spawn fd closing and `AT_SECURE` flag; `nosuid` honoured.
- `user/tools/sudo.c`, installed `root:root 04755`: resolve real uid → passwd entry; require `wheel` membership; read the password from the controlling terminal with no echo; verify `$5$`; `setresgid(0,0,0)`, `setgroups(root's)`, `setresuid(0,0,0)`; spawn the target with a rebuilt minimal environment (`PATH=/bin`, `HOME=/root`, `USER=root`, `TERM` preserved); exit status propagated. No credential caching in this phase.
- **Gate:** QEMU: operator runs `sudo cat /etc/shadow` → success after the correct password, fails with the wrong one, fails for a non-wheel test user; `sudo id` shows `uid=0`; a `04755` binary on a USB (`nosuid`) mount runs unprivileged; a setuid spawn closes unmapped fds above 2; plain `cat /etc/shadow` still `-EACCES`.

### Phase 5: hardening and physical acceptance

- Audit every syscall for credential use (signals, process listing (`ps` shows all, as Linux), networking, power, `SYS_DMESG` restricted to root as `kernel.dmesg_restrict=1`).
- Fuzz the tar header, passwd/group/shadow parsers and spawn options with setuid images.
- Dell 5590: boot, login, `sudo`, USB `nosuid`, ext4 ownership persists across reboot and is read correctly by Linux (`ls -ln` on the stick). Manual physical evidence only; not implied by QEMU.

## 7. Verification matrix

| Target | Harness | Assertions |
| --- | --- | --- |
| `test-perm-matrix-host` | Host ASan/UBSan, real `vfs_permission` | Generated: all 4096 mode values × 7 non-empty masks × {owner, group-egid, group-supplementary, other} × {no caps, DAC_OVERRIDE, DAC_READ_SEARCH} × {file, dir} × {rw, ro mount}, compared against an independent reference implementation written in Python from the Linux rules; the generator emits both |
| `test-perm-create-host` | Host | umask/setgid-parent/non-member setgid stripping for files and directories |
| `test-perm-creds-host` | Host | `setres*`/`setgroups`/`capset` transition table incl. automatic cap clear, 16-group bound, unknown cap bits |
| `test-perm-inode-host` | Host + `e2fsck -fn` | ext2/ext4 roundtrip, checksums, 32-bit IDs |
| `test-perm-db-host` | Host | passwd/group/shadow parsers, `$5$` vectors |
| `test-perm` | QEMU BIOS+UEFI, disposable ext4 data disk | Phase 2 Ring 3 assertions |
| `test-login` | QEMU BIOS+UEFI | Phase 3 assertions |
| `test-sudo` | QEMU BIOS+UEFI, disposable USB image with a `nosuid` probe binary | Phase 4 assertions |
| ext4 crash campaign | Existing Phase 9 harness | `chmod`/`chown`/create-with-mode cut points recover to old-or-new metadata, never mixed |

## 8. Out of scope and handoff

ACLs, xattrs, SELinux-style labels, file capabilities, user namespaces, PAM, `passwd`/`useradd` persistence (needs persistent root), sudo timestamp caching, `/etc/sudoers`, GUI session management. The installer plan inherits: raw block write exposure under `ENABLE_*`, a writable persistent `/etc`, `passwd`, and ownership-preserving package extraction (`tar` must apply mode/uid/gid when run as root and mask with umask otherwise).

## 9. Open decisions

1. Expose the internal NVMe as a read-only `/dev` node, or keep it absent per H6?
2. Live-media operator authentication: build-variable password hash, or passwordless with a boot warning?
3. Keep internal capability numbering, or adopt Linux `CAP_*` numbers for future compatibility of tooling?
