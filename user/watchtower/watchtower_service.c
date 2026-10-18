#include "watchtower.h"

#ifndef WIFEXITED
#define WIFEXITED(s) (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFSIGNALED(s) (((s) & 0x7f) != 0 && ((s) & 0x7f) != 0x7f)
#define WTERMSIG(s) ((s) & 0x7f)
#endif

#ifndef SIGTERM
#define SIGTERM 15
#define SIGKILL 9
#endif

static bool str_equals(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (*a != *b) return false;
        a++;
        b++;
    }
    return *a == *b;
}

void watchtower_table_init(watchtower_table_t *tbl) {
    if (!tbl) return;
    for (size_t i = 0; i < sizeof(watchtower_table_t); i++) {
        ((uint8_t *)tbl)[i] = 0;
    }
    tbl->count = 0;
    tbl->stopping_all = false;
    tbl->shutdown_start_sec = 0;
}

int watchtower_table_add_service(watchtower_table_t *tbl, const service_t *svc) {
    if (!tbl || !svc || tbl->count >= WATCHTOWER_MAX_SERVICES) return -1;
    if (watchtower_find_service(tbl, svc->name) != NULL) return -2; /* duplicate name */

    service_t *dst = &tbl->services[tbl->count];
    *dst = *svc;

    /* Defaults if not set */
    if (dst->order <= 0) dst->order = 100;
    if (dst->restart_limit == 0) dst->restart_limit = 3;
    if (dst->restart_delay_sec == 0) dst->restart_delay_sec = 5;
    if (dst->timeout_start_sec == 0) dst->timeout_start_sec = 5;
    if (dst->timeout_stop_sec == 0) dst->timeout_stop_sec = str_equals(dst->name, "sshd") ? 10 : 5;
    dst->state = SVC_STATE_STOPPED;
    dst->pid = 0;
    dst->pipe_fd = -1;

    tbl->count++;
    watchtower_table_sort_by_order(tbl);
    return 0;
}

void watchtower_table_sort_by_order(watchtower_table_t *tbl) {
    if (!tbl || tbl->count <= 1) return;
    for (size_t i = 0; i < tbl->count - 1; i++) {
        for (size_t j = 0; j < tbl->count - i - 1; j++) {
            if (tbl->services[j].order > tbl->services[j + 1].order) {
                /* A service record exceeds the entire Ring 3 stack. Swap
                 * bytes in place instead of allocating a temporary record. */
                uint8_t *left=(uint8_t *)&tbl->services[j];
                uint8_t *right=(uint8_t *)&tbl->services[j+1];
                for(size_t k=0;k<sizeof(service_t);k++) {
                    uint8_t byte=left[k];left[k]=right[k];right[k]=byte;
                }
            }
        }
    }
}

service_t *watchtower_find_service(watchtower_table_t *tbl, const char *name) {
    if (!tbl || !name) return NULL;
    for (size_t i = 0; i < tbl->count; i++) {
        if (str_equals(tbl->services[i].name, name)) {
            return &tbl->services[i];
        }
    }
    return NULL;
}

service_t *watchtower_find_service_by_pid(watchtower_table_t *tbl, int64_t pid) {
    if (!tbl || pid <= 0) return NULL;
    for (size_t i = 0; i < tbl->count; i++) {
        if (tbl->services[i].pid == pid ||
            (tbl->services[i].type == SVC_TYPE_FORKING && tbl->services[i].initial_pid == pid)) {
            return &tbl->services[i];
        }
    }
    return NULL;
}

int watchtower_start_service(watchtower_table_t *tbl, size_t idx, uint64_t now_sec, const watchtower_sys_ops_t *ops) {
    if (!tbl || idx >= tbl->count || !ops || !ops->spawn) return -1;
    service_t *svc = &tbl->services[idx];

    /* Prevent duplicate starts */
    if (svc->state == SVC_STATE_RUNNING || (svc->state == SVC_STATE_STARTING && (svc->pid > 0 || svc->initial_pid > 0))) {
        return -2; /* Already running / starting */
    }

    /* Prepare argv array */
    const char *argv[WATCHTOWER_MAX_ARGS + 2];
    argv[0] = svc->exec_path;
    int a = 0;
    for (; a < svc->argc && a < WATCHTOWER_MAX_ARGS; a++) {
        argv[a + 1] = svc->args[a];
    }
    argv[a + 1] = NULL;

    /* Prepare envp array */
    const char *envp[WATCHTOWER_MAX_ENV + 1];
    int e = 0;
    for (; e < svc->envc && e < WATCHTOWER_MAX_ENV; e++) {
        envp[e] = svc->env[e];
    }
    envp[e] = NULL;

    const char *cwd = svc->working_dir[0] ? svc->working_dir : NULL;

    int pipe_fd = -1;
    int64_t pid;
    if (ops->spawn_with_pipe) {
        pid = ops->spawn_with_pipe(svc->exec_path, argv, envp, cwd, &pipe_fd);
    } else {
        pid = ops->spawn(svc->exec_path, argv);
    }
    if (pid <= 0) {
        svc->state = SVC_STATE_FAILED;
        svc->pid = 0;
        svc->initial_pid = 0;
        svc->pipe_fd = -1;
        return -3; /* Spawn failed */
    }

    svc->pipe_fd = pipe_fd;
    svc->start_time_sec = now_sec;
    svc->exit_time_sec = 0;
    svc->manual_stop = false;
    svc->next_restart_sec = 0;

    if (svc->type == SVC_TYPE_FORKING) {
        svc->initial_pid = pid;
        svc->pid = 0;
        svc->state = SVC_STATE_STARTING;
        svc->ready_verified = false;
    } else {
        svc->pid = pid;
        svc->initial_pid = 0;
        svc->state = SVC_STATE_STARTING;
        if (svc->readiness_mode == READINESS_NONE) {
            svc->state = SVC_STATE_RUNNING;
            svc->ready_verified = true;
        } else {
            svc->ready_verified = false;
        }
    }

    return 0;
}

int watchtower_stop_service(watchtower_table_t *tbl, size_t idx, uint64_t now_sec, const watchtower_sys_ops_t *ops) {
    if (!tbl || idx >= tbl->count || !ops || !ops->kill) return -1;
    service_t *svc = &tbl->services[idx];

    if (svc->state == SVC_STATE_STOPPED) {
        svc->next_restart_sec = 0;
        return 0;
    }

    /* If it is waiting for restart delay and has no PID, stop immediately */
    if (svc->pid <= 0 && svc->initial_pid <= 0) {
        svc->state = SVC_STATE_STOPPED;
        svc->next_restart_sec = 0;
        svc->manual_stop = true;
        return 0;
    }

    svc->manual_stop = true;
    svc->state = SVC_STATE_STOPPING;
    svc->exit_time_sec = now_sec;
    svc->next_restart_sec = 0;

    int64_t target_pid = (svc->pid > 0) ? svc->pid : svc->initial_pid;
    return ops->kill(target_pid, SIGTERM);
}

int watchtower_restart_service(watchtower_table_t *tbl, size_t idx, uint64_t now_sec, const watchtower_sys_ops_t *ops) {
    if (!tbl || idx >= tbl->count || !ops) return -1;
    service_t *svc = &tbl->services[idx];

    svc->restart_count = 0;
    svc->restart_window_start_sec = 0;

    if (svc->state == SVC_STATE_RUNNING || svc->state == SVC_STATE_STARTING) {
        int r = watchtower_stop_service(tbl, idx, now_sec, ops);
        if (r != 0) return r;
        /* manual_stop is true so handle_child_exit won't treat as crash;
         * next_restart_sec is scheduled for immediate restart upon reap */
        svc->next_restart_sec = now_sec;
        return 0;
    }

    return watchtower_start_service(tbl, idx, now_sec, ops);
}

void watchtower_handle_child_exit(watchtower_table_t *tbl, int64_t pid, int status, uint64_t now_sec) {
    if (!tbl || pid <= 0) return;
    service_t *svc = watchtower_find_service_by_pid(tbl, pid);
    if (!svc) return;

    /* Type=forking initial launcher exit handling */
    if (svc->type == SVC_TYPE_FORKING && pid == svc->initial_pid) {
        svc->initial_pid = 0;
        int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : status;
        if (exit_code != 0) {
            svc->last_exit_code = exit_code;
            svc->state = SVC_STATE_FAILED;
            return;
        }
        /* Launcher exited 0: keep waiting in STARTING state; tick will discover daemon PID */
        return;
    }

    svc->pid = 0;
    svc->exit_time_sec = now_sec;

    if (WIFEXITED(status)) {
        svc->last_exit_code = WEXITSTATUS(status);
        svc->last_term_sig = 0;
    } else if (WIFSIGNALED(status)) {
        svc->last_exit_code = 128 + WTERMSIG(status);
        svc->last_term_sig = WTERMSIG(status);
    } else {
        svc->last_exit_code = status;
        svc->last_term_sig = 0;
    }

    /* Clean exit from deliberate stop */
    if (svc->manual_stop || svc->state == SVC_STATE_STOPPING) {
        svc->state = SVC_STATE_STOPPED;
        svc->manual_stop = false;
        return;
    }

    /* Unexpected termination handling */
    bool is_failure = (svc->last_exit_code != 0 || svc->last_term_sig != 0);
    bool should_restart = false;

    if (svc->restart_policy == RESTART_ALWAYS) {
        should_restart = true;
    } else if (svc->restart_policy == RESTART_ON_FAILURE && is_failure) {
        should_restart = true;
    }

    if (!should_restart) {
        svc->state = is_failure ? SVC_STATE_FAILED : SVC_STATE_STOPPED;
        return;
    }

    /* Reset restart counter window if 60s passed */
    if (svc->restart_window_start_sec == 0 || now_sec >= svc->restart_window_start_sec + 60) {
        svc->restart_window_start_sec = now_sec;
        svc->restart_count = 0;
    }

    if (svc->restart_count < svc->restart_limit) {
        svc->restart_count++;
        svc->state = SVC_STATE_STARTING;
        svc->next_restart_sec = now_sec + svc->restart_delay_sec;
    } else {
        /* Crash loop threshold exceeded: freeze in FAILED */
        svc->state = SVC_STATE_FAILED;
        svc->next_restart_sec = 0;
    }
}

void watchtower_tick(watchtower_table_t *tbl, uint64_t now_sec, const watchtower_sys_ops_t *ops) {
    if (!tbl || !ops) return;

    for (size_t i = 0; i < tbl->count; i++) {
        service_t *svc = &tbl->services[i];

        /* Check pending restart timer */
        if (svc->next_restart_sec > 0 && now_sec >= svc->next_restart_sec && svc->pid == 0) {
            svc->next_restart_sec = 0;
            watchtower_start_service(tbl, i, now_sec, ops);
            continue;
        }

        /* Check Type=forking PID discovery while STARTING */
        if (svc->type == SVC_TYPE_FORKING && svc->state == SVC_STATE_STARTING && svc->pid == 0) {
            if (svc->pid_file[0] != '\0' && ops->read_pid_file) {
                int64_t dpid = ops->read_pid_file(svc->pid_file);
                if (dpid > 0) {
                    svc->pid = dpid;
                    svc->state = SVC_STATE_RUNNING;
                    svc->ready_verified = true;
                }
            }
            if (now_sec >= svc->start_time_sec + svc->timeout_start_sec) {
                svc->state = SVC_STATE_FAILED;
                if (svc->initial_pid > 0 && ops->kill) ops->kill(svc->initial_pid, SIGTERM);
                if (svc->pid > 0 && ops->kill) ops->kill(svc->pid, SIGTERM);
                continue;
            }
        }

        /* Check liveness for Type=forking daemon while RUNNING */
        if (svc->type == SVC_TYPE_FORKING && svc->state == SVC_STATE_RUNNING && svc->pid > 0 && ops->is_alive) {
            if (!ops->is_alive(svc->pid)) {
                watchtower_handle_child_exit(tbl, svc->pid, 1 /* synthetic failure */, now_sec);
                continue;
            }
        }

        /* Check readiness for starting services */
        if (svc->state == SVC_STATE_STARTING && svc->pid > 0 && !svc->ready_verified) {
            if (svc->readiness_mode == READINESS_FILE && ops->file_exists) {
                if (ops->file_exists(svc->ready_path)) {
                    svc->state = SVC_STATE_RUNNING;
                    svc->ready_verified = true;
                } else if (now_sec >= svc->start_time_sec + svc->timeout_start_sec) {
                    /* Readiness timeout reached: mark FAILED and stop */
                    svc->state = SVC_STATE_FAILED;
                    svc->manual_stop = true;
                    if (ops->kill) ops->kill(svc->pid, SIGTERM);
                }
            } else if (svc->readiness_mode == READINESS_NONE) {
                svc->state = SVC_STATE_RUNNING;
                svc->ready_verified = true;
            }
        }

        /* Check stop timeout escalation */
        if (svc->state == SVC_STATE_STOPPING && svc->pid > 0) {
            if (now_sec >= svc->exit_time_sec + svc->timeout_stop_sec) {
                if (ops->kill) ops->kill(svc->pid, SIGKILL);
            }
        }
    }
}

int watchtower_stop_all(watchtower_table_t *tbl, uint64_t now_sec, const watchtower_sys_ops_t *ops) {
    if (!tbl || !ops) return -1;
    tbl->stopping_all = true;
    tbl->shutdown_start_sec = now_sec;

    /* Reverse order shutdown (Order descending) */
    for (size_t i = tbl->count; i > 0; i--) {
        size_t idx = i - 1;
        service_t *svc = &tbl->services[idx];
        if (svc->state == SVC_STATE_RUNNING || svc->state == SVC_STATE_STARTING) {
            watchtower_stop_service(tbl, idx, now_sec, ops);
        } else if (svc->next_restart_sec > 0) {
            svc->next_restart_sec = 0;
            svc->state = SVC_STATE_STOPPED;
        }
    }
    return 0;
}

bool watchtower_all_stopped(const watchtower_table_t *tbl) {
    if (!tbl) return true;
    for (size_t i = 0; i < tbl->count; i++) {
        if (tbl->services[i].pid > 0 ||
            tbl->services[i].state == SVC_STATE_STARTING ||
            tbl->services[i].state == SVC_STATE_RUNNING ||
            tbl->services[i].state == SVC_STATE_STOPPING) {
            return false;
        }
    }
    return true;
}

const char *watchtower_state_name(service_state_t state) {
    switch (state) {
        case SVC_STATE_STOPPED:  return "stopped";
        case SVC_STATE_STARTING: return "starting";
        case SVC_STATE_RUNNING:  return "running";
        case SVC_STATE_STOPPING: return "stopping";
        case SVC_STATE_FAILED:   return "failed";
        default:                 return "unknown";
    }
}
