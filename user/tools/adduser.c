#include "account_tool.h"

static bool append_str(char *out, size_t cap, size_t *n, const char *s) {
    while (*s) {
        if (*n + 1 >= cap) return false;
        out[(*n)++] = *s++;
    }
    out[*n] = 0;
    return true;
}

static bool prompt_line(const char *prompt, char *out, size_t cap) {
    tool_write("adduser", prompt, tool_length(prompt));
    char buf[129];
    if (!account_line(buf)) return false;
    size_t len = tool_length(buf);
    if (len >= cap) return false;
    for (size_t i = 0; i <= len; i++) out[i] = buf[i];
    return true;
}

static bool copy_skel_file(const char *src_path, const char *dst_path, uint32_t uid, uint32_t gid) {
    long in_fd = tool_syscall(SYS_OPEN, (uintptr_t)src_path, VFS_O_RDONLY, 0);
    if (in_fd < 0) return true; /* Skel file optional */

    long out_fd = tool_syscall(SYS_OPEN, (uintptr_t)dst_path, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC, 0600);
    if (out_fd < 0) {
        tool_syscall(SYS_CLOSE, in_fd, 0, 0);
        return false;
    }

    char buf[512];
    bool ok = true;
    for (;;) {
        long r = tool_syscall(SYS_READ, in_fd, (uintptr_t)buf, sizeof(buf));
        if (r < 0) { ok = false; break; }
        if (r == 0) break;
        long w = tool_syscall(SYS_WRITE, out_fd, (uintptr_t)buf, (size_t)r);
        if (w != r) { ok = false; break; }
    }

    tool_syscall(SYS_CLOSE, in_fd, 0, 0);
    tool_syscall(SYS_CLOSE, out_fd, 0, 0);

    if (ok) {
        (void)permission_call4(SYS_CHOWN, (uintptr_t)dst_path, uid, gid, 0);
        (void)tool_syscall(SYS_CHMOD, (uintptr_t)dst_path, 0600, 0);
    } else {
        (void)tool_syscall(SYS_UNLINK, (uintptr_t)dst_path, 0, 0);
    }
    return ok;
}

static bool provision_home(const char *home, uint32_t uid, uint32_t gid) {
    if (tool_syscall(SYS_MKDIR, (uintptr_t)home, 0700, 0) < 0) return false;
    if (permission_call4(SYS_CHOWN, (uintptr_t)home, uid, gid, 0) < 0) {
        (void)tool_syscall(SYS_UNLINK, (uintptr_t)home, 0, 0);
        return false;
    }
    if (tool_syscall(SYS_CHMOD, (uintptr_t)home, 0700, 0) < 0) {
        (void)tool_syscall(SYS_UNLINK, (uintptr_t)home, 0, 0);
        return false;
    }

    /* Provision standard skeleton files */
    char src[128], dst[128];
    const char *skel_files[] = { ".profile", ".shellrc", NULL };
    for (int i = 0; skel_files[i]; i++) {
        size_t n = 0;
        src[0] = 0; dst[0] = 0;
        append_str(src, sizeof(src), &n, "/etc/skel/");
        append_str(src, sizeof(src), &n, skel_files[i]);
        n = 0;
        append_str(dst, sizeof(dst), &n, home);
        append_str(dst, sizeof(dst), &n, "/");
        append_str(dst, sizeof(dst), &n, skel_files[i]);
        if (!copy_skel_file(src, dst, uid, gid)) {
            /* Cleanup on failure */
            for (int j = 0; j <= i; j++) {
                n = 0; dst[0] = 0;
                append_str(dst, sizeof(dst), &n, home);
                append_str(dst, sizeof(dst), &n, "/");
                append_str(dst, sizeof(dst), &n, skel_files[j]);
                (void)tool_syscall(SYS_UNLINK, (uintptr_t)dst, 0, 0);
            }
            (void)tool_syscall(SYS_UNLINK, (uintptr_t)home, 0, 0);
            return false;
        }
    }
    return true;
}

int adduser_main(int argc, char **argv) {
    uint32_t uid, euid, suid;
    const char *username = NULL;
    const char *first_name = NULL;
    const char *last_name = NULL;
    const char *gecos_arg = NULL;
    const char *shell_path = "/bin/shell";
    bool disabled_password = false;
    bool add_to_wheel = false;
    bool interactive = false;
    int result = 1;

    if (tool_syscall(SYS_GETRESUID, (uintptr_t)&uid, (uintptr_t)&euid, (uintptr_t)&suid) || euid != 0) {
        return tool_error("adduser", "only root may add a user to the system", 0);
    }

    for (int i = 1; i < argc; i++) {
        if (tool_equal(argv[i], "--first-name")) {
            if (++i >= argc) return tool_error("adduser", "option '--first-name' requires an argument", 0);
            first_name = argv[i];
        } else if (tool_equal(argv[i], "--last-name")) {
            if (++i >= argc) return tool_error("adduser", "option '--last-name' requires an argument", 0);
            last_name = argv[i];
        } else if (tool_equal(argv[i], "--gecos")) {
            if (++i >= argc) return tool_error("adduser", "option '--gecos' requires an argument", 0);
            gecos_arg = argv[i];
        } else if (tool_equal(argv[i], "--shell")) {
            if (++i >= argc) return tool_error("adduser", "option '--shell' requires an argument", 0);
            shell_path = argv[i];
        } else if (tool_equal(argv[i], "--disabled-password")) {
            disabled_password = true;
        } else if (tool_equal(argv[i], "--wheel")) {
            add_to_wheel = true;
        } else if (argv[i][0] == '-') {
            return tool_error("adduser", "unknown option", 0);
        } else if (!username) {
            username = argv[i];
        } else {
            return tool_error("adduser", "unexpected operand", 0);
        }
    }

    if (!username) {
        return tool_error("adduser", "usage: adduser [OPTIONS] USERNAME", 0);
    }

    size_t name_len = tool_length(username);
    if (!name_len || name_len >= DB_NAME_MAX) {
        return tool_error("adduser", "username must be 1..31 characters", 0);
    }
    for (size_t i = 0; i < name_len; i++) {
        char c = username[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (i > 0 && c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return tool_error("adduser", "username contains invalid characters", 0);
        }
    }

    if (!account_snapshot()) {
        return tool_error("adduser", "cannot read account database or system is read-only", 0);
    }

    if (db_user_name(&account_db, username)) {
        account_clear();
        return tool_error("adduser", "the user already exists", 0);
    }
    if (db_group_name(&account_db, username)) {
        account_clear();
        return tool_error("adduser", "a group with that name already exists", 0);
    }
    if (account_db.nusers >= DB_MAX_RECORDS) {
        account_clear();
        return tool_error("adduser", "maximum user account limit reached (16)", 0);
    }
    if (account_db.ngroups >= DB_MAX_RECORDS) {
        account_clear();
        return tool_error("adduser", "maximum group limit reached (16)", 0);
    }

    interactive = (tool_syscall(SYS_TERMCTL, TERM_ISATTY, 0, 0) == 1);

    char gecos[DB_GECOS_MAX] = {0};
    if (gecos_arg) {
        if (tool_length(gecos_arg) >= sizeof(gecos)) {
            account_clear();
            return tool_error("adduser", "GECOS / full name too long (max 63 chars)", 0);
        }
        for (size_t i = 0; gecos_arg[i]; i++) gecos[i] = gecos_arg[i];
    } else if (first_name || last_name) {
        size_t gn = 0;
        if (first_name) append_str(gecos, sizeof(gecos), &gn, first_name);
        if (first_name && last_name) append_str(gecos, sizeof(gecos), &gn, " ");
        if (last_name) append_str(gecos, sizeof(gecos), &gn, last_name);
    } else if (interactive) {
        char fn[32] = {0}, ln[32] = {0};
        (void)prompt_line("First name: ", fn, sizeof(fn));
        (void)prompt_line("Last name: ", ln, sizeof(ln));
        size_t gn = 0;
        if (fn[0]) append_str(gecos, sizeof(gecos), &gn, fn);
        if (fn[0] && ln[0]) append_str(gecos, sizeof(gecos), &gn, " ");
        if (ln[0]) append_str(gecos, sizeof(gecos), &gn, ln);
    }

    /* Verify no colons in GECOS */
    for (size_t i = 0; gecos[i]; i++) {
        if (gecos[i] == ':') gecos[i] = ' ';
    }

    char hash[78] = {0};
    if (disabled_password) {
        hash[0] = '!'; hash[1] = 0;
    } else if (interactive) {
        if (!account_password(hash)) {
            account_clear();
            return tool_error("adduser", "password setup failed", 0);
        }
        char choice[16] = {0};
        (void)prompt_line("Add user to administrative group 'wheel'? (y/N): ", choice, sizeof(choice));
        if (choice[0] == 'y' || choice[0] == 'Y') {
            add_to_wheel = true;
        }
    } else {
        hash[0] = '!'; hash[1] = 0;
    }

    uint32_t new_uid = account_next_uid();
    uint32_t new_gid = account_next_gid();
    if (!new_uid || !new_gid) {
        account_clear();
        return tool_error("adduser", "cannot allocate UID/GID", 0);
    }

    char home[128] = "/home/";
    size_t hn = 6;
    if (!append_str(home, sizeof(home), &hn, username)) {
        account_clear();
        return tool_error("adduser", "home path too long", 0);
    }

    /* Acquire transaction lease */
    if (!account_begin()) {
        account_clear();
        return tool_error("adduser", "account database is busy; retry command", 0);
    }

    /* Step 2: Provision home directory and copy skel */
    if (!provision_home(home, new_uid, new_gid)) {
        account_clear();
        return tool_error("adduser", "failed to create home directory", 0);
    }

    /* Step 3: Format and append entries */
    char uid_str[12], gid_str[12], line[512];
    tool_format_u64(uid_str, new_uid);
    tool_format_u64(gid_str, new_gid);

    /* /etc/passwd: username:x:uid:gid:gecos:home:shell\n */
    size_t ln = 0;
    if (!append_str(line, sizeof(line), &ln, username) ||
        !append_str(line, sizeof(line), &ln, ":x:") ||
        !append_str(line, sizeof(line), &ln, uid_str) ||
        !append_str(line, sizeof(line), &ln, ":") ||
        !append_str(line, sizeof(line), &ln, gid_str) ||
        !append_str(line, sizeof(line), &ln, ":") ||
        !append_str(line, sizeof(line), &ln, gecos) ||
        !append_str(line, sizeof(line), &ln, ":") ||
        !append_str(line, sizeof(line), &ln, home) ||
        !append_str(line, sizeof(line), &ln, ":") ||
        !append_str(line, sizeof(line), &ln, shell_path) ||
        !append_str(line, sizeof(line), &ln, "\n") ||
        !account_append(0, line)) {
        account_clear();
        return tool_error("adduser", "failed to format passwd entry", 0);
    }

    /* /etc/group: username:x:gid:\n */
    ln = 0;
    if (!append_str(line, sizeof(line), &ln, username) ||
        !append_str(line, sizeof(line), &ln, ":x:") ||
        !append_str(line, sizeof(line), &ln, gid_str) ||
        !append_str(line, sizeof(line), &ln, ":\n") ||
        !account_append(1, line)) {
        account_clear();
        return tool_error("adduser", "failed to format group entry", 0);
    }

    /* If wheel requested, add username to wheel group in /etc/group */
    if (add_to_wheel) {
        const db_group_t *w = db_group_name(&account_db, "wheel");
        if (w) {
            char members[256];
            size_t mn = 0;
            append_str(members, sizeof(members), &mn, w->members);
            if (db_group_add_member(members, sizeof(members), username)) {
                char w_line[512];
                size_t wn = 0;
                char w_gid_str[12];
                tool_format_u64(w_gid_str, w->gid);
                append_str(w_line, sizeof(w_line), &wn, "wheel:x:");
                append_str(w_line, sizeof(w_line), &wn, w_gid_str);
                append_str(w_line, sizeof(w_line), &wn, ":");
                append_str(w_line, sizeof(w_line), &wn, members);
                append_str(w_line, sizeof(w_line), &wn, "\n");
                (void)account_replace_entry(1, "wheel", w_line);
            }
        }
    }

    /* /etc/shadow: username:hash:::::::\n */
    ln = 0;
    if (!append_str(line, sizeof(line), &ln, username) ||
        !append_str(line, sizeof(line), &ln, ":") ||
        !append_str(line, sizeof(line), &ln, hash) ||
        !append_str(line, sizeof(line), &ln, ":::::::\n") ||
        !account_append(2, line)) {
        account_clear();
        return tool_error("adduser", "failed to format shadow entry", 0);
    }

    /* Commit transaction */
    if (!account_commit()) {
        account_clear();
        return tool_error("adduser", "failed to commit account transaction", 0);
    }

    account_clear();
    tool_write("adduser", "User account created successfully.\n", 35);
    result = 0;
    return result;
}
