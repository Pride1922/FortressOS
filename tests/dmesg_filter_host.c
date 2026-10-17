/* Host unit tests for dmesg facility filtering (-f, --facility) under ASan/UBSan */
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

int snprintf(char *str, size_t size, const char *format, ...);

#include "types.h"
#include "vfs.h"
#include "syscall_abi.h"
#include "shell/builtin_exec.h"
#include "shell/builtins.h"

static char s_out_buf[16384];
static size_t s_out_len = 0;
static char s_err_buf[4096];
static size_t s_err_len = 0;

static char s_mock_file[16384];
static size_t s_mock_file_len = 0;
static long s_last_open_flags = 0;

static const char *s_mock_dmesg =
    "[BOOT] Kernel initialized\n"
    "[PMM] Initialized 2048 MiB physical memory\n"
    "[VMM] Kernel PML4 activated\n"
    "[NET] Link carrier detected on eth0\n"
    "[EXT4] Mounted filesystem /mnt (rw)\n"
    "[USB] Device connected on port 1\n"
    "[net] Packet received: length 64\n"
    "[NETWORK] Stray packet dropped\n"
    "  [SMP] AP cores started\n"
    "[EXT4] Journal transaction committed\n"
    "[NET] Socket connection established\n";

static const char *s_mock_dmesg_levels =
    "<0>[TEST] test log level 0 (emerg)\n"
    "<1>[TEST] test log level 1 (alert)\n"
    "<2>[TEST] test log level 2 (crit)\n"
    "<3>[TEST] test log level 3 (err)\n"
    "<4>[TEST] test log level 4 (warn)\n"
    "<5>[TEST] test log level 5 (notice)\n"
    "<6>[TEST] test log level 6 (info)\n"
    "<7>[TEST] test log level 7 (debug)\n"
    "<3>[NET] e1000: Link down error\n"
    "<6>[NET] e1000: Interface up info\n";

static const char *s_current_dmesg = NULL;

void mock_reset(void) {
    s_out_len = 0;
    s_out_buf[0] = '\0';
    s_err_len = 0;
    s_err_buf[0] = '\0';
    s_mock_file_len = 0;
    s_mock_file[0] = '\0';
    s_last_open_flags = 0;
    s_current_dmesg = NULL;
}

long write_bytes_fd(int fd, const void *buf, size_t n) {
    if (fd == 1) {
        if (s_out_len + n < sizeof(s_out_buf)) {
            memcpy(s_out_buf + s_out_len, buf, n);
            s_out_len += n;
            s_out_buf[s_out_len] = '\0';
        }
        return (long)n;
    } else if (fd == 2) {
        if (s_err_len + n < sizeof(s_err_buf)) {
            memcpy(s_err_buf + s_err_len, buf, n);
            s_err_len += n;
            s_err_buf[s_err_len] = '\0';
        }
        return (long)n;
    }
    return (long)n;
}

size_t length(const char *s) { size_t n = 0; while (s && s[n]) n++; return n; }
bool equal(const char *a, const char *b) {
    if (!a || !b) return a == b;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

const void *db_group(const void *db, const char *name) { (void)db; (void)name; return NULL; }
const void *db_passwd(const void *db, const char *name) { (void)db; (void)name; return NULL; }
const void *db_group_id(const void *db, uint32_t gid) { (void)db; (void)gid; return NULL; }
const void *db_user_id(const void *db, uint32_t uid) { (void)db; (void)uid; return NULL; }

void puts(const char *s) {
    if (s) write_bytes_fd(1, s, length(s));
}

void put_dec(size_t val) {
    char buf[32];
    int pos = 0;
    if (val == 0) buf[pos++] = '0';
    else {
        char tmp[32];
        int tpos = 0;
        while (val > 0) { tmp[tpos++] = '0' + (val % 10); val /= 10; }
        while (tpos > 0) buf[pos++] = tmp[--tpos];
    }
    buf[pos] = '\0';
    write_bytes_fd(1, buf, pos);
}

void file_error_err(long error) {
    (void)error;
}

long call(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    if (nr == SYS_DMESG) {
        char *buf = (char *)a;
        size_t cap = (size_t)b;
        const char *src = s_current_dmesg ? s_current_dmesg : s_mock_dmesg;
        size_t mlen = strlen(src);
        if (mlen > cap) mlen = cap;
        memcpy(buf, src, mlen);
        return (long)mlen;
    }
    if (nr == SYS_OPEN) {
        s_last_open_flags = (long)b;
        return 10; /* dummy fd */
    }
    if (nr == SYS_WRITE) {
        int fd = (int)a;
        const char *buf = (const char *)b;
        size_t n = (size_t)c;
        if (fd == 10) {
            if (s_last_open_flags & VFS_O_TRUNC) {
                s_mock_file_len = 0;
            }
            if (s_mock_file_len + n < sizeof(s_mock_file)) {
                memcpy(s_mock_file + s_mock_file_len, buf, n);
                s_mock_file_len += n;
                s_mock_file[s_mock_file_len] = '\0';
            }
            return (long)n;
        }
    }
    if (nr == SYS_CLOSE) {
        return 0;
    }
    return 0;
}

static void print_msg(const char *s) {
    (void)write(1, s, strlen(s));
}

/* 1. Filter parsing variations: -f NET, -f=NET, -fNET, --facility=NET, --facility NET */
static void test_filter_parsing(void) {
    const char *expected =
        "[NET] Link carrier detected on eth0\n"
        "[net] Packet received: length 64\n"
        "[NET] Socket connection established\n";

    /* -f NET */
    mock_reset();
    const char *args1[] = {"dmesg", "-f", "NET"};
    int rc = exec_dmesg(3, args1);
    assert(rc == 0);
    assert(strcmp(s_out_buf, expected) == 0);

    /* -f=NET */
    mock_reset();
    const char *args2[] = {"dmesg", "-f=NET"};
    rc = exec_dmesg(2, args2);
    assert(rc == 0);
    assert(strcmp(s_out_buf, expected) == 0);

    /* -fNET */
    mock_reset();
    const char *args3[] = {"dmesg", "-fNET"};
    rc = exec_dmesg(2, args3);
    assert(rc == 0);
    assert(strcmp(s_out_buf, expected) == 0);

    /* --facility=NET */
    mock_reset();
    const char *args4[] = {"dmesg", "--facility=NET"};
    rc = exec_dmesg(2, args4);
    assert(rc == 0);
    assert(strcmp(s_out_buf, expected) == 0);

    /* --facility NET */
    mock_reset();
    const char *args5[] = {"dmesg", "--facility", "NET"};
    rc = exec_dmesg(3, args5);
    assert(rc == 0);
    assert(strcmp(s_out_buf, expected) == 0);

    print_msg("PASS: filter parsing (-f NET, -f=NET, -fNET, --facility=NET, --facility NET)\n");
}

/* 2. Case-insensitive matching and exact tag check */
static void test_case_insensitive_and_exact(void) {
    const char *expected =
        "[NET] Link carrier detected on eth0\n"
        "[net] Packet received: length 64\n"
        "[NET] Socket connection established\n";

    /* Lowercase -f net */
    mock_reset();
    const char *args1[] = {"dmesg", "-f", "net"};
    int rc = exec_dmesg(3, args1);
    assert(rc == 0);
    assert(strcmp(s_out_buf, expected) == 0);

    /* Mixed case -f Net */
    mock_reset();
    const char *args2[] = {"dmesg", "-f", "Net"};
    rc = exec_dmesg(3, args2);
    assert(rc == 0);
    assert(strcmp(s_out_buf, expected) == 0);

    /* Explicit [NET] with brackets passed to -f */
    mock_reset();
    const char *args3[] = {"dmesg", "-f", "[NET]"};
    rc = exec_dmesg(3, args3);
    assert(rc == 0);
    assert(strcmp(s_out_buf, expected) == 0);

    /* Exact match: [NETWORK] must NOT match -f NET */
    assert(strstr(s_out_buf, "NETWORK") == NULL);

    /* Leading whitespace line: "  [SMP] AP cores started\n" */
    mock_reset();
    const char *args4[] = {"dmesg", "-f", "SMP"};
    rc = exec_dmesg(3, args4);
    assert(rc == 0);
    assert(strcmp(s_out_buf, "  [SMP] AP cores started\n") == 0);

    print_msg("PASS: case-insensitive matching, exact tag boundaries, and whitespace handling\n");
}

/* 3. Multiple filters (OR semantics) */
static void test_multiple_filters(void) {
    mock_reset();
    const char *args[] = {"dmesg", "-f", "NET", "-f", "USB"};
    int rc = exec_dmesg(5, args);
    assert(rc == 0);
    const char *expected =
        "[NET] Link carrier detected on eth0\n"
        "[USB] Device connected on port 1\n"
        "[net] Packet received: length 64\n"
        "[NET] Socket connection established\n";
    assert(strcmp(s_out_buf, expected) == 0);
    assert(strstr(s_out_buf, "EXT4") == NULL);
    assert(strstr(s_out_buf, "BOOT") == NULL);

    /* Three filters */
    mock_reset();
    const char *args3[] = {"dmesg", "-f", "BOOT", "-f", "USB", "-f", "SMP"};
    rc = exec_dmesg(7, args3);
    assert(rc == 0);
    const char *expected3 =
        "[BOOT] Kernel initialized\n"
        "[USB] Device connected on port 1\n"
        "  [SMP] AP cores started\n";
    assert(strcmp(s_out_buf, expected3) == 0);

    print_msg("PASS: multiple filters (-f NET -f USB OR combination)\n");
}

/* 4. No filter (all lines) */
static void test_no_filter(void) {
    mock_reset();
    const char *args[] = {"dmesg"};
    int rc = exec_dmesg(1, args);
    assert(rc == 0);
    assert(strcmp(s_out_buf, s_mock_dmesg) == 0);

    print_msg("PASS: no filter (all lines preserved unchanged)\n");
}

/* 5. Unknown facility (matches nothing, exits 0) */
static void test_unknown_facility(void) {
    mock_reset();
    const char *args[] = {"dmesg", "-f", "NONEXISTENT"};
    int rc = exec_dmesg(3, args);
    assert(rc == 0);
    assert(s_out_len == 0);
    assert(strcmp(s_out_buf, "") == 0);

    print_msg("PASS: unknown facility (matches nothing, empty output, exits 0)\n");
}

/* 6. Combined with -n N / tail */
static void test_combined_with_tail(void) {
    /* -f NET -n 2: last 2 [NET] lines */
    mock_reset();
    const char *args1[] = {"dmesg", "-f", "NET", "-n", "2"};
    int rc = exec_dmesg(5, args1);
    assert(rc == 0);
    const char *expected1 =
        "[net] Packet received: length 64\n"
        "[NET] Socket connection established\n";
    assert(strcmp(s_out_buf, expected1) == 0);

    /* -f NET -n 1: last 1 [NET] line */
    mock_reset();
    const char *args2[] = {"dmesg", "-f", "NET", "-n", "1"};
    rc = exec_dmesg(5, args2);
    assert(rc == 0);
    const char *expected2 =
        "[NET] Socket connection established\n";
    assert(strcmp(s_out_buf, expected2) == 0);

    /* -f EXT4 tail 1: last 1 [EXT4] line */
    mock_reset();
    const char *args3[] = {"dmesg", "-f", "EXT4", "tail", "1"};
    rc = exec_dmesg(5, args3);
    assert(rc == 0);
    assert(strcmp(s_out_buf, "[EXT4] Journal transaction committed\n") == 0);

    print_msg("PASS: combined with -n N and tail N\n");
}

/* 7. Combined with destination file and append */
static void test_combined_with_file(void) {
    mock_reset();
    const char *args1[] = {"dmesg", "-f", "NET", "/var/log/net.log"};
    int rc = exec_dmesg(4, args1);
    assert(rc == 0);
    assert((s_last_open_flags & VFS_O_TRUNC) != 0);
    assert(strstr(s_out_buf, "Saved ") != NULL);
    assert(strstr(s_out_buf, "bytes to /var/log/net.log") != NULL);
    const char *expected =
        "[NET] Link carrier detected on eth0\n"
        "[net] Packet received: length 64\n"
        "[NET] Socket connection established\n";
    assert(strcmp(s_mock_file, expected) == 0);

    /* -a append */
    mock_reset();
    const char *args2[] = {"dmesg", "-f", "USB", "-a", "/var/log/usb.log"};
    rc = exec_dmesg(5, args2);
    assert(rc == 0);
    assert((s_last_open_flags & VFS_O_APPEND) != 0);
    assert(strstr(s_out_buf, "bytes (appended) to /var/log/usb.log") != NULL);
    assert(strcmp(s_mock_file, "[USB] Device connected on port 1\n") == 0);

    /* NONEXISTENT with file: creates empty file, exits 0 */
    mock_reset();
    const char *args3[] = {"dmesg", "-f", "NONEXISTENT", "/var/log/empty.log"};
    rc = exec_dmesg(4, args3);
    assert(rc == 0);
    assert(strstr(s_out_buf, "Saved 0 bytes to /var/log/empty.log") != NULL);
    assert(s_mock_file_len == 0);

    print_msg("PASS: combined with file save and -a append\n");
}

/* 8. Error handling: missing argument, empty argument, filter overflow */
static void test_error_handling(void) {
    /* Missing argument after -f */
    mock_reset();
    const char *args1[] = {"dmesg", "-f"};
    int rc = exec_dmesg(2, args1);
    assert(rc != 0);
    assert(strstr(s_err_buf, "requires a facility argument") != NULL);

    /* Missing argument after --facility */
    mock_reset();
    const char *args2[] = {"dmesg", "--facility"};
    rc = exec_dmesg(2, args2);
    assert(rc != 0);
    assert(strstr(s_err_buf, "requires a facility argument") != NULL);

    /* Empty argument: -f= */
    mock_reset();
    const char *args3[] = {"dmesg", "-f="};
    rc = exec_dmesg(2, args3);
    assert(rc != 0);
    assert(strstr(s_err_buf, "requires a facility argument") != NULL);

    /* Empty argument: --facility= */
    mock_reset();
    const char *args4[] = {"dmesg", "--facility="};
    rc = exec_dmesg(2, args4);
    assert(rc != 0);
    assert(strstr(s_err_buf, "requires a facility argument") != NULL);

    /* More than 8 filters */
    mock_reset();
    const char *args5[] = {
        "dmesg", "-f", "1", "-f", "2", "-f", "3", "-f", "4",
        "-f", "5", "-f", "6", "-f", "7", "-f", "8", "-f", "9"
    };
    rc = exec_dmesg(19, args5);
    assert(rc != 0);
    assert(strstr(s_err_buf, "too many facility filters") != NULL);

    /* Help text lists facilities */
    mock_reset();
    const char *args6[] = {"dmesg", "--help"};
    rc = exec_dmesg(2, args6);
    assert(rc == 0);
    assert(strstr(s_out_buf, "-f FAC") != NULL);
    assert(strstr(s_out_buf, "NET") != NULL);
    assert(strstr(s_out_buf, "EXT4") != NULL);

    print_msg("PASS: error handling (missing/empty options, filter overflow, help text)\n");
}

/* 9. Severity level filtering: -l err, -l err,crit, numeric 0-7, intersection with facility */
static void test_level_filtering(void) {
    /* Test 1: dmesg -l err returns exactly level 3 log (stripped of <3>) */
    mock_reset();
    s_current_dmesg = s_mock_dmesg_levels;
    const char *args1[] = {"dmesg", "-l", "err"};
    int rc = exec_dmesg(3, args1);
    assert(rc == 0);
    const char *expected1 =
        "[TEST] test log level 3 (err)\n"
        "[NET] e1000: Link down error\n";
    assert(strcmp(s_out_buf, expected1) == 0);

    /* Test 2: dmesg -l err,crit returns levels 2 and 3 */
    mock_reset();
    s_current_dmesg = s_mock_dmesg_levels;
    const char *args2[] = {"dmesg", "-l", "err,crit"};
    rc = exec_dmesg(3, args2);
    assert(rc == 0);
    const char *expected2 =
        "[TEST] test log level 2 (crit)\n"
        "[TEST] test log level 3 (err)\n"
        "[NET] e1000: Link down error\n";
    assert(strcmp(s_out_buf, expected2) == 0);

    /* Test 3: dmesg -f TEST -l warn correctly intersects facility and level */
    mock_reset();
    s_current_dmesg = s_mock_dmesg_levels;
    const char *args3[] = {"dmesg", "-f", "TEST", "-l", "warn"};
    rc = exec_dmesg(5, args3);
    assert(rc == 0);
    const char *expected3 = "[TEST] test log level 4 (warn)\n";
    assert(strcmp(s_out_buf, expected3) == 0);

    /* Test 4: Numeric level dmesg -l 3 */
    mock_reset();
    s_current_dmesg = s_mock_dmesg_levels;
    const char *args4[] = {"dmesg", "-l", "3"};
    rc = exec_dmesg(3, args4);
    assert(rc == 0);
    assert(strcmp(s_out_buf, expected1) == 0);

    /* Test 5: Numeric levels dmesg -l 2,3 */
    mock_reset();
    s_current_dmesg = s_mock_dmesg_levels;
    const char *args5[] = {"dmesg", "-l", "2,3"};
    rc = exec_dmesg(3, args5);
    assert(rc == 0);
    assert(strcmp(s_out_buf, expected2) == 0);

    /* Test 6: Combined facility + level: dmesg -f TEST -l 3 */
    mock_reset();
    s_current_dmesg = s_mock_dmesg_levels;
    const char *args6[] = {"dmesg", "-f", "TEST", "-l", "3"};
    rc = exec_dmesg(5, args6);
    assert(rc == 0);
    assert(strcmp(s_out_buf, "[TEST] test log level 3 (err)\n") == 0);

    /* Test 7: Flag formats: --level=warn, -l=warn, -lwarn */
    mock_reset();
    s_current_dmesg = s_mock_dmesg_levels;
    const char *args7[] = {"dmesg", "-f", "TEST", "--level=warn"};
    rc = exec_dmesg(4, args7);
    assert(rc == 0);
    assert(strcmp(s_out_buf, expected3) == 0);

    mock_reset();
    s_current_dmesg = s_mock_dmesg_levels;
    const char *args8[] = {"dmesg", "-f", "TEST", "-l=warn"};
    rc = exec_dmesg(4, args8);
    assert(rc == 0);
    assert(strcmp(s_out_buf, expected3) == 0);

    mock_reset();
    s_current_dmesg = s_mock_dmesg_levels;
    const char *args9[] = {"dmesg", "-f", "TEST", "-lwarn"};
    rc = exec_dmesg(4, args9);
    assert(rc == 0);
    assert(strcmp(s_out_buf, expected3) == 0);

    /* Test 8: All levels individually 0 through 7 */
    const char *level_names[8] = {"emerg", "alert", "crit", "err", "warn", "notice", "info", "debug"};
    char expected_buf[64];
    for (int lvl = 0; lvl <= 7; lvl++) {
        mock_reset();
        s_current_dmesg = s_mock_dmesg_levels;
        const char *lvl_args[] = {"dmesg", "-f", "TEST", "-l", level_names[lvl]};
        rc = exec_dmesg(5, lvl_args);
        assert(rc == 0);
        snprintf(expected_buf, sizeof(expected_buf), "[TEST] test log level %d (%s)\n", lvl, level_names[lvl]);
        assert(strcmp(s_out_buf, expected_buf) == 0);
    }

    /* Test 9: Raw mode dmesg -r preserves <N> */
    mock_reset();
    s_current_dmesg = s_mock_dmesg_levels;
    const char *args_raw[] = {"dmesg", "-r", "-f", "TEST", "-l", "warn"};
    rc = exec_dmesg(6, args_raw);
    assert(rc == 0);
    assert(strcmp(s_out_buf, "<4>[TEST] test log level 4 (warn)\n") == 0);

    print_msg("PASS: severity level filtering (-l, --level, names, 0-7, combined with -f, -r raw)\n");
}

/* 10. Level error handling: invalid names, missing arg, exit code 2 */
static void test_level_error_handling(void) {
    /* Invalid level name: rejects with exit code 2 */
    mock_reset();
    const char *args1[] = {"dmesg", "-l", "invalid"};
    int rc = exec_dmesg(3, args1);
    assert(rc == 2);
    assert(strstr(s_err_buf, "unknown level 'invalid'") != NULL);

    /* Invalid level number 8: rejects with exit code 2 */
    mock_reset();
    const char *args2[] = {"dmesg", "-l", "8"};
    rc = exec_dmesg(3, args2);
    assert(rc == 2);
    assert(strstr(s_err_buf, "unknown level '8'") != NULL);

    /* Missing level arg: rejects with exit code 2 */
    mock_reset();
    const char *args3[] = {"dmesg", "-l"};
    rc = exec_dmesg(2, args3);
    assert(rc == 2);
    assert(strstr(s_err_buf, "requires a level argument") != NULL);

    /* Empty level string: rejects with exit code 2 */
    mock_reset();
    const char *args4[] = {"dmesg", "-l", ""};
    rc = exec_dmesg(3, args4);
    assert(rc == 2);
    assert(strstr(s_err_buf, "requires a level argument") != NULL);

    /* Trailing comma: rejects with exit code 2 */
    mock_reset();
    const char *args5[] = {"dmesg", "-l", "err,"};
    rc = exec_dmesg(3, args5);
    assert(rc == 2);
    assert(strstr(s_err_buf, "invalid level ''") != NULL);

    /* Empty --level= */
    mock_reset();
    const char *args6[] = {"dmesg", "--level="};
    rc = exec_dmesg(2, args6);
    assert(rc == 2);
    assert(strstr(s_err_buf, "requires a level argument") != NULL);

    print_msg("PASS: level error handling (invalid names, out of range 0-7, exit code 2)\n");
}

int main(void) {
    test_filter_parsing();
    test_case_insensitive_and_exact();
    test_multiple_filters();
    test_no_filter();
    test_unknown_facility();
    test_combined_with_tail();
    test_combined_with_file();
    test_error_handling();
    test_level_filtering();
    test_level_error_handling();
    print_msg("ALL DMESG FILTER HOST TESTS PASSED!\n");
    return 0;
}
