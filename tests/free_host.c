/* Host unit test suite for /bin/free under ASan/UBSan with mocked syscalls */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "common.h"

extern int free_main(int argc, char **argv);

static sysinfo_t mock_sysinfo;
static long mock_sysinfo_ret = 0;
static char mock_output[4096];
static size_t mock_output_len = 0;
static char mock_errors[4096];
static size_t mock_errors_len = 0;

long tool_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    (void)b; (void)c;
    if (nr == SYS_SYSINFO) {
        if (mock_sysinfo_ret < 0) return mock_sysinfo_ret;
        sysinfo_t *out = (sysinfo_t *)a;
        if (out) *out = mock_sysinfo;
        return 0;
    }
    if (nr == SYS_WRITE) {
        int fd = (int)a;
        const char *buf = (const char *)b;
        size_t count = (size_t)c;
        if (fd == 1) {
            assert(mock_output_len + count < sizeof(mock_output));
            memcpy(mock_output + mock_output_len, buf, count);
            mock_output_len += count;
            mock_output[mock_output_len] = '\0';
            return (long)count;
        } else if (fd == 2) {
            assert(mock_errors_len + count < sizeof(mock_errors));
            memcpy(mock_errors + mock_errors_len, buf, count);
            mock_errors_len += count;
            mock_errors[mock_errors_len] = '\0';
            return (long)count;
        }
    }
    return 0;
}

static void reset_io(void) {
    mock_output_len = 0;
    mock_output[0] = '\0';
    mock_errors_len = 0;
    mock_errors[0] = '\0';
    mock_sysinfo_ret = 0;
}

static void test_default_human(void) {
    reset_io();
    /* 2 GiB total (2147483648), 1628 MiB free (1707081728), 420 MiB used (440401920) */
    memset(&mock_sysinfo, 0, sizeof(mock_sysinfo));
    mock_sysinfo.total_ram_bytes = 2147483648ULL;
    mock_sysinfo.free_ram_bytes = 1707081728ULL;

    char *argv[] = { "free", NULL };
    assert(free_main(1, argv) == 0);

    /* Verify header */
    assert(strstr(mock_output, "total") != NULL);
    assert(strstr(mock_output, "used") != NULL);
    assert(strstr(mock_output, "free") != NULL);
    assert(strstr(mock_output, "shared") != NULL);
    assert(strstr(mock_output, "buff/cache") != NULL);
    assert(strstr(mock_output, "available") != NULL);

    /* Verify Mem line */
    const char *mem_line = strstr(mock_output, "Mem:");
    assert(mem_line != NULL);
    assert(strstr(mem_line, "2.0Gi") != NULL);
    assert(strstr(mem_line, "420Mi") != NULL);
    assert(strstr(mem_line, "1.6Gi") != NULL);
    assert(strstr(mem_line, "0B") != NULL);

    /* Verify Swap line */
    const char *swap_line = strstr(mock_output, "Swap:");
    assert(swap_line != NULL);

    puts("PASS free host: default human memory summary");
}

static void test_explicit_human_flags(void) {
    reset_io();
    char *argv_h[] = { "free", "-h", NULL };
    assert(free_main(2, argv_h) == 0);
    char out_h[4096];
    assert(mock_output_len < sizeof(out_h));
    memcpy(out_h, mock_output, mock_output_len + 1);

    reset_io();
    char *argv_human[] = { "free", "--human", NULL };
    assert(free_main(2, argv_human) == 0);
    assert(!strcmp(out_h, mock_output));

    puts("PASS free host: -h and --human options match default");
}

static void test_mebibytes(void) {
    reset_io();
    char *argv_m[] = { "free", "-m", NULL };
    assert(free_main(2, argv_m) == 0);

    const char *mem_line = strstr(mock_output, "Mem:");
    assert(mem_line != NULL);
    assert(strstr(mem_line, "2048") != NULL);
    assert(strstr(mem_line, "420") != NULL);
    assert(strstr(mem_line, "1628") != NULL);

    reset_io();
    char *argv_mebi[] = { "free", "--mebi", NULL };
    assert(free_main(2, argv_mebi) == 0);
    assert(strstr(mock_output, "2048") != NULL);

    puts("PASS free host: -m and --mebi scaling");
}

static void test_kibibytes(void) {
    reset_io();
    char *argv_k[] = { "free", "-k", NULL };
    assert(free_main(2, argv_k) == 0);

    const char *mem_line = strstr(mock_output, "Mem:");
    assert(mem_line != NULL);
    assert(strstr(mem_line, "2097152") != NULL);
    assert(strstr(mem_line, "430080") != NULL);
    assert(strstr(mem_line, "1667072") != NULL);

    reset_io();
    char *argv_kibi[] = { "free", "--kibi", NULL };
    assert(free_main(2, argv_kibi) == 0);
    assert(strstr(mock_output, "2097152") != NULL);

    puts("PASS free host: -k and --kibi scaling");
}

static void test_bytes(void) {
    reset_io();
    char *argv_b[] = { "free", "-b", NULL };
    assert(free_main(2, argv_b) == 0);

    const char *mem_line = strstr(mock_output, "Mem:");
    assert(mem_line != NULL);
    assert(strstr(mem_line, "2147483648") != NULL);
    assert(strstr(mem_line, "440401920") != NULL);
    assert(strstr(mem_line, "1707081728") != NULL);

    reset_io();
    char *argv_bytes[] = { "free", "--bytes", NULL };
    assert(free_main(2, argv_bytes) == 0);
    assert(strstr(mock_output, "2147483648") != NULL);

    puts("PASS free host: -b and --bytes scaling");
}

static void test_gibibytes(void) {
    reset_io();
    char *argv_g[] = { "free", "-g", NULL };
    assert(free_main(2, argv_g) == 0);

    const char *mem_line = strstr(mock_output, "Mem:");
    assert(mem_line != NULL);
    assert(strstr(mem_line, "2") != NULL);
    assert(strstr(mem_line, "1") != NULL);

    reset_io();
    char *argv_gibi[] = { "free", "--gibi", NULL };
    assert(free_main(2, argv_gibi) == 0);
    assert(strstr(mock_output, "2") != NULL);

    puts("PASS free host: -g and --gibi scaling");
}

static void test_help(void) {
    reset_io();
    char *argv[] = { "free", "--help", NULL };
    assert(free_main(2, argv) == 0);
    assert(strstr(mock_output, "usage: free") != NULL);
    assert(strstr(mock_output, "-h, --human") != NULL);
    assert(strstr(mock_output, "-m, --mebi") != NULL);

    puts("PASS free host: --help documentation");
}

static void test_invalid_option(void) {
    reset_io();
    char *argv[] = { "free", "-z", NULL };
    assert(free_main(2, argv) != 0);
    assert(strstr(mock_errors, "free: invalid option") != NULL);

    puts("PASS free host: invalid option rejection");
}

static void test_sysinfo_failure(void) {
    reset_io();
    mock_sysinfo_ret = -1;
    char *argv[] = { "free", NULL };
    assert(free_main(1, argv) != 0);
    assert(strstr(mock_errors, "cannot read system info") != NULL);

    puts("PASS free host: SYS_SYSINFO failure handling");
}

static void test_rounding_and_edge_cases(void) {
    /* 1. Large RAM: 16 GiB total, 8 GiB used, 8 GiB free */
    reset_io();
    memset(&mock_sysinfo, 0, sizeof(mock_sysinfo));
    mock_sysinfo.total_ram_bytes = 16ULL * 1024 * 1024 * 1024;
    mock_sysinfo.free_ram_bytes = 8ULL * 1024 * 1024 * 1024;
    char *argv[] = { "free", NULL };
    assert(free_main(1, argv) == 0);
    assert(strstr(mock_output, "16Gi") != NULL);
    assert(strstr(mock_output, "8.0Gi") != NULL);

    /* 2. Small RAM: 256 MiB total, 64 MiB free */
    reset_io();
    memset(&mock_sysinfo, 0, sizeof(mock_sysinfo));
    mock_sysinfo.total_ram_bytes = 256ULL * 1024 * 1024;
    mock_sysinfo.free_ram_bytes = 64ULL * 1024 * 1024;
    assert(free_main(1, argv) == 0);
    assert(strstr(mock_output, "256Mi") != NULL);
    assert(strstr(mock_output, "192Mi") != NULL);
    assert(strstr(mock_output, "64Mi") != NULL);

    /* 3. Tiny RAM: 512 KiB */
    reset_io();
    memset(&mock_sysinfo, 0, sizeof(mock_sysinfo));
    mock_sysinfo.total_ram_bytes = 512 * 1024;
    mock_sysinfo.free_ram_bytes = 128 * 1024;
    assert(free_main(1, argv) == 0);
    assert(strstr(mock_output, "512Ki") != NULL);
    assert(strstr(mock_output, "384Ki") != NULL);
    assert(strstr(mock_output, "128Ki") != NULL);

    /* 4. Zero total RAM defense */
    reset_io();
    memset(&mock_sysinfo, 0, sizeof(mock_sysinfo));
    assert(free_main(1, argv) == 0);
    assert(strstr(mock_output, "0B") != NULL);

    puts("PASS free host: boundary values and rounding coverage");
}

int main(void) {
    test_default_human();
    test_explicit_human_flags();
    test_mebibytes();
    test_kibibytes();
    test_bytes();
    test_gibibytes();
    test_help();
    test_invalid_option();
    test_sysinfo_failure();
    test_rounding_and_edge_cases();
    puts("\n>>> ALL FREE HOST TESTS PASSED (100%) <<<");
    return 0;
}
