# FortressOS User ABI Versioning & Evolution Specification (Issue #7)

## 1. Executive Summary & Design Philosophy

FortressOS implements a freestanding, microkernel-inspired x86_64 operating system designed for deterministic security, strict capability boundaries, and long-term ABI stability. 

As the userspace ecosystem expands—spanning multi-threaded POSIX runtimes (`pthreads`), cryptographic daemons (`sshd`), networking tools (`ifconfig`, `dhcpc`, `wget`), and shell environments—formalizing ABI versioning and evolution contracts is vital to prevent subtle memory corruption, structure tearing, or ABI drift.

Rather than imposing an over-engineered global versioning layer or complex runtime translation daemons, FortressOS follows three proven, high-integrity design patterns:
1. **Pattern A (Explicit Header Struct Versioning)**: Structures beginning with explicit `uint32_t size, version;` header fields (`stat_ext_v1_t`, `spawn_opts_t`, `spawn_opts_v3_t`, `account_request_t`, `install_log_event_v1_t`).
2. **Pattern B (Sized Query Buffer Pattern)**: Syscalls accepting a destination pointer and an explicit capacity/size register (`SYS_MEMINFO`, `SYS_GETGROUPS`, `SYS_DMESG`, `SYS_LOCKSTAT`, `SYS_GETCWD`, `SYS_LIST_DIR`, `SYS_GETRANDOM`).
3. **Pattern C (Fixed-Size Structs with Zero-Reserved Padding)**: Fixed ABI layouts with compile-time `_Static_assert` assertions on size, alignment, and member offsets, where the kernel guarantees zeroing reserved/padding fields on emission and validates zero-reserved inputs (`sysinfo_t`, `proc_info_t`, `mount_info_t`, `block_info_t`, `timespec_t`, `timeval_t`, `tms_t`, `struct pollfd`, `signal_action_t`).

All evolution strictly coordinates with permissions and credential contracts (Issue #4) and storage node lifetime contracts before adding or altering fields.

---

## 2. System Call Table Audit & Numbering Policy

### 2.1 Syscall Allocation Map (Syscalls 0 through 95)

The active system call table in [`src/include/syscall_abi.h`](file:///c:/Sources/FortressOS/src/include/syscall_abi.h) contains 96 allocated system call numbers spanning 0 to 95:

| Number | Identifier | Signature / Purpose | Specification Header |
|---|---|---|---|
| 0 | `SYS_EXIT` | `(int status) -> !` | `syscall_abi.h` |
| 1 | `SYS_WRITE` | `(int fd, const void *buf, size_t count) -> bytes/-errno` | `syscall_abi.h` |
| 2 | `SYS_OPEN` | `(const char *path, int flags, int mode) -> fd/-errno` | `syscall_abi.h` |
| 3 | `SYS_CLOSE` | `(int fd) -> 0/-errno` | `syscall_abi.h` |
| 4 | `SYS_READ` | `(int fd, void *buf, size_t count) -> bytes/-errno` | `syscall_abi.h` |
| 5 | `SYS_STAT` | `(const char *path, vfs_stat_t *out) -> 0/-errno` (legacy 16B) | `syscall_abi.h` |
| 6 | `SYS_READDIR` | `(int fd, vfs_dirent_t *out) -> count/-errno` | `syscall_abi.h` |
| 7 | `SYS_REBOOT` | `(uint32_t magic, uint32_t cmd) -> 0/-errno` | `syscall_abi.h` |
| 8 | `SYS_KBD_LAYOUT` | `(uint32_t layout_id) -> 0/-errno` | `syscall_abi.h` |
| 9 | `SYS_SPAWN` | `(const char *path, const char *const argv[]) -> pid/-errno` | `syscall_abi.h` |
| 10 | `SYS_WAIT` | `(uint64_t pid, int64_t *status) -> 0/-errno` | `syscall_abi.h` |
| 11 | `SYS_MKDIR` | `(const char *path, uint64_t mode) -> 0/-errno` | `syscall_abi.h` |
| 12 | `SYS_UNLINK` | `(const char *path) -> 0/-errno` | `syscall_abi.h` |
| 13 | `SYS_RENAME` | `(const char *oldpath, const char *newpath) -> 0/-errno` | `syscall_abi.h` |
| 14 | `SYS_SYNC` | `() -> 0/-errno` | `syscall_abi.h` |
| 15 | `SYS_DMESG` | `(char *buf, uint64_t cap) -> bytes/-errno` | `syscall_abi.h` |
| 16 | `SYS_TERMCTL` | `(int fd, int cmd, uint64_t arg) -> 0/-errno` | `syscall_abi.h` |
| 17 | `SYS_INPUT_READ` | `(void *buf, size_t count) -> bytes/-errno` | `syscall_abi.h` |
| 18 | `SYS_GETCWD` | `(char *buf, uint64_t size) -> bytes/-errno` | `syscall_abi.h` |
| 19 | `SYS_CHDIR` | `(const char *path) -> 0/-errno` | `syscall_abi.h` |
| 20 | `SYS_SPAWN_EXT` | `(const char *path, const spawn_opts_t *opts, uint64_t size) -> pid/-errno` | `syscall_abi.h` |
| 21 | `SYS_DUP2` | `(int oldfd, int newfd) -> newfd/-errno` | `syscall_abi.h` |
| 22 | `SYS_DUP` | `(int oldfd) -> newfd/-errno` | `syscall_abi.h` |
| 23 | `SYS_FCNTL` | `(int fd, int cmd, uint64_t arg) -> result/-errno` | `syscall_abi.h` |
| 24 | `SYS_PIPE` | `(int pipefd[2], uint32_t flags) -> 0/-errno` | `syscall_abi.h` |
| 25 | `SYS_SETPGID` | `(uint64_t pid, uint64_t pgid) -> 0/-errno` | `syscall_abi.h` |
| 26 | `SYS_GETPGRP` | `() -> pgid` | `syscall_abi.h` |
| 27 | `SYS_KILL` | `(int64_t selector, uint64_t signal) -> 0/-errno` | `syscall_abi.h` |
| 28 | `SYS_SIGACTION` | `(int sig, const signal_action_t *act, signal_action_t *old) -> 0/-errno` | `syscall_abi.h` |
| 29 | `SYS_SIGPROCMASK`| `(int how, const uint64_t *set, uint64_t *old) -> 0/-errno` | `syscall_abi.h` |
| 30 | `SYS_TCSETPGRP` | `(int fd, uint64_t pgid) -> 0/-errno` | `syscall_abi.h` |
| 31 | `SYS_TCGETPGRP` | `(int fd) -> pgid/-errno` | `syscall_abi.h` |
| 32 | `SYS_SIGRETURN` | `() -> restores user interrupt frame` | `syscall_abi.h` |
| 33 | `SYS_WAITPID` | `(int64_t selector, uint64_t *status, uint32_t options) -> pid/-errno` | `syscall_abi.h` |
| 34 | `SYS_TERMATTR` | `(int fd, int op, terminal_attrs_t *attr, size_t size) -> 0/-errno` | `syscall_abi.h` |
| 35 | `SYS_GROUP_RELEASE`| `(uint64_t pgid, uint32_t action) -> 0/-errno` | `syscall_abi.h` |
| 36 | `SYS_PROCINFO` | `(uint64_t index, proc_info_t *out) -> 1/0/-errno` | `syscall_abi.h` |
| 37 | `SYS_SYSINFO` | `(sysinfo_t *out) -> 0/-errno` (72B exact) | `syscall_abi.h` |
| 38 | `SYS_SOCKET` | `(int domain, int type, int protocol) -> fd/-errno` | `syscall_abi.h` |
| 39 | `SYS_BIND` | `(int fd, const struct sockaddr *addr, socklen_t len) -> 0/-errno` | `syscall_abi.h` |
| 40 | `SYS_SENDTO` | `(int fd, const void *buf, size_t len, int flags, const struct sockaddr *addr, socklen_t addrlen) -> bytes/-errno` | `syscall_abi.h` |
| 41 | `SYS_RECVFROM` | `(int fd, void *buf, size_t len, int flags, struct sockaddr *addr, socklen_t *addrlen) -> bytes/-errno` | `syscall_abi.h` |
| 42 | `SYS_NETCTL` | `(uint32_t cmd, void *arg, size_t size) -> 0/-errno` | `netctl_abi.h` |
| 43 | `SYS_CONNECT` | `(int fd, const struct sockaddr *addr, socklen_t len) -> 0/-errno` | `syscall_abi.h` |
| 44 | `SYS_LISTEN` | `(int fd, int backlog) -> 0/-errno` | `syscall_abi.h` |
| 45 | `SYS_ACCEPT` | `(int fd, struct sockaddr *addr, socklen_t *addrlen) -> newfd/-errno` | `syscall_abi.h` |
| 46 | `SYS_SEND` | `(int fd, const void *buf, size_t len, int flags) -> bytes/-errno` | `syscall_abi.h` |
| 47 | `SYS_RECV` | `(int fd, void *buf, size_t len, int flags) -> bytes/-errno` | `syscall_abi.h` |
| 48 | `SYS_SHUTDOWN` | `(int fd, int how) -> 0/-errno` | `syscall_abi.h` |
| 49 | `SYS_SEND_UNTIL` | `(int fd, const void *data, size_t len, uint32_t flags, uint64_t deadline) -> bytes/-errno` | `syscall_abi.h` |
| 50 | `SYS_RECV_UNTIL` | `(int fd, void *buf, size_t cap, uint32_t flags, uint64_t deadline) -> bytes/-errno` | `syscall_abi.h` |
| 51 | `SYS_CONNECT_UNTIL`| `(int fd, const struct sockaddr *addr, socklen_t size, uint64_t deadline) -> 0/-errno` | `syscall_abi.h` |
| 52 | `SYS_MOUNTINFO` | `(uint32_t index, mount_info_t *out) -> 1/0/-errno` | `syscall_abi.h` |
| 53 | `SYS_BLOCKINFO` | `(uint32_t index, block_info_t *out) -> 1/0/-errno` | `syscall_abi.h` |
| 54 | `SYS_LOCKSTAT` | `(char *buf, uint64_t cap) -> bytes/-errno` | `syscall_abi.h` |
| 55 | `SYS_SPAWN_PROFILE`| `(uint32_t type, void *buf, size_t size) -> 0/-errno` | `spawn_profile_abi.h` |
| 56 | `SYS_MEMINFO` | `(sysinfo_mem_t *buf, uint64_t size) -> 0/-errno` ($\ge$ 96B) | `syscall_abi.h` |
| 57 | `SYS_STAT_EXT` | `(const char *path, stat_ext_v1_t *out, uint64_t size, uint64_t version=1) -> 0/-errno` | `syscall_abi.h` |
| 58 | `SYS_UMASK` | `(uint64_t mask) -> old_mask/-errno` | `syscall_abi.h` |
| 59 | `SYS_CHMOD` | `(const char *path, uint64_t mode) -> 0/-errno` | `syscall_abi.h` |
| 60 | `SYS_FCHMOD` | `(int fd, uint64_t mode) -> 0/-errno` | `syscall_abi.h` |
| 61 | `SYS_CHOWN` | `(const char *path, uint32_t uid, uint32_t gid, uint32_t flags) -> 0/-errno` | `syscall_abi.h` |
| 62 | `SYS_GETRESUID` | `(uint32_t *ruid, uint32_t *euid, uint32_t *suid) -> 0/-errno` | `syscall_abi.h` |
| 63 | `SYS_GETRESGID` | `(uint32_t *rgid, uint32_t *egid, uint32_t *sgid) -> 0/-errno` | `syscall_abi.h` |
| 64 | `SYS_GETGROUPS` | `(uint64_t capacity, uint32_t *groups) -> count/-errno` | `syscall_abi.h` |
| 65 | `SYS_TEST_SETCREDS`| Test harness only: fixed UID/GID 1001 credentials | `syscall_abi.h` |
| 66 | `SYS_SETRESUID` | `(uint32_t ruid, uint32_t euid, uint32_t suid, uint32_t flags) -> 0/-errno` | `syscall_abi.h` |
| 67 | `SYS_SETRESGID` | `(uint32_t rgid, uint32_t egid, uint32_t sgid, uint32_t flags) -> 0/-errno` | `syscall_abi.h` |
| 68 | `SYS_SETGROUPS` | `(uint64_t count, const uint32_t *groups) -> 0/-errno` | `syscall_abi.h` |
| 69 | `SYS_CAPSET` | `(uint64_t mask) -> 0/-errno` | `syscall_abi.h` |
| 70 | `SYS_CAPGET` | `() -> mask` | `syscall_abi.h` |
| 71 | `SYS_INSTALLCTL`| `(uint32_t cmd, void *arg, size_t size) -> 0/-errno` | `install_abi.h` |
| 72 | `SYS_ACCOUNTCTL`| `(uint32_t cmd, void *arg, size_t size) -> 0/-errno` | `account_abi.h` |
| 73 | `SYS_INSTALLSEED`| `(uint32_t cmd, void *arg, size_t size) -> 0/-errno` | `account_abi.h` |
| 74 | `SYS_CLOCK_GETTIME`| `(uint32_t clk_id, timespec_t *ts) -> 0/-errno` | `syscall_abi.h` |
| 75 | `SYS_NANOSLEEP` | `(const timespec_t *req, timespec_t *rem) -> 0/-errno` | `syscall_abi.h` |
| 76 | `SYS_TIMES` | `(int64_t pid, tms_t *buf) -> 0/-errno` | `syscall_abi.h` |
| 77 | `SYS_GETRANDOM` | `(void *buf, size_t len, uint64_t flags) -> bytes/-errno` | `syscall_abi.h` |
| 78 | `SYS_FSTAT` | `(int fd, stat_ext_v1_t *out) -> 0/-errno` (40B exact) | `syscall_abi.h` |
| 79 | `SYS_HOSTKEY` | `(uint32_t op, void *buf, size_t size) -> 0/-errno` | `hostkey_abi.h` |
| 80 | `SYS_PTY_PAIR` | `(int fds[2], uint32_t flags) -> 0/-errno` | `syscall_abi.h` |
| 81 | `SYS_LSTAT` | `(const char *path, vfs_stat_t *out) -> 0/-errno` | `syscall_abi.h` |
| 82 | `SYS_LIST_DIR` | `(const char *path, char names[][64], uint64_t max_entries) -> count/-errno` | `syscall_abi.h` |
| 83 | `SYS_CLOCK_SETTIME`| `(uint32_t clk_id, const timespec_t *ts) -> 0/-errno` | `syscall_abi.h` |
| 84 | `SYS_RENAME_REPLACE`| `(const char *oldpath, const char *newpath, uint64_t flags) -> 0/-errno` | `syscall_abi.h` |
| 85 | `SYS_STAGE_WRITE`| `(int fd, const void *buf, size_t count, uint64_t expected_eof, uint64_t flags) -> written/-errno` | `syscall_abi.h` |
| 86 | `SYS_FSYNC` | `(int fd) -> 0/-errno` | `syscall_abi.h` |
| 87 | `SYS_WIFI` | `(uint64_t cmd, void *arg, size_t size) -> 0/-errno` | `wifi_abi.h` |
| 88 | `SYS_POLL` | `(struct pollfd *fds, uint64_t nfds, int64_t timeout_ms) -> count/-errno` | `syscall_abi.h` |
| 89 | `SYS_THREAD_CREATE`| `(uintptr_t entry, uintptr_t stack, uintptr_t arg, uint64_t *out_tid) -> tid/-errno` | `syscall_abi.h` |
| 90 | `SYS_THREAD_EXIT` | `(uint64_t exit_val) -> !` | `syscall_abi.h` |
| 91 | `SYS_THREAD_JOIN` | `(uint64_t tid, uint64_t *out_val) -> 0/-errno` | `syscall_abi.h` |
| 92 | `SYS_THREAD_SELF` | `() -> tid` | `syscall_abi.h` |
| 93 | `SYS_THREAD_YIELD`| `() -> 0` | `syscall_abi.h` |
| 94 | `SYS_FUTEX` | `(uint32_t *uaddr, int op, uint32_t val, const timespec_t *timeout) -> 0/-errno` | `syscall_abi.h` |
| 95 | `SYS_PKGCTL` | `(pkg_request_v1 *, pkg_result_v1 *, size=64) -> 0/-errno` | `pkg_abi.h` |

### 2.2 Syscall Allocation Rules

1. **High-Water Mark & Next Available**:
   ```c
   #define SYSCALL_MAX        95
   #define SYSCALL_NEXT_AVAIL 96
   ```
2. **Contiguous Allocation**: New system call numbers MUST be allocated monotonically starting from `SYSCALL_NEXT_AVAIL` (96).
3. **No Collision or Slot Reuse**: Numbers 71, 72, 73 were previously defined across `install_abi.h` and `account_abi.h`. They are now explicitly documented in `syscall_abi.h` with `#ifndef` guards, preventing accidental reallocation collisions.
4. **Fail-Closed on Unassigned Syscalls**: Any syscall invocation with `RAX > SYSCALL_MAX` or unregistered syscall numbers triggers `default: result = SYSCALL_ENOSYS; break;` in `src/kernel/syscall.c`, cleanly returning `-4` without side effects.

---

## 3. ABI Struct Evolution Patterns

### 3.1 Pattern A: Explicit Header Struct Versioning (`size` + `version`)

Used when a structure is passed as an extensible input/output contract across the syscall boundary.

```c
typedef struct {
    uint32_t size;     /* sizeof(struct_type) */
    uint32_t version;  /* Monotonic version number starting at 1 */
    /* ... versioned fields ... */
} versioned_struct_v1_t;
```

**Kernel Validation Rules**:
- **Size Check**: `user_size == sizeof(versioned_struct_v1_t)`. Undersized or oversized values without matching version descriptors fail closed with `-SYSCALL_EINVAL`.
- **Version Check**: `opts.version == 1`. Unsupported or future versions fail closed with `-SYSCALL_EINVAL`.
- **User Memory Range**: `vmm_validate_user_range(pml4, ptr, user_size, write_mode)`. Invalid addresses return `-SYSCALL_EFAULT` before inspecting structure fields.
- **Canary & Slack Isolation**: On validation failure, the kernel NEVER writes partial fields into the user buffer.

**Evolution Recipe (Adding Fields)**:
1. Preserve existing version struct unaltered (e.g., `stat_ext_v1_t` remains exactly 40 bytes).
2. Define a new struct (e.g., `stat_ext_v2_t`) with `version = 2` and new fields appended at the end.
3. Update the kernel dispatcher to accept both `sizeof(stat_ext_v1_t)` with `version == 1` and `sizeof(stat_ext_v2_t)` with `version == 2`.
4. Existing binaries continue running bit-for-bit unchanged without recompilation.

---

### 3.2 Pattern B: Sized Query Buffer Pattern

Used for observability, metric queries, or streaming buffers where callers provide buffer capacity in a register (`SYS_MEMINFO`, `SYS_DMESG`, `SYS_LOCKSTAT`, `SYS_GETCWD`, `SYS_LIST_DIR`, `SYS_GETGROUPS`).

**Kernel Validation Rules**:
- **Minimum Threshold**: The kernel validates that `user_size >= sizeof(expected_struct_t)`. If `user_size < sizeof(expected_struct_t)`, the syscall returns `-SYSCALL_EINVAL`.
- **Slack Preservation**: If a caller provides `user_size > sizeof(expected_struct_t)` (e.g. 128 bytes provided for a 96-byte `sysinfo_mem_t`), the kernel writes exactly `sizeof(expected_struct_t)` bytes and preserves all slack bytes beyond the struct boundary.
- **Zero-Allocation**: Metric extraction takes place using stack-allocated local snapshots without heap allocation.

---

### 3.3 Pattern C: Fixed Struct Layouts with Zero-Reserved Padding

Used for core POSIX-compatible structures and system snapshots (`sysinfo_t`, `proc_info_t`, `mount_info_t`, `block_info_t`, `timespec_t`, `pollfd_t`, `signal_action_t`).

**Kernel Validation Rules**:
- **Compile-Time Asserts**: Every public struct is covered by `_Static_assert(sizeof(...) == N)` and `_Static_assert(offsetof(...) == M)` in public headers and host tests.
- **Zeroed Reserved Fields on Emission**: The kernel explicitly zeroes all `reserved` or padding bytes (e.g., `info.reserved = 0;` at offset 68 in `sysinfo_t`). Uninitialized kernel stack bytes are never leaked to userland.
- **Fail-Closed on Non-Zero Input Reserved Fields**: If userland provides input with non-zero reserved fields where zero is required, the syscall rejects it with `-SYSCALL_EINVAL`.

---

## 4. Preservation of `SYS_SYSINFO` and `SYS_MEMINFO` Contracts

### 4.1 `SYS_SYSINFO` (Syscall 37) Contract

- **Input**: `RDI = (sysinfo_t *buf)`
- **Output**: 0 on success, `-SYSCALL_EFAULT` on invalid memory address.
- **Layout (72 bytes)**:
  - `0x00`: `uint64_t total_ram_bytes`
  - `0x08`: `uint64_t free_ram_bytes`
  - `0x10`: `uint64_t uptime_ticks`
  - `0x18`: `uint64_t tick_hz`
  - `0x20`: `uint32_t cpu_count`
  - `0x24`: `uint32_t task_count`
  - `0x28`: `uint64_t tsc_hz`
  - `0x30`: `uint64_t kernel_heap_used`
  - `0x38`: `uint64_t kernel_heap_total`
  - `0x40`: `uint32_t thread_count`
  - `0x44`: `uint32_t reserved` (Guaranteed 0)
- **Contract Guarantees**:
  - Exact 72-byte footprint preserved.
  - Zero heap allocation in kernel dispatcher.
  - User pointer validated with `vmm_validate_user_range(pml4, rdi, 72, true)`.
  - Canaries before and after the 72-byte buffer remain untouched.

### 4.2 `SYS_MEMINFO` (Syscall 56) Contract

- **Input**: `RDI = (sysinfo_mem_t *buf)`, `RSI = uint64_t size`
- **Output**: 0 on success, `-SYSCALL_EINVAL` on `size < 96`, `-SYSCALL_EFAULT` on invalid address.
- **Layout (96 bytes)**:
  - `0x00`: `uint32_t struct_size` (Always set to 96 by kernel)
  - `0x04`: `uint32_t flags` (0 in v1)
  - `0x08`: `uint64_t pmm_total_frames`
  - `0x10`: `uint64_t pmm_used_frames`
  - `0x18`: `uint64_t pmm_free_frames`
  - `0x20`: `uint64_t pmm_allocatable_frames`
  - `0x28`: `uint64_t heap_used_bytes`
  - `0x30`: `uint64_t heap_free_bytes`
  - `0x38`: `uint64_t heap_committed_bytes`
  - `0x40`: `uint64_t heap_largest_payload`
  - `0x48`: `uint64_t heap_free_blocks`
  - `0x50`: `uint64_t vmm_table_frames`
  - `0x58`: `uint64_t vmm_deferred_spaces`
- **Contract Guarantees**:
  - `size < 96` rejected unconditionally with `-SYSCALL_EINVAL`.
  - Kernel writes exactly 96 bytes and leaves any caller slack past 96 bytes untouched.
  - Snapshot is instantaneous under ranked locks (`pmm_lock` Rank 4, `heap_lock` Rank 2) without deadlocks.

---

## 5. Memory Safety & Pointer Boundary Rules

FortressOS enforces user-space memory safety via `vmm_validate_user_range`:
1. **Canonical Address Space**: User pointers must reside within `[0x1000, 0x00007fffffffffffULL]`.
2. **Zero-Page Protection**: Pointers below `0x1000` (e.g. NULL `0x0`) fail with `-SYSCALL_EFAULT`.
3. **Non-Canonical & Kernel Space Rejection**: Any address $\ge 0x0000800000000000ULL$ or kernel space $\ge 0xffff800000000000ULL$ is rejected with `-SYSCALL_EFAULT`.
4. **Arithmetic Overflow Protection**: `ptr + size < ptr` is detected and rejected with `-SYSCALL_EFAULT`.
5. **Access Permission**: Write buffers must have writable page table entries (`PTE_WRITABLE`); read-only mappings fail with `-SYSCALL_EFAULT`.

---

## 6. Layout & Invariant Matrix

| Type | Header | Size (Bytes) | Alignment (Bytes) | Key Offsets Checked |
|---|---|---|---|---|
| `stat_ext_v1_t` | `syscall_abi.h` | 40 | 8 | `size:0, version:4, file_size:8, type:16, mode:20, uid:24, gid:28, mnt_flags:32, reserved:36` |
| `vfs_stat_t` | `vfs.h` | 16 | 8 | `size:0, type:8, mode:12` |
| `vfs_dirent_t` | `vfs.h` | 72 | 8 | `name:0, type:56, size:64` |
| `sysinfo_t` | `syscall_abi.h` | 72 | 8 | `total_ram:0, free_ram:8, uptime:16, hz:24, cpu:32, task:36, tsc:40, heap_used:48, heap_total:56, thread:64, reserved:68` |
| `sysinfo_mem_t` | `syscall_abi.h` | 96 | 8 | `size:0, flags:4, pmm_total:8, pmm_used:16, pmm_free:24, pmm_alloc:32, heap_used:40, heap_free:48, heap_commit:56, heap_large:64, heap_blocks:72, vmm_table:80, vmm_deferred:88` |
| `spawn_fd_action_t` | `syscall_abi.h` | 32 | 8 | `type:0, dst_fd:4, src_fd:8, flags:12, mode:16, reserved:20, path:24` |
| `spawn_opts_t` | `syscall_abi.h` | 64 | 8 | `size:0, version:4, flags:8, reserved0:12, argv:16, envp:24, cwd:32, actions:40, count:48, reserved1:52, reserved2:56` |
| `spawn_identity_t` | `syscall_abi.h` | 80 | 4 | `uid:0, gid:4, ngroups:8, reserved:12, groups:16` |
| `spawn_opts_v3_t` | `syscall_abi.h` | 144 | 8 | `base:0, identity:64` |
| `proc_info_t` | `syscall_abi.h` | 64 | 8 | `pid:0, ppid:8, pgid:16, sid:24, state:32, reserved:36, cpu_ticks:40, name:48` |
| `mount_info_t` | `syscall_abi.h` | 336 | 8 | `source:0, mount_path:32, fs_type:288, flags:292, total_blocks:296, free_blocks:304, total_inodes:312, free_inodes:320, block_size:328, reserved:332` |
| `block_info_t` | `syscall_abi.h` | 192 | 8 | `name:0, sector_size:32, reserved:36, size_bytes:40, partuuid:48, label:88, type_guid:128, start_lba:168, sector_count:176, is_partition:184, part_index:188` |
| `timespec_t` | `syscall_abi.h` | 16 | 8 | `tv_sec:0, tv_nsec:8` |
| `timeval_t` | `syscall_abi.h` | 16 | 8 | `tv_sec:0, tv_usec:8` |
| `tms_t` | `syscall_abi.h` | 32 | 8 | `utime_ns:0, stime_ns:8, cutime_ns:16, cstime_ns:24` |
| `struct pollfd` | `syscall_abi.h` | 8 | 4 | `fd:0, events:4, revents:6` |
| `signal_action_t` | `signal_abi.h` | 32 | 8 | `handler:0, mask:8, flags:16, reserved:24` |
| `netctl_ifget_t` | `netctl_abi.h` | 64 | 8 | `version:0, reserved:4, mac:8, reserved2:14, ip:16, netmask:20, gateway:24, mtu:28, link:32, reserved3:36, rx:40, tx:48, reserved4:56` |
| `netctl_ifset_t` | `netctl_abi.h` | 32 | 8 | `version:0, reserved:4, ip:8, netmask:12, gateway:16, flags:20, reserved2:24` |
| `account_request_t`| `account_abi.h` | 64 | 8 | `size:0, version:4, generation:8, token:16, passwd:24, group:32, shadow:40, passwd_bytes:48, group_bytes:52, shadow_bytes:56, flags:60` |
| `install_log_event_v1_t`| `install_abi.h`| 32 | 8 | `size:0, version:4, event:8, stage:12, error:16, reserved:20, call:24` |

---

## 7. Verification Evidence

The user ABI specification is verified by `tests/abi_versioning_host.c` under GCC with AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan):
```bash
make test-abi-version-host
```
**Test Results**:
- `test_abi_struct_layouts`: 100% of static and runtime assertions on struct sizes, alignments, and member offsets passed.
- `test_syscall_table_audit`: 95/95 system calls audited without gaps or number collisions. `SYSCALL_MAX = 94` and `SYSCALL_NEXT_AVAIL = 95` verified.
- `test_pattern_a_stat_ext`: Header versioning, undersize, oversize, and unsupported version fail-closed rejections verified with boundary canary protection.
- `test_pattern_a_spawn_opts`: v1/v2/v3 evolution, flag constraints, and non-zero reserved field rejections verified.
- `test_pattern_b_sysinfo_mem`: Minimum buffer threshold, short buffer rejection, and slack preservation verified.
- `test_pattern_c_sysinfo`: Exact 72-byte footprint, zeroed reserved field emission, and canary preservation verified.
- `test_user_pointer_boundaries`: Zero page, canonical limits, higher-half kernel space, and arithmetic wrap-around rejection verified.
