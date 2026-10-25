#include "common.h"
#include "userdb.h"

static userdb_t database;

static int print_str(const char *s) {
    return tool_write("id", s, tool_length(s));
}

static int print_num(uint32_t val) {
    char text[12];
    size_t n = tool_format_u64(text, val);
    return tool_write("id", text, n);
}

static void print_id_and_name(const char *label, uint32_t id, bool is_group) {
    print_str(label);
    print_num(id);
    const char *name = NULL;
    if (is_group) {
        const db_group_t *g = db_group_id(&database, id);
        if (g) name = g->name;
    } else {
        const db_user_t *u = db_user_id(&database, id);
        if (u) name = u->name;
    }
    if (name) {
        print_str("(");
        print_str(name);
        print_str(")");
    }
}

int id_main(int argc, char **argv) {
    if (argc > 2) {
        return tool_error("id", "usage: id [username]", 0);
    }
    if (argc == 2 && tool_equal(argv[1], "--help")) {
        print_str("Usage: id [username]\n");
        return 0;
    }

    bool db_ok = db_load(&database, false);

    if (argc == 2) {
        if (!db_ok) {
            return tool_error("id", "cannot read account database", 0);
        }
        const db_user_t *u = db_user_name(&database, argv[1]);
        if (!u) {
            db_wipe(&database, sizeof(database));
            return tool_error("id", "no such user", 0);
        }

        print_id_and_name("uid=", u->uid, false);
        print_id_and_name(" gid=", u->gid, true);

        if (u->gecos[0]) {
            print_str(" name=\"");
            print_str(u->gecos);
            print_str("\"");
        }

        uint32_t groups[16];
        unsigned count = 0;
        if (db_groups(&database, u, groups, &count)) {
            print_str(" groups=");
            for (unsigned i = 0; i < count; i++) {
                if (i > 0) print_str(",");
                print_num(groups[i]);
                const db_group_t *g = db_group_id(&database, groups[i]);
                if (g) {
                    print_str("(");
                    print_str(g->name);
                    print_str(")");
                }
            }
        }
        print_str("\n");
        db_wipe(&database, sizeof(database));
        return 0;
    }

    /* No arguments: examine current process identity */
    uint32_t uid, euid, suid, gid, egid, sgid, groups[16];
    long r = tool_syscall(SYS_GETRESUID, (uintptr_t)&uid, (uintptr_t)&euid, (uintptr_t)&suid);
    if (!r) r = tool_syscall(SYS_GETRESGID, (uintptr_t)&gid, (uintptr_t)&egid, (uintptr_t)&sgid);
    if (r < 0) {
        db_wipe(&database, sizeof(database));
        return tool_error("id", "cannot read process identity", 0);
    }
    long count = tool_syscall(SYS_GETGROUPS, 16, (uintptr_t)groups, 0);
    if (count < 0 || count > 16) count = 0;

    print_id_and_name("uid=", uid, false);
    print_id_and_name(" gid=", gid, true);

    if (euid != uid) print_id_and_name(" euid=", euid, false);
    if (egid != gid) print_id_and_name(" egid=", egid, true);
    if (suid != euid) print_id_and_name(" suid=", suid, false);
    if (sgid != egid) print_id_and_name(" sgid=", sgid, true);

    print_str(" groups=");
    print_id_and_name("", egid, true);
    for (long i = 0; i < count; i++) {
        if (groups[i] != egid) {
            print_id_and_name(",", groups[i], true);
        }
    }
    print_str("\n");

    db_wipe(&database, sizeof(database));
    return 0;
}
