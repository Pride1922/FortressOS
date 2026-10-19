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
#define SYS_SPAWN_EXT  20 /* (path, spawn_opts_t v1/v2 or spawn_opts_v3_t, size) -> child PID */
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
#define SYS_MEMINFO       56 /* (sysinfo_mem_t *buf, uint64_t size) -> 0/-errno */
#define SYS_STAT_EXT      57 /* (path, stat_ext_v1_t *out, size, version=1) -> 0/-errno */
#define SYS_UMASK         58 /* (mask) -> previous low-0777 mask */
#define SYS_CHMOD         59 /* (path, mode) -> 0/-errno */
#define SYS_FCHMOD        60 /* (fd, mode) -> 0/-errno */
#define SYS_CHOWN         61 /* (path, uint32 uid, uint32 gid, keep flags) */
#define SYS_GETRESUID     62 /* (uint32 *real, *effective, *saved) -> 0/-errno */
#define SYS_GETRESGID     63 /* Same pointer contract as GETRESUID. */
#define SYS_GETGROUPS     64 /* (capacity, uint32 *groups); capacity=0 queries count */
#define SYS_SETRESUID     66 /* (uint32 real, effective, saved, keep_flags) */
#define SYS_SETRESGID     67 /* Same full-width IDs/keep flags as SETRESUID. */
#define SYS_SETGROUPS     68 /* (count <= 16, const uint32 *groups) */
#define SYS_CAPSET        69 /* (uint64 mask): drop-only, rejects undefined bits */
#define SYS_CAPGET        70 /* () -> effective defined-bit mask */
#define SYS_CLOCK_GETTIME 74 /* (uint32_t clk_id, timespec_t *ts) -> 0/-errno */
#define SYS_NANOSLEEP     75 /* (const timespec_t *req, timespec_t *rem) -> 0/-errno */
#define SYS_TIMES         76 /* (int64_t pid, tms_t *buf) -> 0/-errno */
#define SYS_GETRANDOM     77 /* (void *buf, size_t len, uint64_t flags=0) -> len/-errno */
#define SYS_FSTAT         78 /* (fd, stat_ext_v1_t *out) -> 0/-errno; fixed 40-byte v1 output */
#define SYS_HOSTKEY       79 /* root-only fixed installed-root storage; hostkey_abi.h */
#define SYS_PTY_PAIR      80 /* (int fds[2], uint32_t flags) -> 0 or -errno */
#define SYS_LSTAT         81 /* (path, vfs_stat_t *out) -> 0/-errno */
#define SYS_LIST_DIR      82 /* (const char *path, char names[][64], uint64_t max_entries) -> count/-errno */
#define SYS_CLOCK_SETTIME 83 /* (CLOCK_REALTIME, const timespec_t *); CAP_SYS_ADMIN; 1970..2100 */
#define SYS_RENAME_REPLACE 84 /* (oldpath, newpath, flags=0); existing regular files, journaled EXT4 */
#define MAX_GETRANDOM_LEN 256u

/* Clock identifiers */
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_REALTIME_COARSE    5
#define CLOCK_MONOTONIC_COARSE   6
#define CLOCK_BOOTTIME           7

/* POSIX-compatible time specification structure */
typedef struct {
    int64_t tv_sec;  /* Seconds */
    int64_t tv_nsec; /* Nanoseconds: [0, 999999999] */
} timespec_t;
_Static_assert(sizeof(timespec_t) == 16, "timespec_t size");
_Static_assert(offsetof(timespec_t, tv_sec) == 0, "timespec_t tv_sec offset");
_Static_assert(offsetof(timespec_t, tv_nsec) == 8, "timespec_t tv_nsec offset");

/* Traditional microsecond timeval structure */
typedef struct {
    int64_t tv_sec;  /* Seconds */
    int64_t tv_usec; /* Microseconds: [0, 999999] */
} timeval_t;
_Static_assert(sizeof(timeval_t) == 16, "timeval_t size");
_Static_assert(offsetof(timeval_t, tv_sec) == 0, "timeval_t tv_sec offset");
_Static_assert(offsetof(timeval_t, tv_usec) == 8, "timeval_t tv_usec offset");

/* Process CPU-time accounting structure (nanoseconds) */
typedef struct {
    int64_t tms_utime_ns;   /* user time in nanoseconds */
    int64_t tms_stime_ns;   /* system time in nanoseconds */
    int64_t tms_cutime_ns;  /* children user time */
    int64_t tms_cstime_ns;  /* children system time */
} tms_t;
_Static_assert(sizeof(tms_t) == 32, "tms_t size");
_Static_assert(offsetof(tms_t, tms_utime_ns) == 0, "tms_t tms_utime_ns offset");
_Static_assert(offsetof(tms_t, tms_stime_ns) == 8, "tms_t tms_stime_ns offset");
_Static_assert(offsetof(tms_t, tms_cutime_ns) == 16, "tms_t tms_cutime_ns offset");
_Static_assert(offsetof(tms_t, tms_cstime_ns) == 24, "tms_t tms_cstime_ns offset");

#define CREDS_KEEP_REAL 1u
#define CREDS_KEEP_EFFECTIVE 2u
#define CREDS_KEEP_SAVED 4u
#define UMASK_QUERY UINT64_MAX /* Read current mask without changing it. */
#define CHOWN_KEEP_UID 1u
#define CHOWN_KEEP_GID 2u
#ifdef TEST_PERMISSIONS_ENFORCEMENT
#define SYS_TEST_SETCREDS 65 /* Test image only: fixed UID/GID 1001, no caps. */
#endif
typedef struct {
    uint32_t size, version;
    uint64_t file_size;
    uint32_t type, mode, uid, gid, mnt_flags, reserved;
} stat_ext_v1_t;
_Static_assert(sizeof(stat_ext_v1_t)==40, "stat_ext v1 size");
_Static_assert(offsetof(stat_ext_v1_t,file_size)==8, "stat_ext v1 offset");
#define PROC_INFO_MAX     64
#define GROUP_RELEASE 0
#define GROUP_CANCEL  1
#define SPAWN_SETPGROUP 1u /* v2: reserved2=0 creates group, >0 joins group */
#define SPAWN_STAGED    2u /* v2: fully construct but do not schedule */
#define SPAWN_SETSID    4u /* v2: create a new session and process group (sid=pid, pgid=pid) */
#define SPAWN_V2_FLAGS (SPAWN_SETPGROUP | SPAWN_STAGED | SPAWN_SETSID)
#ifndef WNOHANG
#define WNOHANG    1u
#define WUNTRACED  2u
#define WNOWAIT    4u
#define WCONTINUED 8u
#define WIFEXITED(s) (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFSIGNALED(s) (((s) & 0x7f) != 0 && ((s) & 0x7f) != 0x7f)
#define WTERMSIG(s) ((s) & 0x7f)
#define WIFSTOPPED(s) (((s) & 0xff) == 0x7f)
#define WSTOPSIG(s) (((s) >> 8) & 0xff)
#define WIFCONTINUED(s) ((s) == 0xffff)
#endif

#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
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

/* Explicit child identity, installed before publication. Requires CAP_SETUID
 * and CAP_SETGID, even for an unchanged identity. No set-id image elevation:
 * nonzero UID gets zero capabilities; UID zero inherits (never gains) caps.
 * v1/v2 remain exactly 64 bytes. All unused groups/reserved fields are zero. */
typedef struct {
    uint32_t uid, gid, ngroups, reserved;
    uint32_t groups[16];
} spawn_identity_t;
typedef struct {
    spawn_opts_t base; /* size=144, version=3; v2 flags/pgid semantics */
    spawn_identity_t identity;
} spawn_opts_v3_t;
_Static_assert(sizeof(spawn_identity_t) == 80, "spawn identity ABI");
_Static_assert(sizeof(spawn_opts_v3_t) == 144, "spawn v3 ABI");

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
 * Memory Subsystem Observability ABI (SYS_MEMINFO)
 * ========================================================================= */

typedef struct {
    uint32_t struct_size;          /* sizeof(sysinfo_mem_t) */
    uint32_t flags;                /* reserved, 0 */

    /* PMM frame metrics */
    uint64_t pmm_total_frames;     /* managed physical RAM frames */
    uint64_t pmm_used_frames;      /* currently allocated physical frames */
    uint64_t pmm_free_frames;      /* available physical frames */
    uint64_t pmm_allocatable_frames; /* free frames below current allocation ceiling (boot limit or unlocked total) */

    /* Heap metrics (Rank-2 snapshot; byte totals include 32B block metadata) */
    uint64_t heap_used_bytes;      /* live allocated bytes (payload + boundary tags) */
    uint64_t heap_free_bytes;      /* reusable free bytes within committed capacity */
    uint64_t heap_committed_bytes; /* total committed virtual backing bytes */
    uint64_t heap_largest_payload; /* largest existing free-block payload (excluding 32B block tags) */
    uint64_t heap_free_blocks;     /* count of free blocks in explicit free list */

    /* VMM metrics */
    uint64_t vmm_table_frames;     /* active physical frames allocated to page tables */
    uint64_t vmm_deferred_spaces;  /* address spaces queued for deferred destruction */
} sysinfo_mem_t;

_Static_assert(sizeof(sysinfo_mem_t) == 96, "sysinfo_mem_t ABI size");
_Static_assert(__builtin_offsetof(sysinfo_mem_t, struct_size) == 0, "sysinfo_mem_t.struct_size offset");
_Static_assert(__builtin_offsetof(sysinfo_mem_t, flags) == 4, "sysinfo_mem_t.flags offset");
_Static_assert(__builtin_offsetof(sysinfo_mem_t, pmm_total_frames) == 8, "sysinfo_mem_t.pmm_total_frames offset");
_Static_assert(__builtin_offsetof(sysinfo_mem_t, pmm_used_frames) == 16, "sysinfo_mem_t.pmm_used_frames offset");
_Static_assert(__builtin_offsetof(sysinfo_mem_t, pmm_free_frames) == 24, "sysinfo_mem_t.pmm_free_frames offset");
_Static_assert(__builtin_offsetof(sysinfo_mem_t, pmm_allocatable_frames) == 32, "sysinfo_mem_t.pmm_allocatable_frames offset");
_Static_assert(__builtin_offsetof(sysinfo_mem_t, heap_used_bytes) == 40, "sysinfo_mem_t.heap_used_bytes offset");
_Static_assert(__builtin_offsetof(sysinfo_mem_t, heap_free_bytes) == 48, "sysinfo_mem_t.heap_free_bytes offset");
_Static_assert(__builtin_offsetof(sysinfo_mem_t, heap_committed_bytes) == 56, "sysinfo_mem_t.heap_committed_bytes offset");
_Static_assert(__builtin_offsetof(sysinfo_mem_t, heap_largest_payload) == 64, "sysinfo_mem_t.heap_largest_payload offset");
_Static_assert(__builtin_offsetof(sysinfo_mem_t, heap_free_blocks) == 72, "sysinfo_mem_t.heap_free_blocks offset");
_Static_assert(__builtin_offsetof(sysinfo_mem_t, vmm_table_frames) == 80, "sysinfo_mem_t.vmm_table_frames offset");
_Static_assert(__builtin_offsetof(sysinfo_mem_t, vmm_deferred_spaces) == 88, "sysinfo_mem_t.vmm_deferred_spaces offset");

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
#define SYSCALL_EACCES        (-32)
#define SYSCALL_ELOOP         (-33)

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
