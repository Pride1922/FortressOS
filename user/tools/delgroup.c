#include "account_tool.h"

int delgroup_main(int argc, char **argv) {
    uint32_t uid, euid, suid;
    int result = 1;

    if (tool_syscall(SYS_GETRESUID, (uintptr_t)&uid, (uintptr_t)&euid, (uintptr_t)&suid) || euid != 0) {
        return tool_error("delgroup", "only root may remove a group from the system", 0);
    }

    if (argc != 2 || argv[1][0] == '-') {
        return tool_error("delgroup", "usage: delgroup GROUPNAME", 0);
    }

    const char *groupname = argv[1];
    if (tool_equal(groupname, "root") || tool_equal(groupname, "wheel")) {
        return tool_error("delgroup", "cannot remove critical system group", 0);
    }

    if (!account_snapshot()) {
        return tool_error("delgroup", "cannot read account database or system is read-only", 0);
    }

    const db_group_t *g = db_group_name(&account_db, groupname);
    if (!g) {
        account_clear();
        return tool_error("delgroup", "the group does not exist", 0);
    }

    /* Invariant: Fail closed if the group is any user's primary group */
    for (unsigned i = 0; i < account_db.nusers; i++) {
        if (account_db.users[i].gid == g->gid) {
            account_clear();
            return tool_error("delgroup", "cannot remove group: in use as primary group", 0);
        }
    }

    if (!account_begin()) {
        account_clear();
        return tool_error("delgroup", "account database is busy; retry command", 0);
    }

    if (!account_remove_entry(1, groupname) || !account_commit()) {
        account_clear();
        return tool_error("delgroup", "failed to commit group removal to database", 0);
    }

    account_clear();
    tool_write("delgroup", "Group deleted\n", 14);
    result = 0;
    return result;
}
