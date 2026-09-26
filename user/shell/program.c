#include "program.h"
#include "vars.h"

long program_launch(const char *path, const char *const *argv,
                    const char *const *envp, const spawn_fd_action_t *actions,
                    uint32_t action_count) {
    spawn_opts_t opts = {0};
    opts.size = sizeof(opts);
    opts.version = 1;
    opts.argv = (uintptr_t)argv;
    opts.envp = (uintptr_t)envp;
    opts.fd_actions = action_count ? (uintptr_t)actions : 0;
    opts.action_count = action_count;
    return call(SYS_SPAWN_EXT, (uintptr_t)path, (uintptr_t)&opts, sizeof(opts));
}

int program_error(long error) {
    switch (error) {
        case SYSCALL_ENOENT: puts_err("No such file or directory.\n"); return 127;
        case SYSCALL_ENOEXEC: puts_err("Invalid executable.\n"); return 126;
        case SYSCALL_ENOMEM: puts_err("Out of memory or process capacity.\n"); return 1;
        case SYSCALL_EISDIR: puts_err("Not a regular file.\n"); return 126;
        case SYSCALL_EFBIG: puts_err("Executable exceeds 4 MiB limit.\n"); return 126;
        case SYSCALL_E2BIG: puts_err("Argument list too long.\n"); return 1;
        case SYSCALL_EBADF: puts_err("Bad file descriptor in redirection.\n"); return 1;
        case SYSCALL_EINVAL: puts_err("Invalid redirection or spawn arguments.\n"); return 1;
        case SYSCALL_EROFS: puts_err("Read-only filesystem.\n"); return 1;
        case SYSCALL_EIO: puts_err("I/O error.\n"); return 1;
        case SYSCALL_EMFILE: puts_err("Too many open files.\n"); return 1;
        default: puts_err("Unable to load executable.\n"); return 1;
    }
}

int program_wait(long pid, int64_t *status) {
    if (call(SYS_WAIT, pid, (uintptr_t)status, 0) < 0) {
        puts_err("Unable to wait for child process.\n");
        return 1;
    }
    return 0;
}

int program_resolve(const char *name, char path[VFS_MAX_PATH]) {
    size_t len = length(name);
    if (len >= VFS_MAX_PATH) return 126;
    for (size_t i = 0; i < len; i++) {
        if (name[i] == '/') {
            for (size_t j = 0; j <= len; j++) path[j] = name[j];
            return 0;
        }
    }
    const char *p = vars_get("PATH");
    if (!p || !*p) p = "/bin";
    while (*p) {
        size_t n = 0;
        while (p[n] && p[n] != ':') n++;
        if (n + len + 2 < VFS_MAX_PATH) {
            for (size_t j = 0; j < n; j++) path[j] = p[j];
            path[n] = '/';
            for (size_t j = 0; j <= len; j++) path[n + 1 + j] = name[j];
            vfs_stat_t st;
            if (call(SYS_STAT, (uintptr_t)path, (uintptr_t)&st, 0) == 0 &&
                st.type == VFS_FILE) return 0;
        }
        p += n;
        if (*p == ':') p++;
    }
    return 127;
}
