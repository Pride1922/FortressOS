#ifndef FORTRESS_SYSCALL_ABI_H
#define FORTRESS_SYSCALL_ABI_H
#include "types.h"
#include "signal_abi.h"
#include "netctl_abi.h"
#include "spawn_profile_abi.h"
#include "wait_profile_abi.h"
/* System Call Numbers */
#define SYS_EXIT      0
#define SYS_WRITE     1
#define SYS_OPEN      2
#define SYS_CLOSE     3
#define SYS_READ      4
#define SYS_STAT      5
#define SYS_READDIR   6
#define SYS_REBOOT    7
#define SYS_KBD_LAYOUT 8
#define SYS_SPAWN      9  /* (const char *path, const char *const argv[]) -> child PID */
#define SYS_WAIT       10 /* (uint64_t pid, int64_t *status or NULL) -> 0 */
#define SYS_MKDIR      11 /* (const char *path, uint64_t mode) -> 0 */
#define SYS_UNLINK     12 /* (const char *path) -> 0 */
#define SYS_RENAME     13 /* (const char *oldpath, const char *newpath) -> 0 */
#define SYS_SYNC       14 /* () -> 0; flush the writable /mnt device */
#define SYS_DMESG      15 /* (char *buf, uint64_t cap) -> bytes written */
#define SYS_TERMCTL    16
#define SYS_INPUT_READ 17
#define SYS_GETCWD     18 /* (char *buf, uint64_t size) -> bytes written */
#define SYS_CHDIR      19 /* (const char *path) -> 0 */
#define SYS_SPAWN_EXT  20 /* (const char *path, const spawn_opts_t *opts, uint64_t opts_size) -> child PID */
#define SYS_DUP2       21 /* (int oldfd, int newfd) -> newfd or -errno */
#define SYS_DUP        22 /* (int oldfd) -> lowest available fd or -errno */
#define SYS_FCNTL      23 /* (int fd, int cmd, uint64_t arg) -> result or -errno */
#define SYS_PIPE       24 /* (int pipefd[2], uint32_t flags) -> 0 or -errno */

/* S8 process, signal and terminal APIs (Phases 1–3). */
#define SYS_SETPGID    25 /* (uint64_t pid, uint64_t pgid) -> 0 */
#define SYS_KILL       27 /* (int64_t selector, uint64_t signal) */
#define SYS_SIGACTION  28 /* (signal, const signal_action_t *act, signal_action_t *old) */
#define SYS_SIGPROCMASK 29 /* (how, const uint64_t *set, uint64_t *old) */
#define SYS_TCSETPGRP  30 /* (fd, pgid) -> 0 */
#define SYS_TCGETPGRP  31 /* (fd) -> foreground pgid */
#define SYS_TERMATTR   34 /* (fd, TERM_GET/SET, terminal_attrs_t *, sizeof) */
#define SYS_GETPGRP    26 /* () -> pgid */
#define SYS_SIGRETURN  32 /* () -> kernel restores full context; no user-visible return */
#define SYS_WAITPID    33 /* (int64_t selector, uint64_t *status, uint32_t options) -> pid/0/-errno */
#define SYS_GROUP_RELEASE 35 /* (uint64_t pgid, uint32_t action) -> 0 */
#define SYS_PROCINFO      36 /* (index or PROC_INFO_SELF, proc_info_t *buf) -> 1/0/-errno */
#define PROC_INFO_SELF UINT64_MAX /* Own published identity and scheduler ticks; same layout. */
#define SYS_SYSINFO       37 /* (sysinfo_t *buf) -> 0/-errno */
#define SYS_SOCKET        38
#define SYS_BIND          39
#define SYS_SENDTO        40
#define SYS_RECVFROM      41
#define SYS_NETCTL        42 /* Unchanged: (NETCTL_PING, net_ping_v1_t *, 48). */
#define SYS_CONNECT       43
#define SYS_LISTEN        44 /* Reserved; unsupported until NET-2 step 4. */
#define SYS_ACCEPT        45 /* Reserved; unsupported until NET-2 step 4. */
#define SYS_SEND          46
#define SYS_RECV          47
#define SYS_SHUTDOWN      48
#define SYS_SEND_UNTIL    49 /* (fd, data, len, flags=0, absolute BSP ticks) */
#define SYS_RECV_UNTIL    50 /* (fd, data, cap, flags=0, absolute BSP ticks) */
#define SYS_CONNECT_UNTIL 51 /* (fd, sockaddr, size, absolute BSP ticks) */
#define SYS_MOUNTINFO     52 /* (uint32_t index, mount_info_t *out) -> 1=entry, 0=done, -errno */
#define SYS_BLOCKINFO     53 /* (uint32_t index, block_info_t *out) -> 1=entry, 0=done, -errno */
#define SYS_LOCKSTAT      54 /* (char *buf, uint64_t cap) -> bytes written or -errno */
#define SYS_SPAWN_PROFILE 55 /* Spawn 0..2/160B; waits 3..5/72B; pipe I/O 6..8/184B. */
#define PROC_INFO_MAX     64
#define GROUP_RELEASE 0
#define GROUP_CANCEL  1
#define SPAWN_SETPGROUP 1u /* v2: reserved2=0 creates group, >0 joins group */
#define SPAWN_STAGED    2u /* v2: fully construct but do not schedule */
#define SPAWN_V2_FLAGS (SPAWN_SETPGROUP | SPAWN_STAGED)
#define WNOHANG    1u
#define WUNTRACED  2u
#define WCONTINUED 8u
#define WIFEXITED(s) (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFSIGNALED(s) (((s) & 0x7f) != 0 && ((s) & 0x7f) != 0x7f)
#define WTERMSIG(s) ((s) & 0x7f)
#define WIFSTOPPED(s) (((s) & 0xff) == 0x7f)
#define WSTOPSIG(s) (((s) >> 8) & 0xff)
#define WIFCONTINUED(s) ((s) == 0xffff)

#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC      1

#define DMESG_SIZE     (64 * 1024)

#define SPAWN_FD_ACTION_OPEN  1  /* open path, dup to dst_fd */
#define SPAWN_FD_ACTION_DUP2  2  /* dup2(src_fd, dst_fd) */
#define SPAWN_FD_ACTION_CLOSE 3  /* close(dst_fd) */
#define MAX_SPAWN_ACTIONS    16

/* Existing spawn vector limits, including each string's terminating NUL.
 * Shared with the loader so shells can reject oversized groups before launch. */
#define MAX_SPAWN_ARGS       32
#define MAX_ARG_STRLEN       256
#define MAX_TOTAL_ARGS_LEN   1024
#define MAX_SPAWN_ENVP       32
#define MAX_ENV_STRLEN       256
#define MAX_TOTAL_ENVP_LEN   1024

typedef struct {
    uint32_t type;       /* SPAWN_FD_ACTION_* */
    int32_t  dst_fd;     /* target fd in child (0..31) */
    int32_t  src_fd;     /* source fd for DUP2 (0..31) */
    uint32_t flags;      /* open flags (VFS_O_*) */
    uint32_t mode;       /* open mode */
    uint32_t reserved;   /* 0 */
    uint64_t path;       /* user string pointer for OPEN (or 0) */
} spawn_fd_action_t;

_Static_assert(sizeof(spawn_fd_action_t) == 32, "spawn_fd_action_t must be exactly 32 bytes");

typedef struct {
    uint32_t size;          /* sizeof(spawn_opts_t) = 64 */
    uint32_t version;       /* 1 or 2; layout unchanged */
    uint32_t flags;         /* v1: 0; v2: SPAWN_V2_FLAGS */
    uint32_t reserved0;     /* 0 */
    uint64_t argv;          /* pointer to argv NULL-terminated array of char* */
    uint64_t envp;          /* pointer to envp NULL-terminated array of char* */
    uint64_t cwd;           /* pointer to cwd string (optional, 0 for inherit) */
    uint64_t fd_actions;    /* pointer to spawn_fd_action_t array (or 0) */
    uint32_t action_count;  /* count of fd actions (up to MAX_SPAWN_ACTIONS) */
    uint32_t reserved1;     /* 0 */
    uint64_t reserved2;     /* v1: 0; v2: pgid with SPAWN_SETPGROUP */
} spawn_opts_t;

_Static_assert(sizeof(spawn_opts_t) == 64, "spawn_opts_t must be exactly 64 bytes");

#define PROC_STATE_FREE    0
#define PROC_STATE_RUNNING 1
#define PROC_STATE_STOPPED 2
#define PROC_STATE_ZOMBIE  3
#define PROC_STATE_DONE    4

typedef struct {
    int64_t  pid, ppid, pgid, sid;
    uint32_t state;       /* 0=free, 1=running, 2=stopped, 3=zombie, 4=done(reserved) */
    uint32_t reserved;    /* zero */
    uint64_t cpu_ticks;   /* cumulative scheduler ticks */
    char     name[16];    /* NUL-terminated, unused bytes zero */
} proc_info_t;

_Static_assert(sizeof(proc_info_t) == 64, "proc_info_t ABI size");
_Static_assert(__builtin_offsetof(proc_info_t, pid) == 0, "proc_info_t.pid offset");
_Static_assert(__builtin_offsetof(proc_info_t, ppid) == 8, "proc_info_t.ppid offset");
_Static_assert(__builtin_offsetof(proc_info_t, pgid) == 16, "proc_info_t.pgid offset");
_Static_assert(__builtin_offsetof(proc_info_t, sid) == 24, "proc_info_t.sid offset");
_Static_assert(__builtin_offsetof(proc_info_t, state) == 32, "proc_info_t.state offset");
_Static_assert(__builtin_offsetof(proc_info_t, reserved) == 36, "proc_info_t.reserved offset");
_Static_assert(__builtin_offsetof(proc_info_t, cpu_ticks) == 40, "proc_info_t.cpu_ticks offset");
_Static_assert(__builtin_offsetof(proc_info_t, name) == 48, "proc_info_t.name offset");

typedef struct {
    uint64_t total_ram_bytes;   /* managed RAM (prereq 1 definition) */
    uint64_t free_ram_bytes;    /* free PMM frames × PAGE_SIZE */
    uint64_t uptime_ticks;      /* BSP elapsed timer ticks (prereq 2) */
    uint64_t tick_hz;           /* calibrated frequency; also cpu_ticks' unit */
    uint32_t cpu_count;         /* initialized scheduler CPUs, BSP included */
    uint32_t task_count;        /* enumerable user processes, zombies included */
    uint64_t tsc_hz;            /* calibrated invariant TSC frequency (0 if unavailable) */
    uint64_t kernel_heap_used;  /* bytes allocated in kernel heap */
    uint64_t kernel_heap_total; /* total bytes of kernel heap */
    uint32_t thread_count;      /* total active threads */
    uint32_t reserved;          /* padding/reserved */
} sysinfo_t;

_Static_assert(sizeof(sysinfo_t) == 72, "sysinfo_t ABI size");
_Static_assert(__builtin_offsetof(sysinfo_t, total_ram_bytes) == 0, "sysinfo_t.total_ram_bytes offset");
_Static_assert(__builtin_offsetof(sysinfo_t, free_ram_bytes) == 8, "sysinfo_t.free_ram_bytes offset");
_Static_assert(__builtin_offsetof(sysinfo_t, uptime_ticks) == 16, "sysinfo_t.uptime_ticks offset");
_Static_assert(__builtin_offsetof(sysinfo_t, tick_hz) == 24, "sysinfo_t.tick_hz offset");
_Static_assert(__builtin_offsetof(sysinfo_t, cpu_count) == 32, "sysinfo_t.cpu_count offset");
_Static_assert(__builtin_offsetof(sysinfo_t, task_count) == 36, "sysinfo_t.task_count offset");
_Static_assert(__builtin_offsetof(sysinfo_t, tsc_hz) == 40, "sysinfo_t.tsc_hz offset");
_Static_assert(__builtin_offsetof(sysinfo_t, kernel_heap_used) == 48, "sysinfo_t.kernel_heap_used offset");
_Static_assert(__builtin_offsetof(sysinfo_t, kernel_heap_total) == 56, "sysinfo_t.kernel_heap_total offset");
_Static_assert(__builtin_offsetof(sysinfo_t, thread_count) == 64, "sysinfo_t.thread_count offset");
_Static_assert(__builtin_offsetof(sysinfo_t, reserved) == 68, "sysinfo_t.reserved offset");

/* =========================================================================
 * Storage Observability ABI (SYS_MOUNTINFO, SYS_BLOCKINFO)
 * ========================================================================= */

typedef enum {
    VFS_FS_NONE   = 0,
    VFS_FS_TARFS  = 1,
    VFS_FS_EXT2   = 2,
    VFS_FS_EXT4   = 3
} vfs_fs_type_t;

#define MOUNT_FLAGS_RO  0u
#define MOUNT_FLAGS_RW  1u

typedef struct {
    char           source[32];
    char           mount_path[256];
    uint32_t       fs_type;         /* vfs_fs_type_t */
    uint32_t       flags;           /* MOUNT_FLAGS_RO / MOUNT_FLAGS_RW */
    uint64_t       total_blocks;
    uint64_t       free_blocks;
    uint64_t       total_inodes;
    uint64_t       free_inodes;
    uint32_t       block_size;
    uint32_t       reserved;
} mount_info_t;

_Static_assert(sizeof(mount_info_t) == 336, "mount_info_t ABI size");
_Static_assert(__builtin_offsetof(mount_info_t, source) == 0, "mount_info_t.source offset");
_Static_assert(__builtin_offsetof(mount_info_t, mount_path) == 32, "mount_info_t.mount_path offset");
_Static_assert(__builtin_offsetof(mount_info_t, fs_type) == 288, "mount_info_t.fs_type offset");
_Static_assert(__builtin_offsetof(mount_info_t, flags) == 292, "mount_info_t.flags offset");
_Static_assert(__builtin_offsetof(mount_info_t, total_blocks) == 296, "mount_info_t.total_blocks offset");
_Static_assert(__builtin_offsetof(mount_info_t, free_blocks) == 304, "mount_info_t.free_blocks offset");
_Static_assert(__builtin_offsetof(mount_info_t, total_inodes) == 312, "mount_info_t.total_inodes offset");
_Static_assert(__builtin_offsetof(mount_info_t, free_inodes) == 320, "mount_info_t.free_inodes offset");
_Static_assert(__builtin_offsetof(mount_info_t, block_size) == 328, "mount_info_t.block_size offset");
_Static_assert(__builtin_offsetof(mount_info_t, reserved) == 332, "mount_info_t.reserved offset");

typedef struct {
    char           name[32];
    uint32_t       sector_size;
    uint32_t       reserved;    /* Reserved for v2: Bits 0-7: class, Bits 8-15: durability, Bits 16-31: flags */
    uint64_t       size_bytes;
    char           partuuid[40]; /* Formatted UUID: "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" */
    char           label[40];    /* Partition label / name (NUL-terminated) */
    char           type_guid[40];/* Partition Type GUID string */
    uint64_t       start_lba;    /* Starting LBA (0 if base device) */
    uint64_t       sector_count; /* Sector count */
    uint32_t       is_partition; /* 1 if partition, 0 if base block device */
    uint32_t       part_index;   /* Partition index (1-based, 0 if not partition) */
} block_info_t;

_Static_assert(sizeof(block_info_t) == 192, "block_info_t ABI size");
_Static_assert(__builtin_offsetof(block_info_t, name) == 0, "block_info_t.name offset");
_Static_assert(__builtin_offsetof(block_info_t, sector_size) == 32, "block_info_t.sector_size offset");
_Static_assert(__builtin_offsetof(block_info_t, reserved) == 36, "block_info_t.reserved offset");
_Static_assert(__builtin_offsetof(block_info_t, size_bytes) == 40, "block_info_t.size_bytes offset");
_Static_assert(__builtin_offsetof(block_info_t, partuuid) == 48, "block_info_t.partuuid offset");
_Static_assert(__builtin_offsetof(block_info_t, label) == 88, "block_info_t.label offset");
_Static_assert(__builtin_offsetof(block_info_t, type_guid) == 128, "block_info_t.type_guid offset");
_Static_assert(__builtin_offsetof(block_info_t, start_lba) == 168, "block_info_t.start_lba offset");
_Static_assert(__builtin_offsetof(block_info_t, sector_count) == 176, "block_info_t.sector_count offset");
_Static_assert(__builtin_offsetof(block_info_t, is_partition) == 184, "block_info_t.is_partition offset");
_Static_assert(__builtin_offsetof(block_info_t, part_index) == 188, "block_info_t.part_index offset");

/* System Call Error Codes */
#define SYSCALL_SUCCESS   0
#define SYSCALL_EINVAL   -1  /* Invalid argument / oversized length */
#define SYSCALL_EFAULT   -2  /* Bad address / inaccessible user memory */
#define SYSCALL_EBADF    -3  /* Invalid file descriptor */
#define SYSCALL_ENOSYS   -4  /* Unknown system call number */
#define SYSCALL_ENOENT   -5  /* No such file or directory */
#define SYSCALL_EMFILE   -6  /* Too many open files */
#define SYSCALL_EISDIR   -7  /* Is a directory */
#define SYSCALL_ENOTDIR  -8  /* Not a directory */
#define SYSCALL_EIO      -9  /* I/O error / tainted filesystem */
#define SYSCALL_ENOMEM   -10 /* Out of memory */
#define SYSCALL_EROFS    -11 /* Read-only filesystem */
#define SYSCALL_EFBIG    -12 /* File too large / unsupported indirection */
#define SYSCALL_ENOSPC   -13 /* No space left on device */
#define SYSCALL_EOPNOTSUPP -14 /* Operation not supported */
#define SYSCALL_EEXIST   -15 /* File already exists */
#define SYSCALL_ECHILD   -16 /* Not an uncollected child of this process */
#define SYSCALL_ENOEXEC  -17 /* Invalid or unsupported executable */
#define SYSCALL_E2BIG    -18 /* Argument list or string too long */
#define SYSCALL_ENOTEMPTY -19 /* Directory not empty */
#define SYSCALL_EPIPE    -20 /* Broken pipe: no readers */
#define SYSCALL_EAGAIN   -21 /* Reserved would-block error; pipes now block */

#define SYSCALL_EINTR -22
#define SYSCALL_ESRCH -23
#define SYSCALL_EPERM -24
#define SYSCALL_ENOTTY -25
#define SYSCALL_ECONNREFUSED -26
#define SYSCALL_ECONNRESET -27
#define SYSCALL_ENOTCONN -28
#define SYSCALL_EADDRINUSE -29
#define SYSCALL_ETIMEDOUT -30
#define SYSCALL_EISCONN -31

/* Constraints */
#define MAX_SYSCALL_WRITE_LEN  16384

/*
 * =============================================================================
 * FortressOS Fast System Call ABI (x86_64 Long Mode)
 * =============================================================================
 * Instruction: 'syscall' (fast entry) / 'sysretq' (fast return)
 * Calling Convention:
 *   RAX = System call number
 *   RDI = Argument 1
 *   RSI = Argument 2
 *   RDX = Argument 3
 *   R10 = Argument 4 (matches System V syscall convention)
 *   R8  = Argument 5
 *   R9  = Argument 6
 * Return:
 *   RAX = Return value / negative error code (0 on success, < 0 on failure)
 * Hardware Clobbers:
 *   RCX = Overwritten by hardware with user RIP upon 'syscall'
 *   R11 = Overwritten by hardware with user RFLAGS upon 'syscall'
 * Callee-Preserved:
 *   RBX, RBP, R12, R13, R14, R15, RSP
 * Stack Invariant:
 *   Entry stub performs NO pushes, calls, or writes on the user stack.
 *   Switches to TSS.RSP0 immediately with interrupts disabled via SFMASK.
 * =============================================================================
 */

#endif
