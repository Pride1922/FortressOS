/* Host unit test suite for FortressOS Telnet client under ASan/UBSan */
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE 1

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_HOST 1

#include "types.h"
#include "syscall_abi.h"
#include "socket_abi.h"
#include "vfs.h"
#include "poll.h"
#include "udp_common.h"

/* Forward declare mock syscall dispatcher */
static long host_mock_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c,
                              uintptr_t d, uintptr_t e, uintptr_t f);

#define TELNET_CALL host_mock_syscall

/* Include user/telnet.c directly */
#include "../user/telnet.c"

/* Mock buffers and counters */
static uint8_t s_mock_stdout[8192];
static size_t  s_mock_stdout_len = 0;

static uint8_t s_mock_sock_out[8192];
static size_t  s_mock_sock_out_len = 0;

static const uint8_t *s_mock_stdin_data = NULL;
static size_t s_mock_stdin_len = 0;
static size_t s_mock_stdin_pos = 0;

static const uint8_t *s_mock_sock_in_data = NULL;
static size_t s_mock_sock_in_len = 0;
static size_t s_mock_sock_in_pos = 0;

static int s_mock_poll_rounds = 0;
static bool s_mock_connected = false;

static void reset_mocks(void) {
    s_mock_stdout_len = 0;
    s_mock_sock_out_len = 0;
    s_mock_stdin_data = NULL;
    s_mock_stdin_len = 0;
    s_mock_stdin_pos = 0;
    s_mock_sock_in_data = NULL;
    s_mock_sock_in_len = 0;
    s_mock_sock_in_pos = 0;
    s_mock_poll_rounds = 0;
    s_mock_connected = true;

    s_iac_state = IAC_STATE_NORMAL;
    s_seen_cr = false;
    s_escape_char = DEFAULT_ESCAPE_CHAR;
}

static long host_mock_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c,
                              uintptr_t d, uintptr_t e, uintptr_t f) {
    (void)d; (void)e; (void)f;

    switch (nr) {
    case SYS_SOCKET:
        return 3; /* valid socket fd */

    case SYS_CONNECT:
        s_mock_connected = true;
        return 0;

    case SYS_CLOSE:
        s_mock_connected = false;
        return 0;

    case SYS_SHUTDOWN:
        return 0;

    case SYS_WRITE: {
        int fd = (int)a;
        const uint8_t *data = (const uint8_t *)b;
        size_t len = (size_t)c;

        if (fd == 1 || fd == 2) {
            for (size_t i = 0; i < len && s_mock_stdout_len < sizeof(s_mock_stdout); i++) {
                s_mock_stdout[s_mock_stdout_len++] = data[i];
            }
            return (long)len;
        } else if (fd == 3) {
            for (size_t i = 0; i < len && s_mock_sock_out_len < sizeof(s_mock_sock_out); i++) {
                s_mock_sock_out[s_mock_sock_out_len++] = data[i];
            }
            return (long)len;
        }
        return -1;
    }

    case SYS_READ: {
        int fd = (int)a;
        uint8_t *dst = (uint8_t *)b;
        size_t cap = (size_t)c;

        if (fd == 0) {
            /* Stdin */
            if (!s_mock_stdin_data || s_mock_stdin_pos >= s_mock_stdin_len) return 0;
            size_t avail = s_mock_stdin_len - s_mock_stdin_pos;
            size_t take = avail < cap ? avail : cap;
            memcpy(dst, s_mock_stdin_data + s_mock_stdin_pos, take);
            s_mock_stdin_pos += take;
            return (long)take;
        } else if (fd == 3) {
            /* Socket */
            if (!s_mock_sock_in_data || s_mock_sock_in_pos >= s_mock_sock_in_len) return 0;
            size_t avail = s_mock_sock_in_len - s_mock_sock_in_pos;
            size_t take = avail < cap ? avail : cap;
            memcpy(dst, s_mock_sock_in_data + s_mock_sock_in_pos, take);
            s_mock_sock_in_pos += take;
            return (long)take;
        }
        return -1;
    }

    case SYS_POLL: {
        struct pollfd *pfd = (struct pollfd *)a;
        uint64_t nfds = (uint64_t)b;
        s_mock_poll_rounds++;

        if (s_mock_poll_rounds > 5) return -1;

        int ready = 0;
        for (uint64_t idx = 0; idx < nfds; idx++) {
            if (pfd[idx].fd == 0 && s_mock_stdin_data && s_mock_stdin_pos < s_mock_stdin_len) {
                pfd[idx].revents = POLLIN;
                ready++;
            } else if (pfd[idx].fd == 3 && s_mock_sock_in_data && s_mock_sock_in_pos < s_mock_sock_in_len) {
                pfd[idx].revents = POLLIN;
                ready++;
            } else if (pfd[idx].fd == 3 && s_mock_sock_in_data && s_mock_sock_in_pos >= s_mock_sock_in_len) {
                /* Signal EOF */
                pfd[idx].revents = POLLHUP;
                ready++;
            } else {
                pfd[idx].revents = 0;
            }
        }
        return ready;
    }

    default:
        return 0;
    }
}

/* Test 1: Telnet RFC 854 Option Negotiation */
static void test_telnet_negotiation(void) {
    printf("[1/5] Testing Telnet IAC option negotiation...\n");
    reset_mocks();

    /* Server sends:
     * 1. IAC DO TELOPT_ECHO (1) -> expects client reply IAC WONT 1 (\xff\xfc\x01)
     * 2. IAC WILL TELOPT_SGA (3) -> expects client reply IAC DO 3 (\xff\xfd\x03)
     * 3. IAC WILL TELOPT_TTYPE (24) -> expects client reply IAC DONT 24 (\xff\xfe\x18)
     * 4. IAC SB 24 1 IAC SE (subnegotiation) -> swallowed, no reply, no terminal leak
     * 5. IAC IAC -> emits literal 0xFF to stdout
     */
    uint8_t server_data[] = {
        TELNET_IAC, TELNET_DO, TELOPT_ECHO,
        TELNET_IAC, TELNET_WILL, TELOPT_SGA,
        TELNET_IAC, TELNET_WILL, TELOPT_TTYPE,
        TELNET_IAC, TELNET_SB, TELOPT_TTYPE, 1, TELNET_IAC, TELNET_SE,
        TELNET_IAC, TELNET_IAC
    };

    process_server_bytes(3, server_data, sizeof(server_data));

    /* Verify socket received expected replies */
    uint8_t expected_replies[] = {
        TELNET_IAC, TELNET_WONT, TELOPT_ECHO,
        TELNET_IAC, TELNET_DO, TELOPT_SGA,
        TELNET_IAC, TELNET_DONT, TELOPT_TTYPE
    };

    assert(s_mock_sock_out_len == sizeof(expected_replies));
    assert(memcmp(s_mock_sock_out, expected_replies, sizeof(expected_replies)) == 0);

    /* Verify stdout only received literal 0xFF from IAC IAC */
    assert(s_mock_stdout_len == 1);
    assert(s_mock_stdout[0] == 0xFF);

    printf("  -> IAC negotiation PASS\n");
}

/* Test 2: NVT EOL conversions */
static void test_telnet_nvt_eol(void) {
    printf("[2/5] Testing NVT line ending conversions...\n");
    reset_mocks();

    /* 2a. Server sending \r\n and \r\0 -> both should output \n */
    const char srv_text[] = "Line1\r\nLine2\r\0Line3\n";
    process_server_bytes(3, (const uint8_t *)srv_text, sizeof(srv_text) - 1);

    s_mock_stdout[s_mock_stdout_len] = '\0';
    assert(strcmp((char *)s_mock_stdout, "Line1\nLine2\nLine3\n") == 0);

    /* 2b. Client sending stdin \n -> sends \r\n to socket */
    reset_mocks();
    const char *cli_text = "Echo Hello\n";
    bool ok = process_stdin_bytes(3, (const uint8_t *)cli_text, strlen(cli_text), "127.0.0.1", 23);
    assert(ok);

    s_mock_sock_out[s_mock_sock_out_len] = '\0';
    assert(strcmp((char *)s_mock_sock_out, "Echo Hello\r\n") == 0);

    printf("  -> NVT EOL conversion PASS\n");
}

/* Test 3: Stdin IAC escaping */
static void test_telnet_stdin_iac_escaping(void) {
    printf("[3/5] Testing stdin literal IAC escaping (0xFF -> 0xFF 0xFF)...\n");
    reset_mocks();

    uint8_t cli_bytes[] = { 'A', 0xFF, 'B' };
    bool ok = process_stdin_bytes(3, cli_bytes, sizeof(cli_bytes), "127.0.0.1", 23);
    assert(ok);

    uint8_t expected[] = { 'A', 0xFF, 0xFF, 'B' };
    assert(s_mock_sock_out_len == sizeof(expected));
    assert(memcmp(s_mock_sock_out, expected, sizeof(expected)) == 0);

    printf("  -> Stdin IAC escaping PASS\n");
}

/* Test 4: Escape prompt handling */
static void test_telnet_escape_prompt(void) {
    printf("[4/5] Testing escape character (^]) prompt handling...\n");
    reset_mocks();

    /* Input contains escape char followed by "quit\n" */
    uint8_t input[] = { DEFAULT_ESCAPE_CHAR, 'q', 'u', 'i', 't', '\n' };
    s_mock_stdin_data = input + 1; /* data available when prompt reads */
    s_mock_stdin_len = sizeof(input) - 1;
    s_mock_stdin_pos = 0;

    /* Process the escape byte */
    bool ok = process_stdin_bytes(3, input, 1, "127.0.0.1", 23);

    /* Should return false indicating user quit */
    assert(!ok);

    s_mock_stdout[s_mock_stdout_len] = '\0';
    assert(strstr((char *)s_mock_stdout, "telnet>") != NULL);
    assert(strstr((char *)s_mock_stdout, "Connection closed.") != NULL);

    printf("  -> Escape prompt PASS\n");
}

/* Test 5: Argument parsing & CLI options */
static void test_telnet_cli_parsing(void) {
    printf("[5/5] Testing argument parsing and port defaults...\n");
    reset_mocks();

    /* 5a. --help */
    char *help_argv[] = { "telnet", "--help" };
    int ret = telnet_main(2, help_argv);
    assert(ret == 0);
    s_mock_stdout[s_mock_stdout_len] = '\0';
    assert(strstr((char *)s_mock_stdout, "usage: telnet") != NULL);

    /* 5b. Default port 23 */
    reset_mocks();
    char *def_argv[] = { "telnet", "127.0.0.1" };
    /* Mock socket read EOF immediately to end loop */
    uint8_t eof_data[] = { 0 };
    s_mock_sock_in_data = eof_data;
    s_mock_sock_in_len = 0;

    ret = telnet_main(2, def_argv);
    assert(ret == 0);
    s_mock_stdout[s_mock_stdout_len] = '\0';
    assert(strstr((char *)s_mock_stdout, "Trying 127.0.0.1...") != NULL);
    assert(strstr((char *)s_mock_stdout, "Connected to 127.0.0.1.") != NULL);
    assert(strstr((char *)s_mock_stdout, "Escape character is '^]'.") != NULL);

    /* 5c. Custom port */
    reset_mocks();
    char *port_argv[] = { "telnet", "127.0.0.1", "8080" };
    ret = telnet_main(3, port_argv);
    assert(ret == 0);
    s_mock_stdout[s_mock_stdout_len] = '\0';
    assert(strstr((char *)s_mock_stdout, "Trying 127.0.0.1...") != NULL);

    printf("  -> CLI argument parsing PASS\n");
}

int main(void) {
    printf("=========================================\n");
    printf("  FortressOS Telnet Client Host Tests    \n");
    printf("=========================================\n");

    test_telnet_negotiation();
    test_telnet_nvt_eol();
    test_telnet_stdin_iac_escaping();
    test_telnet_escape_prompt();
    test_telnet_cli_parsing();

    printf("\n>>> ALL TELNET HOST TESTS PASSED (100%%) <<<\n");
    return 0;
}
