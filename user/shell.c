/* A standalone Ring 3 program. Only the public syscall ABI crosses into kernel. */
#include "types.h"
#include "vfs.h"
#include "syscall.h"

static char line[192];
static int64_t last_status = 0;
static long call(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory", "cc");
    return nr;
}
static size_t length(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static bool equal(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
#define WRITE_CHUNK 4096

static void puts(const char *s);      /* defined below */
static void put_dec(size_t val);      /* defined below */

static void write_bytes(const char *s, size_t n) {
    size_t off = 0;
    while (off < n) {
        size_t chunk = n - off;
        if (chunk > WRITE_CHUNK) chunk = WRITE_CHUNK;
        long r = call(SYS_WRITE, 1, (uintptr_t)(s + off), chunk);
        if (r < 0) {
            puts("write error "); put_dec(-r); puts("\n");
            return;
        }
        off += (size_t)r;
        if (r == 0) break;  /* avoid infinite loop on buggy drivers */
    }
}
static void puts(const char *s) { write_bytes(s, length(s)); }
static void file_error(long error) {
    if (error == SYSCALL_EROFS) puts("Read-only filesystem.\n");
    else puts(error == SYSCALL_ENOENT ? "No such file or directory.\n" : "File operation failed.\n");
}

static char dmesg_buf[DMESG_SIZE];

static void list(const char *path) {
    vfs_stat_t st;
    long result = call(5, (uintptr_t)path, (uintptr_t)&st, 0);
    if (result < 0) { file_error(result); last_status = 1; return; }
    if (st.type != VFS_DIRECTORY) { puts(path); puts("\n"); last_status = 0; return; }
    long fd = call(2, (uintptr_t)path, 0, 0);
    if (fd < 0) { file_error(fd); last_status = 1; return; }
    vfs_dirent_t entry;
    while ((result = call(6, fd, (uintptr_t)&entry, 0)) == 1) {
        puts(entry.name);
        puts(entry.type == VFS_DIRECTORY ? "/\n" : "\n");
    }
    if (result < 0) { file_error(result); last_status = 1; }
    else { last_status = 0; }
    (void)call(3, fd, 0, 0);
}
static void cat(const char *path) {
    vfs_stat_t st;
    long result = call(5, (uintptr_t)path, (uintptr_t)&st, 0);
    if (result < 0) { file_error(result); last_status = 1; return; }
    if (st.type != VFS_FILE) { puts("Not a regular file.\n"); last_status = 1; return; }
    long fd = call(2, (uintptr_t)path, 0, 0);
    if (fd < 0) { file_error(fd); last_status = 1; return; }
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
    if (result < 0) { file_error(result); last_status = 1; }
    else { last_status = 0; }
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

#define MAX_EDITOR_LINES 64
#define MAX_LINE_LEN     128

static char editor_lines[MAX_EDITOR_LINES][MAX_LINE_LEN];
static size_t editor_line_count = 0;
static bool editor_modified = false;
static bool editor_save_disabled = false;
static char editor_path[256];
static char edit_line[192];
static char file_buf[8192];

static void put_dec(size_t val) {
    char buf[24];
    size_t i = 0;
    if (val == 0) { puts("0"); return; }
    while (val > 0) {
        buf[i++] = (char)('0' + (val % 10));
        val /= 10;
    }
    for (size_t j = 0; j < i / 2; j++) {
        char tmp = buf[j];
        buf[j] = buf[i - 1 - j];
        buf[i - 1 - j] = tmp;
    }
    buf[i] = 0;
    puts(buf);
}

static size_t parse_dec(const char *s) {
    size_t n = 0;
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (size_t)(*s - '0');
        s++;
    }
    return n;
}

static void editor_copy_str(char *dst, const char *src, size_t max_len) {
    size_t i = 0;
    while (src[i] && i + 1 < max_len) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static bool read_edit_line(char *dst, size_t max_len) {
    size_t used = 0;
    bool overflow = false;
    for (;;) {
        char c;
        if (call(4, 0, (uintptr_t)&c, 1) != 1) { puts("Input unavailable.\n"); return false; }
        if (c == '\n') {
            puts("\n");
            dst[used] = 0;
            if (overflow) puts("Line truncated to fit buffer.\n");
            return true;
        }
        if (c == 4) { /* Ctrl+D EOF */
            puts("\n");
            dst[0] = '.';
            dst[1] = 0;
            return true;
        }
        if (overflow) continue;
        if (c == '\b' || c == 127) {
            if (used) { used--; puts("\b \b"); }
        } else if (c == 3 || c == 21) {
            while (used) { used--; puts("\b \b"); }
        } else if (c >= 32 && c <= 126) {
            if (used + 1 >= max_len) { overflow = true; continue; }
            dst[used++] = c;
            write_bytes(&c, 1);
        }
    }
}

static void editor_print_line(size_t index) {
    if (index >= editor_line_count) {
        puts("Invalid line number.\n");
        return;
    }
    put_dec(index + 1);
    puts(": ");
    puts(editor_lines[index]);
    puts("\n");
}

static void editor_print_all(void) {
    if (editor_line_count == 0) {
        puts("Buffer empty.\n");
        return;
    }
    for (size_t i = 0; i < editor_line_count; i++) {
        editor_print_line(i);
    }
}

static void editor_append_mode(void) {
    puts("Append mode, end with '.' on a blank line:\n");
    for (;;) {
        if (editor_line_count >= MAX_EDITOR_LINES) {
            puts("Buffer full (64 lines max).\n");
            return;
        }
        puts("> ");
        if (!read_edit_line(edit_line, sizeof(edit_line))) return;
        if (equal(edit_line, ".")) return;
        editor_copy_str(editor_lines[editor_line_count], edit_line, MAX_LINE_LEN);
        editor_line_count++;
        editor_modified = true;
    }
}

static void editor_insert_mode(size_t line_num) {
    if (line_num == 0 || line_num > editor_line_count + 1) {
        puts("Invalid line number.\n");
        return;
    }
    if (editor_line_count >= MAX_EDITOR_LINES) {
        puts("Buffer full (64 lines max).\n");
        return;
    }
    size_t idx = line_num - 1;
    puts("Insert text before line ");
    put_dec(line_num);
    puts(", end with '.' on a blank line:\n");
    for (;;) {
        if (editor_line_count >= MAX_EDITOR_LINES) {
            puts("Buffer full (64 lines max).\n");
            return;
        }
        puts("> ");
        if (!read_edit_line(edit_line, sizeof(edit_line))) return;
        if (equal(edit_line, ".")) return;
        for (size_t i = editor_line_count; i > idx; i--) {
            editor_copy_str(editor_lines[i], editor_lines[i - 1], MAX_LINE_LEN);
        }
        editor_copy_str(editor_lines[idx], edit_line, MAX_LINE_LEN);
        editor_line_count++;
        idx++;
        editor_modified = true;
    }
}

static void editor_delete_line(size_t line_num) {
    if (line_num == 0 || line_num > editor_line_count) {
        puts("Invalid line number.\n");
        return;
    }
    size_t idx = line_num - 1;
    for (size_t i = idx; i + 1 < editor_line_count; i++) {
        editor_copy_str(editor_lines[i], editor_lines[i + 1], MAX_LINE_LEN);
    }
    editor_line_count--;
    editor_modified = true;
    puts("Deleted line ");
    put_dec(line_num);
    puts(".\n");
}

static void editor_change_line(size_t line_num) {
    if (line_num == 0 || line_num > editor_line_count) {
        puts("Invalid line number.\n");
        return;
    }
    size_t idx = line_num - 1;
    puts("Current: ");
    puts(editor_lines[idx]);
    puts("\nEnter replacement: ");
    if (!read_edit_line(edit_line, sizeof(edit_line))) return;
    editor_copy_str(editor_lines[idx], edit_line, MAX_LINE_LEN);
    editor_modified = true;
    puts("Updated line ");
    put_dec(line_num);
    puts(".\n");
}

static void editor_save(void) {
    if (!editor_path[0]) {
        puts("No file path specified.\n");
        return;
    }
    if (editor_save_disabled) {
        puts("[EDIT] Save refused: file was incompletely loaded or exceeds editor limits.\n");
        return;
    }
    /* VFS_O_WRONLY (1) | VFS_O_CREAT (0x40) | VFS_O_TRUNC (0x200) */
    long fd = call(2, (uintptr_t)editor_path, 1 | 0x40 | 0x200, 0);
    if (fd < 0) {
        puts("[EDIT] Failed to open file for writing: ");
        file_error(fd);
        if (fd == SYSCALL_EROFS && editor_path[0] == '/' &&
            editor_path[1] == 'm' && editor_path[2] == 'n' &&
            editor_path[3] == 't' && editor_path[4] == '/') {
            puts("[EDIT] To save under /mnt, boot the Writable USB entry for your test USB.\n");
            puts("[EDIT] Check that boot reports /mnt mounted read-write; a fallback remains read-only.\n");
            puts("[EDIT] Buffer preserved in memory only; copy your text before rebooting.\n");
        }
        return;
    }

    size_t total_written = 0;
    bool write_err = false;
    for (size_t i = 0; i < editor_line_count; i++) {
        size_t len = length(editor_lines[i]);
        if (len > 0) {
            long w = call(1, fd, (uintptr_t)editor_lines[i], len);
            if (w != (long)len) { write_err = true; break; }
            total_written += (size_t)w;
        }
        char nl = '\n';
        long w = call(1, fd, (uintptr_t)&nl, 1);
        if (w != 1) { write_err = true; break; }
        total_written += (size_t)w;
    }

    (void)call(3, fd, 0, 0);

    if (write_err) {
        puts("[EDIT] Write error: save failed or incomplete. In-memory buffer preserved.\n");
    } else {
        editor_modified = false;
        puts("[EDIT] Saved ");
        put_dec(total_written);
        puts(" bytes (");
        put_dec(editor_line_count);
        puts(" lines) to ");
        puts(editor_path);
        puts(".\n");
    }
}

static void editor_show_stats(void) {
    puts("File: ");
    puts(editor_path);
    puts(" | Lines: ");
    put_dec(editor_line_count);
    puts("/");
    put_dec(MAX_EDITOR_LINES);
    puts(" | Modified: ");
    puts(editor_modified ? "yes" : "no");
    puts(" | Storage: ");
    if (editor_save_disabled) {
        puts("save disabled (incomplete load/exceeds limits)\n");
    } else {
        puts(editor_modified ? "unwritten changes in buffer (type 'w' to save)\n" : "synced to disk\n");
    }
}

static void editor_show_help(void) {
    puts("Editor commands:\n"
         "  p          Print all lines\n"
         "  p <n>      Print line n\n"
         "  a          Append lines at end (terminate with '.')\n"
         "  i <n>      Insert lines before line n (terminate with '.')\n"
         "  d <n>      Delete line n\n"
         "  c <n>      Change line n\n"
         "  w          Save buffer to file\n"
         "  stats      Show buffer info and modified state\n"
         "  help       Show this help\n"
         "  q          Exit editor to shell\n");
}

static void editor_load(const char *path) {
    if (length(path) >= sizeof(editor_path)) {
        puts("Path exceeds maximum length (256).\n");
        editor_path[0] = 0;
        return;
    }
    editor_line_count = 0;
    editor_modified = false;
    editor_save_disabled = false;
    editor_copy_str(editor_path, path, sizeof(editor_path));

    vfs_stat_t st;
    long res = call(5, (uintptr_t)path, (uintptr_t)&st, 0);
    if (res < 0) {
        puts("[EDIT] New buffer for ");
        puts(path);
        puts(".\n");
        puts("Type 'help' for commands, 'p' to view, 'w' to save, 'q' to return to shell.\n");
        return;
    }
    if (st.type != VFS_FILE) {
        puts("Not a regular file.\n");
        editor_path[0] = 0;
        return;
    }

    if (st.size > sizeof(file_buf)) {
        puts("[EDIT] Warning: file size (");
        put_dec(st.size);
        puts(" bytes) exceeds buffer capacity. Save disabled.\n");
        editor_save_disabled = true;
    }

    long fd = call(2, (uintptr_t)path, 0, 0);
    if (fd < 0) {
        file_error(fd);
        editor_path[0] = 0;
        return;
    }

    size_t total_read = 0;
    long n;
    while (total_read < sizeof(file_buf) &&
           (n = call(4, fd, (uintptr_t)(file_buf + total_read), sizeof(file_buf) - total_read)) > 0) {
        total_read += (size_t)n;
    }
    (void)call(3, fd, 0, 0);

    if (n < 0 || (st.size <= sizeof(file_buf) && total_read < (size_t)st.size)) {
        puts("[EDIT] Warning: incomplete file read. Save disabled.\n");
        editor_save_disabled = true;
    }

    size_t line_pos = 0;
    bool line_truncated = false;
    bool lines_exceeded = false;
    for (size_t i = 0; i < total_read; i++) {
        char c = file_buf[i];
        if (c == '\r') continue;
        if (c == '\n') {
            if (editor_line_count < MAX_EDITOR_LINES) {
                editor_lines[editor_line_count][line_pos] = 0;
                editor_line_count++;
                line_pos = 0;
            } else {
                lines_exceeded = true;
            }
        } else {
            if (editor_line_count < MAX_EDITOR_LINES) {
                if (line_pos + 1 < MAX_LINE_LEN) {
                    editor_lines[editor_line_count][line_pos++] = c;
                } else {
                    line_truncated = true;
                }
            } else {
                lines_exceeded = true;
            }
        }
    }
    if (line_pos > 0) {
        if (editor_line_count < MAX_EDITOR_LINES) {
            editor_lines[editor_line_count][line_pos] = 0;
            editor_line_count++;
        } else {
            lines_exceeded = true;
        }
    }

    if (line_truncated) {
        puts("[EDIT] Warning: line exceeded 127 characters. Save disabled.\n");
        editor_save_disabled = true;
    }
    if (lines_exceeded) {
        puts("[EDIT] Warning: file exceeded 64 lines. Save disabled.\n");
        editor_save_disabled = true;
    }

    puts("[EDIT] Loaded ");
    put_dec(total_read);
    puts(" bytes (");
    put_dec(editor_line_count);
    puts(" lines) from ");
    puts(path);
    puts(".\nType 'help' for commands, 'p' to view, 'w' to save, 'q' to return to shell.\n");
}

static void editor_loop(void) {
    for (;;) {
        puts("edit> ");
        if (!read_edit_line(edit_line, sizeof(edit_line))) return;

        char *cmd = edit_line;
        while (*cmd == ' ') cmd++;
        char *arg = cmd;
        while (*arg && *arg != ' ') arg++;
        if (*arg) *arg++ = 0;
        while (*arg == ' ') arg++;
        size_t n = length(arg);
        while (n && arg[n - 1] == ' ') arg[--n] = 0;

        if (!*cmd) continue;

        if (equal(cmd, "p") || equal(cmd, "print")) {
            if (*arg) {
                size_t l = parse_dec(arg);
                editor_print_line(l ? l - 1 : 0);
            } else {
                editor_print_all();
            }
        } else if (equal(cmd, "a") || equal(cmd, "append")) {
            editor_append_mode();
        } else if (equal(cmd, "i") || equal(cmd, "insert")) {
            size_t l = parse_dec(arg);
            if (l == 0) puts("Usage: i <line_number>\n");
            else editor_insert_mode(l);
        } else if (equal(cmd, "d") || equal(cmd, "delete")) {
            size_t l = parse_dec(arg);
            if (l == 0) puts("Usage: d <line_number>\n");
            else editor_delete_line(l);
        } else if (equal(cmd, "c") || equal(cmd, "change")) {
            size_t l = parse_dec(arg);
            if (l == 0) puts("Usage: c <line_number>\n");
            else editor_change_line(l);
        } else if (equal(cmd, "w") || equal(cmd, "write") || equal(cmd, "save")) {
            editor_save();
        } else if (equal(cmd, "stats")) {
            editor_show_stats();
        } else if (equal(cmd, "help")) {
            editor_show_help();
        } else if (equal(cmd, "q") || equal(cmd, "quit")) {
            if (editor_modified) {
                puts("Exited editor. In-memory changes discarded (type 'w' to save).\n");
            } else {
                puts("Exited editor.\n");
            }
            return;
        } else {
            puts("Unknown editor command. Type 'help'.\n");
        }
    }
}

static void run_program(char *args) {
    if (!*args) { puts("Usage: run /path [arg...]\n"); last_status = 1; return; }
    const char *argv[33];
    int argc = 0;
    char *p = args;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (argc >= 32) {
            puts("Too many arguments (max 32).\n");
            last_status = 1;
            return;
        }
        if (*p == '"') {
            p++;
            argv[argc++] = p;
            while (*p && *p != '"') p++;
            if (*p) {
                *p++ = '\0';
            }
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ') p++;
            if (*p) {
                *p++ = '\0';
            }
        }
    }
    if (argc == 0) {
        puts("Usage: run /path [arg...]\n");
        last_status = 1;
        return;
    }
    argv[argc] = NULL;

    long pid = call(SYS_SPAWN, (uintptr_t)argv[0], (uintptr_t)argv, 0);
    if (pid < 0) {
        switch (pid) {
            case SYSCALL_ENOENT: puts("No such file or directory.\n"); last_status = 127; break;
            case SYSCALL_ENOEXEC: puts("Invalid executable.\n"); last_status = 126; break;
            case SYSCALL_ENOMEM: puts("Out of memory or process capacity.\n"); last_status = 1; break;
            case SYSCALL_EISDIR: puts("Not a regular file.\n"); last_status = 126; break;
            case SYSCALL_EFBIG: puts("Executable exceeds 4 MiB limit.\n"); last_status = 126; break;
            case SYSCALL_E2BIG: puts("Argument list too long.\n"); last_status = 1; break;
            default: puts("Unable to load executable.\n"); last_status = 1; break;
        }
        return;
    }
    int64_t status;
    if (call(SYS_WAIT, pid, (uintptr_t)&status, 0) < 0) {
        puts("Unable to wait for child process.\n");
        last_status = 1;
    } else {
        last_status = status;
        if (status >= 128 && status < 160) {
            puts("[PROCESS] Faulted (exception vector ");
            put_dec((uint64_t)status - 128);
            puts(")\n");
        } else if (status) {
            puts("[PROCESS] Exit status ");
            if (status < 0) { puts("-"); put_dec(0 - (uint64_t)status); }
            else put_dec((uint64_t)status);
            puts("\n");
        }
    }
}

static void echo_cmd(const char *arg) {
    if (!arg) { puts("\n"); last_status = 0; return; }
    while (*arg) {
        if (arg[0] == '$' && arg[1] == '?') {
            if (last_status < 0) { puts("-"); put_dec(0 - (uint64_t)last_status); }
            else put_dec((uint64_t)last_status);
            arg += 2;
        } else {
            char ch[2] = { *arg++, 0 };
            puts(ch);
        }
    }
    puts("\n");
    last_status = 0;
}

static void dmesg_cmd(const char *arg) {
    long n = call(SYS_DMESG, (uintptr_t)dmesg_buf, DMESG_SIZE, 0);
    if (n < 0) {
        puts("dmesg: kernel log unavailable (");
        put_dec(-n);
        puts(")\n");
        last_status = 1;
        return;
    }

    /* dmesg /path — save the log to a file instead of printing it */
    if (*arg) {
        /* VFS_O_WRONLY (1) | VFS_O_CREAT (0x40) | VFS_O_TRUNC (0x200) */
        long fd = call(2, (uintptr_t)arg, 1 | 0x40 | 0x200, 0);
        if (fd < 0) { file_error(fd); last_status = 1; return; }

        size_t total = (size_t)n, off = 0;
        bool err = false;
        while (off < total) {
            size_t chunk = total - off;
            if (chunk > WRITE_CHUNK) chunk = WRITE_CHUNK;
            long w = call(1, fd, (uintptr_t)(dmesg_buf + off), chunk);
            if (w <= 0) { err = true; break; }
            off += (size_t)w;
        }
        (void)call(3, fd, 0, 0);

        if (err) {
            puts("dmesg: write failed, file may be incomplete\n");
            last_status = 1;
        } else {
            puts("Saved ");
            put_dec(total);
            puts(" bytes to ");
            puts(arg);
            puts("\n");
            last_status = 0;
        }
        return;
    }

    /* plain dmesg — print to console */
    put_dec((uint64_t)n);
    puts(" bytes\n");
    write_bytes(dmesg_buf, (size_t)n);
    if (dmesg_buf[n - 1] != '\n') puts("\n");
    last_status = 0;
}

static void execute_simple_command(char *cmd_line) {
    while (*cmd_line == ' ') cmd_line++;
    char *cmd = cmd_line;
    char *arg = cmd;
    while (*arg && *arg != ' ') arg++;
    if (*arg) *arg++ = 0;
    while (*arg == ' ') arg++;
    size_t n = length(arg);
    while (n && arg[n - 1] == ' ') arg[--n] = 0;
    if (!*cmd) return;

    if (equal(cmd, "help")) {
        puts("help           Show commands\nls [path]      List files (default /)\n"
             "dmesg [path]   Print or save the boot log\n"
             "cat /path      Read a text file\nedit /path     Text editor\n"
             "mkdir /path    Create a directory\nrm /path       Remove a file or empty directory\n"
             "mv /old /new   Rename or move a file/directory\n"
             "sync           Flush writable /mnt storage to device\n"
             "echo [text]    Print text (supports $?)\n"
             "run /path [args]Run a program with optional arguments\n"
             "layout [layout]Switch layout (us | azerty)\n"
             "reboot         Restart the system\nshutdown       Power off the system\n"
             "exit           Restart the shell\nBackspace edits the current line.\n"
             "Supports command chaining with && and ||.\n");
        last_status = 0;
    } else if (equal(cmd, "echo")) {
        echo_cmd(arg);
    } else if (equal(cmd, "ls")) {
        list(*arg ? arg : "/");
    } else if (equal(cmd, "run")) {
        run_program(arg);
    } else if (equal(cmd, "cat")) {
        if (*arg) cat(arg);
        else { puts("Usage: cat /path\n"); last_status = 1; }
    } else if (equal(cmd, "edit")) {
        if (*arg) {
            editor_load(arg);
            if (editor_path[0]) editor_loop();
            last_status = 0;
        } else {
            puts("Usage: edit /path\n");
            last_status = 1;
        }
    } else if (equal(cmd, "mkdir")) {
        if (*arg) {
            long r = call(11, (uintptr_t)arg, 0755, 0);
            if (r < 0) {
                puts("mkdir: cannot create directory\n");
                last_status = 1;
            } else {
                last_status = 0;
            }
        } else {
            puts("Usage: mkdir /path\n");
            last_status = 1;
        }
    } else if (equal(cmd, "rm")) {
        if (*arg) {
            long r = call(12, (uintptr_t)arg, 0, 0);
            if (r < 0) {
                if (r == -19) puts("rm: directory not empty\n");
                else puts("rm: cannot remove\n");
                last_status = 1;
            } else {
                last_status = 0;
            }
        } else {
            puts("Usage: rm /path\n");
            last_status = 1;
        }
    } else if (equal(cmd, "mv")) {
        char *src = arg;
        char *dst = src;
        while (*dst && *dst != ' ') dst++;
        if (*dst) *dst++ = 0;
        while (*dst == ' ') dst++;
        if (*src && *dst) {
            long r = call(13, (uintptr_t)src, (uintptr_t)dst, 0);
            if (r < 0) {
                puts("mv: cannot rename\n");
                last_status = 1;
            } else {
                last_status = 0;
            }
        } else {
            puts("Usage: mv /old /new\n");
            last_status = 1;
        }
    } else if (equal(cmd, "layout")) {
        if (equal(arg, "azerty")) {
            (void)call(8, 1, 0, 0);
            puts("Keyboard layout set to Belgian AZERTY.\n");
            last_status = 0;
        } else if (equal(arg, "us")) {
            (void)call(8, 0, 0, 0);
            puts("Keyboard layout set to US QWERTY.\n");
            last_status = 0;
        } else if (!*arg) {
            long curr = call(8, (uintptr_t)-1, 0, 0);
            if (curr == 1) puts("Active keyboard layout: Belgian AZERTY\n");
            else puts("Active keyboard layout: US QWERTY\n");
            last_status = 0;
        } else {
            puts("Usage: layout [us | azerty]\n");
            last_status = 1;
        }
    } else if (equal(cmd, "reboot")) {
        puts("Restarting system...\n");
        (void)call(7, 1, 0, 0);
        last_status = 0;
    } else if (equal(cmd, "shutdown") || equal(cmd, "poweroff")) {
        puts("Shutting down system...\n");
        (void)call(7, 2, 0, 0);
        last_status = 0;
    } else if (equal(cmd, "sync")) {
        long r = call(14, 0, 0, 0);
        if (r == 0) {
            puts("Filesystem synced.\n");
            last_status = 0;
        } else {
            puts("Sync failed (check USB connection or mount mode).\n");
            last_status = 1;
        }
    } else if (equal(cmd, "exit")) {
        call(0, (uintptr_t)last_status, 0, 0);
    } else if (equal(cmd, "dmesg")) {
    dmesg_cmd(arg);
    } else {
        puts("Unknown command. Type help.\n");
        last_status = 127;
    }
}

static void execute_line(char *p) {
    while (*p) {
        /* Find next delimiter: "&&" or "||" */
        char *delim = p;
        int next_op = 0; /* 0 = none, 1 = &&, 2 = || */
        while (*delim) {
            if (delim[0] == '&' && delim[1] == '&') {
                next_op = 1;
                break;
            }
            if (delim[0] == '|' && delim[1] == '|') {
                next_op = 2;
                break;
            }
            delim++;
        }
        if (next_op != 0) {
            *delim = '\0';
        }
        execute_simple_command(p);
        if (next_op == 0) break;
        p = delim + 2;
        /* If next_op == 1 (&&) and last_status != 0, skip until next || or end */
        /* If next_op == 2 (||) and last_status == 0, skip until next && or end */
        while (next_op != 0 &&
               ((next_op == 1 && last_status != 0) ||
                (next_op == 2 && last_status == 0))) {
            char *skip = p;
            next_op = 0;
            while (*skip) {
                if (skip[0] == '&' && skip[1] == '&') {
                    next_op = 1;
                    p = skip + 2;
                    break;
                }
                if (skip[0] == '|' && skip[1] == '|') {
                    next_op = 2;
                    p = skip + 2;
                    break;
                }
                skip++;
            }
            if (next_op == 0) {
                p = skip;
                break;
            }
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
        execute_line(line);
    }
}
