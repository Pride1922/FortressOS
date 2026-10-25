#include "account_tool.h"

static bool append_str(char *out, size_t cap, size_t *n, const char *s) {
    while (*s) {
        if (*n + 1 >= cap) return false;
        out[(*n)++] = *s++;
    }
    out[*n] = 0;
    return true;
}

static void remove_home_dir(const char *home) {
    /* List files in home directory and unlink them */
    char names[16][256];
    for (int iter = 0; iter < 10; iter++) {
        long count = tool_syscall(SYS_LIST_DIR, (uintptr_t)home, (uintptr_t)names, 16);
        if (count <= 0) break;
        bool deleted_any = false;
        for (long i = 0; i < count; i++) {
            if (tool_equal(names[i], ".") || tool_equal(names[i], "..")) continue;
            char path[512];
            size_t n = 0;
            append_str(path, sizeof(path), &n, home);
            append_str(path, sizeof(path), &n, "/");
            append_str(path, sizeof(path), &n, names[i]);
            (void)tool_syscall(SYS_UNLINK, (uintptr_t)path, 0, 0);
            deleted_any = true;
        }
        if (!deleted_any) break;
    }
    /* Try to unlink the known skel files just in case */
    const char *known[] = { ".profile", ".shellrc", NULL };
    for (int i = 0; known[i]; i++) {
        char path[512];
        size_t n = 0;
        append_str(path, sizeof(path), &n, home);
        append_str(path, sizeof(path), &n, "/");
        append_str(path, sizeof(path), &n, known[i]);
        (void)tool_syscall(SYS_UNLINK, (uintptr_t)path, 0, 0);
    }
    (void)tool_syscall(SYS_UNLINK, (uintptr_t)home, 0, 0);
}

int deluser_main(int argc, char **argv) {
    uint32_t uid, euid, suid;
    const char *username = NULL;
    bool remove_home = false;
    int result = 1;

    if (tool_syscall(SYS_GETRESUID, (uintptr_t)&uid, (uintptr_t)&euid, (uintptr_t)&suid) || euid != 0) {
        return tool_error("deluser", "only root may remove a user from the system", 0);
    }

    for (int i = 1; i < argc; i++) {
        if (tool_equal(argv[i], "--remove-home")) {
            remove_home = true;
        } else if (argv[i][0] == '-') {
            return tool_error("deluser", "usage: deluser [--remove-home] USERNAME", 0);
        } else if (!username) {
            username = argv[i];
        } else {
            return tool_error("deluser", "unexpected operand", 0);
        }
    }

    if (!username) {
        return tool_error("deluser", "usage: deluser [--remove-home] USERNAME", 0);
    }

    if (tool_equal(username, "root")) {
        return tool_error("deluser", "cannot delete superuser account 'root'", 0);
    }

    if (!account_snapshot()) {
        return tool_error("deluser", "cannot read account database or system is read-only", 0);
    }

    const db_user_t *u = db_user_name(&account_db, username);
    if (!u) {
        account_clear();
        return tool_error("deluser", "the user does not exist", 0);
    }

    /* Refuse deleting the current user */
    if (u->uid == uid) {
        account_clear();
        return tool_error("deluser", "cannot delete the currently logged-in user", 0);
    }

    /* Check if user is the only member of wheel */
    const db_group_t *w = db_group_name(&account_db, "wheel");
    if (w && db_group_has_member(w, username)) {
        char members[256];
        size_t mn = 0;
        append_str(members, sizeof(members), &mn, w->members);
        db_group_remove_member(members, username);
        if (!members[0]) {
            tool_write("deluser", "Warning: deleting the only member of administrative group 'wheel'\n", 68);
        }
    }

    /* Copy home directory path before clearing account_db */
    char user_home[128];
    size_t hn = 0;
    append_str(user_home, sizeof(user_home), &hn, u->home);

    /* Acquire transaction lease */
    if (!account_begin()) {
        account_clear();
        return tool_error("deluser", "account database is busy; retry command", 0);
    }

    /* Step 1: Remove user from /etc/passwd and /etc/shadow */
    if (!account_remove_entry(0, username) || !account_remove_entry(2, username)) {
        account_clear();
        return tool_error("deluser", "failed to remove user records", 0);
    }

    /* Step 2: Remove user from groups in /etc/group */
    for (unsigned i = 0; i < account_db.ngroups; i++) {
        const db_group_t *g = &account_db.groups[i];
        if (tool_equal(g->name, username)) {
            /* If this is the user's private group and has no other members, remove it */
            (void)account_remove_entry(1, g->name);
        } else if (db_group_has_member(g, username)) {
            char members[256];
            size_t mn = 0;
            append_str(members, sizeof(members), &mn, g->members);
            if (db_group_remove_member(members, username)) {
                char g_line[512], gid_str[12];
                size_t gn = 0;
                tool_format_u64(gid_str, g->gid);
                append_str(g_line, sizeof(g_line), &gn, g->name);
                append_str(g_line, sizeof(g_line), &gn, ":x:");
                append_str(g_line, sizeof(g_line), &gn, gid_str);
                append_str(g_line, sizeof(g_line), &gn, ":");
                append_str(g_line, sizeof(g_line), &gn, members);
                append_str(g_line, sizeof(g_line), &gn, "\n");
                (void)account_replace_entry(1, g->name, g_line);
            }
        }
    }

    /* Step 3: Commit transaction */
    if (!account_commit()) {
        account_clear();
        return tool_error("deluser", "failed to commit account deletion", 0);
    }

    account_clear();

    /* Step 4: Remove home directory if requested */
    if (remove_home && user_home[0] == '/' && user_home[1] == 'h' &&
        user_home[2] == 'o' && user_home[3] == 'm' && user_home[4] == 'e' && user_home[5] == '/') {
        remove_home_dir(user_home);
    }

    tool_write("deluser", "User deleted successfully.\n", 27);
    result = 0;
    return result;
}
