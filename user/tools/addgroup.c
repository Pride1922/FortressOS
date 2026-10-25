#include "account_tool.h"

static bool append_str(char *out, size_t cap, size_t *n, const char *s) {
    while (*s) {
        if (*n + 1 >= cap) return false;
        out[(*n)++] = *s++;
    }
    out[*n] = 0;
    return true;
}

int addgroup_main(int argc, char **argv) {
    uint32_t uid, euid, suid, custom_gid = 0;
    const char *groupname = NULL;
    int result = 1;

    if (tool_syscall(SYS_GETRESUID, (uintptr_t)&uid, (uintptr_t)&euid, (uintptr_t)&suid) || euid != 0) {
        return tool_error("addgroup", "only root may add a group to the system", 0);
    }

    for (int i = 1; i < argc; i++) {
        if (tool_equal(argv[i], "--gid") || tool_equal(argv[i], "-g")) {
            if (i + 1 >= argc) {
                return tool_error("addgroup", "usage: addgroup [--gid GID] GROUPNAME", 0);
            }
            const char *p = argv[++i];
            uint32_t val = 0;
            if (!*p) return tool_error("addgroup", "invalid GID", 0);
            for (; *p; p++) {
                if (*p < '0' || *p > '9' || val > (UINT32_MAX - (unsigned)(*p - '0')) / 10)
                    return tool_error("addgroup", "invalid GID", 0);
                val = val * 10 + (*p - '0');
            }
            custom_gid = val;
        } else if (argv[i][0] == '-') {
            return tool_error("addgroup", "usage: addgroup [--gid GID] GROUPNAME", 0);
        } else if (!groupname) {
            groupname = argv[i];
        } else {
            return tool_error("addgroup", "usage: addgroup [--gid GID] GROUPNAME", 0);
        }
    }

    if (!groupname) {
        return tool_error("addgroup", "usage: addgroup [--gid GID] GROUPNAME", 0);
    }

    size_t name_len = tool_length(groupname);
    if (!name_len || name_len >= DB_NAME_MAX) {
        return tool_error("addgroup", "group name must be 1..31 characters", 0);
    }
    for (size_t i = 0; i < name_len; i++) {
        char c = groupname[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (i > 0 && c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return tool_error("addgroup", "group name contains invalid characters", 0);
        }
    }

    if (!account_snapshot()) {
        return tool_error("addgroup", "cannot read account database or system is read-only", 0);
    }

    if (db_group_name(&account_db, groupname)) {
        account_clear();
        return tool_error("addgroup", "the group already exists", 0);
    }

    if (account_db.ngroups >= DB_MAX_RECORDS) {
        account_clear();
        return tool_error("addgroup", "maximum group limit reached (16)", 0);
    }

    uint32_t gid = custom_gid ? custom_gid : account_next_gid();
    if (!gid) {
        account_clear();
        return tool_error("addgroup", "no available GID", 0);
    }
    if (custom_gid && db_group_id(&account_db, custom_gid)) {
        account_clear();
        return tool_error("addgroup", "the GID is already in use", 0);
    }

    if (!account_begin()) {
        account_clear();
        return tool_error("addgroup", "account database is busy; retry command", 0);
    }

    char gid_str[12], line[512];
    tool_format_u64(gid_str, gid);
    size_t n = 0;
    if (!append_str(line, sizeof(line), &n, groupname) ||
        !append_str(line, sizeof(line), &n, ":x:") ||
        !append_str(line, sizeof(line), &n, gid_str) ||
        !append_str(line, sizeof(line), &n, ":\n") ||
        !account_append(1, line) ||
        !account_commit()) {
        account_clear();
        return tool_error("addgroup", "failed to commit group to database", 0);
    }

    account_clear();
    tool_write("addgroup", "Group created\n", 14);
    result = 0;
    return result;
}
