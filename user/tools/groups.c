#include "common.h"
#include "userdb.h"

static userdb_t database;

static int print_str(const char *s) {
    return tool_write("groups", s, tool_length(s));
}

int groups_main(int argc, char **argv) {
    if (argc > 2) {
        return tool_error("groups", "usage: groups [username]", 0);
    }
    if (argc == 2 && tool_equal(argv[1], "--help")) {
        print_str("Usage: groups [username]\n");
        return 0;
    }

    if (!db_load(&database, false)) {
        return tool_error("groups", "cannot read account database", 0);
    }

    const db_user_t *u = NULL;
    if (argc == 2) {
        u = db_user_name(&database, argv[1]);
        if (!u) {
            db_wipe(&database, sizeof(database));
            return tool_error("groups", "user does not exist", 0);
        }
    } else {
        uint32_t uid, euid, suid;
        if (tool_syscall(SYS_GETRESUID, (uintptr_t)&uid, (uintptr_t)&euid, (uintptr_t)&suid) < 0) {
            db_wipe(&database, sizeof(database));
            return tool_error("groups", "cannot read process identity", 0);
        }
        u = db_user_id(&database, uid);
        if (!u) {
            db_wipe(&database, sizeof(database));
            return tool_error("groups", "cannot find current user record", 0);
        }
    }

    uint32_t groups[16];
    unsigned count = 0;
    if (!db_groups(&database, u, groups, &count)) {
        db_wipe(&database, sizeof(database));
        return tool_error("groups", "failed to resolve group memberships", 0);
    }

    for (unsigned i = 0; i < count; i++) {
        const db_group_t *g = db_group_id(&database, groups[i]);
        if (g) {
            print_str(g->name);
        } else {
            char num[12];
            tool_format_u64(num, groups[i]);
            print_str(num);
        }
        if (i + 1 < count) {
            print_str(" ");
        }
    }
    print_str("\n");

    db_wipe(&database, sizeof(database));
    return 0;
}
