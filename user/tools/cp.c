#include "common.h"

static const char *get_basename(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    return base;
}

static bool make_target_path(char *out, size_t out_cap, const char *dir, const char *src) {
    const char *base = get_basename(src);
    size_t dlen = tool_length(dir);
    size_t blen = tool_length(base);
    bool need_slash = (dlen > 0 && dir[dlen - 1] != '/');
    if (dlen + (need_slash ? 1 : 0) + blen + 1 > out_cap) return false;
    size_t pos = 0;
    for (size_t i = 0; i < dlen; i++) out[pos++] = dir[i];
    if (need_slash) out[pos++] = '/';
    for (size_t i = 0; i < blen; i++) out[pos++] = base[i];
    out[pos] = '\0';
    return true;
}

static void warn_preserving_permissions(const char *dst, long err) {
    (void)tool_syscall(SYS_WRITE, 2, (uintptr_t)"cp: preserving permissions for '", 32);
    (void)tool_syscall(SYS_WRITE, 2, (uintptr_t)dst, tool_length(dst));
    if (err == SYSCALL_EPERM || err == SYSCALL_EACCES) {
        (void)tool_syscall(SYS_WRITE, 2, (uintptr_t)"' failed: Permission denied\n", 28);
    } else if (err == SYSCALL_EROFS) {
        (void)tool_syscall(SYS_WRITE, 2, (uintptr_t)"' failed: Read-only filesystem\n", 31);
    } else {
        (void)tool_syscall(SYS_WRITE, 2, (uintptr_t)"' failed: Operation failed\n", 27);
    }
}

static int copy_single_file(const char *src, const char *dst) {
    if (tool_equal(src, dst)) {
        return tool_error("cp", "are the same file", src);
    }

    vfs_stat_t src_st;
    long r = tool_syscall(SYS_STAT, (uintptr_t)src, (uintptr_t)&src_st, 0);
    if (r < 0) {
        return tool_sys_error("cp", "cannot stat", src, r);
    }
    if (src_st.type == VFS_DIRECTORY) {
        return tool_error("cp", "omitting directory", src);
    }

    long sfd = tool_syscall(SYS_OPEN, (uintptr_t)src, VFS_O_RDONLY, 0);
    if (sfd < 0) {
        return tool_sys_error("cp", "cannot open source", src, sfd);
    }

    long dfd = tool_syscall(SYS_OPEN, (uintptr_t)dst, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, 0);
    if (dfd < 0) {
        tool_syscall(SYS_CLOSE, (uintptr_t)sfd, 0, 0);
        return tool_sys_error("cp", "cannot create regular file", dst, dfd);
    }

    int ret = 0;
    for (;;) {
        long n = tool_syscall(SYS_READ, (uintptr_t)sfd, (uintptr_t)tool_buffer, TOOL_BUFFER_SIZE);
        if (n < 0) {
            tool_error("cp", "error reading", src);
            ret = 1;
            break;
        }
        if (n == 0) break;

        size_t rem = (size_t)n;
        const unsigned char *p = tool_buffer;
        while (rem > 0) {
            long w = tool_syscall(SYS_WRITE, (uintptr_t)dfd, (uintptr_t)p, rem);
            if (w <= 0) {
                tool_sys_error("cp", "error writing", dst, w < 0 ? w : SYSCALL_EIO);
                ret = 1;
                break;
            }
            p += (size_t)w;
            rem -= (size_t)w;
        }
        if (ret) break;
    }

    tool_syscall(SYS_CLOSE, (uintptr_t)sfd, 0, 0);
    long c = tool_syscall(SYS_CLOSE, (uintptr_t)dfd, 0, 0);
    if (c < 0 && ret == 0) {
        tool_sys_error("cp", "error closing", dst, c);
        ret = 1;
    }

    if (ret == 0 && (src_st.mode & 07777)) {
        long chr = tool_syscall(SYS_CHMOD, (uintptr_t)dst, src_st.mode & 07777, 0);
        if (chr < 0) {
            warn_preserving_permissions(dst, chr);
            ret = 1;
        }
    }
    return ret;
}

int cp_main(int argc, char **argv) {
    if (argc == 2 && tool_equal(argv[1], "--help")) {
        const char *s = "Usage: cp SOURCE DEST\n  or:  cp SOURCE... DIRECTORY\n";
        return tool_write("cp", s, tool_length(s));
    }
    if (argc < 3) {
        return tool_error("cp", "missing file operand", NULL);
    }

    const char *target = argv[argc - 1];
    vfs_stat_t target_st;
    long tr = tool_syscall(SYS_STAT, (uintptr_t)target, (uintptr_t)&target_st, 0);
    bool target_is_dir = (tr == 0 && target_st.type == VFS_DIRECTORY);

    if (argc > 3 && !target_is_dir) {
        return tool_error("cp", "target is not a directory", target);
    }

    int status = 0;
    if (target_is_dir) {
        char full_dst[VFS_MAX_PATH];
        for (int i = 1; i < argc - 1; i++) {
            if (!make_target_path(full_dst, sizeof(full_dst), target, argv[i])) {
                status = tool_error("cp", "path too long", argv[i]);
                continue;
            }
            int r = copy_single_file(argv[i], full_dst);
            if (r) status = r;
        }
    } else {
        status = copy_single_file(argv[1], target);
    }
    return status;
}
