#include "common.h"
#include "userdb.h"
#include "terminal.h"
#include "../permissions_cli.h"
#include "../entry_security.h"

static userdb_t database;

static bool password_line(int tty, char out[DB_PASSWORD_MAX + 1]) {
    unsigned n = 0; bool bad = false;
    for (;;) {
        unsigned char c; long r = tool_syscall(SYS_READ, tty, (uintptr_t)&c, 1);
        if (r != 1) { db_wipe(out, DB_PASSWORD_MAX + 1); return false; }
        if (c == '\n' || c == '\r') { out[n] = 0; return !bad; }
        if (c == 8 || c == 127) { if (n) out[--n] = 0; continue; }
        if (c < 32 || c > 126 || n == DB_PASSWORD_MAX) { bad = true; continue; }
        out[n++] = (char)c;
    }
}

static void env_str(char *out, const char *key, const char *val) {
    while (*key) *out++ = *key++;
    while (*val) *out++ = *val++;
    *out = 0;
}

int su_main(int argc, char **argv, const char *const *envp) {
    uint32_t uid, euid, suid;
    bool login_shell = false;
    const char *cmd = NULL;
    const char *target_user = "root";

    for (int i = 1; i < argc; i++) {
        if (tool_equal(argv[i], "-") || tool_equal(argv[i], "-l") || tool_equal(argv[i], "--login")) {
            login_shell = true;
        } else if (tool_equal(argv[i], "-c")) {
            if (i + 1 < argc) {
                cmd = argv[++i];
            } else {
                return tool_error("su", "option requires an argument -- 'c'", NULL);
            }
        } else if (argv[i][0] != '-') {
            target_user = argv[i];
        } else {
            return tool_error("su", "usage: su [-] [-l] [-c command] [username]", NULL);
        }
    }

    if (tool_syscall(SYS_GETRESUID, (uintptr_t)&uid, (uintptr_t)&euid, (uintptr_t)&suid) < 0 ||
        euid != 0 || (uid && !user_entry_secure(envp)))
        return tool_error("su", "privileged installation required", NULL);

    bool loaded = false;
    for (int attempts = 0; attempts < 20; attempts++) {
        if (db_load(&database, true)) { loaded = true; break; }
        if (!db_busy()) break;
        timespec_t req = { .tv_sec = 0, .tv_nsec = 25000000L };
        (void)tool_syscall(SYS_NANOSLEEP, (uintptr_t)&req, 0, 0);
    }
    if (!loaded)
        return tool_error("su", "privileged installation required", NULL);

    const db_user_t *target = db_user_name(&database, target_user);
    if (!target) {
        db_wipe(&database, sizeof(database));
        return tool_error("su", "user does not exist", NULL);
    }

    const db_shadow_t *shadow = db_shadow_name(&database, target->name);

    if (uid != 0) {
        if (!shadow || shadow->restricted) {
            db_wipe(&database, sizeof(database));
            return tool_error("su", "account is locked", NULL);
        }

        if (*shadow->hash != '\0') {
            int tty = tool_syscall(SYS_OPEN, (uintptr_t)"/dev/tty", VFS_O_RDWR, 0);
            if (tty < 0 || tool_syscall(SYS_TERMCTL, TERM_ISATTY, tty, 0) != 1) {
                if (tty >= 0) tool_syscall(SYS_CLOSE, tty, 0, 0);
                db_wipe(&database, sizeof(database));
                return tool_error("su", "controlling terminal required", NULL);
            }

            terminal_attrs_t saved, hidden;
            bool ok = false;
            char password[DB_PASSWORD_MAX + 1] = {0};
            if (permission_call4(SYS_TERMATTR, tty, TERM_GET, (uintptr_t)&saved, sizeof(saved)) == 0) {
                hidden = saved;
                hidden.input_flags &= ~TERM_ECHO;
                if (permission_call4(SYS_TERMATTR, tty, TERM_SET, (uintptr_t)&hidden, sizeof(hidden)) == 0) {
                    const char *prompt = "Password: ";
                    tool_syscall(SYS_WRITE, tty, (uintptr_t)prompt, tool_length(prompt));
                    ok = password_line(tty, password) && db_verify(password, shadow->hash);
                    (void)permission_call4(SYS_TERMATTR, tty, TERM_SET, (uintptr_t)&saved, sizeof(saved));
                    tool_syscall(SYS_WRITE, tty, (uintptr_t)"\n", 1);
                }
            }
            db_wipe(password, sizeof(password));
            tool_syscall(SYS_CLOSE, tty, 0, 0);
            if (!ok) {
                db_wipe(&database, sizeof(database));
                return tool_error("su", "Authentication failure", NULL);
            }
        }
    }

    if (!db_check()) {
        db_wipe(&database, sizeof(database));
        return tool_error("su", "accounts changed; retry command", NULL);
    }

    /* If home directory is /run/user/<uid>, create it if not present */
    if (target->uid != 0 && target->home[0] == '/' && target->home[1] == 'r' &&
        target->home[2] == 'u' && target->home[3] == 'n' && target->home[4] == '/' &&
        target->home[5] == 'u' && target->home[6] == 's' && target->home[7] == 'e' &&
        target->home[8] == 'r' && target->home[9] == '/') {
        stat_ext_v1_t st;
        long r = permission_call4(SYS_STAT_EXT, (uintptr_t)target->home, (uintptr_t)&st, sizeof(st), 1);
        if (r == SYSCALL_ENOENT) {
            if (tool_syscall(SYS_MKDIR, (uintptr_t)target->home, 0700, 0) == 0) {
                (void)permission_call4(SYS_CHOWN, (uintptr_t)target->home, target->uid, target->gid, 0);
                (void)tool_syscall(SYS_CHMOD, (uintptr_t)target->home, 0700, 0);
            }
        }
    }

    uint32_t groups[16]; unsigned count;
    if (!db_groups(&database, target, groups, &count) ||
        permission_call4(SYS_SETRESGID, target->gid, target->gid, target->gid, 0) < 0 ||
        tool_syscall(SYS_SETGROUPS, count, (uintptr_t)groups, 0) < 0 ||
        permission_call4(SYS_SETRESUID, target->uid, target->uid, target->uid, 0) < 0) {
        db_wipe(&database, sizeof(database));
        return tool_error("su", "credential transition failed", NULL);
    }

    db_wipe(&database, sizeof(database));

    char env_home[144], env_user[48], env_logname[48], env_shell[144];
    env_str(env_home, "HOME=", target->home);
    env_str(env_user, "USER=", target->name);
    env_str(env_logname, "LOGNAME=", target->name);
    const char *shell_path = (target->shell[0]) ? target->shell : "/bin/shell";
    env_str(env_shell, "SHELL=", shell_path);

    const char *environment[] = {
        env_home, env_user, env_logname, env_shell,
        "PATH=/bin:/sbin", "TERM=fortress", NULL
    };

    const char *args[5];
    int arg_idx = 0;
    args[arg_idx++] = shell_path;
    if (cmd) {
        args[arg_idx++] = "-c";
        args[arg_idx++] = cmd;
    }
    args[arg_idx] = NULL;

    spawn_opts_t opts = {
        .size = sizeof(opts),
        .version = 1,
        .argv = (uintptr_t)args,
        .envp = (uintptr_t)environment,
        .cwd = login_shell ? (uintptr_t)target->home : 0
    };

    long pid = tool_syscall(SYS_SPAWN_EXT, (uintptr_t)shell_path, (uintptr_t)&opts, sizeof(opts));
    if (pid < 0) return tool_error("su", "cannot start shell", NULL);

    uint64_t status = 0;
    long r = tool_syscall(SYS_WAIT, pid, (uintptr_t)&status, 0);
    return r < 0 ? 1 : (int)status;
}
