#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include "string.h"
#include "../user/crond/crond.h"

int crond_run_with_ops(int argc, char **argv, const crond_ops_t *ops, volatile int *stop_flag);

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

static void host_str_cat(char *dst, const char *src, size_t cap) {
    if (!dst || cap == 0) return;
    size_t len = strlen(dst);
    if (len >= cap) return;
    size_t i = 0;
    if (src) {
        while (src[i] && len + 1 < cap) {
            dst[len++] = src[i++];
        }
    }
    dst[len] = '\0';
}

/* =========================================================================
 * Mock Framework
 * ========================================================================= */

#define MOCK_MAX_SPAWNS 64
typedef struct {
    char path[CROND_MAX_PATH_LEN];
    char args[CROND_MAX_ARGS][CROND_MAX_ARG_LEN];
    int argc;
    int64_t pid;
} mock_spawn_record_t;

typedef struct {
    mock_spawn_record_t spawns[MOCK_MAX_SPAWNS];
    int spawn_count;
    int64_t next_pid;
    bool fail_next_spawn;

    uint64_t simulated_uptime_sec;
    int sleep_call_count;
    int reap_call_count;

    char last_log[512];
    int log_count;

    char primary_file_content[CROND_CONFIG_MAX_SIZE];
    bool primary_file_exists;

    char fallback_file_content[CROND_CONFIG_MAX_SIZE];
    bool fallback_file_exists;
    bool root_readonly;
} mock_env_t;

static mock_env_t g_mock;

static void mock_reset(void) {
    memset(&g_mock, 0, sizeof(g_mock));
    g_mock.next_pid = 1000;
}

static int64_t mock_spawn(const char *path, const char *const argv[]) {
    if (g_mock.fail_next_spawn) {
        g_mock.fail_next_spawn = false;
        return -1;
    }
    if (g_mock.spawn_count >= MOCK_MAX_SPAWNS) return -1;

    int idx = g_mock.spawn_count++;
    mock_spawn_record_t *rec = &g_mock.spawns[idx];
    host_str_copy(rec->path, path, sizeof(rec->path));
    rec->pid = g_mock.next_pid++;

    rec->argc = 0;
    if (argv) {
        /* argv[0] is program path, argv[1..] are arguments */
        int a = 1;
        while (argv[a] && rec->argc < CROND_MAX_ARGS) {
            host_str_copy(rec->args[rec->argc], argv[a], sizeof(rec->args[rec->argc]));
            rec->argc++;
            a++;
        }
    }
    return rec->pid;
}

static uint64_t mock_get_uptime_sec(void) {
    return g_mock.simulated_uptime_sec;
}

static void mock_sleep_sec(uint32_t sec) {
    g_mock.sleep_call_count++;
    g_mock.simulated_uptime_sec += sec;
}

static void mock_log_err(const char *msg) {
    if (!msg) return;
    g_mock.log_count++;
    host_str_copy(g_mock.last_log, msg, sizeof(g_mock.last_log));
}

static int mock_read_file(const char *path, char *buf, size_t buf_cap, size_t *out_len) {
    if (!path || !buf || buf_cap == 0 || !out_len) return -1;

    if (strcmp(path, CROND_CONFIG_PRIMARY) == 0) {
        if (!g_mock.primary_file_exists) return -1;
        size_t len = strlen(g_mock.primary_file_content);
        if (len >= buf_cap) return -1;
        memcpy(buf, g_mock.primary_file_content, len + 1);
        *out_len = len;
        return 0;
    }

    if (strcmp(path, CROND_CONFIG_FALLBACK) == 0) {
        if (!g_mock.fallback_file_exists) return -1;
        size_t len = strlen(g_mock.fallback_file_content);
        if (len >= buf_cap) return -1;
        memcpy(buf, g_mock.fallback_file_content, len + 1);
        *out_len = len;
        return 0;
    }

    return -1;
}

static int mock_write_file(const char *path, const char *buf, size_t len) {
    if (!path || !buf) return -1;
    if (g_mock.root_readonly) return -1;
    if (strcmp(path, CROND_CONFIG_PRIMARY) == 0) {
        if (len >= sizeof(g_mock.primary_file_content)) return -1;
        memcpy(g_mock.primary_file_content, buf, len);
        g_mock.primary_file_content[len] = '\0';
        g_mock.primary_file_exists = true;
        return 0;
    }
    return -1;
}

static void mock_reap_zombies(void) {
    g_mock.reap_call_count++;
}

static const crond_ops_t g_ops = {
    .spawn = mock_spawn,
    .get_uptime_sec = mock_get_uptime_sec,
    .sleep_sec = mock_sleep_sec,
    .log_err = mock_log_err,
    .read_file = mock_read_file,
    .write_file = mock_write_file,
    .reap_zombies = mock_reap_zombies
};

/* =========================================================================
 * Test Cases
 * ========================================================================= */

static void test_config_parse_valid(void) {
    printf("[TEST] test_config_parse_valid...\n");
    const char *cfg =
        "# System crontab for FortressOS\n"
        "\n"
        "60 /sbin/logrotate /etc/logrotate.conf\n"
        "5 /bin/sync\n"
        "10 /bin/echo \"hello world\" 'fortress os'\n"
        "1440 /sbin/daily-backup --full /var/backup\n";

    crond_table_t tbl;
    char err[256];
    int r = crond_parse_config(cfg, strlen(cfg), &tbl, err, sizeof(err));
    assert(r == 0);
    assert(tbl.task_count == 4);

    /* Task 0 */
    assert(tbl.tasks[0].interval_minutes == 60);
    assert(strcmp(tbl.tasks[0].exec_path, "/sbin/logrotate") == 0);
    assert(tbl.tasks[0].argc == 1);
    assert(strcmp(tbl.tasks[0].args[0], "/etc/logrotate.conf") == 0);

    /* Task 1 */
    assert(tbl.tasks[1].interval_minutes == 5);
    assert(strcmp(tbl.tasks[1].exec_path, "/bin/sync") == 0);
    assert(tbl.tasks[1].argc == 0);

    /* Task 2 (quoted args) */
    assert(tbl.tasks[2].interval_minutes == 10);
    assert(strcmp(tbl.tasks[2].exec_path, "/bin/echo") == 0);
    assert(tbl.tasks[2].argc == 2);
    assert(strcmp(tbl.tasks[2].args[0], "hello world") == 0);
    assert(strcmp(tbl.tasks[2].args[1], "fortress os") == 0);

    /* Task 3 */
    assert(tbl.tasks[3].interval_minutes == 1440);
    assert(strcmp(tbl.tasks[3].exec_path, "/sbin/daily-backup") == 0);
    assert(tbl.tasks[3].argc == 2);
    assert(strcmp(tbl.tasks[3].args[0], "--full") == 0);
    assert(strcmp(tbl.tasks[3].args[1], "/var/backup") == 0);

    printf("PASS: test_config_parse_valid\n");
}

static void test_config_parse_boundary_16_tasks(void) {
    printf("[TEST] test_config_parse_boundary_16_tasks...\n");
    char cfg[2048];
    cfg[0] = '\0';
    for (int i = 1; i <= 16; i++) {
        char line[64];
        snprintf(line, sizeof(line), "%d /bin/task%d arg%d\n", i, i, i);
        host_str_cat(cfg, line, sizeof(cfg));
    }

    crond_table_t tbl;
    char err[256];
    int r = crond_parse_config(cfg, strlen(cfg), &tbl, err, sizeof(err));
    assert(r == 0);
    assert(tbl.task_count == 16);
    assert(tbl.tasks[15].interval_minutes == 16);
    assert(strcmp(tbl.tasks[15].exec_path, "/bin/task16") == 0);

    printf("PASS: test_config_parse_boundary_16_tasks\n");
}

static void test_config_parse_reject_relative_path(void) {
    printf("[TEST] test_config_parse_reject_relative_path...\n");
    crond_table_t tbl;
    char err[256];

    /* Relative path without slash */
    const char *cfg1 = "60 logrotate /etc/logrotate.conf\n";
    assert(crond_parse_config(cfg1, strlen(cfg1), &tbl, err, sizeof(err)) != 0);
    assert(strstr(err, "absolute") != NULL);

    /* Relative path with subdirs */
    const char *cfg2 = "60 sbin/logrotate\n";
    assert(crond_parse_config(cfg2, strlen(cfg2), &tbl, err, sizeof(err)) != 0);

    /* Dot relative path */
    const char *cfg3 = "60 ./logrotate\n";
    assert(crond_parse_config(cfg3, strlen(cfg3), &tbl, err, sizeof(err)) != 0);

    /* Quoted relative path */
    const char *cfg4 = "60 \"logrotate\"\n";
    assert(crond_parse_config(cfg4, strlen(cfg4), &tbl, err, sizeof(err)) != 0);

    printf("PASS: test_config_parse_reject_relative_path\n");
}

static void test_config_parse_reject_bad_intervals(void) {
    printf("[TEST] test_config_parse_reject_bad_intervals...\n");
    crond_table_t tbl;
    char err[256];

    /* Interval 0 */
    const char *cfg0 = "0 /bin/echo\n";
    assert(crond_parse_config(cfg0, strlen(cfg0), &tbl, err, sizeof(err)) != 0);

    /* Negative interval */
    const char *cfg_neg = "-5 /bin/echo\n";
    assert(crond_parse_config(cfg_neg, strlen(cfg_neg), &tbl, err, sizeof(err)) != 0);

    /* Alphabetic interval */
    const char *cfg_alpha = "abc /bin/echo\n";
    assert(crond_parse_config(cfg_alpha, strlen(cfg_alpha), &tbl, err, sizeof(err)) != 0);

    /* Alphanumeric suffix */
    const char *cfg_mix = "60m /bin/echo\n";
    assert(crond_parse_config(cfg_mix, strlen(cfg_mix), &tbl, err, sizeof(err)) != 0);

    /* Overflow interval (> 32-bit limit) */
    const char *cfg_ovf = "99999999999999999999 /bin/echo\n";
    assert(crond_parse_config(cfg_ovf, strlen(cfg_ovf), &tbl, err, sizeof(err)) != 0);

    printf("PASS: test_config_parse_reject_bad_intervals\n");
}

static void test_config_parse_reject_missing_path(void) {
    printf("[TEST] test_config_parse_reject_missing_path...\n");
    crond_table_t tbl;
    char err[256];

    const char *cfg1 = "60\n";
    assert(crond_parse_config(cfg1, strlen(cfg1), &tbl, err, sizeof(err)) != 0);

    const char *cfg2 = "60    \n";
    assert(crond_parse_config(cfg2, strlen(cfg2), &tbl, err, sizeof(err)) != 0);

    printf("PASS: test_config_parse_reject_missing_path\n");
}

static void test_config_parse_reject_excess_tasks(void) {
    printf("[TEST] test_config_parse_reject_excess_tasks...\n");
    char cfg[2048];
    cfg[0] = '\0';
    for (int i = 1; i <= 17; i++) {
        char line[64];
        snprintf(line, sizeof(line), "%d /bin/task%d\n", i, i);
        host_str_cat(cfg, line, sizeof(cfg));
    }

    crond_table_t tbl;
    char err[256];
    int r = crond_parse_config(cfg, strlen(cfg), &tbl, err, sizeof(err));
    assert(r != 0);
    assert(strstr(err, "16") != NULL);

    printf("PASS: test_config_parse_reject_excess_tasks\n");
}

static void test_config_parse_reject_excess_args(void) {
    printf("[TEST] test_config_parse_reject_excess_args...\n");
    char cfg[1024];
    cfg[0] = '\0';
    host_str_copy(cfg, "5 /bin/cmd", sizeof(cfg));
    for (int i = 0; i < 17; i++) {
        host_str_cat(cfg, " arg", sizeof(cfg));
    }
    host_str_cat(cfg, "\n", sizeof(cfg));

    crond_table_t tbl;
    char err[256];
    int r = crond_parse_config(cfg, strlen(cfg), &tbl, err, sizeof(err));
    assert(r != 0);
    assert(strstr(err, "argument count exceeds") != NULL);

    printf("PASS: test_config_parse_reject_excess_args\n");
}

static void test_config_load_precedence_and_fallback(void) {
    printf("[TEST] test_config_load_precedence_and_fallback...\n");
    mock_reset();

    /* Case 1: Primary exists -> loaded */
    g_mock.primary_file_exists = true;
    host_str_copy(g_mock.primary_file_content, "10 /bin/primary\n", sizeof(g_mock.primary_file_content));
    g_mock.fallback_file_exists = true;
    host_str_copy(g_mock.fallback_file_content, "20 /bin/fallback\n", sizeof(g_mock.fallback_file_content));

    crond_table_t tbl;
    char err[256];
    int r = crond_load_config(CROND_CONFIG_PRIMARY, CROND_CONFIG_FALLBACK, &tbl, err, sizeof(err), &g_ops);
    assert(r == 0);
    assert(tbl.task_count == 1);
    assert(strcmp(tbl.tasks[0].exec_path, "/bin/primary") == 0);

    /* Case 2: Primary missing, fallback exists -> fallback loaded */
    g_mock.primary_file_exists = false;
    r = crond_load_config(CROND_CONFIG_PRIMARY, CROND_CONFIG_FALLBACK, &tbl, err, sizeof(err), &g_ops);
    assert(r == 0);
    assert(tbl.task_count == 1);
    assert(strcmp(tbl.tasks[0].exec_path, "/bin/fallback") == 0);

    /* Case 3: Both missing, writable root -> auto-create default /etc/crontab */
    g_mock.fallback_file_exists = false;
    g_mock.root_readonly = false;
    r = crond_load_config(CROND_CONFIG_PRIMARY, CROND_CONFIG_FALLBACK, &tbl, err, sizeof(err), &g_ops);
    assert(r == 0);
    assert(tbl.task_count == 1);
    assert(strcmp(tbl.tasks[0].exec_path, "/sbin/logrotate") == 0);
    assert(strstr(g_mock.last_log, "created default /etc/crontab") != NULL);

    /* Case 3b: Both missing, read-only root -> run idle with 0 tasks (never exit 1) */
    mock_reset();
    g_mock.primary_file_exists = false;
    g_mock.fallback_file_exists = false;
    g_mock.root_readonly = true;
    r = crond_load_config(CROND_CONFIG_PRIMARY, CROND_CONFIG_FALLBACK, &tbl, err, sizeof(err), &g_ops);
    assert(r == 0);
    assert(tbl.task_count == 0);
    assert(strstr(g_mock.last_log, "no crontab found; running idle") != NULL);

    /* Case 4: Primary has syntax error -> fail closed (returns -1) */
    mock_reset();
    g_mock.primary_file_exists = true;
    host_str_copy(g_mock.primary_file_content, "bad_interval /bin/echo\n", sizeof(g_mock.primary_file_content));
    r = crond_load_config(CROND_CONFIG_PRIMARY, CROND_CONFIG_FALLBACK, &tbl, err, sizeof(err), &g_ops);
    assert(r != 0);
    assert(strstr(g_mock.last_log, "invalid config") != NULL);

    printf("PASS: test_config_load_precedence_and_fallback\n");
}

static void test_task_interval_matching(void) {
    printf("[TEST] test_task_interval_matching...\n");
    crond_task_t t;
    memset(&t, 0, sizeof(t));
    t.interval_minutes = 5;

    assert(crond_task_should_run(&t, 0) == false);
    assert(crond_task_should_run(&t, 1) == false);
    assert(crond_task_should_run(&t, 2) == false);
    assert(crond_task_should_run(&t, 3) == false);
    assert(crond_task_should_run(&t, 4) == false);
    assert(crond_task_should_run(&t, 5) == true);
    assert(crond_task_should_run(&t, 6) == false);
    assert(crond_task_should_run(&t, 10) == true);
    assert(crond_task_should_run(&t, 15) == true);

    t.interval_minutes = 60;
    assert(crond_task_should_run(&t, 59) == false);
    assert(crond_task_should_run(&t, 60) == true);
    assert(crond_task_should_run(&t, 61) == false);
    assert(crond_task_should_run(&t, 120) == true);

    t.interval_minutes = 1;
    assert(crond_task_should_run(&t, 1) == true);
    assert(crond_task_should_run(&t, 2) == true);
    assert(crond_task_should_run(&t, 3) == true);

    printf("PASS: test_task_interval_matching\n");
}

static void test_mock_task_execution_exact_intervals(void) {
    printf("[TEST] test_mock_task_execution_exact_intervals...\n");
    mock_reset();

    const char *cfg =
        "2 /bin/task2 argA\n"
        "5 /bin/task5 argB1 argB2\n";

    crond_table_t tbl;
    char err[256];
    assert(crond_parse_config(cfg, strlen(cfg), &tbl, err, sizeof(err)) == 0);

    /* Simulate minutes 1 through 10 */
    int runs_task2 = 0;
    int runs_task5 = 0;

    for (uint64_t minute = 1; minute <= 10; minute++) {
        int initial_spawns = g_mock.spawn_count;
        int spawned = crond_tick(&tbl, minute, &g_ops);

        if (minute == 1) {
            assert(spawned == 0);
        } else if (minute == 2) {
            assert(spawned == 1);
            runs_task2++;
            assert(strcmp(g_mock.spawns[initial_spawns].path, "/bin/task2") == 0);
            assert(g_mock.spawns[initial_spawns].argc == 1);
            assert(strcmp(g_mock.spawns[initial_spawns].args[0], "argA") == 0);
        } else if (minute == 3) {
            assert(spawned == 0);
        } else if (minute == 4) {
            assert(spawned == 1);
            runs_task2++;
            assert(strcmp(g_mock.spawns[initial_spawns].path, "/bin/task2") == 0);
        } else if (minute == 5) {
            assert(spawned == 1);
            runs_task5++;
            assert(strcmp(g_mock.spawns[initial_spawns].path, "/bin/task5") == 0);
            assert(g_mock.spawns[initial_spawns].argc == 2);
            assert(strcmp(g_mock.spawns[initial_spawns].args[0], "argB1") == 0);
            assert(strcmp(g_mock.spawns[initial_spawns].args[1], "argB2") == 0);
        } else if (minute == 6) {
            assert(spawned == 1);
            runs_task2++;
            assert(strcmp(g_mock.spawns[initial_spawns].path, "/bin/task2") == 0);
        } else if (minute == 7) {
            assert(spawned == 0);
        } else if (minute == 8) {
            assert(spawned == 1);
            runs_task2++;
            assert(strcmp(g_mock.spawns[initial_spawns].path, "/bin/task2") == 0);
        } else if (minute == 9) {
            assert(spawned == 0);
        } else if (minute == 10) {
            /* Both task2 (10%2==0) and task5 (10%5==0) match! */
            assert(spawned == 2);
            runs_task2++;
            runs_task5++;
        }
    }

    assert(runs_task2 == 5); /* minutes 2, 4, 6, 8, 10 */
    assert(runs_task5 == 2); /* minutes 5, 10 */
    assert(g_mock.spawn_count == 7);

    printf("PASS: test_mock_task_execution_exact_intervals\n");
}

static void test_mock_duplicate_suppression_same_minute(void) {
    printf("[TEST] test_mock_duplicate_suppression_same_minute...\n");
    mock_reset();

    const char *cfg = "2 /bin/task2\n";
    crond_table_t tbl;
    char err[256];
    assert(crond_parse_config(cfg, strlen(cfg), &tbl, err, sizeof(err)) == 0);

    /* First tick at minute 2: spawns 1 task */
    int s1 = crond_tick(&tbl, 2, &g_ops);
    assert(s1 == 1);
    assert(g_mock.spawn_count == 1);

    /* Second tick at same minute 2: 0 tasks spawned */
    int s2 = crond_tick(&tbl, 2, &g_ops);
    assert(s2 == 0);
    assert(g_mock.spawn_count == 1);

    printf("PASS: test_mock_duplicate_suppression_same_minute\n");
}

static void test_spawn_failure_resilience(void) {
    printf("[TEST] test_spawn_failure_resilience...\n");
    mock_reset();

    const char *cfg = "1 /bin/failing_task\n";
    crond_table_t tbl;
    char err[256];
    assert(crond_parse_config(cfg, strlen(cfg), &tbl, err, sizeof(err)) == 0);

    g_mock.fail_next_spawn = true;
    int spawned = crond_tick(&tbl, 1, &g_ops);
    assert(spawned == 0);
    assert(strstr(g_mock.last_log, "failed to spawn") != NULL);

    /* Next tick succeeds normally without daemon crash */
    int spawned2 = crond_tick(&tbl, 2, &g_ops);
    assert(spawned2 == 1);
    assert(strstr(g_mock.last_log, "spawned /bin/failing_task") != NULL);

    printf("PASS: test_spawn_failure_resilience\n");
}

static void test_zombie_reaping(void) {
    printf("[TEST] test_zombie_reaping...\n");
    mock_reset();

    crond_table_t tbl;
    crond_table_init(&tbl);

    assert(g_mock.reap_call_count == 0);
    crond_tick(&tbl, 1, &g_ops);
    assert(g_mock.reap_call_count == 1);
    crond_tick(&tbl, 2, &g_ops);
    assert(g_mock.reap_call_count == 2);

    printf("PASS: test_zombie_reaping\n");
}

static void test_watchtower_survival_and_stop(void) {
    printf("[TEST] test_watchtower_survival_and_stop...\n");
    mock_reset();

    g_mock.primary_file_exists = true;
    host_str_copy(g_mock.primary_file_content, "1 /bin/mock_task\n", sizeof(g_mock.primary_file_content));

    /* 1. Test CLI help */
    char *help_argv[] = {"crond", "--help", NULL};
    volatile int stop_flag = 0;
    int r_help = crond_run_with_ops(2, help_argv, &g_ops, &stop_flag);
    assert(r_help == 0);

    /* 2. Test invalid config fails closed */
    g_mock.primary_file_exists = true;
    host_str_copy(g_mock.primary_file_content, "bad_interval /bin/cmd\n", sizeof(g_mock.primary_file_content));
    char *fg_argv[] = {"crond", "-f", NULL};
    int r_bad = crond_run_with_ops(2, fg_argv, &g_ops, &stop_flag);
    assert(r_bad == 1);

    /* 3. Test loop termination via stop_flag (SIGTERM / Watchtower stop) */
    g_mock.primary_file_exists = true;
    host_str_copy(g_mock.primary_file_content, "1 /bin/mock_task\n", sizeof(g_mock.primary_file_content));
    stop_flag = 0;

    crond_table_t tbl;
    char err[256];
    assert(crond_load_config(CROND_CONFIG_PRIMARY, CROND_CONFIG_FALLBACK, &tbl, err, sizeof(err), &g_ops) == 0);

    /* Run 3 ticks then trigger stop_flag */
    for (int t = 1; t <= 3; t++) {
        mock_sleep_sec(CROND_TICK_SECONDS);
        uint64_t uptime_min = g_mock.simulated_uptime_sec / 60;
        crond_tick(&tbl, uptime_min, &g_ops);
    }
    assert(g_mock.spawn_count == 3);

    /* Simulate Watchtower SIGTERM arrival */
    stop_flag = 1;
    int r_loop = crond_run_loop(&tbl, &g_ops, &stop_flag);
    assert(r_loop == 0);
    assert(strstr(g_mock.last_log, "stopping daemon loop") != NULL);

    printf("PASS: test_watchtower_survival_and_stop\n");
}

/* =========================================================================
 * Main Runner
 * ========================================================================= */

int main(void) {
    printf("===================================================\n");
    printf("FortressOS /sbin/crond Host Test Suite (ASan/UBSan)\n");
    printf("===================================================\n");

    test_config_parse_valid();
    test_config_parse_boundary_16_tasks();
    test_config_parse_reject_relative_path();
    test_config_parse_reject_bad_intervals();
    test_config_parse_reject_missing_path();
    test_config_parse_reject_excess_tasks();
    test_config_parse_reject_excess_args();
    test_config_load_precedence_and_fallback();
    test_task_interval_matching();
    test_mock_task_execution_exact_intervals();
    test_mock_duplicate_suppression_same_minute();
    test_spawn_failure_resilience();
    test_zombie_reaping();
    test_watchtower_survival_and_stop();

    printf("\nALL 14 CROND HOST TESTS PASSED!\n");
    return 0;
}
