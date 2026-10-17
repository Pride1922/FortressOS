#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/socket.h>

#ifndef CRYPTO_HOST_TEST
#define CRYPTO_HOST_TEST
#endif
#include "user/sshd/sshd_config.h"
#include "user/sshd/sshd_readiness.h"
#include "user/sshd/sshd_admission.h"
#include "user/sshd/sshd_teardown.h"
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
    assert(strstr(err_buf, "out-of-range value '99999' for Port at line 1") != NULL);

    /* Test invalid IP */
    const char *bad_ip = "ListenAddress 999.1.1.1\n";
    err_buf[0] = '\0';
    assert(sshd_config_parse_buffer(bad_ip, strlen(bad_ip), &cfg, err_buf, sizeof(err_buf)) == -1);
    assert(strstr(err_buf, "out-of-range value '999.1.1.1' for ListenAddress at line 1") != NULL);

    /* Test PermitRootLogin yes rejection (must be "no", reject "yes") */
    const char *root_yes_conf = "PermitRootLogin yes\n";
    err_buf[0] = '\0';
    assert(sshd_config_parse_buffer(root_yes_conf, strlen(root_yes_conf), &cfg, err_buf, sizeof(err_buf)) == -1);
    assert(strstr(err_buf, "PermitRootLogin at line 1 (must be 'no')") != NULL);

    /* Test strict parser: unknown directive -> refuse to start with sshd: unknown directive '<name>' at line <N> */
    const char *unknown_conf =
        "# Valid comment\n"
        "Port 22\n"
        "X11Forwarding yes\n";
    err_buf[0] = '\0';
    assert(sshd_config_parse_buffer(unknown_conf, strlen(unknown_conf), &cfg, err_buf, sizeof(err_buf)) == -1);
    assert(strstr(err_buf, "sshd: unknown directive 'X11Forwarding' at line 3") != NULL);

    /* Test MaxAuthTries (1-5) and LoginGraceTime (seconds) */
    const char *full_conf =
        "Port 22\n"
        "MaxAuthTries 4\n"
        "LoginGraceTime 120\n";
    err_buf[0] = '\0';
    assert(sshd_config_parse_buffer(full_conf, strlen(full_conf), &cfg, err_buf, sizeof(err_buf)) == 0);
    assert(cfg.max_auth_tries == 4);
    assert(cfg.login_grace_time == 120);

    /* Test MaxAuthTries out-of-range (>5) */
    const char *bad_tries = "MaxAuthTries 10\n";
    err_buf[0] = '\0';
    assert(sshd_config_parse_buffer(bad_tries, strlen(bad_tries), &cfg, err_buf, sizeof(err_buf)) == -1);
    assert(strstr(err_buf, "out-of-range value '10' for MaxAuthTries at line 1 (1-5)") != NULL);

    /* Test MaxSessions out-of-range (>2) */
    const char *bad_sessions = "MaxSessions 4\n";
    err_buf[0] = '\0';
    assert(sshd_config_parse_buffer(bad_sessions, strlen(bad_sessions), &cfg, err_buf, sizeof(err_buf)) == -1);
    assert(strstr(err_buf, "out-of-range value '4' for MaxSessions at line 1 (1-2)") != NULL);

    printf("PASS: test_config_defaults_and_parsing\n");
}

static void test_daemon_admission_limits(void) {
    printf("Running test_daemon_admission_limits...\n");

    /* Test 1: sshd_format_disconnect_packet unit verification */
    uint8_t pkt[128];
    int pkt_len = sshd_format_disconnect_packet(SSH_DISCONNECT_BY_APPLICATION,
                                                "Too many concurrent sessions",
                                                pkt, sizeof(pkt));
    assert(pkt_len > 0);
    assert(pkt_len % 8 == 0); /* Total wire packet length is a multiple of 8 */
    uint32_t plen = ((uint32_t)pkt[0] << 24) | ((uint32_t)pkt[1] << 16) |
                    ((uint32_t)pkt[2] << 8) | (uint32_t)pkt[3];
    assert((int)plen + 4 == pkt_len);
    assert(plen % 8 == 4); /* packet_len + 4 == total_wire */
    uint8_t pad_len = pkt[4];
    assert(pad_len >= 4);
    assert(pkt[5] == (uint8_t)SSH_MSG_DISCONNECT); /* 1 */
    uint32_t reason = ((uint32_t)pkt[6] << 24) | ((uint32_t)pkt[7] << 16) |
                      ((uint32_t)pkt[8] << 8) | (uint32_t)pkt[9];
    assert(reason == SSH_DISCONNECT_BY_APPLICATION); /* 11 */
    uint32_t dlen = ((uint32_t)pkt[10] << 24) | ((uint32_t)pkt[11] << 16) |
                    ((uint32_t)pkt[12] << 8) | (uint32_t)pkt[13];
    assert(dlen == strlen("Too many concurrent sessions"));
    assert(memcmp(pkt + 14, "Too many concurrent sessions", dlen) == 0);

    /* Test buffer overflow rejection */
    assert(sshd_format_disconnect_packet(SSH_DISCONNECT_BY_APPLICATION, "test", pkt, 10) == -1);
    assert(sshd_format_disconnect_packet(SSH_DISCONNECT_BY_APPLICATION, "test", NULL, 128) == -1);

    /* Test 2: Global admission cap across connections */
    sshd_worker_slot_t slots[SSHD_MAX_CONCURRENT_SESSIONS];
    memset(slots, 0, sizeof(slots));

    /* Connection 1 (from IP A) arrives -> admitted to slot 0 */
    int s1 = sshd_admission_find_slot(slots);
    assert(s1 == 0);
    slots[s1].active = true;
    slots[s1].pid = 101;
    slots[s1].p_mon_in = 10;
    slots[s1].p_mon_out = 11;

    /* Connection 2 (from IP B) arrives -> admitted to slot 1 */
    int s2 = sshd_admission_find_slot(slots);
    assert(s2 == 1);
    slots[s2].active = true;
    slots[s2].pid = 102;
    slots[s2].p_mon_in = 12;
    slots[s2].p_mon_out = 13;

    /* Connection 3 arrives -> global cap reached (2), rejected before worker spawn */
    int s3 = sshd_admission_find_slot(slots);
    assert(s3 == -1); /* Admission cap enforced before SYS_SPAWN_EXT! */

    /* Test 3: What connection 3 sees on the wire (accept -> SSH_MSG_DISCONNECT -> close) */
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    int client_sv = sv[0];
    int peer_sv = sv[1];

    /* Daemon rejects connection 3 */
    const char *ver_banner = "SSH-2.0-FortressSSH_0.1\r\n";
    sshd_reject_connection_capacity(client_sv, ver_banner);
    /* client_sv is closed inside sshd_reject_connection_capacity */

    /* Peer reads what server sent */
    char recv_buf[256];
    size_t total_recv = 0;
    ssize_t nr;
    while ((nr = read(peer_sv, recv_buf + total_recv, sizeof(recv_buf) - total_recv)) > 0) {
        total_recv += (size_t)nr;
    }
    assert(nr == 0); /* Verified clean close by daemon! */
    close(peer_sv);

    /* Peer verifies version banner */
    size_t banner_len = strlen(ver_banner);
    assert(total_recv > banner_len);
    assert(memcmp(recv_buf, ver_banner, banner_len) == 0);

    /* Peer verifies SSH_MSG_DISCONNECT packet */
    const uint8_t *wire_pkt = (const uint8_t *)recv_buf + banner_len;
    size_t wire_len = total_recv - banner_len;
    assert(wire_len >= 16);
    uint32_t peer_plen = ((uint32_t)wire_pkt[0] << 24) | ((uint32_t)wire_pkt[1] << 16) |
                         ((uint32_t)wire_pkt[2] << 8) | (uint32_t)wire_pkt[3];
    assert((size_t)peer_plen + 4 == wire_len);
    assert(wire_pkt[5] == (uint8_t)SSH_MSG_DISCONNECT);
    uint32_t peer_reason = ((uint32_t)wire_pkt[6] << 24) | ((uint32_t)wire_pkt[7] << 16) |
                           ((uint32_t)wire_pkt[8] << 8) | (uint32_t)wire_pkt[9];
    assert(peer_reason == SSH_DISCONNECT_BY_APPLICATION);

    /* Test 4: Verify no leaked workers or slots */
    assert(slots[0].active == true && slots[0].pid == 101);
    assert(slots[1].active == true && slots[1].pid == 102);

    /* Connection 1 disconnects / reaped */
    slots[s1].active = false;
    slots[s1].pid = 0;
    slots[s1].p_mon_in = -1;
    slots[s1].p_mon_out = -1;

    /* Connection 4 arrives -> accepted into vacated slot 0 */
    int s4 = sshd_admission_find_slot(slots);
    assert(s4 == 0);
    slots[s4].active = true;
    slots[s4].pid = 104;

    printf("PASS: test_daemon_admission_limits\n");
}

static void test_config_path_precedence(void) {
    printf("Running test_config_path_precedence...\n");

    const char *tmp_installed = "/tmp/test_installed_sshd_config";
    const char *tmp_live = "/tmp/test_live_sshd_conf";
    const char *tmp_nonexistent_inst = "/tmp/nonexistent_installed_sshd_config";
    const char *tmp_nonexistent_live = "/tmp/nonexistent_live_sshd_conf";

    unlink(tmp_installed);
    unlink(tmp_live);
    unlink(tmp_nonexistent_inst);
    unlink(tmp_nonexistent_live);

    /* Write installed root config: Port 2201 */
    FILE *f_inst = fopen(tmp_installed, "w");
    assert(f_inst != NULL);
    fputs("Port 2201\nEnabled yes\n", f_inst);
    fclose(f_inst);

    /* Write live media fallback config: Port 2202 */
    FILE *f_live = fopen(tmp_live, "w");
    assert(f_live != NULL);
    fputs("Port 2202\nEnabled yes\n", f_live);
    fclose(f_live);

    sshd_config_t cfg;
    char err_buf[256] = {0};

    /* Case 1: Both exist -> Installed root is authoritative (/etc/ssh/sshd_config) */
    assert(sshd_config_load_from_paths(tmp_installed, tmp_live, &cfg, err_buf, sizeof(err_buf)) == 0);
    assert(cfg.enabled == true);
    assert(cfg.port == 2201);

    /* Case 2: Only live media exists -> Fallback loaded (/mnt/.fortress/sshd.conf) */
    memset(&cfg, 0, sizeof(cfg));
    err_buf[0] = '\0';
    assert(sshd_config_load_from_paths(tmp_nonexistent_inst, tmp_live, &cfg, err_buf, sizeof(err_buf)) == 0);
    assert(cfg.enabled == true);
    assert(cfg.port == 2202);

    /* Case 3: Neither exists -> SSH disabled, refuse to start with clear diagnostic */
    memset(&cfg, 0, sizeof(cfg));
    err_buf[0] = '\0';
    int rc = sshd_config_load_from_paths(tmp_nonexistent_inst, tmp_nonexistent_live, &cfg, err_buf, sizeof(err_buf));
    assert(rc == -1);
    assert(cfg.enabled == false);
    assert(strstr(err_buf, "SSH disabled: no configuration file found") != NULL);

    /* Case 4: Explicit CLI path nonexistent -> Refuses to start with diagnostic */
    memset(&cfg, 0, sizeof(cfg));
    err_buf[0] = '\0';
    rc = sshd_config_load(tmp_nonexistent_inst, &cfg, err_buf, sizeof(err_buf));
    assert(rc == -1);
    assert(cfg.enabled == false);
    assert(strstr(err_buf, "SSH disabled: configuration file not found") != NULL);

    /* Cleanup temporary files */
    unlink(tmp_installed);
    unlink(tmp_live);

    printf("PASS: test_config_path_precedence\n");
}

static int mock_cfg_ok = 1;
static int mock_cfg_enabled = 1;
static int mock_host_key_ok = 1;
static bool mock_userdb_ok = true;
static long mock_random_res = 16;
static long mock_bind_res = 42;
static int mock_listen_res = 0;

static int mock_load_config(const char *path, sshd_config_t *cfg, char *err_msg, size_t err_cap) {
    (void)path;
    if (!mock_cfg_ok) {
        if (err_msg && err_cap > 0) {
            snprintf(err_msg, err_cap, "SSH disabled: no configuration file found (/etc/ssh/sshd_config or /mnt/.fortress/sshd.conf)");
        }
        return -1;
    }
    sshd_config_init_defaults(cfg);
    cfg->enabled = (mock_cfg_enabled != 0);
    cfg->port = 22;
    cfg->listen_address = 0;
    return 0;
}

static int mock_load_host_key(uint8_t out_secret[64], uint8_t out_public[32]) {
    if (!mock_host_key_ok) return -1;
    memset(out_secret, 0xaa, 64);
    memset(out_public, 0xbb, 32);
    return 0;
}

static bool mock_load_userdb(userdb_t *db, bool shadow) {
    (void)shadow;
    if (!mock_userdb_ok) return false;
    memset(db, 0, sizeof(*db));
    return true;
}

static long mock_getrandom(void *buf, size_t len) {
    if (mock_random_res != (long)len) return mock_random_res;
    memset(buf, 0x42, len);
    return (long)len;
}

static long mock_bind_socket(uint32_t listen_addr, uint16_t port, char *err_msg, size_t err_cap) {
    if (mock_bind_res < 0) {
        if (err_msg && err_cap > 0) {
            sshd_format_bind_error(err_msg, err_cap, listen_addr, port);
        }
        return -1;
    }
    return mock_bind_res;
}

static int mock_listen_socket(long sock_fd) {
    (void)sock_fd;
    return mock_listen_res;
}

static void test_boot_readiness_gate_sequence(void) {
    printf("Running test_boot_readiness_gate_sequence...\n");

    sshd_readiness_ops_t ops = {
        .load_config = mock_load_config,
        .load_host_key = mock_load_host_key,
        .load_userdb = mock_load_userdb,
        .getrandom = mock_getrandom,
        .bind_socket = mock_bind_socket,
        .listen_socket = mock_listen_socket,
    };

    sshd_config_t cfg;
    uint8_t secret[64];
    userdb_t db;
    long sock_fd = -1;
    char diag[256] = {0};
    bool is_disabled = false;

    /* Gate 1: Config missing -> fails closed with distinct diagnostic */
    mock_cfg_ok = 0;
    diag[0] = '\0';
    int rc = sshd_check_readiness(NULL, &ops, &cfg, secret, &db, &sock_fd, diag, sizeof(diag), &is_disabled);
    assert(rc != 0);
    assert(is_disabled == false);
    assert(strstr(diag, "SSH disabled: no configuration file found") != NULL);

    /* Gate 2: Config found with Enabled no -> exits cleanly (rc == 0, is_disabled == true) */
    mock_cfg_ok = 1;
    mock_cfg_enabled = 0;
    diag[0] = '\0';
    is_disabled = false;
    rc = sshd_check_readiness(NULL, &ops, &cfg, secret, &db, &sock_fd, diag, sizeof(diag), &is_disabled);
    assert(rc == 0);
    assert(is_disabled == true);
    assert(strcmp(diag, "Service disabled in configuration.") == 0);

    /* Gate 3: Load host key via SYS_HOSTKEY -> fail = "host key unavailable/corrupt" */
    mock_cfg_enabled = 1;
    mock_host_key_ok = 0;
    diag[0] = '\0';
    is_disabled = false;
    rc = sshd_check_readiness(NULL, &ops, &cfg, secret, &db, &sock_fd, diag, sizeof(diag), &is_disabled);
    assert(rc != 0);
    assert(is_disabled == false);
    assert(strcmp(diag, "host key unavailable/corrupt") == 0);

    /* Gate 4: Load account database (userdb_t) -> fail = "account database unavailable" */
    mock_host_key_ok = 1;
    mock_userdb_ok = false;
    diag[0] = '\0';
    rc = sshd_check_readiness(NULL, &ops, &cfg, secret, &db, &sock_fd, diag, sizeof(diag), &is_disabled);
    assert(rc != 0);
    assert(is_disabled == false);
    assert(strcmp(diag, "account database unavailable") == 0);

    /* Gate 5: Verify SYS_GETRANDOM succeeds -> fail = "strong randomness unavailable" (fails closed) */
    mock_userdb_ok = true;
    mock_random_res = -1; /* Total failure */
    diag[0] = '\0';
    rc = sshd_check_readiness(NULL, &ops, &cfg, secret, &db, &sock_fd, diag, sizeof(diag), &is_disabled);
    assert(rc != 0);
    assert(is_disabled == false);
    assert(strcmp(diag, "strong randomness unavailable") == 0);

    mock_random_res = 8; /* Short/partial read (must also be rejected without weaker fallback!) */
    diag[0] = '\0';
    rc = sshd_check_readiness(NULL, &ops, &cfg, secret, &db, &sock_fd, diag, sizeof(diag), &is_disabled);
    assert(rc != 0);
    assert(strcmp(diag, "strong randomness unavailable") == 0);

    /* Gate 6: Bind port 22 -> fail = "bind failed on <addr>:<port>" */
    mock_random_res = 16;
    mock_bind_res = -1;
    diag[0] = '\0';
    rc = sshd_check_readiness(NULL, &ops, &cfg, secret, &db, &sock_fd, diag, sizeof(diag), &is_disabled);
    assert(rc != 0);
    assert(is_disabled == false);
    assert(strcmp(diag, "bind failed on 0.0.0.0:22") == 0);

    /* Gate 7: Complete readiness success -> Enter accept loop */
    mock_bind_res = 42;
    mock_listen_res = 0;
    diag[0] = '\0';
    sock_fd = -1;
    rc = sshd_check_readiness(NULL, &ops, &cfg, secret, &db, &sock_fd, diag, sizeof(diag), &is_disabled);
    assert(rc == 0);
    assert(is_disabled == false);
    assert(sock_fd == 42);
    assert(cfg.port == 22);

    /* Test helper: sshd_format_bind_error with custom address and port */
    char bind_str[64];
    sshd_format_bind_error(bind_str, sizeof(bind_str), 0, 22);
    assert(strcmp(bind_str, "bind failed on 0.0.0.0:22") == 0);

    uint32_t custom_ip = (192) | (168 << 8) | (10 << 16) | (5 << 24);
    sshd_format_bind_error(bind_str, sizeof(bind_str), custom_ip, 2222);
    assert(strcmp(bind_str, "bind failed on 192.168.10.5:2222") == 0);

    printf("PASS: test_boot_readiness_gate_sequence\n");
}

/* Mock state for daemon teardown verification */
static int mock_td_kill_sigterm_count = 0;
static int mock_td_kill_sigkill_count = 0;
static int64_t mock_td_last_signaled_pid = 0;
static int mock_td_closed_fds[16];
static int mock_td_closed_fd_count = 0;
static int mock_td_reap_behavior = 0; /* 0: instant reap on WNOHANG, 1: stuck child (escalates to SIGKILL) */
static uint32_t mock_td_simulated_time_ms = 0;

static int mock_td_kill(int64_t pid, int sig) {
    mock_td_last_signaled_pid = pid;
    if (sig == SIGTERM) mock_td_kill_sigterm_count++;
    if (sig == SIGKILL) mock_td_kill_sigkill_count++;
    return 0;
}

static int64_t mock_td_waitpid(int64_t pid, uint64_t *status, int options) {
    if (status) *status = 0;
    if (mock_td_reap_behavior == 0) {
        return pid; /* Child exits promptly on SIGTERM */
    } else {
        /* Child is stuck until SIGKILL or blocking wait */
        if (options == WNOHANG) {
            if (mock_td_kill_sigkill_count > 0) return pid;
            return 0; /* Still alive */
        }
        return pid; /* Reaped on blocking wait after SIGKILL */
    }
}

static int mock_td_close(int fd) {
    if (mock_td_closed_fd_count < (int)(sizeof(mock_td_closed_fds)/sizeof(mock_td_closed_fds[0]))) {
        mock_td_closed_fds[mock_td_closed_fd_count++] = fd;
    }
    return 0;
}

static void mock_td_sleep_ms(uint32_t ms) {
    mock_td_simulated_time_ms += ms;
}

static void test_daemon_teardown_sequence(void) {
    printf("Running test_daemon_teardown_sequence...\n");

    /* Case 1: Clean prompt exit with 2 active workers upon SIGTERM.
     * Teardown sequence:
     * 1. Receive SIGTERM
     * 2. Stop accepting new connections -> listening sock_fd closed immediately
     * 3. SIGTERM each active worker
     * 4. Bounded wait reaps workers cleanly
     * 5. Monitor IPC pipes closed, slots marked inactive, no SIGKILL escalation needed.
     */
    sshd_worker_slot_t slots[SSHD_MAX_CONCURRENT_SESSIONS];
    memset(slots, 0, sizeof(slots));

    slots[0].active = true;
    slots[0].pid = 201;
    slots[0].p_mon_in = 30;
    slots[0].p_mon_out = 31;

    slots[1].active = true;
    slots[1].pid = 202;
    slots[1].p_mon_in = 32;
    slots[1].p_mon_out = 33;

    long listen_sock_fd = 20;

    mock_td_kill_sigterm_count = 0;
    mock_td_kill_sigkill_count = 0;
    mock_td_closed_fd_count = 0;
    mock_td_simulated_time_ms = 0;
    mock_td_reap_behavior = 0;

    sshd_teardown_ops_t ops = {
        .kill = mock_td_kill,
        .waitpid = mock_td_waitpid,
        .close = mock_td_close,
        .sleep_ms = mock_td_sleep_ms
    };

    int rc = sshd_daemon_teardown(slots, listen_sock_fd, 5000, &ops);
    assert(rc == 0);

    /* Verify Step 2: Listening socket closed immediately */
    assert(mock_td_closed_fd_count >= 1);
    assert(mock_td_closed_fds[0] == 20);

    /* Verify Step 3: Both workers sent SIGTERM */
    assert(mock_td_kill_sigterm_count == 2);
    assert(mock_td_kill_sigkill_count == 0); /* No escalation needed */

    /* Verify Step 5: Both slots marked inactive */
    assert(slots[0].active == false);
    assert(slots[1].active == false);

    /* Verify all IPC pipe descriptors closed */
    bool found_30 = false, found_31 = false, found_32 = false, found_33 = false;
    for (int i = 0; i < mock_td_closed_fd_count; i++) {
        if (mock_td_closed_fds[i] == 30) found_30 = true;
        if (mock_td_closed_fds[i] == 31) found_31 = true;
        if (mock_td_closed_fds[i] == 32) found_32 = true;
        if (mock_td_closed_fds[i] == 33) found_33 = true;
    }
    assert(found_30 && found_31 && found_32 && found_33);

    /* Case 2: Stuck child process triggers 5s timeout and escalates to SIGKILL */
    memset(slots, 0, sizeof(slots));
    slots[0].active = true;
    slots[0].pid = 301;
    slots[0].p_mon_in = 40;
    slots[0].p_mon_out = 41;

    mock_td_kill_sigterm_count = 0;
    mock_td_kill_sigkill_count = 0;
    mock_td_closed_fd_count = 0;
    mock_td_simulated_time_ms = 0;
    mock_td_reap_behavior = 1; /* Stuck worker */

    rc = sshd_daemon_teardown(slots, -1, 5000, &ops);
    assert(rc == 0);

    /* Verify SIGTERM was sent first */
    assert(mock_td_kill_sigterm_count == 1);

    /* Verify 5000ms bounded wait elapsed */
    assert(mock_td_simulated_time_ms >= 5000);

    /* Verify escalation to SIGKILL */
    assert(mock_td_kill_sigkill_count == 1);
    assert(mock_td_last_signaled_pid == 301);

    /* Verify worker reaped and IPC pipes closed */
    assert(slots[0].active == false);
    assert(slots[0].p_mon_in == -1);
    assert(slots[0].p_mon_out == -1);

    printf("PASS: test_daemon_teardown_sequence\n");
}

int main(void) {
    printf("Starting SSH Daemon Lifecycle & Deployment test suite...\n");
    test_config_defaults_and_parsing();
    test_config_path_precedence();
    test_boot_readiness_gate_sequence();
    test_daemon_admission_limits();
    test_daemon_teardown_sequence();

    printf("\nALL TESTS PASSED (tests/ssh_daemon_host.c)\n");
    return 0;
}
