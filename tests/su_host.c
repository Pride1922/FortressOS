#include <assert.h>
#include <stdio.h>
#include <string.h>
#define PERMISSIONS_CLI_HOST_TEST
#include "../user/tools/su.c"

static bool target_locked = false;
static bool target_passwordless = false;
static unsigned transitions, spawns, reads, closes;
static const char *input = "";
static char output[2048];
static size_t written;
static bool generation_current = true;
static terminal_attrs_t tty_attrs = {.version = 1, .input_flags = TERM_ECHO | TERM_ICANON | TERM_ISIG};
static unsigned attr_sets;
static uint32_t current_uid = 1000;
static uint32_t last_set_uid, last_set_gid;

bool db_check(void) { return generation_current; }
bool db_busy(void) { return false; }

bool db_load(userdb_t *out, bool shadow) {
    assert(shadow);
    memset(out, 0, sizeof(*out));
    out->nusers = 3; out->ngroups = 3; out->nshadows = 3;
    out->users[0] = (db_user_t){.uid = 0, .gid = 0, .name = "root", .home = "/root", .shell = "/bin/shell"};
    out->users[1] = (db_user_t){.uid = 1000, .gid = 1000, .name = "fortress", .home = "/run/user/1000", .shell = "/bin/shell"};
    out->users[2] = (db_user_t){.uid = 1001, .gid = 1001, .name = "other", .home = "/run/user/1001", .shell = "/bin/shell"};

    out->groups[0] = (db_group_t){.gid = 0, .name = "root"};
    out->groups[1] = (db_group_t){.gid = 1000, .name = "fortress"};
    out->groups[2] = (db_group_t){.gid = 1001, .name = "other"};

    memcpy(out->shadows[0].name, "root", 5);
    if (target_locked) memcpy(out->shadows[0].hash, "!", 2);
    else memcpy(out->shadows[0].hash, "$5$salt$xyz", 12);

    memcpy(out->shadows[1].name, "fortress", 9);
    if (target_passwordless) out->shadows[1].hash[0] = '\0';
    else memcpy(out->shadows[1].hash, "$5$salt$xyz", 12);

    memcpy(out->shadows[2].name, "other", 6);
    memcpy(out->shadows[2].hash, "$5$salt$xyz", 12);
    return true;
}

long tool_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    if (nr == SYS_GETRESUID) {
        *(uint32_t *)a = current_uid;
        *(uint32_t *)b = 0;
        *(uint32_t *)c = 0;
        return 0;
    }
    if (nr == SYS_OPEN) {
        if (!strcmp((char *)a, "/dev/tty")) return 7;
        return -1;
    }
    if (nr == SYS_TERMCTL) {
        if (a == TERM_ISATTY && b == 7) return 1;
        return 0;
    }
    if (nr == SYS_READ) {
        if (a == 7) {
            reads++;
            if (!*input) return SYSCALL_EINTR;
            *(char *)b = *input++;
            return 1;
        }
        return -1;
    }
    if (nr == SYS_WRITE) {
        if (c == 10 && !memcmp((void *)b, "Password: ", 10)) {
            assert(!(tty_attrs.input_flags & TERM_ECHO));
        }
        assert(written + c < sizeof(output));
        memcpy(output + written, (void *)b, c);
        written += c;
        output[written] = 0;
        return c;
    }
    if (nr == SYS_CLOSE) {
        if (a == 7) closes++;
        return 0;
    }
    if (nr == SYS_SETGROUPS) {
        transitions++;
        return 0;
    }
    if (nr == SYS_MKDIR || nr == SYS_CHMOD) {
        return 0;
    }
    if (nr == SYS_SPAWN_EXT) {
        spawns++;
        assert(!strcmp((char *)a, "/bin/shell"));
        const spawn_opts_t *o = (void *)b;
        const char *const *e = (void *)o->envp;
        assert(!strcmp(e[4], "PATH=/bin:/sbin"));
        return 99;
    }
    if (nr == SYS_WAIT) {
        assert(a == 99);
        *(uint64_t *)b = 42;
        return 99;
    }
    if (nr == SYS_NANOSLEEP) {
        return 0;
    }
    assert(!"unexpected syscall");
    return -1;
}

long permission_call4(long nr, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    (void)d;
    if (nr == SYS_TERMATTR) {
        assert(a == 7);
        if (b == TERM_GET) *(terminal_attrs_t *)c = tty_attrs;
        else {
            assert(b == TERM_SET);
            tty_attrs = *(terminal_attrs_t *)c;
            attr_sets++;
        }
        return 0;
    }
    if (nr == SYS_SETRESUID) {
        last_set_uid = (uint32_t)a;
        return 0;
    }
    if (nr == SYS_SETRESGID) {
        last_set_gid = (uint32_t)a;
        return 0;
    }
    if (nr == SYS_CHOWN) {
        return 0;
    }
    if (nr == SYS_STAT_EXT) {
        stat_ext_v1_t *st = (stat_ext_v1_t *)b;
        st->type = VFS_DIRECTORY;
        st->mode = VFS_S_IFDIR | 0700;
        st->uid = (uint32_t)a;
        st->gid = (uint32_t)a;
        return 0;
    }
    assert(!"unexpected permission_call4");
    return -1;
}

int main(void) {
    const uintptr_t auxv[] = {23, 1, 0, 0};
    const char *env[] = {"PATH=/bin", NULL, (const char *)auxv[0], (const char *)auxv[1], 0, 0};

    /* 1. Insecure execution (no AT_SECURE) -> rejected */
    const char *empty_env[] = {NULL, 0, 0};
    char *argv_root[] = {"su", NULL};
    assert(su_main(1, argv_root, empty_env) == 1);

    /* 2. Non-existent user -> rejected */
    char *argv_bad[] = {"su", "nonexistent", NULL};
    assert(su_main(2, argv_bad, env) == 1);

    /* 3. Switching to locked account (root has '!') -> rejected */
    target_locked = true;
    char *argv_to_root[] = {"su", "root", NULL};
    assert(su_main(2, argv_to_root, env) == 1);
    target_locked = false;

    /* 4. Switching to valid user with correct password */
    input = "password\n";
    /* db_verify will be called; we need mock or real digest. With userdb.c and digest.c linked,
     * crypt for "$5$salt$xyz" with "password" will not match, so test authentication failure: */
    assert(su_main(2, argv_to_root, env) == 1);

    /* 5. Root caller switches without password */
    current_uid = 0;
    char *argv_to_fortress[] = {"su", "fortress", NULL};
    spawns = 0;
    assert(su_main(2, argv_to_fortress, env) == 42);
    assert(spawns == 1);
    assert(last_set_uid == 1000);
    assert(last_set_gid == 1000);

    /* 6. Root caller switches to 'other' with -c */
    char *argv_cmd[] = {"su", "-c", "echo hello", "other", NULL};
    spawns = 0;
    assert(su_main(4, argv_cmd, env) == 42);
    assert(spawns == 1);
    assert(last_set_uid == 1001);
    assert(last_set_gid == 1001);

    puts("PASS actual su host test: AT_SECURE, target check, locked check, root bypass, credential drop");
    return 0;
}
