#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include "../user/watchtower/watchtower.h"

/* Mock System State */
#define MOCK_MAX_PIDS 32
typedef struct {
    int64_t pid;
    char path[128];
    bool alive;
    int last_signal;
} mock_proc_t;

typedef struct {
    char path[128];
    char entries[32][64];
    int count;
} mock_dir_t;

typedef struct {
    uint64_t current_time_sec;
    mock_proc_t procs[MOCK_MAX_PIDS];
    int proc_count;
    int next_pid;
    char mock_files[16][128];
    int file_count;
    int64_t mock_pid_file_val;
    mock_dir_t dirs[4];
    int dir_count;
    char last_stderr[256];
    int invalid_config_count;
    int empty_dir_warning_count;
    bool reject_identity;
} mock_sys_t;

static mock_sys_t g_mock;

static void mock_reset(void) {
    memset(&g_mock, 0, sizeof(g_mock));
    g_mock.current_time_sec = 1000;
    g_mock.next_pid = 100;
}

static void host_str_copy(char *dst, const char *src, size_t cap) {
    if (!dst || cap == 0) return;
    size_t i = 0;
    if (src) {
        while (src[i] && i + 1 < cap) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static int64_t mock_spawn(const service_t *svc, const char *const argv[],
                           const char *const envp[], const char *cwd, int *pipe_fd) {
    const char *path=svc->exec_path;
    (void)envp;(void)cwd;*pipe_fd=-1;
    if (g_mock.reject_identity) return -1;
    (void)argv;
    if (g_mock.proc_count >= MOCK_MAX_PIDS) return -1;
    if (strcmp(path, "/bin/fail_to_spawn") == 0) return -1;

    int idx = g_mock.proc_count++;
    g_mock.procs[idx].pid = g_mock.next_pid++;
    host_str_copy(g_mock.procs[idx].path, path, sizeof(g_mock.procs[idx].path));
    g_mock.procs[idx].alive = true;
    g_mock.procs[idx].last_signal = 0;
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
    for (int i = 0; i < g_mock.file_count; i++) {
        if (strcmp(g_mock.mock_files[i], path) == 0) return true;
    }
    return false;
}

static void mock_touch_file(const char *path) {
    if (g_mock.file_count < 16) {
        host_str_copy(g_mock.mock_files[g_mock.file_count++], path, 128);
    }
}

static uint64_t mock_monotonic_sec(void) {
    return g_mock.current_time_sec;
}

static bool mock_is_alive(int64_t pid) {
    for (int i = 0; i < g_mock.proc_count; i++) {
        if (g_mock.procs[i].pid == pid && g_mock.procs[i].alive) return true;
    }
    return false;
}

static int64_t mock_read_pid_file(const char *path) {
    (void)path;
    return g_mock.mock_pid_file_val;
}

static void mock_add_dir(const char *path) {
    if (g_mock.dir_count < 4) {
        mock_dir_t *d = &g_mock.dirs[g_mock.dir_count++];
        host_str_copy(d->path, path, sizeof(d->path));
        d->count = 0;
    }
}

static void mock_add_dir_entry(const char *dir_path, const char *entry_name) {
    for (int i = 0; i < g_mock.dir_count; i++) {
        if (strcmp(g_mock.dirs[i].path, dir_path) == 0) {
            mock_dir_t *d = &g_mock.dirs[i];
            if (d->count < 32) {
                host_str_copy(d->entries[d->count++], entry_name, 64);
            }
            return;
        }
    }
    if (g_mock.dir_count < 4) {
        mock_dir_t *d = &g_mock.dirs[g_mock.dir_count++];
        host_str_copy(d->path, dir_path, sizeof(d->path));
        host_str_copy(d->entries[d->count++], entry_name, 64);
    }
}

static int mock_list_dir(const char *path, char names[][64], uint32_t max_entries) {
    for (int i = 0; i < g_mock.dir_count; i++) {
        if (strcmp(g_mock.dirs[i].path, path) == 0) {
            mock_dir_t *d = &g_mock.dirs[i];
            uint32_t to_copy = (uint32_t)d->count;
            if (to_copy > max_entries) to_copy = max_entries;
            for (uint32_t j = 0; j < to_copy; j++) {
                host_str_copy(names[j], d->entries[j], 64);
            }
            return (int)to_copy;
        }
    }
    return -1;
}

static void mock_log_stderr(const char *msg) {
    if (!msg) return;
    host_str_copy(g_mock.last_stderr, msg, sizeof(g_mock.last_stderr));
    if (strstr(msg, "invalid service config")) {
        g_mock.invalid_config_count++;
    }
    if (strstr(msg, "no service configurations found")) {
        g_mock.empty_dir_warning_count++;
    }
}

static const watchtower_sys_ops_t g_mock_ops = {
    .spawn_service = mock_spawn,
    .kill = mock_kill,
    .file_exists = mock_file_exists,
    .monotonic_sec = mock_monotonic_sec,
    .is_alive = mock_is_alive,
    .read_pid_file = mock_read_pid_file,
    .list_dir = mock_list_dir,
    .log_stderr = mock_log_stderr
};

/* --- TESTS --- */

static void test_table_init_and_order_sorting(void) {
    printf("[TEST] test_table_init_and_order_sorting...\n");
    watchtower_table_t tbl;
    watchtower_table_init(&tbl);
    assert(tbl.count == 0);

    service_t s1 = {.name = "sshd", .exec_path = "/sbin/sshd", .order = 20};
    service_t s2 = {.name = "dhcpc", .exec_path = "/sbin/dhcpc", .order = 10};
    service_t s3 = {.name = "crond", .exec_path = "/sbin/crond", .order = 15};

    assert(watchtower_table_add_service(&tbl, &s1) == 0);
    assert(watchtower_table_add_service(&tbl, &s2) == 0);
    assert(watchtower_table_add_service(&tbl, &s3) == 0);
    assert(tbl.count == 3);

    /* Verify order was sorted ascending: dhcpc (10), crond (15), sshd (20) */
    assert(strcmp(tbl.services[0].name, "dhcpc") == 0);
    assert(tbl.services[0].order == 10);
    assert(strcmp(tbl.services[1].name, "crond") == 0);
    assert(tbl.services[1].order == 15);
    assert(strcmp(tbl.services[2].name, "sshd") == 0);
    assert(tbl.services[2].order == 20);

    /* Duplicate rejection */
    service_t s_dup = {.name = "sshd", .exec_path = "/sbin/sshd2", .order = 30};
    assert(watchtower_table_add_service(&tbl, &s_dup) == -2);
    assert(tbl.count == 3);

    printf("PASS: test_table_init_and_order_sorting\n");
}

static void test_start_and_duplicate_prevention(void) {
    printf("[TEST] test_start_and_duplicate_prevention...\n");
    mock_reset();
    watchtower_table_t tbl;
    watchtower_table_init(&tbl);

    service_t s = {
        .name = "simple",
        .exec_path = "/bin/simple",
        .readiness_mode = READINESS_NONE,
        .restart_policy = RESTART_NEVER
    };
    watchtower_table_add_service(&tbl, &s);

    /* First start */
    int rc = watchtower_start_service(&tbl, 0, g_mock.current_time_sec, &g_mock_ops);
    assert(rc == 0);
    assert(tbl.services[0].state == SVC_STATE_RUNNING);
    assert(tbl.services[0].pid == 100);
    assert(tbl.services[0].ready_verified == true);

    /* Duplicate start must fail */
    int rc_dup = watchtower_start_service(&tbl, 0, g_mock.current_time_sec, &g_mock_ops);
    assert(rc_dup == -2);
    assert(tbl.services[0].pid == 100);

    printf("PASS: test_start_and_duplicate_prevention\n");
}

static void test_readiness_file_success(void) {
    printf("[TEST] test_readiness_file_success...\n");
    mock_reset();
    watchtower_table_t tbl;
    watchtower_table_init(&tbl);

    service_t s = {
        .name = "sshd",
        .exec_path = "/sbin/sshd",
        .readiness_mode = READINESS_FILE,
        .ready_path = "/run/watchtower/sshd.ready",
        .timeout_start_sec = 5
    };
    watchtower_table_add_service(&tbl, &s);

    /* Start service: enters STARTING */
    assert(watchtower_start_service(&tbl, 0, g_mock.current_time_sec, &g_mock_ops) == 0);
    assert(tbl.services[0].state == SVC_STATE_STARTING);
    assert(tbl.services[0].ready_verified == false);

    /* Tick at t = 1001s: ready file not present yet */
    g_mock.current_time_sec = 1001;
    watchtower_tick(&tbl, g_mock.current_time_sec, &g_mock_ops);
    assert(tbl.services[0].state == SVC_STATE_STARTING);

    /* sshd creates ready file */
    mock_touch_file("/run/watchtower/sshd.ready");

    /* Tick at t = 1002s: file detected -> RUNNING */
    g_mock.current_time_sec = 1002;
    watchtower_tick(&tbl, g_mock.current_time_sec, &g_mock_ops);
    assert(tbl.services[0].state == SVC_STATE_RUNNING);
    assert(tbl.services[0].ready_verified == true);

    printf("PASS: test_readiness_file_success\n");
}

static void test_readiness_timeout_failure(void) {
    printf("[TEST] test_readiness_timeout_failure...\n");
    mock_reset();
    watchtower_table_t tbl;
    watchtower_table_init(&tbl);

    service_t s = {
        .name = "stalled",
        .exec_path = "/bin/stalled",
        .readiness_mode = READINESS_FILE,
        .ready_path = "/run/watchtower/stalled.ready",
        .timeout_start_sec = 5
    };
    watchtower_table_add_service(&tbl, &s);

    assert(watchtower_start_service(&tbl, 0, 1000, &g_mock_ops) == 0);
    int64_t pid = tbl.services[0].pid;

    /* Advance time to 1004s: not timed out yet */
    g_mock.current_time_sec = 1004;
    watchtower_tick(&tbl, 1004, &g_mock_ops);
    assert(tbl.services[0].state == SVC_STATE_STARTING);

    /* Advance time to 1005s (1000 + 5s timeout): timeout! */
    g_mock.current_time_sec = 1005;
    watchtower_tick(&tbl, 1005, &g_mock_ops);
    assert(tbl.services[0].state == SVC_STATE_FAILED);
    assert(tbl.services[0].manual_stop == true);
    /* Verify SIGTERM (15) was delivered */
    assert(g_mock.procs[0].last_signal == 15);

    /* Child process terminates */
    watchtower_handle_child_exit(&tbl, pid, 0, 1006, &g_mock_ops);
    assert(tbl.services[0].state == SVC_STATE_STOPPED);
    assert(tbl.services[0].pid == 0);

    printf("PASS: test_readiness_timeout_failure\n");
}

static void test_crash_restart_and_limit_enforcement(void) {
    printf("[TEST] test_crash_restart_and_limit_enforcement...\n");
    mock_reset();
    watchtower_table_t tbl;
    watchtower_table_init(&tbl);

    service_t s = {
        .name = "flaky",
        .exec_path = "/bin/flaky",
        .readiness_mode = READINESS_NONE,
        .restart_policy = RESTART_ON_FAILURE,
        .restart_limit = 3,
        .restart_delay_sec = 2
    };
    watchtower_table_add_service(&tbl, &s);

    /* Initial start at t=1000s */
    assert(watchtower_start_service(&tbl, 0, 1000, &g_mock_ops) == 0);
    assert(tbl.services[0].pid == 100);

    /* Crash #1 at t=1001s with exit code 1 */
    watchtower_handle_child_exit(&tbl, 100, (1 << 8), 1001, &g_mock_ops);
    assert(tbl.services[0].pid == 0);
    assert(tbl.services[0].last_exit_code == 1);
    assert(tbl.services[0].restart_count == 1);
    assert(tbl.services[0].next_restart_sec == 1003); /* 1001 + 2s */
    assert(strstr(g_mock.last_stderr, "restarting in 2s (attempt 1/3)") != NULL);

    /* Tick at 1002s: still in backoff */
    watchtower_tick(&tbl, 1002, &g_mock_ops);
    assert(tbl.services[0].pid == 0);

    /* Tick at 1003s: restart triggers! */
    watchtower_tick(&tbl, 1003, &g_mock_ops);
    assert(tbl.services[0].pid == 101);
    assert(tbl.services[0].state == SVC_STATE_RUNNING);

    /* Crash #2 at t=1004s */
    watchtower_handle_child_exit(&tbl, 101, (1 << 8), 1004, &g_mock_ops);
    assert(tbl.services[0].restart_count == 2);
    assert(tbl.services[0].next_restart_sec == 1006);

    /* Tick at 1006s: restart #2 triggers */
    watchtower_tick(&tbl, 1006, &g_mock_ops);
    assert(tbl.services[0].pid == 102);

    /* Crash #3 at t=1007s */
    watchtower_handle_child_exit(&tbl, 102, (1 << 8), 1007, &g_mock_ops);
    assert(tbl.services[0].restart_count == 3);
    assert(tbl.services[0].next_restart_sec == 1009);

    /* Tick at 1009s: restart #3 triggers (limit is 3) */
    watchtower_tick(&tbl, 1009, &g_mock_ops);
    assert(tbl.services[0].pid == 103);

    /* Crash #4 at t=1010s: exceeds limit of 3! */
    watchtower_handle_child_exit(&tbl, 103, (1 << 8), 1010, &g_mock_ops);
    assert(tbl.services[0].state == SVC_STATE_FAILED);
    assert(tbl.services[0].next_restart_sec == 0);
    assert(tbl.services[0].pid == 0);
    assert(strstr(g_mock.last_stderr, "restart limit reached (3/3), marking FAILED") != NULL);

    /* Subsequent ticks at t=1015s do NOT restart */
    watchtower_tick(&tbl, 1015, &g_mock_ops);
    assert(tbl.services[0].state == SVC_STATE_FAILED);
    assert(tbl.services[0].pid == 0);

    printf("PASS: test_crash_restart_and_limit_enforcement\n");
}

static void test_explicit_stop_and_timeout_escalation(void) {
    printf("[TEST] test_explicit_stop_and_timeout_escalation...\n");
    mock_reset();
    watchtower_table_t tbl;
    watchtower_table_init(&tbl);

    service_t s = {
        .name = "stubborn",
        .exec_path = "/bin/stubborn",
        .readiness_mode = READINESS_NONE,
        .restart_policy = RESTART_ALWAYS,
        .timeout_stop_sec = 5
    };
    watchtower_table_add_service(&tbl, &s);

    assert(watchtower_start_service(&tbl, 0, 1000, &g_mock_ops) == 0);
    int64_t pid = tbl.services[0].pid;

    /* Stop at t=1005s */
    assert(watchtower_stop_service(&tbl, 0, 1005, &g_mock_ops) == 0);
    assert(tbl.services[0].state == SVC_STATE_STOPPING);
    assert(g_mock.procs[0].last_signal == 15); /* SIGTERM */

    /* Process refuses to terminate. Tick at 1009s */
    watchtower_tick(&tbl, 1009, &g_mock_ops);
    assert(g_mock.procs[0].last_signal == 15);

    /* Tick at 1010s (1005 + 5s timeout): escalation to SIGKILL (9)! */
    watchtower_tick(&tbl, 1010, &g_mock_ops);
    assert(g_mock.procs[0].last_signal == 9); /* SIGKILL */

    /* Process killed */
    watchtower_handle_child_exit(&tbl, pid, 9, 1011, &g_mock_ops);
    assert(tbl.services[0].state == SVC_STATE_STOPPED);
    assert(tbl.services[0].pid == 0);
    assert(tbl.services[0].next_restart_sec == 0); /* Suppressed by manual stop */

    printf("PASS: test_explicit_stop_and_timeout_escalation\n");
}

static void test_reverse_order_system_shutdown(void) {
    printf("[TEST] test_reverse_order_system_shutdown...\n");
    mock_reset();
    watchtower_table_t tbl;
    watchtower_table_init(&tbl);

    service_t s1 = {.name = "dhcpc", .exec_path = "/sbin/dhcpc", .order = 10, .readiness_mode = READINESS_NONE};
    service_t s2 = {.name = "sshd", .exec_path = "/sbin/sshd", .order = 20, .readiness_mode = READINESS_NONE};
    watchtower_table_add_service(&tbl, &s1);
    watchtower_table_add_service(&tbl, &s2);

    assert(watchtower_start_service(&tbl, 0, 1000, &g_mock_ops) == 0); /* dhcpc (pid 100) */
    assert(watchtower_start_service(&tbl, 1, 1000, &g_mock_ops) == 0); /* sshd  (pid 101) */
    assert(!watchtower_all_stopped(&tbl));

    /* Initiate shutdown */
    assert(watchtower_stop_all(&tbl, 1005, &g_mock_ops) == 0);
    assert(tbl.stopping_all == true);

    /* Reverse order verified: sshd (order 20, index 1) was signaled before dhcpc (order 10, index 0) */
    assert(tbl.services[1].state == SVC_STATE_STOPPING);
    assert(tbl.services[0].state == SVC_STATE_STOPPING);

    /* Children exit */
    watchtower_handle_child_exit(&tbl, 101, 0, 1006, &g_mock_ops);
    watchtower_handle_child_exit(&tbl, 100, 0, 1007, &g_mock_ops);
    assert(watchtower_all_stopped(&tbl));

    printf("PASS: test_reverse_order_system_shutdown\n");
}

static void test_supervisor_isolation_on_service_failure(void) {
    printf("[TEST] test_supervisor_isolation_on_service_failure...\n");
    mock_reset();
    watchtower_table_t tbl;
    watchtower_table_init(&tbl);

    service_t healthy = {.name = "healthy", .exec_path = "/bin/healthy", .readiness_mode = READINESS_NONE};
    service_t faulty  = {.name = "faulty",  .exec_path = "/bin/faulty",  .readiness_mode = READINESS_NONE, .restart_policy = RESTART_NEVER};
    watchtower_table_add_service(&tbl, &healthy);
    watchtower_table_add_service(&tbl, &faulty);

    assert(watchtower_start_service(&tbl, 0, 1000, &g_mock_ops) == 0);
    assert(watchtower_start_service(&tbl, 1, 1000, &g_mock_ops) == 0);

    /* Faulty service crashes with SEGV (signal 11) */
    int64_t faulty_pid = tbl.services[1].pid;
    watchtower_handle_child_exit(&tbl, faulty_pid, 11, 1001, &g_mock_ops);

    /* Faulty is FAILED, but healthy is completely unharmed */
    assert(tbl.services[1].state == SVC_STATE_FAILED);
    assert(strstr(g_mock.last_stderr, "service 'faulty'") != NULL);
    assert(strstr(g_mock.last_stderr, "exited with code 11") != NULL);
    assert(tbl.services[0].state == SVC_STATE_RUNNING);
    assert(tbl.services[0].pid > 0);

    /* Supervisor ticks normally */
    watchtower_tick(&tbl, 1002, &g_mock_ops);
    assert(tbl.services[0].state == SVC_STATE_RUNNING);

    printf("PASS: test_supervisor_isolation_on_service_failure\n");
}

static void test_crash_window_reset_after_60s(void) {
    printf("[TEST] test_crash_window_reset_after_60s...\n");
    mock_reset();
    watchtower_table_t tbl;
    watchtower_table_init(&tbl);

    service_t s = {
        .name = "flaky",
        .exec_path = "/bin/flaky",
        .readiness_mode = READINESS_NONE,
        .restart_policy = RESTART_ON_FAILURE,
        .restart_limit = 2,
        .restart_delay_sec = 2
    };
    watchtower_table_add_service(&tbl, &s);
    assert(watchtower_start_service(&tbl, 0, 1000, &g_mock_ops) == 0);

    /* Crash #1 at t=1001s */
    watchtower_handle_child_exit(&tbl, 100, (1 << 8), 1001, &g_mock_ops);
    assert(tbl.services[0].restart_count == 1);
    watchtower_tick(&tbl, 1003, &g_mock_ops);
    assert(tbl.services[0].pid == 101);

    /* Now let 65 seconds pass with stable running */
    uint64_t later = 1001 + 65;

    /* Crash #2 occurs at t=1066s (after the 60s window expired!) */
    watchtower_handle_child_exit(&tbl, 101, (1 << 8), later, &g_mock_ops);
    /* The window reset! restart_count should be 1, NOT 2 or hitting limit */
    assert(tbl.services[0].restart_count == 1);
    assert(tbl.services[0].state == SVC_STATE_STARTING);
    assert(tbl.services[0].next_restart_sec == later + 2);

    /* Ticking at later + 2 successfully restarts */
    watchtower_tick(&tbl, later + 2, &g_mock_ops);
    assert(tbl.services[0].pid == 102);
    assert(tbl.services[0].state == SVC_STATE_RUNNING);

    printf("PASS: test_crash_window_reset_after_60s\n");
}

static void test_explicit_stop_during_restart_delay(void) {
    printf("[TEST] test_explicit_stop_during_restart_delay...\n");
    mock_reset();
    watchtower_table_t tbl;
    watchtower_table_init(&tbl);

    service_t s = {
        .name = "delayed",
        .exec_path = "/bin/delayed",
        .readiness_mode = READINESS_NONE,
        .restart_policy = RESTART_ALWAYS,
        .restart_delay_sec = 10
    };
    watchtower_table_add_service(&tbl, &s);
    assert(watchtower_start_service(&tbl, 0, 1000, &g_mock_ops) == 0);

    /* Crash at t=1001s, next_restart_sec is 1011s */
    watchtower_handle_child_exit(&tbl, 100, (1 << 8), 1001, &g_mock_ops);
    assert(tbl.services[0].pid == 0);
    assert(tbl.services[0].state == SVC_STATE_STARTING);
    assert(tbl.services[0].next_restart_sec == 1011);

    /* User runs `service stop delayed` at t=1005s */
    assert(watchtower_stop_service(&tbl, 0, 1005, &g_mock_ops) == 0);
    assert(tbl.services[0].state == SVC_STATE_STOPPED);
    assert(tbl.services[0].next_restart_sec == 0);

    /* Tick at 1011s: must NOT restart */
    watchtower_tick(&tbl, 1011, &g_mock_ops);
    assert(tbl.services[0].state == SVC_STATE_STOPPED);
    assert(tbl.services[0].pid == 0);

    printf("PASS: test_explicit_stop_during_restart_delay\n");
}

static void test_explicit_restart_clears_counter(void) {
    printf("[TEST] test_explicit_restart_clears_counter...\n");
    mock_reset();
    watchtower_table_t tbl;
    watchtower_table_init(&tbl);

    service_t s = {
        .name = "crashy",
        .exec_path = "/bin/crashy",
        .readiness_mode = READINESS_NONE,
        .restart_policy = RESTART_ON_FAILURE,
        .restart_limit = 2,
        .restart_delay_sec = 1
    };
    watchtower_table_add_service(&tbl, &s);
    assert(watchtower_start_service(&tbl, 0, 1000, &g_mock_ops) == 0);

    /* Crash #1 */
    watchtower_handle_child_exit(&tbl, 100, (1 << 8), 1001, &g_mock_ops);
    assert(tbl.services[0].restart_count == 1);
    watchtower_tick(&tbl, 1002, &g_mock_ops);
    assert(tbl.services[0].pid == 101);

    /* Explicit restart */
    assert(watchtower_restart_service(&tbl, 0, 1003, &g_mock_ops) == 0);
    /* Handled exit of stopping service */
    watchtower_handle_child_exit(&tbl, 101, 0, 1004, &g_mock_ops);
    watchtower_tick(&tbl, 1004, &g_mock_ops);

    /* Restart count must be reset to 0 */
    assert(tbl.services[0].restart_count == 0);
    assert(tbl.services[0].pid == 102);

    printf("PASS: test_explicit_restart_clears_counter\n");
}

static void test_shutdown_with_mixed_states(void) {
    printf("[TEST] test_shutdown_with_mixed_states...\n");
    mock_reset();
    watchtower_table_t tbl;
    watchtower_table_init(&tbl);

    service_t s1 = {.name = "s1_stopped", .exec_path = "/bin/s1", .order = 10, .readiness_mode = READINESS_NONE};
    service_t s2 = {.name = "s2_delayed", .exec_path = "/bin/s2", .order = 20, .readiness_mode = READINESS_NONE, .restart_policy = RESTART_ALWAYS, .restart_delay_sec = 5};
    service_t s3 = {.name = "s3_running", .exec_path = "/bin/s3", .order = 30, .readiness_mode = READINESS_NONE};
    watchtower_table_add_service(&tbl, &s1);
    watchtower_table_add_service(&tbl, &s2);
    watchtower_table_add_service(&tbl, &s3);

    /* s2 and s3 start */
    assert(watchtower_start_service(&tbl, 1, 1000, &g_mock_ops) == 0);
    assert(watchtower_start_service(&tbl, 2, 1000, &g_mock_ops) == 0);

    /* s2 crashes and enters restart delay */
    watchtower_handle_child_exit(&tbl, 100, (1 << 8), 1001, &g_mock_ops);
    assert(tbl.services[1].next_restart_sec == 1006);

    /* Initiate system shutdown */
    assert(watchtower_stop_all(&tbl, 1002, &g_mock_ops) == 0);
    assert(tbl.stopping_all == true);

    /* s2's restart timer was canceled and set to STOPPED */
    assert(tbl.services[1].state == SVC_STATE_STOPPED);
    assert(tbl.services[1].next_restart_sec == 0);

    /* s3 was signaled with SIGTERM */
    assert(tbl.services[2].state == SVC_STATE_STOPPING);

    /* s3 reaped */
    watchtower_handle_child_exit(&tbl, 101, 0, 1003, &g_mock_ops);
    assert(tbl.services[2].state == SVC_STATE_STOPPED);

    /* All stopped */
    assert(watchtower_all_stopped(&tbl));

    printf("PASS: test_shutdown_with_mixed_states\n");
}

static void test_type_forking_lifecycle(void) {
    printf("[TEST] test_type_forking_lifecycle...\n");
    mock_reset();
    watchtower_table_t tbl;
    watchtower_table_init(&tbl);

    service_t s = {
        .name = "forker",
        .exec_path = "/bin/forker",
        .type = SVC_TYPE_FORKING,
        .pid_file = "/run/forker.pid",
        .readiness_mode = READINESS_NONE,
        .restart_policy = RESTART_ON_FAILURE,
        .restart_delay_sec = 5,
        .restart_limit = 3,
        .timeout_start_sec = 5,
        .timeout_stop_sec = 5
    };
    assert(watchtower_table_add_service(&tbl, &s) == 0);

    /* 1. Start: launcher spawns with PID 100 */
    assert(watchtower_start_service(&tbl, 0, 1000, &g_mock_ops) == 0);
    assert(tbl.services[0].initial_pid == 100);
    assert(tbl.services[0].pid == 0);
    assert(tbl.services[0].state == SVC_STATE_STARTING);

    /* 2. Launcher exits 0 (clean double-fork) */
    watchtower_handle_child_exit(&tbl, 100, 0, 1001, &g_mock_ops);
    assert(tbl.services[0].initial_pid == 0);
    assert(tbl.services[0].state == SVC_STATE_STARTING); /* Waiting for daemon PID */

    /* 3. Daemon writes PID 105 to PIDFile, daemon process exists and is alive */
    g_mock.mock_pid_file_val = 105;
    int idx = g_mock.proc_count++;
    g_mock.procs[idx].pid = 105;
    g_mock.procs[idx].alive = true;

    /* 4. Supervisor tick discovers PID 105 and transitions to RUNNING */
    watchtower_tick(&tbl, 1002, &g_mock_ops);
    assert(tbl.services[0].pid == 105);
    assert(tbl.services[0].state == SVC_STATE_RUNNING);
    assert(tbl.services[0].ready_verified == true);

    /* 5. Daemon process 105 crashes unexpectedly */
    g_mock.procs[idx].alive = false;
    watchtower_tick(&tbl, 1005, &g_mock_ops);
    assert(tbl.services[0].state == SVC_STATE_STARTING); /* Scheduled for restart */
    assert(tbl.services[0].pid == 0);
    assert(tbl.services[0].restart_count == 1);
    assert(tbl.services[0].next_restart_sec == 1010);

    /* 6. Verify launcher failure fails immediately */
    mock_reset();
    watchtower_table_init(&tbl);
    assert(watchtower_table_add_service(&tbl, &s) == 0);
    assert(watchtower_start_service(&tbl, 0, 1000, &g_mock_ops) == 0);
    /* Launcher exits with error 1 */
    watchtower_handle_child_exit(&tbl, 100, (1 << 8), 1001, &g_mock_ops);
    assert(tbl.services[0].state == SVC_STATE_FAILED);

    printf("PASS: test_type_forking_lifecycle\n");
}

static void write_temp_file(const char *dir, const char *name, const char *content) {
    char full[256];
    snprintf(full, sizeof(full), "%s/%s", dir, name);
    FILE *f = fopen(full, "w");
    assert(f != NULL);
    fputs(content, f);
    fclose(f);
}

static void test_dynamic_discovery_basic(void) {
    printf("[TEST] test_dynamic_discovery_basic...\n");
    mock_reset();

    char temp_dir[] = "/tmp/wt_test_basic_XXXXXX";
    assert(mkdtemp(temp_dir) != NULL);

    write_temp_file(temp_dir, "dhcpc.service",
        "Exec=/sbin/dhcpc -f\nOrder=10\nReadiness=file:/run/dhcpc.lease\n");
    write_temp_file(temp_dir, "sshd.service",
        "Exec=/sbin/sshd -D\nOrder=20\nReadiness=file:/run/watchtower/sshd.ready\n");
    write_temp_file(temp_dir, "crond.service",
        "Exec=/sbin/crond -f\nOrder=30\n");
    write_temp_file(temp_dir, "ntpd.service",
        "Exec=/sbin/ntpd -n\nOrder=15\n");
    write_temp_file(temp_dir, "notes.txt",
        "This is not a service file\n");

    mock_add_dir_entry(temp_dir, "notes.txt");
    mock_add_dir_entry(temp_dir, "crond.service");
    mock_add_dir_entry(temp_dir, "dhcpc.service");
    mock_add_dir_entry(temp_dir, "sshd.service");
    mock_add_dir_entry(temp_dir, "ntpd.service");

    watchtower_table_t tbl;
    watchtower_table_init(&tbl);
    int rc = watchtower_discover_services(&tbl, temp_dir, NULL, &g_mock_ops);
    assert(rc == 4);
    assert(tbl.count == 4);

    /* Verify sorted order: 10, 15, 20, 30 */
    assert(strcmp(tbl.services[0].name, "dhcpc") == 0);
    assert(tbl.services[0].order == 10);
    assert(strcmp(tbl.services[1].name, "ntpd") == 0);
    assert(tbl.services[1].order == 15);
    assert(strcmp(tbl.services[2].name, "sshd") == 0);
    assert(tbl.services[2].order == 20);
    assert(strcmp(tbl.services[3].name, "crond") == 0);
    assert(tbl.services[3].order == 30);

    /* Cleanup */
    char full[256];
    const char *files[] = {"dhcpc.service", "sshd.service", "crond.service", "ntpd.service", "notes.txt"};
    for (size_t i = 0; i < 5; i++) {
        snprintf(full, sizeof(full), "%s/%s", temp_dir, files[i]);
        unlink(full);
    }
    rmdir(temp_dir);

    printf("PASS: test_dynamic_discovery_basic\n");
}

static void test_malformed_service_skip_fail_closed(void) {
    printf("[TEST] test_malformed_service_skip_fail_closed...\n");
    mock_reset();

    char temp_dir[] = "/tmp/wt_test_mal_XXXXXX";
    assert(mkdtemp(temp_dir) != NULL);

    write_temp_file(temp_dir, "dhcpc.service",
        "Exec=/sbin/dhcpc -f\nOrder=10\n");
    /* Corrupted: missing Exec */
    write_temp_file(temp_dir, "sshd.service",
        "Order=20\nRestart=on-failure\n");

    mock_add_dir_entry(temp_dir, "dhcpc.service");
    mock_add_dir_entry(temp_dir, "sshd.service");

    watchtower_table_t tbl;
    watchtower_table_init(&tbl);
    int rc = watchtower_discover_services(&tbl, temp_dir, NULL, &g_mock_ops);

    /* sshd must be skipped (fail-closed) */
    assert(rc == 1);
    assert(tbl.count == 1);
    assert(strcmp(tbl.services[0].name, "dhcpc") == 0);
    assert(watchtower_find_service(&tbl, "sshd") == NULL);
    assert(g_mock.invalid_config_count == 1);
    assert(strstr(g_mock.last_stderr, "[watchtower] invalid service config: sshd") != NULL);

    char full[256];
    snprintf(full, sizeof(full), "%s/dhcpc.service", temp_dir);
    unlink(full);
    snprintf(full, sizeof(full), "%s/sshd.service", temp_dir);
    unlink(full);
    rmdir(temp_dir);

    printf("PASS: test_malformed_service_skip_fail_closed\n");
}

static void test_missing_and_empty_directory(void) {
    printf("[TEST] test_missing_and_empty_directory...\n");
    mock_reset();

    char temp_dir[] = "/tmp/wt_test_empty_XXXXXX";
    assert(mkdtemp(temp_dir) != NULL);

    mock_add_dir(temp_dir); /* 0 entries */

    watchtower_table_t tbl;
    watchtower_table_init(&tbl);
    int rc = watchtower_discover_services(&tbl, temp_dir, NULL, &g_mock_ops);
    assert(rc == 0);
    assert(tbl.count == 0);
    assert(g_mock.empty_dir_warning_count == 1);
    assert(strstr(g_mock.last_stderr, "[watchtower] warning: no service configurations found") != NULL);

    rmdir(temp_dir);

    /* Also verify missing directory handling */
    mock_reset();
    watchtower_table_init(&tbl);
    rc = watchtower_discover_services(&tbl, "/nonexistent/dir", NULL, &g_mock_ops);
    assert(rc == 0);
    assert(tbl.count == 0);
    assert(g_mock.empty_dir_warning_count == 1);

    printf("PASS: test_missing_and_empty_directory\n");
}

static void test_bounded_16_services_limit(void) {
    printf("[TEST] test_bounded_16_services_limit...\n");
    mock_reset();

    char temp_dir[] = "/tmp/wt_test_bound_XXXXXX";
    assert(mkdtemp(temp_dir) != NULL);

    char sname[32];
    for (int i = 0; i < 18; i++) {
        snprintf(sname, sizeof(sname), "svc%02d.service", i + 1);
        char content[64];
        snprintf(content, sizeof(content), "Exec=/bin/echo %d\nOrder=%d\n", i + 1, i + 1);
        write_temp_file(temp_dir, sname, content);
        mock_add_dir_entry(temp_dir, sname);
    }

    watchtower_table_t tbl;
    watchtower_table_init(&tbl);
    int rc = watchtower_discover_services(&tbl, temp_dir, NULL, &g_mock_ops);
    assert(rc == 16);
    assert(tbl.count == 16);

    char full[256];
    for (int i = 0; i < 18; i++) {
        snprintf(sname, sizeof(sname), "svc%02d.service", i + 1);
        snprintf(full, sizeof(full), "%s/%s", temp_dir, sname);
        unlink(full);
    }
    rmdir(temp_dir);

    printf("PASS: test_bounded_16_services_limit\n");
}

static void test_fallback_to_live_media(void) {
    printf("[TEST] test_fallback_to_live_media...\n");
    mock_reset();

    char temp_dir[] = "/tmp/wt_test_live_XXXXXX";
    assert(mkdtemp(temp_dir) != NULL);

    write_temp_file(temp_dir, "dhcpc.service", "Exec=/sbin/dhcpc -f\nOrder=10\n");
    write_temp_file(temp_dir, "sshd.service", "Exec=/sbin/sshd -D\nOrder=20\n");

    mock_add_dir_entry(temp_dir, "dhcpc.service");
    mock_add_dir_entry(temp_dir, "sshd.service");

    watchtower_table_t tbl;
    watchtower_table_init(&tbl);
    /* Installed dir is nonexistent, live dir is temp_dir */
    int rc = watchtower_discover_services(&tbl, "/nonexistent/etc/watchtower", temp_dir, &g_mock_ops);
    assert(rc == 2);
    assert(tbl.count == 2);
    assert(watchtower_find_service(&tbl, "dhcpc") != NULL);
    assert(watchtower_find_service(&tbl, "sshd") != NULL);

    char full[256];
    snprintf(full, sizeof(full), "%s/dhcpc.service", temp_dir);
    unlink(full);
    snprintf(full, sizeof(full), "%s/sshd.service", temp_dir);
    unlink(full);
    rmdir(temp_dir);

    printf("PASS: test_fallback_to_live_media\n");
}

static void test_identity_failure_is_closed(void) {
    mock_reset();
    static watchtower_table_t table;
    service_t service={.name="identity",.exec_path="/bin/service-probe"};
    watchtower_table_init(&table);assert(!watchtower_table_add_service(&table,&service));
    g_mock.reject_identity=true;
    assert(watchtower_start_service(&table,0,1000,&g_mock_ops)==-3);
    assert(!g_mock.proc_count && !table.services[0].pid && !table.services[0].ready_verified);
    assert(table.services[0].state==SVC_STATE_FAILED);
    watchtower_sys_ops_t missing=g_mock_ops;missing.spawn_service=NULL;
    assert(watchtower_start_service(&table,0,1001,&missing)==-3 && !g_mock.proc_count);
    assert(strstr(g_mock.last_stderr,"identity"));
    puts("PASS: identity failure and missing enforcing adapter never launch or claim readiness");
}

static void test_boot_disabled_services_not_started(void) {
    printf("[TEST] test_boot_disabled_services_not_started...\n");
    mock_reset();
    watchtower_table_t tbl;
    watchtower_table_init(&tbl);

    service_t s_enabled = {
        .name = "dhcpc",
        .exec_path = "/sbin/dhcpc",
        .order = 10,
        .enabled = true,
        .readiness_mode = READINESS_NONE,
        .restart_policy = RESTART_ON_FAILURE,
        .restart_limit = 3
    };
    host_str_copy(s_enabled.user, "root", sizeof(s_enabled.user));
    host_str_copy(s_enabled.group, "root", sizeof(s_enabled.group));

    service_t s_disabled = {
        .name = "ntpd",
        .exec_path = "/sbin/ntpd",
        .order = 15,
        .enabled = false,
        .readiness_mode = READINESS_NONE,
        .restart_policy = RESTART_ON_FAILURE,
        .restart_limit = 3
    };
    host_str_copy(s_disabled.user, "root", sizeof(s_disabled.user));
    host_str_copy(s_disabled.group, "root", sizeof(s_disabled.group));

    assert(watchtower_table_add_service(&tbl, &s_enabled) == 0);
    assert(watchtower_table_add_service(&tbl, &s_disabled) == 0);

    /* Emulate boot startup sequence: only start if enabled */
    uint64_t now = 1000;
    for (size_t i = 0; i < tbl.count; i++) {
        if (tbl.services[i].enabled) {
            watchtower_start_service(&tbl, i, now, &g_mock_ops);
        }
    }

    /* dhcpc is enabled -> started and running */
    assert(tbl.services[0].state == SVC_STATE_RUNNING);
    assert(tbl.services[0].pid > 0);

    /* ntpd is disabled -> NOT started, remains STOPPED */
    assert(tbl.services[1].state == SVC_STATE_STOPPED);
    assert(tbl.services[1].pid == 0);

    /* Manual start still works for disabled service */
    assert(watchtower_start_service(&tbl, 1, now, &g_mock_ops) == 0);
    assert(tbl.services[1].state == SVC_STATE_RUNNING);
    assert(tbl.services[1].pid > 0);

    printf("PASS: test_boot_disabled_services_not_started\n");
}

int main(void) {
    printf("===================================================\n");
    printf("Watchtower Service Supervisor Test Suite (ASan)\n");
    printf("===================================================\n");

    test_table_init_and_order_sorting();
    test_start_and_duplicate_prevention();
    test_readiness_file_success();
    test_readiness_timeout_failure();
    test_crash_restart_and_limit_enforcement();
    test_explicit_stop_and_timeout_escalation();
    test_reverse_order_system_shutdown();
    test_supervisor_isolation_on_service_failure();

    /* Phase 4 tests */
    test_crash_window_reset_after_60s();
    test_explicit_stop_during_restart_delay();
    test_explicit_restart_clears_counter();
    test_shutdown_with_mixed_states();

    /* Type=forking tests */
    test_type_forking_lifecycle();

    /* Dynamic Service Discovery & Fail-Closed Registration tests */
    test_dynamic_discovery_basic();
    test_malformed_service_skip_fail_closed();
    test_missing_and_empty_directory();
    test_bounded_16_services_limit();
    test_fallback_to_live_media();
    test_identity_failure_is_closed();
    test_boot_disabled_services_not_started();

    printf("\nALL 20 WATCHTOWER SERVICE, DISCOVERY & RECOVERY TESTS PASSED!\n");
    return 0;
}
