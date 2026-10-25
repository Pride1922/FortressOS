#include "account_tool.h"

static bool append_str(char *out, size_t cap, size_t *n, const char *s) {
    while (*s) {
        if (*n + 1 >= cap) return false;
        out[(*n)++] = *s++;
    }
    out[*n] = 0;
    return true;
}

int usermod_main(int argc, char **argv) {
    uint32_t uid, euid, suid;
    const char *username = NULL;
    const char *append_group = NULL;
    const char *new_shell = NULL;
    const char *new_gecos = NULL;
    const char *first_name = NULL;
    const char *last_name = NULL;
    bool lock_account = false;
    bool unlock_account = false;
    int result = 1;

    if (tool_syscall(SYS_GETRESUID, (uintptr_t)&uid, (uintptr_t)&euid, (uintptr_t)&suid) || euid != 0) {
        return tool_error("usermod", "only root may modify user accounts", 0);
    }

    for (int i = 1; i < argc; i++) {
        if (tool_equal(argv[i], "-aG") || tool_equal(argv[i], "--append-group")) {
            if (++i >= argc) return tool_error("usermod", "option requires an argument -- 'aG'", 0);
            append_group = argv[i];
        } else if (tool_equal(argv[i], "-s") || tool_equal(argv[i], "--shell")) {
            if (++i >= argc) return tool_error("usermod", "option requires an argument -- 's'", 0);
            new_shell = argv[i];
        } else if (tool_equal(argv[i], "-c") || tool_equal(argv[i], "--comment")) {
            if (++i >= argc) return tool_error("usermod", "option requires an argument -- 'c'", 0);
            new_gecos = argv[i];
        } else if (tool_equal(argv[i], "--first-name")) {
            if (++i >= argc) return tool_error("usermod", "option requires an argument -- 'first-name'", 0);
            first_name = argv[i];
        } else if (tool_equal(argv[i], "--last-name")) {
            if (++i >= argc) return tool_error("usermod", "option requires an argument -- 'last-name'", 0);
            last_name = argv[i];
        } else if (tool_equal(argv[i], "-L") || tool_equal(argv[i], "--lock")) {
            lock_account = true;
        } else if (tool_equal(argv[i], "-U") || tool_equal(argv[i], "--unlock")) {
            unlock_account = true;
        } else if (argv[i][0] == '-') {
            return tool_error("usermod", "unknown option", 0);
        } else if (!username) {
            username = argv[i];
        } else {
            return tool_error("usermod", "unexpected operand", 0);
        }
    }

    if (!username || (lock_account && unlock_account)) {
        return tool_error("usermod", "usage: usermod [OPTIONS] USERNAME", 0);
    }

    if (!account_snapshot()) {
        return tool_error("usermod", "cannot read account database or system is read-only", 0);
    }

    const db_user_t *u = db_user_name(&account_db, username);
    if (!u) {
        account_clear();
        return tool_error("usermod", "user does not exist", 0);
    }

    const db_shadow_t *s = db_shadow_name(&account_db, username);
    if (!s) {
        account_clear();
        return tool_error("usermod", "shadow entry does not exist", 0);
    }

    if ((lock_account || unlock_account) && tool_equal(username, "root")) {
        account_clear();
        return tool_error("usermod", "cannot change lock state of superuser account 'root'", 0);
    }

    /* Validate append-group */
    const db_group_t *target_grp = NULL;
    if (append_group) {
        target_grp = db_group_name(&account_db, append_group);
        if (!target_grp) {
            account_clear();
            return tool_error("usermod", "group does not exist", 0);
        }
    }

    /* Validate new shell */
    if (new_shell) {
        if (!tool_equal(new_shell, "/bin/shell") && !tool_equal(new_shell, "/bin/nologin")) {
            account_clear();
            return tool_error("usermod", "shell is not an authorized login shell (/bin/shell or /bin/nologin)", 0);
        }
    }

    /* Construct GECOS if needed */
    char gecos_buf[DB_GECOS_MAX];
    gecos_buf[0] = 0;
    if (new_gecos) {
        if (tool_length(new_gecos) >= sizeof(gecos_buf)) {
            account_clear();
            return tool_error("usermod", "GECOS / full name too long (max 63 chars)", 0);
        }
        for (size_t i = 0; new_gecos[i]; i++) gecos_buf[i] = new_gecos[i];
    } else if (first_name || last_name) {
        size_t gn = 0;
        if (first_name) append_str(gecos_buf, sizeof(gecos_buf), &gn, first_name);
        if (first_name && last_name) append_str(gecos_buf, sizeof(gecos_buf), &gn, " ");
        if (last_name) append_str(gecos_buf, sizeof(gecos_buf), &gn, last_name);
    }

    /* Acquire transaction lease */
    if (!account_begin()) {
        account_clear();
        return tool_error("usermod", "account database is busy; retry command", 0);
    }

    /* Apply group append */
    if (target_grp) {
        char members[256];
        size_t mn = 0;
        append_str(members, sizeof(members), &mn, target_grp->members);
        if (db_group_add_member(members, sizeof(members), username)) {
            char g_line[512], gid_str[12];
            size_t gn = 0;
            tool_format_u64(gid_str, target_grp->gid);
            append_str(g_line, sizeof(g_line), &gn, target_grp->name);
            append_str(g_line, sizeof(g_line), &gn, ":x:");
            append_str(g_line, sizeof(g_line), &gn, gid_str);
            append_str(g_line, sizeof(g_line), &gn, ":");
            append_str(g_line, sizeof(g_line), &gn, members);
            append_str(g_line, sizeof(g_line), &gn, "\n");
            if (!account_replace_entry(1, target_grp->name, g_line)) {
                account_clear();
                return tool_error("usermod", "failed to update group members", 0);
            }
        }
    }

    /* Apply shell or GECOS modification */
    if (new_shell || new_gecos || first_name || last_name) {
        const char *shell_val = new_shell ? new_shell : u->shell;
        const char *gecos_val = (new_gecos || first_name || last_name) ? gecos_buf : u->gecos;
        char p_line[512], uid_str[12], gid_str[12];
        size_t pn = 0;
        tool_format_u64(uid_str, u->uid);
        tool_format_u64(gid_str, u->gid);
        append_str(p_line, sizeof(p_line), &pn, u->name);
        append_str(p_line, sizeof(p_line), &pn, ":x:");
        append_str(p_line, sizeof(p_line), &pn, uid_str);
        append_str(p_line, sizeof(p_line), &pn, ":");
        append_str(p_line, sizeof(p_line), &pn, gid_str);
        append_str(p_line, sizeof(p_line), &pn, ":");
        append_str(p_line, sizeof(p_line), &pn, gecos_val);
        append_str(p_line, sizeof(p_line), &pn, ":");
        append_str(p_line, sizeof(p_line), &pn, u->home);
        append_str(p_line, sizeof(p_line), &pn, ":");
        append_str(p_line, sizeof(p_line), &pn, shell_val);
        append_str(p_line, sizeof(p_line), &pn, "\n");
        if (!account_replace_entry(0, u->name, p_line)) {
            account_clear();
            return tool_error("usermod", "failed to update passwd record", 0);
        }
    }

    /* Apply account lock or unlock */
    if (lock_account || unlock_account) {
        char new_hash[96];
        size_t hn = 0;
        if (lock_account) {
            if (s->hash[0] != '!') {
                new_hash[hn++] = '!';
            }
            append_str(new_hash, sizeof(new_hash), &hn, s->hash);
        } else {
            /* Unlock: skip leading '!' if present */
            const char *src = (s->hash[0] == '!') ? s->hash + 1 : s->hash;
            append_str(new_hash, sizeof(new_hash), &hn, src);
        }
        char s_line[512];
        size_t sn = 0;
        append_str(s_line, sizeof(s_line), &sn, s->name);
        append_str(s_line, sizeof(s_line), &sn, ":");
        append_str(s_line, sizeof(s_line), &sn, new_hash);
        append_str(s_line, sizeof(s_line), &sn, ":::::::\n");
        if (!account_replace_entry(2, s->name, s_line)) {
            account_clear();
            return tool_error("usermod", "failed to update shadow record", 0);
        }
    }

    /* Commit transaction */
    if (!account_commit()) {
        account_clear();
        return tool_error("usermod", "failed to commit account changes", 0);
    }

    account_clear();
    tool_write("usermod", "User account modified successfully.\n", 36);
    result = 0;
    return result;
}
