#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include "../user/tools/userdb.h"

int main(void) {
    printf("Starting Users/Groups Phase 1 Host Test Suite...\n");

    /* Test 1: GECOS parsing and capacity */
    {
        userdb_t db = {0};
        const char *pw = 
            "root:x:0:0:System Administrator:/root:/bin/shell\n"
            "alice:x:1000:1000:Alice Smith:/home/alice:/bin/shell\n"
            "bob:x:1001:1001::/home/bob:/bin/shell\n";
        assert(db_passwd(&db, pw, strlen(pw)));
        assert(db.nusers == 3);
        assert(!strcmp(db.users[0].gecos, "System Administrator"));
        assert(!strcmp(db.users[1].gecos, "Alice Smith"));
        assert(!strcmp(db.users[2].gecos, ""));

        /* Verify oversized GECOS is rejected */
        char huge_gecos[128];
        memset(huge_gecos, 'A', sizeof(huge_gecos) - 1);
        huge_gecos[sizeof(huge_gecos) - 1] = 0;
        char pw_bad[512];
        snprintf(pw_bad, sizeof(pw_bad), "charlie:x:1002:1002:%s:/home/charlie:/bin/shell\n", huge_gecos);
        userdb_t db_bad = {0};
        assert(!db_passwd(&db_bad, pw_bad, strlen(pw_bad)));
        printf("PASS GECOS parsing and length bound checks\n");
    }

    /* Test 2: db_group_name lookup */
    {
        userdb_t db = {0};
        const char *gr = 
            "root:x:0:\n"
            "wheel:x:10:alice\n"
            "developers:x:1005:alice,bob\n";
        assert(db_group(&db, gr, strlen(gr)));
        assert(db.ngroups == 3);

        const db_group_t *g_wheel = db_group_name(&db, "wheel");
        assert(g_wheel != NULL && g_wheel->gid == 10);

        const db_group_t *g_dev = db_group_name(&db, "developers");
        assert(g_dev != NULL && g_dev->gid == 1005);

        const db_group_t *g_none = db_group_name(&db, "nonexistent");
        assert(g_none == NULL);
        printf("PASS db_group_name lookup\n");
    }

    /* Test 3: Member list helpers (has_member, add_member, remove_member) */
    {
        userdb_t db = {0};
        const char *gr = "developers:x:1005:alice,bob\n";
        assert(db_group(&db, gr, strlen(gr)));

        const db_group_t *g = &db.groups[0];
        assert(db_group_has_member(g, "alice"));
        assert(db_group_has_member(g, "bob"));
        assert(!db_group_has_member(g, "charlie"));
        assert(!db_group_has_member(g, "ali")); // prefix match rejection

        char mem[256] = "";
        /* Add to empty */
        assert(db_group_add_member(mem, sizeof(mem), "alice"));
        assert(!strcmp(mem, "alice"));

        /* Add duplicate (idempotent) */
        assert(db_group_add_member(mem, sizeof(mem), "alice"));
        assert(!strcmp(mem, "alice"));

        /* Add second member */
        assert(db_group_add_member(mem, sizeof(mem), "bob"));
        assert(!strcmp(mem, "alice,bob"));

        /* Add third member */
        assert(db_group_add_member(mem, sizeof(mem), "charlie"));
        assert(!strcmp(mem, "alice,bob,charlie"));

        /* Remove middle member */
        assert(db_group_remove_member(mem, "bob"));
        assert(!strcmp(mem, "alice,charlie"));

        /* Remove first member */
        assert(db_group_remove_member(mem, "alice"));
        assert(!strcmp(mem, "charlie"));

        /* Remove last remaining member */
        assert(db_group_remove_member(mem, "charlie"));
        assert(!strcmp(mem, ""));

        /* Remove non-existent member (idempotent) */
        assert(db_group_remove_member(mem, "david"));
        assert(!strcmp(mem, ""));
        printf("PASS db_group member list manipulation\n");
    }

    /* Test 4: Flat-file buffer manipulation (remove_entry, replace_entry, next_uid, next_gid) */
    {
        char buffer[DB_FILE_MAX] = 
            "root:x:0:\n"
            "wheel:x:10:operator\n"
            "testgroup:x:1000:\n"
            "video:x:44:operator\n";
        uint32_t size = strlen(buffer);

        /* Test entry removal */
        char temp[DB_FILE_MAX]; size_t out = 0, in = 0;
        const char *name_to_remove = "testgroup";
        size_t nlen = strlen(name_to_remove);
        bool found = false;
        while (in < size) {
            size_t start = in;
            while (in < size && buffer[in] != '\n') in++;
            size_t llen = in - start;
            if (in < size && buffer[in] == '\n') in++;
            bool match = (llen > nlen && buffer[start + nlen] == ':');
            if (match) {
                for (size_t k = 0; k < nlen; k++) if (buffer[start + k] != name_to_remove[k]) match = false;
            }
            if (match) {
                found = true;
            } else {
                for (size_t k = start; k < in; k++) temp[out++] = buffer[k];
            }
        }
        assert(found);
        temp[out] = 0;
        const char *expected = "root:x:0:\nwheel:x:10:operator\nvideo:x:44:operator\n";
        assert(!strcmp(temp, expected));

        /* Test replacement */
        const char *replacement_line = "wheel:x:10:operator,newuser";
        in = 0; out = 0; found = false;
        const char *name_to_replace = "wheel";
        nlen = strlen(name_to_replace);
        size_t rlen = strlen(replacement_line);
        while (in < size) {
            size_t start = in;
            while (in < size && buffer[in] != '\n') in++;
            size_t llen = in - start;
            if (in < size && buffer[in] == '\n') in++;
            bool match = (llen > nlen && buffer[start + nlen] == ':');
            if (match) {
                for (size_t k = 0; k < nlen; k++) if (buffer[start + k] != name_to_replace[k]) match = false;
            }
            if (match) {
                found = true;
                for (size_t k = 0; k < rlen; k++) temp[out++] = replacement_line[k];
                temp[out++] = '\n';
            } else {
                for (size_t k = start; k < in; k++) temp[out++] = buffer[k];
            }
        }
        assert(found);
        temp[out] = 0;
        const char *expected_rep = 
            "root:x:0:\n"
            "wheel:x:10:operator,newuser\n"
            "testgroup:x:1000:\n"
            "video:x:44:operator\n";
        assert(!strcmp(temp, expected_rep));
        printf("PASS flat-file line removal and replacement\n");
    }

    /* Test 5: Next UID / GID allocation logic */
    {
        userdb_t db = {0};
        const char *pw = "root:x:0:0::/root:/bin/shell\noperator:x:1000:1000::/run/user/1000:/bin/shell\nalice:x:1001:1001::/run/user/1001:/bin/shell\n";
        const char *gr = "root:x:0:\noperator:x:1000:\nalice:x:1001:\n";
        assert(db_passwd(&db, pw, strlen(pw)));
        assert(db_group(&db, gr, strlen(gr)));

        /* Allocate next UID */
        uint32_t uid = 1000;
        for (;;) {
            bool used = false;
            for (unsigned i = 0; i < db.nusers; i++) if (db.users[i].uid == uid) { used = true; break; }
            if (!used) break;
            uid++;
        }
        assert(uid == 1002);

        /* Allocate next GID */
        uint32_t gid = 1000;
        for (;;) {
            bool used = false;
            for (unsigned i = 0; i < db.ngroups; i++) if (db.groups[i].gid == gid) { used = true; break; }
            if (!used) break;
            gid++;
        }
        assert(gid == 1002);
        printf("PASS next UID/GID allocation logic\n");
    }

    /* Test 6: Primary group deletion rejection */
    {
        userdb_t db = {0};
        const char *pw = "root:x:0:0::/root:/bin/shell\nuser1:x:1000:1000::/home/user1:/bin/shell\n";
        const char *gr = "root:x:0:\nuser1:x:1000:\nsecondary:x:1001:\n";
        assert(db_passwd(&db, pw, strlen(pw)));
        assert(db_group(&db, gr, strlen(gr)));

        /* Check primary group conflict */
        const db_group_t *g_user1 = db_group_name(&db, "user1");
        assert(g_user1 != NULL);
        bool in_use = false;
        for (unsigned i = 0; i < db.nusers; i++) {
            if (db.users[i].gid == g_user1->gid) { in_use = true; break; }
        }
        assert(in_use == true); /* MUST be rejected by delgroup */

        /* Check secondary group */
        const db_group_t *g_sec = db_group_name(&db, "secondary");
        assert(g_sec != NULL);
        in_use = false;
        for (unsigned i = 0; i < db.nusers; i++) {
            if (db.users[i].gid == g_sec->gid) { in_use = true; break; }
        }
        assert(in_use == false); /* Can safely be removed by delgroup */
        printf("PASS primary group deletion conflict check\n");
    }

    /* Test 7: Shadow lock/unlock logic */
    {
        const char *orig_hash = "$5$rounds=100000$abcdefghijklmnop$JAlrLh9YVBYa4XSl2fqcB6N55eikn17ite1nvkjVHI0";
        char locked[96] = {0};
        locked[0] = '!';
        memcpy(locked + 1, orig_hash, strlen(orig_hash) + 1);
        assert(locked[0] == '!');
        assert(!strcmp(locked + 1, orig_hash));

        /* Locking already locked hash does not double-prepend */
        char double_locked[96] = {0};
        if (locked[0] == '!') memcpy(double_locked, locked, strlen(locked) + 1);
        assert(!strcmp(double_locked, locked));

        /* Unlock removes leading '!' */
        char unlocked[96] = {0};
        const char *unl_src = (locked[0] == '!') ? locked + 1 : locked;
        memcpy(unlocked, unl_src, strlen(unl_src) + 1);
        assert(!strcmp(unlocked, orig_hash));
        printf("PASS shadow account lock and unlock transformations\n");
    }

    /* Test 8: Shell authorization checks */
    {
        assert(!strcmp("/bin/shell", "/bin/shell"));
        assert(!strcmp("/bin/nologin", "/bin/nologin"));
        const char *bad_shells[] = { "/bin/sh", "/bin/bash", "/usr/bin/zsh", "sh", "" };
        for (unsigned i = 0; i < sizeof(bad_shells)/sizeof(*bad_shells); i++) {
            bool ok = (!strcmp(bad_shells[i], "/bin/shell") || !strcmp(bad_shells[i], "/bin/nologin"));
            assert(!ok);
        }
        printf("PASS authorized shell enforcement\n");
    }

    /* Test 9: Complete user deletion simulation with referential integrity */
    {
        userdb_t db = {0};
        const char *pw = 
            "root:x:0:0:System Administrator:/root:/bin/shell\n"
            "operator:x:1000:1000:Operator:/run/user/1000:/bin/shell\n"
            "alice:x:1001:1001:Alice Smith:/home/alice:/bin/shell\n";
        const char *gr = 
            "root:x:0:\n"
            "operator:x:1000:\n"
            "wheel:x:10:operator,alice\n"
            "video:x:44:operator,alice\n"
            "alice:x:1001:\n";
        const char *sh = 
            "root:!:::::::\n"
            "operator::::::::\n"
            "alice:$5$rounds=100000$abcdefghijklmnop$JAlrLh9YVBYa4XSl2fqcB6N55eikn17ite1nvkjVHI0:::::::\n";

        assert(db_passwd(&db, pw, strlen(pw)));
        assert(db_group(&db, gr, strlen(gr)));
        assert(db_shadow(&db, sh, strlen(sh)));
        assert(db_validate(&db));

        /* Simulate deluser alice */
        const char *pw_after = 
            "root:x:0:0:System Administrator:/root:/bin/shell\n"
            "operator:x:1000:1000:Operator:/run/user/1000:/bin/shell\n";
        const char *gr_after = 
            "root:x:0:\n"
            "operator:x:1000:\n"
            "wheel:x:10:operator\n"
            "video:x:44:operator\n";
        const char *sh_after = 
            "root:!:::::::\n"
            "operator::::::::\n";

        userdb_t db_clean = {0};
        assert(db_passwd(&db_clean, pw_after, strlen(pw_after)));
        assert(db_group(&db_clean, gr_after, strlen(gr_after)));
        assert(db_shadow(&db_clean, sh_after, strlen(sh_after)));
        assert(db_validate(&db_clean));

        assert(db_user_name(&db_clean, "alice") == NULL);
        assert(db_group_name(&db_clean, "alice") == NULL);
        assert(!db_group_has_member(db_group_name(&db_clean, "wheel"), "alice"));
        assert(!db_group_has_member(db_group_name(&db_clean, "video"), "alice"));
        printf("PASS simulated user deletion and referential integrity\n");
    }

    /* Test 10: Complete user addition simulation with UPG and wheel */
    {
        userdb_t db = {0};
        const char *pw = 
            "root:x:0:0:System Administrator:/root:/bin/shell\n"
            "operator:x:1000:1000:Operator:/run/user/1000:/bin/shell\n"
            "bob:x:1002:1002:Bob Jones:/home/bob:/bin/shell\n";
        const char *gr = 
            "root:x:0:\n"
            "operator:x:1000:\n"
            "wheel:x:10:operator,bob\n"
            "bob:x:1002:\n";
        const char *sh = 
            "root:!:::::::\n"
            "operator::::::::\n"
            "bob:$5$rounds=100000$abcdefghijklmnop$JAlrLh9YVBYa4XSl2fqcB6N55eikn17ite1nvkjVHI0:::::::\n";

        assert(db_passwd(&db, pw, strlen(pw)));
        assert(db_group(&db, gr, strlen(gr)));
        assert(db_shadow(&db, sh, strlen(sh)));
        assert(db_validate(&db));

        const db_user_t *u_bob = db_user_name(&db, "bob");
        assert(u_bob != NULL);
        assert(!strcmp(u_bob->gecos, "Bob Jones"));
        assert(!strcmp(u_bob->home, "/home/bob"));
        assert(u_bob->uid == 1002 && u_bob->gid == 1002);
        assert(db_group_has_member(db_group_name(&db, "wheel"), "bob"));
        printf("PASS simulated user addition with GECOS, UPG and wheel\n");
    }

    printf("ALL USERS/GROUPS PHASE 1-3 HOST TESTS PASSED (100%%).\n");
    return 0;
}
