#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../user/watchtower/watchtower.h"
#include "../user/watchtower/watchtower_ctl.h"

/* Mock System State */
#define MOCK_MAX_PIDS 16
typedef struct {
    int64_t pid;
    char path[128];
    bool alive;
    int last_signal;
} mock_proc_t;

typedef struct {
    uint64_t current_time_sec;
    mock_proc_t procs[MOCK_MAX_PIDS];
    int proc_count;
    int next_pid;
} mock_sys_t;

static mock_sys_t g_mock;

static void mock_reset(void) {
    memset(&g_mock, 0, sizeof(g_mock));
    g_mock.current_time_sec = 2000;
    g_mock.next_pid = 200;
}

static int64_t mock_spawn(const service_t *svc, const char *const argv[],
                           const char *const envp[], const char *cwd, int *pipe_fd) {
    const char *path=svc->exec_path;
    (void)envp;(void)cwd;*pipe_fd=-1;
    (void)argv;
    if (g_mock.proc_count >= MOCK_MAX_PIDS) return -1;
    int idx = g_mock.proc_count++;
    g_mock.procs[idx].pid = g_mock.next_pid++;
    size_t i = 0;
    while (path && path[i] && i + 1 < sizeof(g_mock.procs[idx].path)) {
        g_mock.procs[idx].path[i] = path[i];
        i++;
    }
    g_mock.procs[idx].path[i] = '\0';
    g_mock.procs[idx].alive = true;
    return g_mock.procs[idx].pid;
}

static int mock_kill(int64_t pid, int sig) {
    for (int i = 0; i < g_mock.proc_count; i++) {
        if (g_mock.procs[i].pid == pid && g_mock.procs[i].alive) {
            g_mock.procs[i].last_signal = sig;
            return 0;
        }
    }
    return -1;
}

static bool mock_file_exists(const char *path) {
    (void)path;
    return true;
}

static uint64_t mock_monotonic_sec(void) {
    return g_mock.current_time_sec;
}

static const watchtower_sys_ops_t g_mock_ops = {
    .spawn_service = mock_spawn,
    .kill = mock_kill,
    .file_exists = mock_file_exists,
    .monotonic_sec = mock_monotonic_sec
};

static void host_strcpy(char *dest, const char *src) {
    while (*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

static void setup_test_table(watchtower_table_t *tbl) {
    watchtower_table_init(tbl);
    service_t s1 = {
        .name = "dhcpc",
        .exec_path = "/sbin/dhcpc",
        .order = 10,
        .enabled = true,
        .readiness_mode = READINESS_NONE,
        .restart_policy = RESTART_ON_FAILURE,
        .restart_limit = 3
    };
    host_strcpy(s1.user, "root");
    host_strcpy(s1.group, "root");
    s1.args[0][0] = '-'; s1.args[0][1] = 'f'; s1.args[0][2] = '\0';
    s1.argc = 1;

    service_t s2 = {
        .name = "sshd",
        .exec_path = "/sbin/sshd",
        .order = 20,
        .enabled = true,
        .readiness_mode = READINESS_FILE,
        .ready_path = "/run/watchtower/sshd.ready",
        .restart_policy = RESTART_ON_FAILURE,
        .restart_limit = 3
    };
    host_strcpy(s2.user, "sshd");
    host_strcpy(s2.group, "sshd");
    s2.args[0][0] = '-'; s2.args[0][1] = 'D'; s2.args[0][2] = '\0';
    s2.argc = 1;

    watchtower_table_add_service(tbl, &s1);
    watchtower_table_add_service(tbl, &s2);
}

static void test_ctl_list_command(void) {
    printf("[TEST] test_ctl_list_command...\n");
    mock_reset();
    watchtower_table_t tbl;
    setup_test_table(&tbl);

    char res[WATCHTOWER_CTL_MAX_RES];
    int r = watchtower_ctl_process_request(&tbl, "LIST", 1000, 1000, res, sizeof(res), &g_mock_ops);
    assert(r == 0);
    assert(strstr(res, "NAME         STATE      PID     ORDER  ENABLED  RESTART") != NULL);
    assert(strstr(res, "dhcpc        stopped    -       10     yes      on-failure") != NULL);
    assert(strstr(res, "sshd         stopped    -       20     yes      on-failure") != NULL);
    printf("PASS: test_ctl_list_command\n");
}

static void test_ctl_status_command(void) {
    printf("[TEST] test_ctl_status_command...\n");
    mock_reset();
    watchtower_table_t tbl;
    setup_test_table(&tbl);

    /* Start dhcpc */
    watchtower_start_service(&tbl, 0, 2000, &g_mock_ops);

    char res[WATCHTOWER_CTL_MAX_RES];
    int r = watchtower_ctl_process_request(&tbl, "STATUS dhcpc", 1000, 1000, res, sizeof(res), &g_mock_ops);
    assert(r == 0);
    assert(strstr(res, "Service: dhcpc") != NULL);
    assert(strstr(res, "State: running (PID: 200)") != NULL);
    assert(strstr(res, "Readiness: ready (mode: none)") != NULL);
    assert(strstr(res, "Executable: /sbin/dhcpc -f") != NULL);
    assert(strstr(res, "Order: 10") != NULL);
    assert(strstr(res, "Enabled: yes") != NULL);
    assert(strstr(res, "Identity: root:root") != NULL);
    assert(strstr(res, "Restart Policy: on-failure (restarts: 0/3)") != NULL);
    assert(strstr(res, "Dropped Output: 0 bytes") != NULL);
    assert(strstr(res, "LogFile: ") != NULL);
    assert(strstr(res, "Recent Log: ") != NULL);

    /* Status of non-existent service */
    r = watchtower_ctl_process_request(&tbl, "STATUS nonexistent", 1000, 1000, res, sizeof(res), &g_mock_ops);
    assert(r == -1);
    assert(strstr(res, "ERR service 'nonexistent' not found") != NULL);
    printf("PASS: test_ctl_status_command\n");
}

static void test_ctl_check_command(void) {
    printf("[TEST] test_ctl_check_command...\n");
    mock_reset();
    watchtower_table_t tbl;
    setup_test_table(&tbl);

    char res[WATCHTOWER_CTL_MAX_RES];
    /* dhcpc stopped */
    int r = watchtower_ctl_process_request(&tbl, "CHECK dhcpc", 1000, 1000, res, sizeof(res), &g_mock_ops);
    assert(r == 1);
    assert(strstr(res, "ERR service 'dhcpc' is not running") != NULL);

    /* Start dhcpc (readiness_mode NONE -> instantly ready) */
    watchtower_start_service(&tbl, 0, 2000, &g_mock_ops);
    r = watchtower_ctl_process_request(&tbl, "CHECK dhcpc", 1000, 1000, res, sizeof(res), &g_mock_ops);
    assert(r == 0);
    assert(strstr(res, "OK service 'dhcpc' is ready") != NULL);

    /* Check nonexistent */
    r = watchtower_ctl_process_request(&tbl, "CHECK no_such_svc", 1000, 1000, res, sizeof(res), &g_mock_ops);
    assert(r == -1);
    assert(strstr(res, "ERR service 'no_such_svc' not found") != NULL);
    printf("PASS: test_ctl_check_command\n");
}

static void test_ctl_authorization_enforcement(void) {
    printf("[TEST] test_ctl_authorization_enforcement...\n");
    mock_reset();
    watchtower_table_t tbl;
    setup_test_table(&tbl);

    char res[WATCHTOWER_CTL_MAX_RES];

    /* Unprivileged user (UID 1000, GID 1000) tries mutating command START */
    int r = watchtower_ctl_process_request(&tbl, "START dhcpc", 1000, 1000, res, sizeof(res), &g_mock_ops);
    assert(r == -1);
    assert(strstr(res, "ERR permission denied: root or wheel required") != NULL);
    assert(tbl.services[0].state == SVC_STATE_STOPPED);

    /* Unprivileged user tries STOP */
    r = watchtower_ctl_process_request(&tbl, "STOP dhcpc", 1000, 1000, res, sizeof(res), &g_mock_ops);
    assert(r == -1);
    assert(strstr(res, "ERR permission denied: root or wheel required") != NULL);

    /* Unprivileged user tries RESTART */
    r = watchtower_ctl_process_request(&tbl, "RESTART dhcpc", 1000, 1000, res, sizeof(res), &g_mock_ops);
    assert(r == -1);
    assert(strstr(res, "ERR permission denied: root or wheel required") != NULL);

    /* Wheel group member (UID 1000, GID 10) permitted! */
    r = watchtower_ctl_process_request(&tbl, "START dhcpc", 1000, 10, res, sizeof(res), &g_mock_ops);
    assert(r == 0);
    assert(strstr(res, "OK service 'dhcpc' started") != NULL);
    assert(tbl.services[0].state == SVC_STATE_RUNNING);

    /* Root user (UID 0, GID 0) permitted! */
    r = watchtower_ctl_process_request(&tbl, "STOP dhcpc", 0, 0, res, sizeof(res), &g_mock_ops);
    assert(r == 0);
    assert(strstr(res, "OK stopping service 'dhcpc'") != NULL);

    printf("PASS: test_ctl_authorization_enforcement\n");
}

static void test_ctl_start_stop_restart_lifecycle(void) {
    printf("[TEST] test_ctl_start_stop_restart_lifecycle...\n");
    mock_reset();
    watchtower_table_t tbl;
    setup_test_table(&tbl);

    char res[WATCHTOWER_CTL_MAX_RES];

    /* 1. Start dhcpc */
    assert(watchtower_ctl_process_request(&tbl, "START dhcpc", 0, 0, res, sizeof(res), &g_mock_ops) == 0);
    assert(strstr(res, "OK service 'dhcpc' started") != NULL);
    assert(tbl.services[0].state == SVC_STATE_RUNNING);
    int64_t pid = tbl.services[0].pid;
    assert(pid > 0);

    /* 2. Starting already running service returns ERR */
    assert(watchtower_ctl_process_request(&tbl, "START dhcpc", 0, 0, res, sizeof(res), &g_mock_ops) == -1);
    assert(strstr(res, "ERR service 'dhcpc' is already running") != NULL);

    /* 3. Restart dhcpc */
    assert(watchtower_ctl_process_request(&tbl, "RESTART dhcpc", 0, 0, res, sizeof(res), &g_mock_ops) == 0);
    assert(strstr(res, "OK restarting service 'dhcpc'") != NULL);
    assert(tbl.services[0].state == SVC_STATE_STOPPING);

    /* Child exits cleanly */
    watchtower_handle_child_exit(&tbl, pid, 0, 2001, &g_mock_ops);
    /* Advance time to trigger restart */
    watchtower_tick(&tbl, 2002, &g_mock_ops);
    assert(tbl.services[0].state == SVC_STATE_RUNNING);
    assert(tbl.services[0].pid > pid);

    /* 4. Stop dhcpc */
    assert(watchtower_ctl_process_request(&tbl, "STOP dhcpc", 0, 0, res, sizeof(res), &g_mock_ops) == 0);
    assert(strstr(res, "OK stopping service 'dhcpc'") != NULL);

    /* Child exits */
    watchtower_handle_child_exit(&tbl, tbl.services[0].pid, 0, 2003, &g_mock_ops);
    assert(tbl.services[0].state == SVC_STATE_STOPPED);

    /* 5. Stopping already stopped service returns OK */
    assert(watchtower_ctl_process_request(&tbl, "STOP dhcpc", 0, 0, res, sizeof(res), &g_mock_ops) == 0);
    assert(strstr(res, "OK service 'dhcpc' is already stopped") != NULL);

    printf("PASS: test_ctl_start_stop_restart_lifecycle\n");
}

static void test_ctl_unknown_and_malformed(void) {
    printf("[TEST] test_ctl_unknown_and_malformed...\n");
    mock_reset();
    watchtower_table_t tbl;
    setup_test_table(&tbl);

    char res[WATCHTOWER_CTL_MAX_RES];

    /* Unknown command */
    int r = watchtower_ctl_process_request(&tbl, "DESTROY all", 0, 0, res, sizeof(res), &g_mock_ops);
    assert(r == -1);
    assert(strstr(res, "ERR unknown command 'DESTROY'") != NULL);

    /* Empty request */
    r = watchtower_ctl_process_request(&tbl, "   ", 0, 0, res, sizeof(res), &g_mock_ops);
    assert(r == -1);
    assert(strstr(res, "ERR empty request") != NULL);

    /* NULL request */
    r = watchtower_ctl_process_request(&tbl, NULL, 0, 0, res, sizeof(res), &g_mock_ops);
    assert(r == -1);
    assert(strstr(res, "ERR empty request") != NULL);

    printf("PASS: test_ctl_unknown_and_malformed\n");
}

static void write_test_file(const char *path, const char *content) {
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    assert(fd >= 0);
    assert(write(fd, content, strlen(content)) == (ssize_t)strlen(content));
    close(fd);
}

static void test_ctl_enable_disable(void) {
    printf("[TEST] test_ctl_enable_disable...\n");
    mock_reset();
    watchtower_table_t tbl;
    setup_test_table(&tbl);

    char dir[] = "/tmp/wt_ctl_dir_XXXXXX";
    assert(mkdtemp(dir) != NULL);
    watchtower_ctl_set_dirs(dir, NULL);

    char svc_path[256];
    snprintf(svc_path, sizeof(svc_path), "%s/sshd.service", dir);
    write_test_file(svc_path, "[Service]\nExec = /sbin/sshd -D\nOrder = 20\nEnabled = yes\nUser = sshd\nGroup = sshd\n");

    char res[WATCHTOWER_CTL_MAX_RES];

    /* 1. Unprivileged user (UID 1000, GID 1000) tries DISABLE -> rejected */
    int r = watchtower_ctl_process_request(&tbl, "DISABLE sshd", 1000, 1000, res, sizeof(res), &g_mock_ops);
    assert(r == -1);
    assert(strstr(res, "ERR permission denied") != NULL);
    assert(tbl.services[1].enabled == true);

    /* 2. Wheel user (UID 1000, GID 10) runs DISABLE sshd -> OK */
    r = watchtower_ctl_process_request(&tbl, "DISABLE sshd", 1000, 10, res, sizeof(res), &g_mock_ops);
    assert(r == 0);
    assert(strstr(res, "OK service 'sshd' disabled") != NULL);
    assert(tbl.services[1].enabled == false);

    /* Verify service list shows 'disabled' state for stopped+disabled service */
    r = watchtower_ctl_process_request(&tbl, "LIST", 1000, 1000, res, sizeof(res), &g_mock_ops);
    assert(r == 0);
    assert(strstr(res, "sshd         disabled   -       20     no       on-failure") != NULL);

    /* 3. Root user (UID 0, GID 0) runs ENABLE sshd -> OK */
    r = watchtower_ctl_process_request(&tbl, "ENABLE sshd", 0, 0, res, sizeof(res), &g_mock_ops);
    assert(r == 0);
    assert(strstr(res, "OK service 'sshd' enabled") != NULL);
    assert(tbl.services[1].enabled == true);

    /* Verify service list now shows 'stopped' state and Enabled 'yes' */
    r = watchtower_ctl_process_request(&tbl, "LIST", 1000, 1000, res, sizeof(res), &g_mock_ops);
    assert(r == 0);
    assert(strstr(res, "sshd         stopped    -       20     yes      on-failure") != NULL);

    /* 4. Missing service file fails */
    r = watchtower_ctl_process_request(&tbl, "ENABLE no_file", 0, 0, res, sizeof(res), &g_mock_ops);
    assert(r == -1);
    assert(strstr(res, "ERR") != NULL);

    unlink(svc_path);
    rmdir(dir);
    watchtower_ctl_set_dirs(NULL, NULL);
    printf("PASS: test_ctl_enable_disable\n");
}

static void test_ctl_reload(void) {
    printf("[TEST] test_ctl_reload...\n");
    mock_reset();
    watchtower_table_t tbl;
    setup_test_table(&tbl);

    char dir[] = "/tmp/wt_ctl_rel_XXXXXX";
    assert(mkdtemp(dir) != NULL);
    watchtower_ctl_set_dirs(dir, NULL);

    char svc_path[256];
    snprintf(svc_path, sizeof(svc_path), "%s/dhcpc.service", dir);
    write_test_file(svc_path, "[Service]\nExec = /sbin/dhcpc -f\nOrder = 10\nTimeoutStop = 5\n");

    /* Start dhcpc in table */
    watchtower_start_service(&tbl, 0, 2000, &g_mock_ops);
    assert(tbl.services[0].state == SVC_STATE_RUNNING);
    int64_t initial_pid = tbl.services[0].pid;
    assert(initial_pid > 0);
    assert(tbl.services[0].timeout_stop_sec == 5);

    char res[WATCHTOWER_CTL_MAX_RES];

    /* 1. Unprivileged user tries RELOAD -> rejected */
    int r = watchtower_ctl_process_request(&tbl, "RELOAD dhcpc", 1000, 1000, res, sizeof(res), &g_mock_ops);
    assert(r == -1);
    assert(strstr(res, "ERR permission denied") != NULL);

    /* 2. Update service file on disk with new TimeoutStop and arguments */
    write_test_file(svc_path, "[Service]\nExec = /sbin/dhcpc -f -v\nOrder = 10\nTimeoutStop = 15\n");

    /* 3. Privileged user runs RELOAD */
    r = watchtower_ctl_process_request(&tbl, "RELOAD dhcpc", 0, 0, res, sizeof(res), &g_mock_ops);
    assert(r == 0);
    assert(strstr(res, "OK service 'dhcpc' reloaded") != NULL);

    /* Verify PID is UNCHANGED and still running, but TimeoutStop and args updated */
    assert(tbl.services[0].pid == initial_pid);
    assert(tbl.services[0].state == SVC_STATE_RUNNING);
    assert(tbl.services[0].timeout_stop_sec == 15);
    assert(tbl.services[0].argc == 2);
    assert(strcmp(tbl.services[0].args[1], "-v") == 0);

    /* 4. Invalid service file -> rejected, old configuration preserved */
    write_test_file(svc_path, "[Service]\nExec = /sbin/dhcpc -f\nInvalidDirective = 999\n");
    r = watchtower_ctl_process_request(&tbl, "RELOAD dhcpc", 0, 0, res, sizeof(res), &g_mock_ops);
    assert(r == -1);
    assert(strstr(res, "ERR") != NULL);
    assert(tbl.services[0].pid == initial_pid);
    assert(tbl.services[0].timeout_stop_sec == 15); /* Still 15 */

    /* 5. Service file deleted -> removed from in-memory table */
    unlink(svc_path);
    r = watchtower_ctl_process_request(&tbl, "RELOAD dhcpc", 0, 0, res, sizeof(res), &g_mock_ops);
    assert(r == 0);
    assert(strstr(res, "OK service 'dhcpc' removed") != NULL);
    assert(watchtower_find_service(&tbl, "dhcpc") == NULL);
    assert(tbl.count == 1); /* Only sshd remains */

    rmdir(dir);
    watchtower_ctl_set_dirs(NULL, NULL);
    printf("PASS: test_ctl_reload\n");
}

int main(void) {
    printf("===================================================\n");
    printf("Watchtower Phase 3 Control CLI Test Suite (ASan)\n");
    printf("===================================================\n");

    test_ctl_list_command();
    test_ctl_status_command();
    test_ctl_check_command();
    test_ctl_authorization_enforcement();
    test_ctl_start_stop_restart_lifecycle();
    test_ctl_unknown_and_malformed();
    test_ctl_enable_disable();
    test_ctl_reload();

    printf("\nALL 8 PHASE 3 WATCHTOWER CTL TESTS PASSED!\n");
    return 0;
}
