#include "common.h"
#include "syscall_abi.h"

static void write_fd(int fd, const char *s) {
    if (!s) return;
    size_t len = tool_length(s);
    if (len > 0) {
        tool_syscall(SYS_WRITE, (uintptr_t)fd, (uintptr_t)s, len);
    }
}

size_t time_format_default(char *buf, size_t max, int64_t ns) {
    if (!buf || max == 0) return 0;
    if (ns < 0) ns = 0;

    uint64_t total_sec = (uint64_t)ns / 1000000000ULL;
    uint64_t rem_ns = (uint64_t)ns % 1000000000ULL;
    uint64_t ms = rem_ns / 1000000ULL;

    uint64_t minutes = total_sec / 60;
    uint64_t seconds = total_sec % 60;

    size_t pos = 0;
    char num_tmp[32];

    /* <minutes>m */
    size_t m_len = tool_format_u64(num_tmp, minutes);
    for (size_t i = 0; i < m_len && pos + 1 < max; i++) buf[pos++] = num_tmp[i];
    if (pos + 1 < max) buf[pos++] = 'm';

    /* <seconds>. */
    size_t s_len = tool_format_u64(num_tmp, seconds);
    for (size_t i = 0; i < s_len && pos + 1 < max; i++) buf[pos++] = num_tmp[i];
    if (pos + 1 < max) buf[pos++] = '.';

    /* <ms_3digits>s */
    if (pos + 4 < max) {
        buf[pos++] = (char)('0' + ((ms / 100) % 10));
        buf[pos++] = (char)('0' + ((ms / 10) % 10));
        buf[pos++] = (char)('0' + (ms % 10));
        buf[pos++] = 's';
    }
    buf[pos] = '\0';
    return pos;
}

size_t time_format_posix(char *buf, size_t max, int64_t ns) {
    if (!buf || max == 0) return 0;
    if (ns < 0) ns = 0;

    uint64_t total_csec = (uint64_t)ns / 10000000ULL;
    uint64_t sec = total_csec / 100;
    uint64_t csec = total_csec % 100;

    size_t pos = 0;
    char num_tmp[32];

    /* <sec>. */
    size_t s_len = tool_format_u64(num_tmp, sec);
    for (size_t i = 0; i < s_len && pos + 1 < max; i++) buf[pos++] = num_tmp[i];
    if (pos + 1 < max) buf[pos++] = '.';

    /* <csec_2digits> */
    if (pos + 2 < max) {
        buf[pos++] = (char)('0' + ((csec / 10) % 10));
        buf[pos++] = (char)('0' + (csec % 10));
    }
    buf[pos] = '\0';
    return pos;
}

static void print_help(void) {
    write_fd(1, "Usage: time [-p] command [arg...]\n"
                "Run command and summarize CPU and wall time usage.\n\n"
                "Options:\n"
                "  -p          Use the POSIX output format: 'real %f\\nuser %f\\nsys %f\\n'\n"
                "  -h, --help  Display this help and exit\n");
}

int time_main(int argc, char **argv, char **envp) {
    bool posix_format = false;
    int cmd_idx = 1;

    while (cmd_idx < argc) {
        const char *arg = argv[cmd_idx];
        if (tool_equal(arg, "-p")) {
            posix_format = true;
            cmd_idx++;
        } else if (tool_equal(arg, "-h") || tool_equal(arg, "--help")) {
            print_help();
            return 0;
        } else if (tool_equal(arg, "--")) {
            cmd_idx++;
            break;
        } else if (arg[0] == '-' && arg[1] != '\0') {
            write_fd(2, "time: unrecognized option '");
            write_fd(2, arg);
            write_fd(2, "'\nTry 'time --help' for more information.\n");
            return 2;
        } else {
            break;
        }
    }

    if (cmd_idx >= argc) {
        write_fd(2, "time: missing command\n"
                    "Try 'time --help' for more information.\n");
        return 2;
    }

    const char *cmd = argv[cmd_idx];
    char cmd_path[VFS_MAX_PATH];
    if (cmd[0] == '/') {
        size_t len = tool_length(cmd);
        if (len >= sizeof(cmd_path)) len = sizeof(cmd_path) - 1;
        for (size_t i = 0; i < len; i++) cmd_path[i] = cmd[i];
        cmd_path[len] = '\0';
    } else {
        /* Search /bin/ then /sbin/ */
        char probe[VFS_MAX_PATH];
        size_t clen = tool_length(cmd);
        const char *pbin = "/bin/";
        size_t blen = 5;
        for (size_t i = 0; i < blen; i++) probe[i] = pbin[i];
        for (size_t i = 0; i <= clen && (blen + i) < sizeof(probe); i++) probe[blen + i] = cmd[i];

        vfs_stat_t st;
        if (tool_syscall(SYS_STAT, (uintptr_t)probe, (uintptr_t)&st, 0) == 0 && st.type == VFS_FILE) {
            for (size_t i = 0; probe[i] && i + 1 < sizeof(cmd_path); i++) cmd_path[i] = probe[i];
            cmd_path[tool_length(probe)] = '\0';
        } else {
            const char *psbin = "/sbin/";
            size_t sblen = 6;
            for (size_t i = 0; i < sblen; i++) probe[i] = psbin[i];
            for (size_t i = 0; i <= clen && (sblen + i) < sizeof(probe); i++) probe[sblen + i] = cmd[i];

            if (tool_syscall(SYS_STAT, (uintptr_t)probe, (uintptr_t)&st, 0) == 0 && st.type == VFS_FILE) {
                for (size_t i = 0; probe[i] && i + 1 < sizeof(cmd_path); i++) cmd_path[i] = probe[i];
                cmd_path[tool_length(probe)] = '\0';
            } else {
                /* Default to /bin/<cmd> */
                for (size_t i = 0; i < blen; i++) cmd_path[i] = pbin[i];
                for (size_t i = 0; i <= clen && (blen + i) < sizeof(cmd_path); i++) cmd_path[blen + i] = cmd[i];
            }
        }
    }

    timespec_t ts_start = {0};
    timespec_t ts_end = {0};
    tool_syscall(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (uintptr_t)&ts_start, 0);

    spawn_opts_t opts = {
        .size = sizeof(opts),
        .version = 1,
        .flags = 0,
        .reserved0 = 0,
        .argv = (uintptr_t)&argv[cmd_idx],
        .envp = (uintptr_t)envp,
        .cwd = 0,
        .fd_actions = 0,
        .action_count = 0,
        .reserved1 = 0,
        .reserved2 = 0
    };

    long child_pid = tool_syscall(SYS_SPAWN_EXT, (uintptr_t)cmd_path, (uintptr_t)&opts, sizeof(opts));
    if (child_pid < 0) {
        tool_sys_error("time", "cannot run command", cmd, child_pid);
        return (child_pid == SYSCALL_ENOENT) ? 127 : 126;
    }

    uint64_t wait_status = 0;
    /* Wait for child to exit without reaping yet (WNOWAIT) */
    tool_syscall(SYS_WAITPID, (uintptr_t)child_pid, (uintptr_t)&wait_status, WNOWAIT);

    tool_syscall(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (uintptr_t)&ts_end, 0);

    tms_t tms = {0};
    tool_syscall(SYS_TIMES, (uintptr_t)child_pid, (uintptr_t)&tms, 0);

    /* Reap the zombie child */
    tool_syscall(SYS_WAITPID, (uintptr_t)child_pid, (uintptr_t)&wait_status, 0);

    int64_t real_ns = (ts_end.tv_sec - ts_start.tv_sec) * 1000000000LL + (ts_end.tv_nsec - ts_start.tv_nsec);
    if (real_ns < 0) real_ns = 0;
    int64_t user_ns = tms.tms_utime_ns;
    if (user_ns < 0) user_ns = 0;
    int64_t sys_ns = tms.tms_stime_ns;
    if (sys_ns < 0) sys_ns = 0;

    char r_buf[64], u_buf[64], s_buf[64];
    if (posix_format) {
        time_format_posix(r_buf, sizeof(r_buf), real_ns);
        time_format_posix(u_buf, sizeof(u_buf), user_ns);
        time_format_posix(s_buf, sizeof(s_buf), sys_ns);

        write_fd(2, "real ");
        write_fd(2, r_buf);
        write_fd(2, "\nuser ");
        write_fd(2, u_buf);
        write_fd(2, "\nsys ");
        write_fd(2, s_buf);
        write_fd(2, "\n");
    } else {
        time_format_default(r_buf, sizeof(r_buf), real_ns);
        time_format_default(u_buf, sizeof(u_buf), user_ns);
        time_format_default(s_buf, sizeof(s_buf), sys_ns);

        write_fd(2, "real\t");
        write_fd(2, r_buf);
        write_fd(2, "\nuser\t");
        write_fd(2, u_buf);
        write_fd(2, "\nsys\t");
        write_fd(2, s_buf);
        write_fd(2, "\n");
    }

    int exit_code = 0;
    if (WIFEXITED(wait_status)) {
        exit_code = WEXITSTATUS(wait_status);
    } else if ((wait_status & 0x7f) != 0) {
        exit_code = 128 + (int)(wait_status & 0x7f);
    }
    return exit_code;
}
