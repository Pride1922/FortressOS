/* A standalone Ring 3 program. Only the public syscall ABI crosses into kernel. */
#include "types.h"
#include "vfs.h"

static char line[192];
static long call(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory", "cc");
    return nr;
}
static size_t length(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static bool equal(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static void write_bytes(const char *s, size_t n) { (void)call(1, 1, (uintptr_t)s, n); }
static void puts(const char *s) { write_bytes(s, length(s)); }
static void file_error(long error) {
    puts(error == -5 ? "No such file or directory.\n" : "File operation failed.\n");
}
static void list(const char *path) {
    vfs_stat_t st;
    long result = call(5, (uintptr_t)path, (uintptr_t)&st, 0);
    if (result < 0) { file_error(result); return; }
    if (st.type != VFS_DIRECTORY) { puts(path); puts("\n"); return; }
    long fd = call(2, (uintptr_t)path, 0, 0);
    if (fd < 0) { file_error(fd); return; }
    vfs_dirent_t entry;
    while ((result = call(6, fd, (uintptr_t)&entry, 0)) == 1) {
        puts(entry.name);
        puts(entry.type == VFS_DIRECTORY ? "/\n" : "\n");
    }
    if (result < 0) file_error(result);
    (void)call(3, fd, 0, 0);
}
static void cat(const char *path) {
    vfs_stat_t st;
    long result = call(5, (uintptr_t)path, (uintptr_t)&st, 0);
    if (result < 0) { file_error(result); return; }
    if (st.type != VFS_FILE) { puts("Not a regular file.\n"); return; }
    long fd = call(2, (uintptr_t)path, 0, 0);
    if (fd < 0) { file_error(fd); return; }
    char buf[512];
    bool newline = true;
    while ((result = call(4, fd, (uintptr_t)buf, sizeof(buf))) > 0) {
        /* Display text safely; binary control bytes cannot edit the prompt. */
        for (long i = 0; i < result; i++) {
            unsigned char c = buf[i];
            if (c != '\n' && c != '\t' && (c < 32 || c > 126)) buf[i] = '.';
        }
        newline = buf[result - 1] == '\n';
        write_bytes(buf, result);
    }
    if (!newline) puts("\n");
    if (result < 0) file_error(result);
    (void)call(3, fd, 0, 0);
}
static bool read_line(void) {
    size_t used = 0;
    bool overflow = false;
    for (;;) {
        char c;
        if (call(4, 0, (uintptr_t)&c, 1) != 1) { puts("Input unavailable.\n"); return false; }
        if (c == '\n') {
            puts("\n");
            line[used] = 0;
            if (overflow) { puts("Line too long; command discarded.\n"); line[0] = 0; }
            return true;
        }
        if (overflow) continue; /* Never execute a silently truncated command. */
        if (c == '\b' || c == 127) {
            if (used) { used--; puts("\b \b"); }
        } else if (c == 3 || c == 21) {
            while (used) { used--; puts("\b \b"); }
        } else if (c >= 32 && c <= 126) {
            if (used + 1 == sizeof(line)) { overflow = true; continue; }
            line[used++] = c;
            write_bytes(&c, 1);
        }
    }
}
void shell_main(void) {
    /* Verify stdin validates user buffers before it ever sleeps or consumes a key. */
    if (call(4, 0, 0, 1) != -2 || call(4, 0, (uintptr_t)"readonly", 1) != -2 ||
        call(4, 0, 0, 0) != 0) {
        puts("[FAIL] stdin validation\n");
        return;
    }
    puts("\nFortressOS shell (Ring 3)\nType help for commands. Paths start at /.\n");
    for (;;) {
        puts("fortress> ");
        if (!read_line()) return;
        char *cmd = line;
        while (*cmd == ' ') cmd++;
        char *arg = cmd;
        while (*arg && *arg != ' ') arg++;
        if (*arg) *arg++ = 0;
        while (*arg == ' ') arg++;
        size_t n = length(arg);
        while (n && arg[n - 1] == ' ') arg[--n] = 0;
        if (!*cmd) continue;
        if (equal(cmd, "help")) {
            puts("help           Show commands\nls [path]      List files (default /)\n"
                 "cat /path      Read a text file\necho text      Print text\n"
                 "exit           Restart the shell\nBackspace edits the current line. US keyboard layout.\n");
        } else if (equal(cmd, "echo")) { puts(arg); puts("\n"); }
        else if (equal(cmd, "ls")) list(*arg ? arg : "/");
        else if (equal(cmd, "cat")) {
            if (*arg) cat(arg); else puts("Usage: cat /path\n");
        } else if (equal(cmd, "exit")) return;
        else puts("Unknown command. Type help.\n");
    }
}
