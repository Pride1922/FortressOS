#include "boot_snapshot.h"
#include "common.h"
#include "../permissions_cli.h"
#include "vfs.h"

static char s_dmesg_buf[DMESG_SIZE];

static int check_dir(const char *dir) {
    stat_ext_v1_t st;
    long r = permission_call4(SYS_STAT_EXT, (uintptr_t)dir, (uintptr_t)&st, sizeof(st), 1);
    if (r != 0) {
        if (r == SYSCALL_ENOENT || r == SYSCALL_EROFS) {
            return 1; /* Missing or read-only; caller may fall back */
        }
        if (r == SYSCALL_EACCES || r == SYSCALL_EPERM) {
            tool_error("boot-snapshot", "permission denied accessing directory", dir);
            return -1; /* Fail closed */
        }
        tool_sys_error("boot-snapshot", "cannot access directory", dir, r);
        return -1;
    }
    if (st.type != VFS_DIRECTORY && (st.mode & VFS_S_IFMT) != VFS_S_IFDIR) {
        tool_error("boot-snapshot", "log destination path is not a directory", dir);
        return -1;
    }
    if (st.uid != 0) {
        tool_error("boot-snapshot", "insecure log directory (not owned by root)", dir);
        return -1;
    }
    if (st.mode & 0002) {
        tool_error("boot-snapshot", "insecure log directory (world-writable)", dir);
        return -1;
    }
    return 0; /* OK */
}

static int check_file(const char *file) {
    vfs_stat_t lst;
    long r = tool_syscall(SYS_LSTAT, (uintptr_t)file, (uintptr_t)&lst, 0);
    if (r == 0) {
        if ((lst.mode & VFS_S_IFMT) == VFS_S_IFLNK) {
            tool_error("boot-snapshot", "insecure log file (symlink rejected)", file);
            return -1;
        }
    }
    stat_ext_v1_t fst;
    r = permission_call4(SYS_STAT_EXT, (uintptr_t)file, (uintptr_t)&fst, sizeof(fst), 1);
    if (r == 0) {
        if (fst.uid != 0) {
            tool_error("boot-snapshot", "insecure log file (not owned by root)", file);
            return -1;
        }
        if (fst.mode & 0002) {
            tool_error("boot-snapshot", "insecure log file (world-writable)", file);
            return -1;
        }
    } else if (r != SYSCALL_ENOENT) {
        if (r == SYSCALL_EACCES || r == SYSCALL_EPERM) {
            tool_error("boot-snapshot", "permission denied accessing log file", file);
            return -1;
        }
    }
    return 0;
}

static int do_snapshot(const char *active_path, const char *prev_path, bool is_fallback,
                       const char *dmesg_data, size_t dmesg_len) {
    /* 1. Pre-rotate active to prev if active exists */
    vfs_stat_t ast;
    long r_stat = tool_syscall(SYS_LSTAT, (uintptr_t)active_path, (uintptr_t)&ast, 0);
    if (r_stat == 0) {
        /* Active exists. If prev exists, unlink it first */
        vfs_stat_t pst;
        if (tool_syscall(SYS_LSTAT, (uintptr_t)prev_path, (uintptr_t)&pst, 0) == 0) {
            long uerr = tool_syscall(SYS_UNLINK, (uintptr_t)prev_path, 0, 0);
            if (uerr != 0 && uerr != SYSCALL_ENOENT) {
                if (!is_fallback && uerr == SYSCALL_EROFS) {
                    return 2; /* Signal ROFS fallback */
                }
                tool_sys_error("boot-snapshot", "failed to unlink previous log", prev_path, uerr);
                return -1;
            }
        }
        /* Rename active to prev */
        long r_ren = tool_syscall(SYS_RENAME, (uintptr_t)active_path, (uintptr_t)prev_path, 0);
        if (r_ren != 0) {
            if (!is_fallback && r_ren == SYSCALL_EROFS) {
                return 2; /* Signal ROFS fallback */
            }
            tool_sys_error("boot-snapshot", "failed to rotate active log to prev", active_path, r_ren);
            return -1;
        }
    } else if (r_stat != SYSCALL_ENOENT) {
        if (!is_fallback && r_stat == SYSCALL_EROFS) {
            return 2;
        }
        tool_sys_error("boot-snapshot", "cannot stat active log", active_path, r_stat);
        return -1;
    }

    /* 2. Open new active file with VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC */
    long fd = tool_syscall(SYS_OPEN, (uintptr_t)active_path,
                           VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC, 0);
    if (fd < 0) {
        if (!is_fallback && (fd == SYSCALL_EROFS || fd == SYSCALL_ENOENT)) {
            return 2; /* Signal ROFS fallback */
        }
        tool_sys_error("boot-snapshot", "cannot create boot log file", active_path, fd);
        return -1;
    }

    tool_syscall(SYS_FCHMOD, fd, 0640, 0);
    permission_call4(SYS_CHOWN, (uintptr_t)active_path, 0, 10, 0);

    /* 3. Write dmesg contents in bounded chunks (max 4096) */
    size_t written = 0;
    while (written < dmesg_len) {
        size_t chunk = dmesg_len - written;
        if (chunk > 4096) chunk = 4096;
        long w = tool_syscall(SYS_WRITE, fd, (uintptr_t)(dmesg_data + written), chunk);
        if (w < 0) {
            /* If writing to runfs and capacity reached, clamp gracefully */
            if (is_fallback && (w == SYSCALL_ENOSPC || w == -VFS_ENOSPC || w == -VFS_EFBIG)) {
                break;
            }
            tool_sys_error("boot-snapshot", "cannot write to boot log", active_path, w);
            tool_syscall(SYS_CLOSE, fd, 0, 0);
            return -1;
        }
        if (w == 0) break;
        written += (size_t)w;
    }

    tool_syscall(SYS_CLOSE, fd, 0, 0);
    tool_syscall(SYS_SYNC, 0, 0, 0);

    /* Print diagnostic notice */
    tool_write("boot-snapshot", "boot-snapshot: saved ", 21);
    char num_buf[32];
    size_t nlen = tool_format_u64(num_buf, (uint64_t)written);
    tool_write("boot-snapshot", num_buf, nlen);
    tool_write("boot-snapshot", " bytes to ", 10);
    tool_write("boot-snapshot", active_path, tool_length(active_path));
    tool_write("boot-snapshot", "\n", 1);

    return 0;
}

int boot_snapshot_main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (tool_equal(arg, "-h") || tool_equal(arg, "--help")) {
            tool_write("boot-snapshot", "Usage: boot-snapshot [-h|--help]\n"
                       "Takes a snapshot of the kernel boot log into /var/log/boot.log (or /run/boot.log).\n", 114);
            return 0;
        }
        if (arg[0] == '-') {
            tool_error("boot-snapshot", "unrecognized option", arg);
            return 1;
        }
    }

    long n = tool_syscall(SYS_DMESG, (uintptr_t)s_dmesg_buf, sizeof(s_dmesg_buf), 0);
    if (n < 0) {
        if (n == SYSCALL_EPERM || n == SYSCALL_EACCES) {
            tool_error("boot-snapshot", "permission denied; root access required (use sudo)", NULL);
        } else {
            tool_sys_error("boot-snapshot", "kernel log unavailable", NULL, n);
        }
        return 1;
    }
    size_t dmesg_len = (size_t)n;

    /* Step 1: Attempt primary destination /var/log/boot.log */
    int dir_status = check_dir(BOOT_SNAPSHOT_DIR_PRIMARY);
    bool use_fallback = false;

    if (dir_status == 0) {
        if (check_file(BOOT_SNAPSHOT_FILE_PRIMARY) == 0 &&
            check_file(BOOT_SNAPSHOT_PREV_PRIMARY) == 0) {
            int r = do_snapshot(BOOT_SNAPSHOT_FILE_PRIMARY, BOOT_SNAPSHOT_PREV_PRIMARY,
                                false, s_dmesg_buf, dmesg_len);
            if (r == 0) {
                return 0;
            }
            if (r == 2) {
                use_fallback = true;
            } else {
                return 1;
            }
        } else {
            return 1;
        }
    } else if (dir_status == 1) {
        use_fallback = true;
    } else {
        return 1;
    }

    /* Step 2: Fallback to /run/boot.log */
    if (use_fallback) {
        if (check_dir(BOOT_SNAPSHOT_DIR_FALLBACK) != 0) {
            return 1;
        }
        if (check_file(BOOT_SNAPSHOT_FILE_FALLBACK) != 0 ||
            check_file(BOOT_SNAPSHOT_PREV_FALLBACK) != 0) {
            return 1;
        }
        int r = do_snapshot(BOOT_SNAPSHOT_FILE_FALLBACK, BOOT_SNAPSHOT_PREV_FALLBACK,
                            true, s_dmesg_buf, dmesg_len);
        return (r == 0) ? 0 : 1;
    }

    return 0;
}
