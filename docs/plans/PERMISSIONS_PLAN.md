# FortressOS permissions implementation plan

Status: Phases 0–3 COMPLETE locally (2026-10-10); production DAC and login are enabled.
Phases 4–5 remain open. [Phase 3 evidence](../roadmap/permissions-phase3-gates.md).
The user approved registry global-lock consolidation, completion of Phase 0
and the reproduced EXT2 lifetime repair, then bounded GPT boot scratch repair.
GitHub issue/project state is unchanged. No physical testing is included.
Memory/SMP checkpoint `9d5c5ee` and paused performance limitations are preserved.
The protocol document distinguishes current APIs, historical audit and proposed
authorization. Existing local evidence/checklists track individual gates.
Based on the externally supplied "Final Plan: FortressOS Permission System" draft, reconciled with the current `vfs.h`/`vfs.c`, `thread.h`, `syscall.c`, `tarfs.c`, `ext2.c` and `ext4.c` contracts. Target semantics are Linux-flavoured POSIX DAC plus a single 64-bit capability mask.

## 1. Decision and corrections

Implement Unix DAC (owner/group/other mode bits, supplementary groups, umask, setgid directories, sticky bit) enforced in the VFS against resolved nodes, with a kernel capability mask for privileged overrides. Deliver in six phases: node metadata and `/dev` + `/run`, call-site wiring with a permissive stub, enforcement plus metadata syscalls, users and login, setuid and `sudo`, then hardening. No ACLs, xattrs, namespaces, LSM or per-file capabilities in this plan.

Corrections to the source draft (historical baseline; delivered Phase 0 changes
are recorded in §6 and the local evidence checklist):

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
- **Capability inheritance across spawn was unspecified.** Ordinary spawn copies the parent's remaining effective capabilities; UID 0 alone must not restore capabilities dropped through `capset`. Privilege elevation is limited to the explicitly admitted setuid transition (§4.3).
- **`CAP_DAC_READ_SEARCH`.** The draft only matched `mask == MAY_READ || mask == MAY_EXEC` on directories, so `MAY_READ|MAY_EXEC` slipped through, and Linux also grants file reads. Corrected in §4.2.
- **Password hashing.** A single SHA-256 over `salt||password` is too fast for offline attack resistance. Use sha-crypt `$5$` (SHA-256-crypt, default 5000 rounds, configurable `rounds=`) and the standard 9-field `/etc/shadow` layout, so Linux tools can read and generate FortressOS shadow files. Reuse the SHA-256 core behind `user/tools/digest.h`.
- **Missing user-facing surface.** The draft had no `chmod`, `chown`, `fchmod`, uid/gid in `stat`, `getresuid`/`getresgid`/`getgroups`, or tools (`ls -l`, `id`, `whoami`, `chmod`, `chown`, `umask` builtin). Without them the installer and package manager cannot set ownership. Added to Phase 2.
- **Matrix size.** Owner/group/other bits alone are 512 patterns, before masks × caller identity × caps × node type. The draft's "64-case" matrix is replaced by the exhaustive generated matrix in §7.
- **Static user database.** `/etc` is on read-only TarFS. Until a persistent root exists (installer plan), `passwd`/`useradd` cannot persist. This plan ships a build-time database only; mutation tools are deferred.
- **No committed default password hashes.** The live image must not ship a known password. Root is locked (`!`) by default; the operator hash is generated at build time from an explicit build variable, or the account is passwordless on live media with that fact logged at boot.

## 2. Contracts and integration boundaries

Read PROTECTED.md and AGENTS.md sections 4, 7.1, 7.4, 7.6 and 9 before implementation. Preserve:

- Lock ranks and the no-lock-across-switch rule. Resolve credentials into a consistent snapshot at syscall entry. Cross-thread readers (e.g. process inspection and signal authorization) and current-thread credential writers must share a synchronization protocol; a reader-only lock is insufficient. Choose the protocol against the final SMP process-table lifetime/locking APIs, not the former single global process lock. Never retain a target TCB beyond its protected lifetime or hold that lock across filesystem I/O or scheduling.
- `ENABLE_*` raw-write gates, hardware storage exclusions (H6: the internal NVMe is not mounted) and DMA quarantine. **Permissions are an additional layer, never a replacement.** A `root:disk 0660` block node with `CAP_SYS_RAWIO` still cannot write unless the existing gate allows it.
- `-EROFS` (policy) vs `-EIO` (taint) distinction. Permission denial is `-EACCES` (DAC) or `-EPERM` (ownership/capability operations such as `chown`, `setresuid`, sticky-bit unlink), matching Linux.
- ext4 metadata writes (`chmod`, `chown`, create with mode/uid/gid) go through the existing JBD2 transaction engine and must be covered by the Phase 9 crash-injection harness before journaled RW is advertised.
- Kernel-internal lookups (boot init spawn, `usb_mount`, initramfs population, the terminal node) bypass permission checks through explicit `_kernel` entry points, never by passing a fake root credential.

Each user `tcb_t` is one process today (it owns `fd_table` and `cwd`). Credentials live in `tcb_t`. If shared-address-space user threads are introduced, credentials move to a shared process object first; that move is a precondition, not a follow-up.

### 2.1 Foundation readiness and SMP handoff

The verified post-consolidation ownership map, historical G/shard discrepancy,
credential lifecycle and filesystem boundaries are in
[PERMISSIONS_PROTOCOLS.md](PERMISSIONS_PROTOCOLS.md). Global consolidation is
implemented and tested; TCB credentials now use complete validated publication,
value snapshots and explicit detachment before destruction. Process exclusion
never nests with filesystem or scheduler exclusion. Owned VFS references protect
lifetime, while metadata and mutation decisions require filesystem exclusion.
The approved EXT2 repair retains referenced tombstones and rejects active-open
unlink/replacement before media writes; it does not add POSIX open-unlinked disk
retention. See the local checklist for acceptance still outstanding.

No additional scheduler, allocator, USB transport or network protocol milestone is required solely for permissions. Before implementation, reconcile the plan with the completed SMP fixes. Commit `438fae0` introduces AP scheduling, unpinned workloads, cross-core signals and PID-bucket process locks; this is context, not a claim that its outstanding bugs are fixed or its tests have been rerun here.

| Foundation | Required integration / gate |
| --- | --- |
| SMP | Consistent credential publication, spawn inheritance and cross-core signal authorization using the final process lifetime/locking protocol; stress concurrent inspection, credential changes and target exit without mixed identities or stale TCB access. |
| Memory / ABI | Reuse existing address-space isolation and user-buffer validation; bounded group arrays, size/version-checked stat and credential ABI, fault/overflow tests, and no partial credential publication on invalid input. No allocator redesign. |
| USB | Propagate `nosuid`/`nodev` to all selected-volume nodes; retain durability admission, raw-write gates, DMA quarantine and internal-NVMe exclusion. No transport changes required. |
| Network | Audit privileged network configuration and low-port bind at syscall entry; preserve BSP protocol ownership unless the separate SMP/network work explicitly changes it. No TCP/UDP changes required. |
| EXT4 / ext2 | Decode and preserve full mode and 32-bit ownership; route EXT4 metadata changes through the journal and add focused old-or-new metadata recovery tests. Existing acceptance does not certify these new operations. |
| VFS | Synchronize permission checks with authoritative node identity/metadata and namespace mutation. Checks followed by unlocked rename/unlink/create are insufficient; document lock order and revalidation before mutation. Keep explicit kernel entry points. |

Phase 0 must document the credential snapshot/publication and VFS authorization/mutation protocols before enforcement work begins. Add regressions proving ordinary spawn cannot restore dropped capabilities, and that denied operations leave namespace, inode metadata and allocation unchanged. Do not hold process locks across filesystem locks/I/O; follow the existing rank-1 non-nesting contract.

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
#define CAP_FSETID          (1ULL << 11) /* explicit set-ID preservation rules */
#define CAP_ALL             ((1ULL << 12) - 1)

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

`vfs_node_type_t` stays; invariant: `type == VFS_DIRECTORY ⇔ S_ISDIR(mode)`, `type == VFS_FILE` denotes regular or block-backed linear I/O, device and stream nodes carry `S_IFCHR`/`S_IFBLK`/`S_IFIFO`. Constructors retain the existing I/O enum; metadata carries the precise inode/device type. `S_*` and `MAY_*` constants go in `vfs.h` with `VFS_` prefixes where they could collide with user headers.

`mnt_flags` is copied by the filesystem when it materialises a node (TarFS: `RDONLY`; ext2/ext4 from mount state; USB mounts add `NOSUID|NODEV`; devfs/runfs: 0). `can_write` remains the source of dynamic EROFS/EIO (taint).

`vfs_stat_t` stays 16 bytes; `mode` carries full mode. Current SYS_STAT_EXT (57), version 1, is a separate size-checked 40-byte interface carrying uid/gid and mount flags; old callers remain compatible.

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
| Spawn, ordinary image with no admitted identity transition | Child copies a consistent parent `creds_t` snapshot, including umask, groups and remaining effective caps; never replenish dropped caps solely because euid is 0 |
| Spawn, `S_ISUID` image, mount allows suid | `euid = suid = node->uid`; caps = `CAP_ALL` iff new euid is 0 |
| Spawn, `S_ISGID` image (with group-x) | `egid = sgid = node->gid` |
| Spawn, setuid/setgid with `nosuid` mount | Bits ignored, spawn succeeds unprivileged (Linux behaviour) |
| `setresuid` | Unprivileged: each new value ∈ {uid, euid, suid}. `CAP_SETUID`: any. After: if none of uid/euid/suid is 0, clear `cap_effective` |
| `setresgid`, `setgroups` | Unprivileged `setresgid` as above; `setgroups` requires `CAP_SETGID`; `ngroups > 16` → `-VFS_EINVAL` |
| `capset(mask)` | `new = mask & cap_effective` only (drop-only); unknown bits → `-VFS_EINVAL` |
| `umask(m)` | Returns old; stores `m & 0777` |

Setuid spawns are "secure" spawns: the kernel closes inherited descriptors above 2 that are not explicitly mapped by `spawn_opts` fd actions, and passes an `AT_SECURE`-style flag in the entry block (see `user-entry-envp.md`) so the user runtime ignores `PATH`-like inputs. Environment filtering itself is done by `sudo` in user space, not the kernel.

Before Phase 2, specify set-ID clearing/preservation for create, chmod, chown, ordinary writes and truncation, including `CAP_FSETID` and group membership. Perform any required mode-bit clearing in the same filesystem transaction as the associated metadata mutation; test denied/no-op/failed operations separately. Ordinary spawn with `nosuid`-ignored bits follows the ordinary inheritance rule. Genuine admitted setuid elevation remains an explicit Phase 4 transition, not a side effect of ordinary root spawn.

Phase 2's local value policy is now specified/tested in
[`permission_values.h`](../../src/fs/permission_values.h) and the
[Phase 2 checkpoint](../roadmap/permissions-phase2-gates.md). These helpers
are not connected to production admission. chmod strips requested setgid for
a caller outside the inode group unless CAP_FSETID is held. Changing ownership
of a non-directory clears setuid and group-executable setgid even with
CAP_FSETID, including an admitted request with identical IDs. Non-executable
setgid also clears outside the inode group without CAP_FSETID. Directories
retain their set-ID bits on chown. Positive regular-file writes and admitted
truncation (including unchanged size) apply the same stripping, with CAP_FSETID
preservation. Denied/failed operations and zero-byte writes publish no proposed mode. Each future adapter
must commit the content/identity and mode proposal in the same transaction.

### 4.4 Call sites (all in `src/fs/vfs.c` unless noted)

| Path | Check |
| --- | --- |
| Path walk in `vfs_lookup_creds` | `MAY_EXEC` on every traversed directory |
| `vfs_open_ext` | `MAY_READ`/`MAY_WRITE` per `O_ACCMODE`; `O_TRUNC` implies `MAY_WRITE`; `O_CREAT` on a missing name → parent `MAY_WRITE|MAY_EXEC` + `vfs_create_mode`; device nodes on `nodev` mounts → `-VFS_EACCES` |
| `vfs_create_ext`, `vfs_mkdir` | Parent `MAY_WRITE|MAY_EXEC`; `vfs_create_mode`. `create` callback gains `(mode, uid, gid)` |
| `vfs_unlink` (files and directories) | `vfs_may_delete` |
| `vfs_rename` | `vfs_may_delete` on source; parent `MAY_WRITE|MAY_EXEC` on destination, plus `vfs_may_delete` on an existing destination; moving a directory to a new parent also requires `MAY_WRITE` on the moved directory (its `..` changes) |
| `vfs_readdir_file` via `SYS_READDIR` | READ right admitted at directory open; no fresh path DAC, filesystem errors retained |
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

### 5.1 Directory layout and ownership baseline

| Path | Initial ownership / mode | Lifetime / policy |
| --- | --- | --- |
| `/`, `/bin`, `/etc` | root:root 0755 | Boot image; individual files use their declared modes; `/etc/shadow` is 0600. |
| `/dev` | root:root 0755 | Boot-lifetime devfs; node rules above. |
| `/tmp` | root:root 1777 | Existing memory-backed directory; clear at reboot; enforce sticky deletion/rename rules and explicit resource bounds. |
| `/run`, `/run/user` | root:root 0755 | Bounded runtime filesystem; clear at reboot. |
| `/run/user/<uid>` | user:primary-group 0700 | Created and assigned by privileged login before credential drop. |
| `/mnt` | From the selected filesystem's root inode | Persistent USB data; do not silently rewrite existing ownership to make login work. |
| Live-media user home | user:primary-group 0700 | User-approved Phase 3 default: temporary `/run/user/<uid>`. Persistent homes await writable root support; selected USB ownership is not changed. |

Creating the layout is part of Phase 0. A persistent root, generic mount-point expansion, symlinks and package management are separate work; no root filesystem migration is implied here.

## 6. Phases

### Phase 0: node metadata, devfs and runfs (no enforcement)

Implementation update: bounded TCB credentials, root initialization, snapshot
inheritance and validated publication are implemented. Metadata callbacks,
compatible extended stat, full inode mode/owner creation, normalized TarFS,
minimal devfs and bounded runfs are implemented. Authoritative creation accepts
already-derived attributes; user actor/umask/setgid wiring remains Phase 1/2.
No enforcement, login, sudo or user credential mutation syscall exists.
The [local delivery checklist and evidence](../roadmap/permissions-phase0-values.md)
records completed finite acceptance and retained failures. EXT2 active-open
unlink/replacement returns EOPNOTSUPP; runfs has 64 nodes and 4 KiB per file.
Generic VFS shared-read offsets/reference accounting and foreign EXT2 creator-OS
semantics are not certified by this foundation. See the protocol for those gates.

- [x] `creds.h`; `creds_t` in `tcb_t`; root init credentials; spawn copies credentials.
- [x] `vfs_node_t` fields, compatible extended stat uid/gid/mode, `VFS_EACCES`/`VFS_ENOTDIR`.
- [x] TarFS header parse; Makefile normalises archive ownership and modes.
- [x] ext4: retain full `i_mode`, decode 32-bit uid/gid; create with requested mode/uid/gid (remove `0x4180`/`0x8180`). ext2: decode uid/gid; create with requested mode.
- [x] devfs replaces the special-cased nodes; runfs mounted at `/run`.
- [x] Establish §5.1 ownership/lifetime defaults and document §2.1 synchronization protocols against the final SMP APIs; remain non-enforcing.
- [x] **Gate:** existing host/shell/EXT2, EXT4 mounted/namespace/integration and 66-case Phase 9 crash checks pass within the recorded finite coverage. The directory creation oracle changes to the intended 0755 default; an obsolete shell prompt matcher was corrected. New EXT2/EXT4 full mode/owner matrices, 1000 atomic creation cuts, Linux fsck/stat and Ring 3 ABI/namespace/spawn checks pass. Final raw USB peer checks pass BIOS/UEFI × SMP=1/4 after the approved GPT scratch repair. Exact source snapshots, failed attempts and limitations are retained in the evidence checklist; no broader IRQ/hardware or permissions-enforcement claim follows.

### Phase 1: call-site wiring with a permissive stub

- [x] Actor-aware path/open/exec/namespace/readdir hooks use actual published credential snapshots; trusted production paths use explicit `_kernel` variants. Hooks remain permissive.
- [x] Spawn uses one actor for image, explicit cwd, FD actions and inheritance; regular-file admission and complete error propagation are present.
- [x] Creation carries requested mode and actual euid/egid end to end. Umask/setgid derivation remains Phase 2.
- [x] User signal decisions run against current bound actor/target values under G; capability hooks precede power and low-port network side effects.
- [x] **Gate:** final aggregate/focused host checks PASS; isolated BIOS/UEFI trace shell 2/2, USB Ring 3 4/4, normal untraced control 4/4 PASS; finite source/callback bypass inventory classifies 139 sites with no unclassified production path/trusted calls. See [wiring evidence](../roadmap/permissions-phase1-wiring.md). Trace coverage is finite and does not establish that arbitrary executions cannot bypass a hook.

**Before enforcement:** authoritative filesystem-owned authorization/mutation
adapters, complete traversal semantics for components removed by lexical
`.`/`..` normalization, same-path rename no-op/error precedence, descriptor
rights and umask/setgid derivation remain explicit gates. Do not turn on DAC
by replacing the permissive VFS stub outside filesystem exclusion. No Phase 2
implementation or enforcement support is approved by Phase 1 acceptance.

### Phase 2: enforcement plus metadata syscalls and tools

- [x] Filesystem-owned DAC, namespace, creation and metadata decisions under EXT2/EXT4/runfs exclusion, original-component traversal, locked no-op rename, immutable TarFS/devfs and NODEV/raw-I/O policy. Production builds enable `FORTRESS_DAC_ENFORCED`.
- [x] Syscalls 58–64: `umask`, `chmod`, `fchmod`, `chown`, `getresuid`, `getresgid`, `getgroups`. Extended stat remains the separate 40-byte v1 ABI at 57. Explicit chown keep flags preserve all 32-bit IDs; `UMASK_QUERY` reads without temporary mutation.
- [x] Numeric `ls -l`, `chmod`, `chown`, `id`, and parent-shell `umask` builtin.
- [x] Generated sanitizer oracle, actual-filesystem denial/interleaving tests, capability/signal/ABI checks, EXT4 metadata/content old-or-new crash inventory and Linux audits. Debug-only fixed UID/GID 1001, zero-capability Ring 3 acceptance uses disposable USB journal fixtures under BIOS/UEFI at SMP=1/4; root tools and shell checks pass. Test credential mutation is absent from the normal kernel. See [Phase 2 evidence and limits](../roadmap/permissions-phase2-gates.md).

### Phase 3: user database, login, credential syscalls

- [x] Syscalls 66–70: `setresuid`, `setresgid`, `setgroups`, drop-only `capset`, and `capget`. Explicit keep flags preserve all 32-bit IDs; complete expected-old publication uses existing registry exclusion. Existing `SYS_INPUT_READ` provides non-echoed terminal input; no new terminal ABI.
- [x] `/etc/passwd` (0644), `/etc/group` (0644), `/etc/shadow` (0600, root:root) staged at build. Groups: `root:0`, `tty:5`, `disk:6`, `wheel:10`, `video:44`, `input:104`, `operator:1000`. Members: `operator` in `wheel,video,input`. User-approved live-media default: locked root, passwordless operator with a warning; optional `FORTRESS_OPERATOR_HASH_FILE` supplies an explicit build hash. No known password/hash in source.
- [x] `user/tools/login.c`: bounded parsers and `$5$` verification with a fixed-length digest comparison; two-second BSP-time failure delay; privileged runtime directory creation/validation, `setgroups → setresgid → setresuid`, zero capabilities, minimal shell environment and temporary home. Unexpected owner/group/type/mode/mount fails without repairing the existing path. The shell imports its loader environment before history/commands. Login ignores prompt INT/TSTP while waiting for the shell.
- [x] Actual boot launcher in `src/kernel/main.c` starts `/bin/login`. The old plan's `user/init.asm` reference was incorrect: that binary is a standalone Ring 3 self-test, not the shell launcher. `LOGIN_TEST=1` compiles the test-only `login=0` escape; normal kernels ignore it. `create_ext4_guest_workspace.py --login-test` prepares legacy direct-shell test ISO configuration; build that snapshot with `LOGIN_TEST=1`.
- [x] `whoami`; `ls -l` resolves database names with full-width numeric fallback.
- [x] **Gate:** actual-code host sanitizer parser/hash/login-helper/credential-ABI tests and BIOS/UEFI × SMP=1/4 password login PASS, plus BIOS/UEFI root-bypass controls, passwordless control and BIOS/UEFI normal-kernel login=0 rejection: 9/9 PASS. See [Phase 3 gates](../roadmap/permissions-phase3-gates.md).

Database bounds: 16 records/file, 8192 bytes/file, 512 bytes/line, 31-byte
names, 127-byte absolute canonical paths and 128 printable ASCII password
bytes. SHA-256-crypt supports default 5000 rounds or explicit 1000–100000
rounds; higher work factors reject rather than silently clamp. Nine-field
shadow aging fields are validated; configurations requiring password change
or calendar expiration fail closed because no calendar-clock policy exists.
Parsers use process-local static scratch to preserve the existing 4 KiB user
stack; they are not reentrant/shared-user-thread APIs.

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
| `test-perm-matrix-host` | Host ASan/UBSan, actual pure value engine | Generated: all 4096 mode values × 7 non-empty masks × {owner, group-egid, group-supplementary, other} × {no caps, DAC_OVERRIDE, DAC_READ_SEARCH} × {file, dir} × {rw, ro mount}, compared against an independent Python reference |
| `test-perm-create-host` | Host | umask/setgid-parent/non-member setgid stripping for files and directories |
| `test-perm-creds-host` | Host | `setres*`/`setgroups`/`capset` transition table incl. automatic cap clear, 16-group bound, unknown cap bits |
| `test-perm-inode-host` | Host + `e2fsck -fn` | ext2/ext4 roundtrip, checksums, 32-bit IDs |
| `test-perm-db-host` | Host | passwd/group/shadow parsers, `$5$` vectors |
| `test-perm-runfs-host`, `test-perm-filesystems-host PERM_FIXTURE_DIR=...` | Actual VFS/filesystem adapters, ASan/UBSan | Denied mutation invariance, controlled admission interleavings, metadata/content publication and EXT4 crash cuts |
| `test-perm-syscalls-host`, `test-perm-privileges-host` | Actual syscall/process/network adapters | Output validation, keep flags, umask publication, signal and capability admission |
| `PERM_PHASE2_EXPECT=1 python3 scripts/test_perm_guest.py build/permissions-phase0/source.ext4 --usb` | Isolated TEST_PERMISSIONS_ENFORCEMENT build; BIOS/UEFI × SMP=1/4 | Non-root Ring 3 enforcement and root tools; explicit disposable USB journal fixtures |
| `test-login` | QEMU BIOS+UEFI | Phase 3 assertions |
| `test-sudo` | QEMU BIOS+UEFI, disposable USB image with a `nosuid` probe binary | Phase 4 assertions |
| ext4 crash campaign | Existing Phase 9 harness | `chmod`/`chown`/create-with-mode cut points recover to old-or-new metadata, never mixed |

## 8. Out of scope and handoff

ACLs, xattrs, SELinux-style labels, file capabilities, user namespaces, PAM, `passwd`/`useradd` persistence (needs persistent root), sudo timestamp caching, `/etc/sudoers`, GUI session management. The installer plan inherits: raw block write exposure under `ENABLE_*`, a writable persistent `/etc`, `passwd`, and ownership-preserving package extraction (`tar` must apply mode/uid/gid when run as root and mask with umask otherwise).

## 9. Open decisions

1. Expose the internal NVMe as a read-only `/dev` node, or keep it absent per H6?
2. Resolved for Phase 3: passwordless live-media operator with a warning; optional explicit build hash file.
3. Keep internal capability numbering, or adopt Linux `CAP_*` numbers for future compatibility of tooling?
4. Resolved for live media: temporary `/run/user/1000`. Persistent home naming/backing remains part of writable-root work.
