#include "history_persist.h"
#include "lineedit.h"
#include "syscall_abi.h"
#include "io.h"
#include "vfs.h"

#define HISTORY_PATH "/mnt/.fortress/history"
#define HISTORY_DIR  "/mnt/.fortress"
#define HISTORY_MAGIC "FOSHIS1\n"

static char io_buf[512];

static void put_number_str(char *buf, size_t *pos, uint64_t n) {
    char tmp[24];
    int tp = 0;
    if (n == 0) tmp[tp++] = '0';
    while (n > 0) {
        tmp[tp++] = (char)('0' + (n % 10));
        n /= 10;
    }
    while (tp > 0) {
        buf[(*pos)++] = tmp[--tp];
    }
}

bool history_save(void) {
    /* 1. Ensure directory /mnt/.fortress exists */
    long r = call(SYS_MKDIR, (uintptr_t)HISTORY_DIR, 0755, 0);
    if (r < 0 && r == SYSCALL_EROFS) {
        puts("history: persistent storage is read-only\n");
        return false;
    }

    /* 2. Open /mnt/.fortress/history with O_WRONLY | O_CREAT | O_TRUNC */
    long fd = call(SYS_OPEN, (uintptr_t)HISTORY_PATH, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, 0);
    if (fd < 0) {
        if (fd == SYSCALL_EROFS) {
            puts("history: persistent storage is read-only\n");
        }
        return false;
    }

    /* 3. Write magic header */
    const char *magic = HISTORY_MAGIC;
    size_t mlen = 0;
    while (magic[mlen]) mlen++;
    if (call(SYS_WRITE, fd, (uintptr_t)magic, mlen) <= 0) {
        (void)call(SYS_CLOSE, fd, 0, 0);
        return false;
    }

    /* 4. Write entries */
    size_t count = history_count();
    for (size_t i = 0; i < count; i++) {
        const char *entry = history_get(i);
        size_t elen = 0;
        while (entry[elen]) elen++;

        size_t bpos = 0;
        put_number_str(io_buf, &bpos, elen);
        io_buf[bpos++] = '\n';
        if (call(SYS_WRITE, fd, (uintptr_t)io_buf, bpos) <= 0) {
            (void)call(SYS_CLOSE, fd, 0, 0);
            return false;
        }

        if (elen > 0) {
            if (call(SYS_WRITE, fd, (uintptr_t)entry, elen) <= 0) {
                (void)call(SYS_CLOSE, fd, 0, 0);
                return false;
            }
        }
        char nl = '\n';
        if (call(SYS_WRITE, fd, (uintptr_t)&nl, 1) <= 0) {
            (void)call(SYS_CLOSE, fd, 0, 0);
            return false;
        }
    }

    (void)call(SYS_CLOSE, fd, 0, 0);

    /* 5. Sync storage */
    (void)call(SYS_SYNC, 0, 0, 0);
    return true;
}

bool history_load(void) {
    long fd = call(SYS_OPEN, (uintptr_t)HISTORY_PATH, VFS_O_RDONLY, 0);
    if (fd < 0) return false;

    /* Verify magic */
    char hdr[8];
    long rd = call(SYS_READ, fd, (uintptr_t)hdr, 8);
    if (rd < 8) {
        (void)call(SYS_CLOSE, fd, 0, 0);
        return false;
    }
    const char *magic = HISTORY_MAGIC;
    for (int i = 0; i < 8; i++) {
        if (hdr[i] != magic[i]) {
            (void)call(SYS_CLOSE, fd, 0, 0);
            return false;
        }
    }

    /* Read entries */
    static char line_buf[LINE_CAP];
    for (;;) {
        /* Read length decimal */
        uint64_t len = 0;
        char c = 0;
        bool got_digits = false;
        for (;;) {
            long n = call(SYS_READ, fd, (uintptr_t)&c, 1);
            if (n <= 0) break;
            if (c == '\n') break;
            if (c >= '0' && c <= '9') {
                len = len * 10 + (uint64_t)(c - '0');
                got_digits = true;
            } else {
                (void)call(SYS_CLOSE, fd, 0, 0);
                return false;
            }
        }
        if (!got_digits) break;
        if (len >= LINE_CAP) {
            (void)call(SYS_CLOSE, fd, 0, 0);
            return false;
        }

        size_t off = 0;
        while (off < len) {
            long n = call(SYS_READ, fd, (uintptr_t)(line_buf + off), len - off);
            if (n <= 0) {
                (void)call(SYS_CLOSE, fd, 0, 0);
                return false;
            }
            off += (size_t)n;
        }
        line_buf[len] = '\0';

        /* Consume trailing newline */
        (void)call(SYS_READ, fd, (uintptr_t)&c, 1);

        history_add(line_buf);
    }

    (void)call(SYS_CLOSE, fd, 0, 0);
    return true;
}
