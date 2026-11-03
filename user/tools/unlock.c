#include "common.h"
#include "userdb.h"
#include "../permissions_cli.h"

static userdb_t database;

static void secure_delay(void) {
    timespec_t req = { .tv_sec = 0, .tv_nsec = 500000000L }; // 500ms penalty
    (void)tool_syscall(SYS_NANOSLEEP, (uintptr_t)&req, 0, 0);
}

static bool read_password_stdin(char *buf, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap) {
        char c;
        long r = tool_syscall(SYS_READ, 0, (uintptr_t)&c, 1);
        if (r <= 0) break;
        if (c == '\n' || c == '\r') break;
        buf[n++] = c;
    }
    buf[n] = 0;
    return true;
}

#ifndef TOOL_HOST_TEST
void _start(void) __attribute__((naked, section(".text._start")));
void _start(void) {
    __asm__ volatile (
        "andq $-16, %%rsp\n"
        "xorl %%ebp, %%ebp\n"
        "call unlock_main\n"
        "movl %%eax, %%edi\n"
        "xorl %%eax, %%eax\n"
        "syscall\n"
        "ud2\n"
        ::: "memory"
    );
}
#endif

int unlock_main(int argc, char **argv) {
    uint32_t uid, euid, suid;
    if (tool_syscall(SYS_GETRESUID, (uintptr_t)&uid, (uintptr_t)&euid, (uintptr_t)&suid) < 0 || euid != 0) {
        return tool_error("desktop-unlock", "permission denied (not suid root)", 0);
    }

    bool loaded = false;
    for (int attempts = 0; attempts < 20; attempts++) {
        if (db_load(&database, true)) { loaded = true; break; }
        if (!db_busy()) break;
        timespec_t req = { .tv_sec = 0, .tv_nsec = 25000000L };
        (void)tool_syscall(SYS_NANOSLEEP, (uintptr_t)&req, 0, 0);
    }
    if (!loaded) return tool_error("desktop-unlock", "cannot read account database", 0);

    const db_user_t *u = NULL;
    if (argc >= 2 && argv[1] && argv[1][0]) {
        // Only real root (uid == 0) can specify an arbitrary user name
        if (uid != 0) {
            secure_delay();
            return tool_error("desktop-unlock", "unauthorized user selection", argv[1]);
        }
        u = db_user_name(&database, argv[1]);
    } else {
        // Non-root or unprivileged user session: lookup caller's real UID
        u = db_user_id(&database, uid);
    }

    if (!u) {
        secure_delay();
        return tool_error("desktop-unlock", "user does not exist", argc >= 2 ? argv[1] : 0);
    }

    const db_shadow_t *shadow = db_shadow_name(&database, u->name);
    if (!shadow || shadow->restricted) {
        secure_delay();
        return tool_error("desktop-unlock", "account is locked or restricted", u->name);
    }

    char password[DB_PASSWORD_MAX + 1] = {0};
    read_password_stdin(password, sizeof(password));

    bool ok = false;
    if (!*shadow->hash) {
        // Passwordless account (e.g. live-media operator)
        ok = (password[0] == 0);
    } else {
        ok = db_verify(password, shadow->hash);
    }

    db_wipe(password, sizeof(password));

    if (!ok) {
        secure_delay();
        return tool_error("desktop-unlock", "authentication failed for user", u->name);
    }

    return 0;
}
