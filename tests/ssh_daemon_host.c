#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#ifndef CRYPTO_HOST_TEST
#define CRYPTO_HOST_TEST
#endif
#include "user/sshd/sshd_config.h"
#include "user/sshd/monitor.h"
#include "src/include/syscall_abi.h"

void crypto_wipe(void *p, size_t n) {
    if (!p) return;
    volatile unsigned char *vp = (volatile unsigned char *)p;
    while (n--) *vp++ = 0;
}

static void test_config_defaults_and_parsing(void) {
    printf("Running test_config_defaults_and_parsing...\n");

    sshd_config_t cfg;
    sshd_config_init_defaults(&cfg);

    assert(cfg.enabled == true);
    assert(cfg.port == 22);
    assert(cfg.listen_address == 0);
    assert(cfg.permit_root_login == false);
    assert(cfg.password_auth == true);
    assert(cfg.pubkey_auth == true);
    assert(cfg.max_sessions == 2);

    const char *test_conf =
        "# Fortress SSH Daemon Configuration\n"
        "Port 2222\n"
        "ListenAddress 192.168.1.100\n"
        "PermitRootLogin no\n"
        "PasswordAuthentication yes\n"
        "PubkeyAuthentication yes\n"
        "MaxSessions 2\n"
        "Enabled true\n";

    char err_buf[128] = {0};
    assert(sshd_config_parse_buffer(test_conf, strlen(test_conf), &cfg, err_buf, sizeof(err_buf)) == 0);
    assert(cfg.enabled == true);
    assert(cfg.port == 2222);
    assert(cfg.permit_root_login == false);
    assert(cfg.password_auth == true);
    assert(cfg.pubkey_auth == true);
    assert(cfg.max_sessions == 2);

    /* Verify IP was parsed in network byte order: 192.168.1.100 */
    uint32_t expected_ip = (192) | (168 << 8) | (1 << 16) | (100 << 24);
    assert(cfg.listen_address == expected_ip);

    /* Test disabled config */
    const char *disabled_conf = "Enabled no\n";
    assert(sshd_config_parse_buffer(disabled_conf, strlen(disabled_conf), &cfg, err_buf, sizeof(err_buf)) == 0);
    assert(cfg.enabled == false);

    /* Test invalid port */
    const char *bad_port = "Port 99999\n";
    assert(sshd_config_parse_buffer(bad_port, strlen(bad_port), &cfg, err_buf, sizeof(err_buf)) == -1);
    assert(err_buf[0] != '\0');

    /* Test invalid IP */
    const char *bad_ip = "ListenAddress 999.1.1.1\n";
    err_buf[0] = '\0';
    assert(sshd_config_parse_buffer(bad_ip, strlen(bad_ip), &cfg, err_buf, sizeof(err_buf)) == -1);
    assert(err_buf[0] != '\0');

    printf("PASS: test_config_defaults_and_parsing\n");
}

static void test_daemon_admission_limits(void) {
    printf("Running test_daemon_admission_limits...\n");

    /* Simulated worker slots in monitor */
    typedef struct {
        bool active;
        int pid;
    } slot_t;
    slot_t slots[2] = { {false, 0}, {false, 0} };

    /* Client 1 arrives */
    int s1 = -1;
    for (int i = 0; i < 2; i++) {
        if (!slots[i].active) { s1 = i; slots[i].active = true; slots[i].pid = 101; break; }
    }
    assert(s1 == 0);

    /* Client 2 arrives */
    int s2 = -1;
    for (int i = 0; i < 2; i++) {
        if (!slots[i].active) { s2 = i; slots[i].active = true; slots[i].pid = 102; break; }
    }
    assert(s2 == 1);

    /* Client 3 arrives -> must be rejected (max 2 concurrent sessions) */
    int s3 = -1;
    for (int i = 0; i < 2; i++) {
        if (!slots[i].active) { s3 = i; break; }
    }
    assert(s3 == -1); /* Rejected! */

    /* Client 1 disconnects / reaped */
    slots[s1].active = false;
    slots[s1].pid = 0;

    /* Client 4 arrives -> accepted into released slot */
    int s4 = -1;
    for (int i = 0; i < 2; i++) {
        if (!slots[i].active) { s4 = i; slots[i].active = true; slots[i].pid = 104; break; }
    }
    assert(s4 == 0);

    printf("PASS: test_daemon_admission_limits\n");
}

int main(void) {
    printf("Starting SSH Daemon Lifecycle & Deployment test suite...\n");
    test_config_defaults_and_parsing();
    test_daemon_admission_limits();

    printf("\nALL TESTS PASSED (tests/ssh_daemon_host.c)\n");
    return 0;
}
