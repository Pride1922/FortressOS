#ifndef FORTRESS_SYSCALL_ABI_H
#define FORTRESS_SYSCALL_ABI_H
#include "types.h"
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
    uint32_t version;       /* 1 */
    uint32_t flags;         /* 0 */
    uint32_t reserved0;     /* 0 */
    uint64_t argv;          /* pointer to argv NULL-terminated array of char* */
    uint64_t envp;          /* pointer to envp NULL-terminated array of char* */
    uint64_t cwd;           /* pointer to cwd string (optional, 0 for inherit) */
    uint64_t fd_actions;    /* pointer to spawn_fd_action_t array (or 0) */
    uint32_t action_count;  /* count of fd actions (up to MAX_SPAWN_ACTIONS) */
    uint32_t reserved1;     /* 0 */
    uint64_t reserved2;     /* 0 */
} spawn_opts_t;

_Static_assert(sizeof(spawn_opts_t) == 64, "spawn_opts_t must be exactly 64 bytes");

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
