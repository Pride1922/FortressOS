#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include "../user/watchtower/watchtower_config.h"

static void test_parse_minimal_valid(void) {
    printf("[TEST] test_parse_minimal_valid...\n");
    const char *cfg =
        "[Service]\n"
        "Exec=/sbin/dhcpc -f\n";

    service_t svc;
    char err[256];
    int r = watchtower_config_parse_service(cfg, strlen(cfg), "dhcpc", &svc, err, sizeof(err));
    assert(r == 0);
    assert(strcmp(svc.name, "dhcpc") == 0);
    assert(strcmp(svc.exec_path, "/sbin/dhcpc") == 0);
    assert(svc.argc == 1);
    assert(strcmp(svc.args[0], "-f") == 0);
    assert(svc.order == 100);
    assert(svc.enabled == true);
    assert(strcmp(svc.user, "root") == 0);
    assert(strcmp(svc.group, "root") == 0);
    assert(svc.restart_policy == RESTART_ON_FAILURE);
    assert(svc.restart_limit == 3);
    assert(svc.restart_delay_sec == 5);
    assert(svc.timeout_start_sec == 5);
    assert(svc.timeout_stop_sec == 5);
    assert(svc.readiness_mode == READINESS_NONE);
    printf("PASS: test_parse_minimal_valid\n");
}

static void test_parse_full_valid(void) {
    printf("[TEST] test_parse_full_valid...\n");
    service_t svc;
    char err[256];
    const char *strict_cfg =
        "# SSH Daemon Watchtower Service\n"
        "[Service]\n"
        "Exec = /sbin/sshd -D -p 2222\n"
        "Order = 20\n"
        "Enabled = yes\n"
        "User = sshd\n"
        "Group = sshd\n"
        "Restart = always\n"
        "RestartLimit = 5\n"
        "RestartDelay = 3\n"
        "TimeoutStart = 10\n"
        "TimeoutStop = 8\n"
        "Readiness = file:/run/watchtower/sshd.ready\n";

    int r = watchtower_config_parse_service(strict_cfg, strlen(strict_cfg), "sshd", &svc, err, sizeof(err));
    assert(r == 0);
    assert(strcmp(svc.name, "sshd") == 0);
    assert(strcmp(svc.exec_path, "/sbin/sshd") == 0);
    assert(svc.argc == 3);
    assert(strcmp(svc.args[0], "-D") == 0);
    assert(strcmp(svc.args[1], "-p") == 0);
    assert(strcmp(svc.args[2], "2222") == 0);
    assert(svc.order == 20);
    assert(svc.enabled == true);
    assert(strcmp(svc.user, "sshd") == 0);
    assert(strcmp(svc.group, "sshd") == 0);
    assert(svc.restart_policy == RESTART_ALWAYS);
    assert(svc.restart_limit == 5);
    assert(svc.restart_delay_sec == 3);
    assert(svc.timeout_start_sec == 10);
    assert(svc.timeout_stop_sec == 8);
    assert(svc.readiness_mode == READINESS_FILE);
    assert(strcmp(svc.ready_path, "/run/watchtower/sshd.ready") == 0);
    printf("PASS: test_parse_full_valid\n");
}

static void test_parse_quoted_args(void) {
    printf("[TEST] test_parse_quoted_args...\n");
    const char *cfg =
        "Exec=/bin/echo \"hello world\" 'single quoted' standard_arg\n";

    service_t svc;
    char err[256];
    int r = watchtower_config_parse_service(cfg, strlen(cfg), "echo", &svc, err, sizeof(err));
    assert(r == 0);
    assert(strcmp(svc.exec_path, "/bin/echo") == 0);
    assert(svc.argc == 3);
    assert(strcmp(svc.args[0], "hello world") == 0);
    assert(strcmp(svc.args[1], "single quoted") == 0);
    assert(strcmp(svc.args[2], "standard_arg") == 0);
    printf("PASS: test_parse_quoted_args\n");
}

static void test_reject_missing_exec(void) {
    printf("[TEST] test_reject_missing_exec...\n");
    const char *cfg =
        "[Service]\n"
        "Order=10\n"
        "Enabled=yes\n";

    service_t svc;
    char err[256];
    int r = watchtower_config_parse_service(cfg, strlen(cfg), "noexec", &svc, err, sizeof(err));
    assert(r == -1);
    assert(strstr(err, "missing mandatory Exec directive") != NULL);
    printf("PASS: test_reject_missing_exec\n");
}

static void test_reject_unknown_directive(void) {
    printf("[TEST] test_reject_unknown_directive...\n");
    const char *cfg =
        "Exec=/bin/sh\n"
        "BogusOption=123\n";

    service_t svc;
    char err[256];
    int r = watchtower_config_parse_service(cfg, strlen(cfg), "test", &svc, err, sizeof(err));
    assert(r == -1);
    assert(strstr(err, "unknown directive 'BogusOption'") != NULL);
    printf("PASS: test_reject_unknown_directive\n");
}

static void test_reject_duplicate_directive(void) {
    printf("[TEST] test_reject_duplicate_directive...\n");
    const char *cfg =
        "Exec=/bin/sh\n"
        "Order=10\n"
        "Order=20\n";

    service_t svc;
    char err[256];
    int r = watchtower_config_parse_service(cfg, strlen(cfg), "test", &svc, err, sizeof(err));
    assert(r == -1);
    assert(strstr(err, "duplicate directive 'Order'") != NULL);
    printf("PASS: test_reject_duplicate_directive\n");
}

static void test_reject_out_of_range_values(void) {
    printf("[TEST] test_reject_out_of_range_values...\n");
    char err[256];
    service_t svc;

    /* Order = 0 */
    const char *cfg1 = "Exec=/bin/sh\nOrder=0\n";
    assert(watchtower_config_parse_service(cfg1, strlen(cfg1), "t1", &svc, err, sizeof(err)) == -1);
    assert(strstr(err, "out-of-range value (1..999) for Order") != NULL);

    /* Order = 1000 */
    const char *cfg2 = "Exec=/bin/sh\nOrder=1000\n";
    assert(watchtower_config_parse_service(cfg2, strlen(cfg2), "t2", &svc, err, sizeof(err)) == -1);
    assert(strstr(err, "out-of-range value (1..999) for Order") != NULL);

    /* RestartLimit = 101 */
    const char *cfg3 = "Exec=/bin/sh\nRestartLimit=101\n";
    assert(watchtower_config_parse_service(cfg3, strlen(cfg3), "t3", &svc, err, sizeof(err)) == -1);
    assert(strstr(err, "out-of-range value (1..100) for RestartLimit") != NULL);

    /* Restart = bogus */
    const char *cfg4 = "Exec=/bin/sh\nRestart=sometimes\n";
    assert(watchtower_config_parse_service(cfg4, strlen(cfg4), "t4", &svc, err, sizeof(err)) == -1);
    assert(strstr(err, "invalid value for Restart") != NULL);

    printf("PASS: test_reject_out_of_range_values\n");
}

static void test_reject_excessive_arguments(void) {
    printf("[TEST] test_reject_excessive_arguments...\n");
    const char *cfg =
        "Exec=/bin/echo 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17\n";

    service_t svc;
    char err[256];
    int r = watchtower_config_parse_service(cfg, strlen(cfg), "test", &svc, err, sizeof(err));
    assert(r == -1);
    assert(strstr(err, "exceeded maximum argument count") != NULL);
    printf("PASS: test_reject_excessive_arguments\n");
}

static void test_precedence_and_file_loading(void) {
    printf("[TEST] test_precedence_and_file_loading...\n");
    char installed_path[] = "/tmp/wt_test_installed_XXXXXX";
    char live_path[] = "/tmp/wt_test_live_XXXXXX";
    int fd_inst = mkstemp(installed_path);
    int fd_live = mkstemp(live_path);
    assert(fd_inst >= 0 && fd_live >= 0);

    const char *inst_content = "Exec=/sbin/dhcpc -f\nOrder=10\n";
    const char *live_content = "Exec=/sbin/dhcpc -f\nOrder=99\n";

    assert(write(fd_inst, inst_content, strlen(inst_content)) == (ssize_t)strlen(inst_content));
    assert(write(fd_live, live_content, strlen(live_content)) == (ssize_t)strlen(live_content));
    close(fd_inst);
    close(fd_live);

    service_t svc;
    char err[256];

    /* Case 1: Both installed and live exist -> installed is authoritative */
    int r = watchtower_config_load_service(installed_path, live_path, "dhcpc", &svc, err, sizeof(err));
    assert(r == 0);
    assert(svc.order == 10); /* From installed, not live! */

    /* Case 2: Installed absent -> falls back to live media */
    r = watchtower_config_load_service("/no/such/installed/path", live_path, "dhcpc", &svc, err, sizeof(err));
    assert(r == 0);
    assert(svc.order == 99); /* From live! */

    /* Case 3: Both absent -> fails with diagnostic */
    r = watchtower_config_load_service("/no/such/installed", "/no/such/live", "dhcpc", &svc, err, sizeof(err));
    assert(r == -1);
    assert(strstr(err, "no configuration file found") != NULL);

    /* Case 4: Installed exists but is MALFORMED -> must FAIL CLOSED, never fall back to live! */
    int fd_bad = open(installed_path, O_WRONLY | O_TRUNC);
    assert(fd_bad >= 0);
    const char *bad_content = "Exec=/sbin/dhcpc\nBadDirective=123\n";
    assert(write(fd_bad, bad_content, strlen(bad_content)) == (ssize_t)strlen(bad_content));
    close(fd_bad);

    r = watchtower_config_load_service(installed_path, live_path, "dhcpc", &svc, err, sizeof(err));
    assert(r == -1);
    assert(strstr(err, "unknown directive 'BadDirective'") != NULL);

    unlink(installed_path);
    unlink(live_path);
    printf("PASS: test_precedence_and_file_loading\n");
}

static void test_parse_new_directives(void) {
    printf("[TEST] test_parse_new_directives...\n");
    const char *cfg =
        "Exec=/bin/app --daemon\n"
        "Type=forking\n"
        "WorkingDirectory=/var/run\n"
        "PIDFile=/run/app.pid\n"
        "Environment=FOO=bar\n"
        "Environment=BAZ=qux\n"
        "EnvironmentFile=/etc/default/app\n";

    service_t svc;
    char err[256];
    int r = watchtower_config_parse_service(cfg, strlen(cfg), "app", &svc, err, sizeof(err));
    assert(r == 0);
    assert(svc.type == SVC_TYPE_FORKING);
    assert(strcmp(svc.working_dir, "/var/run") == 0);
    assert(strcmp(svc.pid_file, "/run/app.pid") == 0);
    assert(strcmp(svc.env_file, "/etc/default/app") == 0);
    assert(svc.envc == 2);
    assert(strcmp(svc.env[0], "FOO=bar") == 0);
    assert(strcmp(svc.env[1], "BAZ=qux") == 0);
    printf("PASS: test_parse_new_directives\n");
}

static void test_reject_invalid_new_directives(void) {
    printf("[TEST] test_reject_invalid_new_directives...\n");
    service_t svc;
    char err[256];

    /* Invalid Type */
    const char *cfg1 = "Exec=/bin/app\nType=bogus\n";
    assert(watchtower_config_parse_service(cfg1, strlen(cfg1), "a", &svc, err, sizeof(err)) == -1);
    assert(strstr(err, "invalid value for Type") != NULL);

    /* Relative WorkingDirectory */
    const char *cfg2 = "Exec=/bin/app\nWorkingDirectory=relative/dir\n";
    assert(watchtower_config_parse_service(cfg2, strlen(cfg2), "a", &svc, err, sizeof(err)) == -1);
    assert(strstr(err, "invalid WorkingDirectory") != NULL);

    /* Relative PIDFile */
    const char *cfg3 = "Exec=/bin/app\nPIDFile=app.pid\n";
    assert(watchtower_config_parse_service(cfg3, strlen(cfg3), "a", &svc, err, sizeof(err)) == -1);
    assert(strstr(err, "invalid PIDFile") != NULL);

    /* Environment without '=' */
    const char *cfg4 = "Exec=/bin/app\nEnvironment=NO_EQUALS_HERE\n";
    assert(watchtower_config_parse_service(cfg4, strlen(cfg4), "a", &svc, err, sizeof(err)) == -1);
    assert(strstr(err, "Environment must be KEY=VAL") != NULL);

    /* Relative EnvironmentFile */
    const char *cfg5 = "Exec=/bin/app\nEnvironmentFile=relative.env\n";
    assert(watchtower_config_parse_service(cfg5, strlen(cfg5), "a", &svc, err, sizeof(err)) == -1);
    assert(strstr(err, "invalid EnvironmentFile") != NULL);

    printf("PASS: test_reject_invalid_new_directives\n");
}

static void test_parse_boot_log_service(void) {
    printf("[TEST] test_parse_boot_log_service...\n");
    const char *cfg =
        "Exec=/bin/boot-snapshot\n"
        "Order=01\n"
        "Type=simple\n"
        "Restart=no\n";
    service_t svc;
    char err[256];
    int r = watchtower_config_parse_service(cfg, strlen(cfg), "boot-log", &svc, err, sizeof(err));
    assert(r == 0);
    assert(strcmp(svc.name, "boot-log") == 0);
    assert(strcmp(svc.exec_path, "/bin/boot-snapshot") == 0);
    assert(svc.order == 1);
    assert(svc.type == SVC_TYPE_SIMPLE);
    assert(svc.restart_policy == RESTART_NEVER);
    printf("PASS: test_parse_boot_log_service\n");
}

static void test_config_set_enabled(void) {
    printf("[TEST] test_config_set_enabled...\n");
    char path[] = "/tmp/wt_cfg_enable_test_XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);

    const char *initial =
        "[Service]\n"
        "Exec = /sbin/sshd -D\n"
        "Order = 20\n"
        "Enabled = yes\n"
        "User = sshd\n";
    assert(write(fd, initial, strlen(initial)) == (ssize_t)strlen(initial));
    close(fd);

    char err[256];

    /* 1. Disable the service */
    int r = watchtower_config_set_enabled(path, NULL, false, err, sizeof(err));
    assert(r == 0);

    /* Verify file on disk now has Enabled=no */
    service_t svc;
    r = watchtower_config_load_file(path, "sshd", &svc, err, sizeof(err));
    assert(r == 0);
    assert(svc.enabled == false);
    assert(svc.order == 20);
    assert(strcmp(svc.exec_path, "/sbin/sshd") == 0);
    assert(strcmp(svc.user, "sshd") == 0);

    /* 2. Re-enable the service */
    r = watchtower_config_set_enabled(path, NULL, true, err, sizeof(err));
    assert(r == 0);

    r = watchtower_config_load_file(path, "sshd", &svc, err, sizeof(err));
    assert(r == 0);
    assert(svc.enabled == true);
    assert(svc.order == 20);

    unlink(path);

    /* 3. Test appending Enabled when directive was absent */
    char path2[] = "/tmp/wt_cfg_no_en_XXXXXX";
    fd = mkstemp(path2);
    assert(fd >= 0);
    const char *no_en =
        "[Service]\n"
        "Exec = /sbin/dhcpc -f\n";
    assert(write(fd, no_en, strlen(no_en)) == (ssize_t)strlen(no_en));
    close(fd);

    r = watchtower_config_set_enabled(path2, NULL, false, err, sizeof(err));
    assert(r == 0);

    r = watchtower_config_load_file(path2, "dhcpc", &svc, err, sizeof(err));
    assert(r == 0);
    assert(svc.enabled == false);
    assert(strcmp(svc.exec_path, "/sbin/dhcpc") == 0);

    unlink(path2);

    /* 4. Missing file fails with clear error */
    r = watchtower_config_set_enabled("/tmp/wt_nonexistent_service.service", NULL, false, err, sizeof(err));
    assert(r != 0);
    assert(strstr(err, "not found") != NULL);

    printf("PASS: test_config_set_enabled\n");
}

int main(void) {
    printf("===================================================\n");
    printf("Watchtower Phase 2 Configuration Test Suite (ASan)\n");
    printf("===================================================\n");

    test_parse_minimal_valid();
    test_parse_full_valid();
    test_parse_quoted_args();
    test_reject_missing_exec();
    test_reject_unknown_directive();
    test_reject_duplicate_directive();
    test_reject_out_of_range_values();
    test_reject_excessive_arguments();
    test_precedence_and_file_loading();
    test_parse_new_directives();
    test_reject_invalid_new_directives();
    test_parse_boot_log_service();
    test_config_set_enabled();

    printf("\nALL 13 PHASE 2 WATCHTOWER CONFIG TESTS PASSED!\n");
    return 0;
}
