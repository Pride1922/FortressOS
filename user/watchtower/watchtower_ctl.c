#include "watchtower_ctl.h"

static void ctl_append(char *dst, const char *src, size_t cap) {
    if (!dst || cap == 0) return;
    size_t len = 0;
    while (dst[len] && len < cap) len++;
    size_t i = 0;
    while (src && src[i] && len + 1 < cap) {
        dst[len++] = src[i++];
    }
    dst[len] = '\0';
}

static void ctl_append_u64(char *dst, uint64_t val, size_t cap) {
    char tmp[32];
    int idx = 0;
    if (val == 0) {
        tmp[idx++] = '0';
    } else {
        while (val > 0) {
            tmp[idx++] = (char)('0' + (val % 10));
            val /= 10;
        }
    }
    char rev[32];
    for (int i = 0; i < idx; i++) {
        rev[i] = tmp[idx - 1 - i];
    }
    rev[idx] = '\0';
    ctl_append(dst, rev, cap);
}

static void ctl_pad(char *dst, size_t target_col, size_t cap) {
    if (!dst || cap == 0) return;
    size_t len = 0;
    while (dst[len] && len < cap) len++;
    while (len < target_col && len + 1 < cap) {
        dst[len++] = ' ';
    }
    dst[len] = '\0';
}

static bool ctl_strcasecmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        char c1 = (*s1 >= 'A' && *s1 <= 'Z') ? (char)(*s1 + 32) : *s1;
        char c2 = (*s2 >= 'A' && *s2 <= 'Z') ? (char)(*s2 + 32) : *s2;
        if (c1 != c2) return false;
        s1++;
        s2++;
    }
    return *s1 == '\0' && *s2 == '\0';
}

static bool ctl_isspace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

int watchtower_ctl_process_request(watchtower_table_t *tbl, const char *req,
                                   uint32_t caller_uid, uint32_t caller_gid,
                                   char *res, size_t res_cap,
                                   const watchtower_sys_ops_t *ops) {
    if (!tbl || !res || res_cap == 0) return -1;
    res[0] = '\0';

    if (!req) {
        ctl_append(res, "ERR empty request\n", res_cap);
        return -1;
    }

    /* Skip leading whitespace */
    size_t i = 0;
    while (req[i] && ctl_isspace(req[i])) i++;
    if (!req[i]) {
        ctl_append(res, "ERR empty request\n", res_cap);
        return -1;
    }

    /* Extract command word */
    char cmd[32];
    size_t c = 0;
    while (req[i] && !ctl_isspace(req[i]) && c + 1 < sizeof(cmd)) {
        cmd[c++] = req[i++];
    }
    cmd[c] = '\0';

    /* Skip whitespace before operand */
    while (req[i] && ctl_isspace(req[i])) i++;

    /* Extract operand (e.g. service name) */
    char operand[WATCHTOWER_MAX_NAME_LEN];
    size_t o = 0;
    while (req[i] && !ctl_isspace(req[i]) && o + 1 < sizeof(operand)) {
        operand[o++] = req[i++];
    }
    operand[o] = '\0';

    uint64_t now = (ops && ops->monotonic_sec) ? ops->monotonic_sec() : 0;

    /* Command: LIST */
    if (ctl_strcasecmp(cmd, "LIST")) {
        ctl_append(res, "NAME         STATE      PID     ORDER  ENABLED  RESTART\n", res_cap);
        for (size_t s = 0; s < tbl->count; s++) {
            service_t *svc = &tbl->services[s];
            size_t row_start = 0;
            while (res[row_start]) row_start++;

            ctl_append(res, svc->name, res_cap);
            ctl_pad(res, row_start + 13, res_cap);

            ctl_append(res, watchtower_state_name(svc->state), res_cap);
            ctl_pad(res, row_start + 24, res_cap);

            if (svc->pid > 0) {
                ctl_append_u64(res, (uint64_t)svc->pid, res_cap);
            } else {
                ctl_append(res, "-", res_cap);
            }
            ctl_pad(res, row_start + 32, res_cap);

            ctl_append_u64(res, (uint64_t)svc->order, res_cap);
            ctl_pad(res, row_start + 39, res_cap);

            ctl_append(res, svc->enabled ? "yes" : "no", res_cap);
            ctl_pad(res, row_start + 48, res_cap);

            if (svc->restart_policy == RESTART_ALWAYS) ctl_append(res, "always", res_cap);
            else if (svc->restart_policy == RESTART_ON_FAILURE) ctl_append(res, "on-failure", res_cap);
            else ctl_append(res, "never", res_cap);

            ctl_append(res, "\n", res_cap);
        }
        return 0;
    }

    /* Command: STATUS <name> */
    if (ctl_strcasecmp(cmd, "STATUS")) {
        if (!operand[0]) {
            ctl_append(res, "ERR missing service name for status\n", res_cap);
            return -1;
        }
        service_t *svc = watchtower_find_service(tbl, operand);
        if (!svc) {
            ctl_append(res, "ERR service '", res_cap);
            ctl_append(res, operand, res_cap);
            ctl_append(res, "' not found\n", res_cap);
            return -1;
        }

        ctl_append(res, "Service: ", res_cap);
        ctl_append(res, svc->name, res_cap);
        ctl_append(res, "\nState: ", res_cap);
        ctl_append(res, watchtower_state_name(svc->state), res_cap);
        if (svc->pid > 0) {
            ctl_append(res, " (PID: ", res_cap);
            ctl_append_u64(res, (uint64_t)svc->pid, res_cap);
            ctl_append(res, ")", res_cap);
        }
        ctl_append(res, "\nReadiness: ", res_cap);
        if (svc->ready_verified) {
            ctl_append(res, "ready", res_cap);
        } else if (svc->state == SVC_STATE_STARTING) {
            ctl_append(res, "awaiting readiness", res_cap);
        } else {
            ctl_append(res, "unverified", res_cap);
        }
        if (svc->readiness_mode == READINESS_FILE) {
            ctl_append(res, " (mode: file, path: ", res_cap);
            ctl_append(res, svc->ready_path, res_cap);
            ctl_append(res, ")", res_cap);
        } else if (svc->readiness_mode == READINESS_NONE) {
            ctl_append(res, " (mode: none)", res_cap);
        }

        ctl_append(res, "\nExecutable: ", res_cap);
        ctl_append(res, svc->exec_path, res_cap);
        for (int a = 0; a < svc->argc; a++) {
            ctl_append(res, " ", res_cap);
            ctl_append(res, svc->args[a], res_cap);
        }

        ctl_append(res, "\nOrder: ", res_cap);
        ctl_append_u64(res, (uint64_t)svc->order, res_cap);

        ctl_append(res, "\nEnabled: ", res_cap);
        ctl_append(res, svc->enabled ? "yes" : "no", res_cap);

        ctl_append(res, "\nType: ", res_cap);
        ctl_append(res, svc->type == SVC_TYPE_FORKING ? "forking" : "simple", res_cap);

        if (svc->working_dir[0] != '\0') {
            ctl_append(res, "\nWorkingDirectory: ", res_cap);
            ctl_append(res, svc->working_dir, res_cap);
        }
        if (svc->pid_file[0] != '\0') {
            ctl_append(res, "\nPIDFile: ", res_cap);
            ctl_append(res, svc->pid_file, res_cap);
        }
        if (svc->envc > 0) {
            ctl_append(res, "\nEnvironment: ", res_cap);
            ctl_append_u64(res, (uint64_t)svc->envc, res_cap);
            ctl_append(res, " variable(s)", res_cap);
        }

        ctl_append(res, "\nIdentity: ", res_cap);
        ctl_append(res, svc->user, res_cap);
        ctl_append(res, ":", res_cap);
        ctl_append(res, svc->group, res_cap);

        ctl_append(res, "\nRestart Policy: ", res_cap);
        if (svc->restart_policy == RESTART_ALWAYS) ctl_append(res, "always", res_cap);
        else if (svc->restart_policy == RESTART_ON_FAILURE) ctl_append(res, "on-failure", res_cap);
        else ctl_append(res, "never", res_cap);
        ctl_append(res, " (restarts: ", res_cap);
        ctl_append_u64(res, (uint64_t)svc->restart_count, res_cap);
        ctl_append(res, "/", res_cap);
        ctl_append_u64(res, (uint64_t)svc->restart_limit, res_cap);
        ctl_append(res, ")", res_cap);

        ctl_append(res, "\nUptime: ", res_cap);
        if (svc->pid > 0 && now >= svc->start_time_sec) {
            ctl_append_u64(res, now - svc->start_time_sec, res_cap);
            ctl_append(res, "s", res_cap);
        } else {
            ctl_append(res, "0s", res_cap);
        }

        ctl_append(res, "\nLast Exit: ", res_cap);
        if (svc->last_term_sig > 0) {
            ctl_append(res, "signaled (signal ", res_cap);
            ctl_append_u64(res, (uint64_t)svc->last_term_sig, res_cap);
            ctl_append(res, ")", res_cap);
        } else if (svc->last_exit_code != 0) {
            ctl_append(res, "exit code ", res_cap);
            ctl_append_u64(res, (uint64_t)svc->last_exit_code, res_cap);
        } else {
            ctl_append(res, "clean (0)", res_cap);
        }

        ctl_append(res, "\nDropped Output: ", res_cap);
        ctl_append_u64(res, svc->dropped_bytes, res_cap);
        ctl_append(res, " bytes\n", res_cap);
        return 0;
    }

    /* Command: CHECK <name> */
    if (ctl_strcasecmp(cmd, "CHECK")) {
        if (!operand[0]) {
            ctl_append(res, "ERR missing service name for check\n", res_cap);
            return -1;
        }
        service_t *svc = watchtower_find_service(tbl, operand);
        if (!svc) {
            ctl_append(res, "ERR service '", res_cap);
            ctl_append(res, operand, res_cap);
            ctl_append(res, "' not found\n", res_cap);
            return -1;
        }
        if (svc->state == SVC_STATE_RUNNING && svc->ready_verified) {
            ctl_append(res, "OK service '", res_cap);
            ctl_append(res, operand, res_cap);
            ctl_append(res, "' is ready\n", res_cap);
            return 0;
        } else if (svc->state == SVC_STATE_STARTING) {
            ctl_append(res, "WARN service '", res_cap);
            ctl_append(res, operand, res_cap);
            ctl_append(res, "' is starting\n", res_cap);
            return 1;
        } else {
            ctl_append(res, "ERR service '", res_cap);
            ctl_append(res, operand, res_cap);
            ctl_append(res, "' is not running\n", res_cap);
            return 1;
        }
    }

    /* Mutating Commands: START, STOP, RESTART require privilege (caller_uid == 0 or caller_gid == 10) */
    bool is_privileged = (caller_uid == 0 || caller_gid == 10);

    /* Command: START <name> */
    if (ctl_strcasecmp(cmd, "START")) {
        if (!is_privileged) {
            ctl_append(res, "ERR permission denied: root or wheel required\n", res_cap);
            return -1;
        }
        if (!operand[0]) {
            ctl_append(res, "ERR missing service name for start\n", res_cap);
            return -1;
        }
        size_t idx = 0;
        bool found = false;
        for (; idx < tbl->count; idx++) {
            if (ctl_strcasecmp(tbl->services[idx].name, operand)) {
                found = true;
                break;
            }
        }
        if (!found) {
            ctl_append(res, "ERR service '", res_cap);
            ctl_append(res, operand, res_cap);
            ctl_append(res, "' not found\n", res_cap);
            return -1;
        }
        service_t *svc = &tbl->services[idx];
        if (svc->state == SVC_STATE_RUNNING || (svc->state == SVC_STATE_STARTING && svc->pid > 0)) {
            ctl_append(res, "ERR service '", res_cap);
            ctl_append(res, operand, res_cap);
            ctl_append(res, "' is already running\n", res_cap);
            return -1;
        }
        svc->restart_count = 0;
        int r = watchtower_start_service(tbl, idx, now, ops);
        if (r == 0) {
            ctl_append(res, "OK service '", res_cap);
            ctl_append(res, operand, res_cap);
            ctl_append(res, "' started (PID: ", res_cap);
            ctl_append_u64(res, (uint64_t)svc->pid, res_cap);
            ctl_append(res, ")\n", res_cap);
            return 0;
        } else {
            ctl_append(res, "ERR failed to start service '", res_cap);
            ctl_append(res, operand, res_cap);
            ctl_append(res, "'\n", res_cap);
            return -1;
        }
    }

    /* Command: STOP <name> */
    if (ctl_strcasecmp(cmd, "STOP")) {
        if (!is_privileged) {
            ctl_append(res, "ERR permission denied: root or wheel required\n", res_cap);
            return -1;
        }
        if (!operand[0]) {
            ctl_append(res, "ERR missing service name for stop\n", res_cap);
            return -1;
        }
        size_t idx = 0;
        bool found = false;
        for (; idx < tbl->count; idx++) {
            if (ctl_strcasecmp(tbl->services[idx].name, operand)) {
                found = true;
                break;
            }
        }
        if (!found) {
            ctl_append(res, "ERR service '", res_cap);
            ctl_append(res, operand, res_cap);
            ctl_append(res, "' not found\n", res_cap);
            return -1;
        }
        service_t *svc = &tbl->services[idx];
        if (svc->state == SVC_STATE_STOPPED) {
            ctl_append(res, "OK service '", res_cap);
            ctl_append(res, operand, res_cap);
            ctl_append(res, "' is already stopped\n", res_cap);
            return 0;
        }
        int r = watchtower_stop_service(tbl, idx, now, ops);
        if (r == 0) {
            ctl_append(res, "OK stopping service '", res_cap);
            ctl_append(res, operand, res_cap);
            ctl_append(res, "'\n", res_cap);
            return 0;
        } else {
            ctl_append(res, "ERR failed to stop service '", res_cap);
            ctl_append(res, operand, res_cap);
            ctl_append(res, "'\n", res_cap);
            return -1;
        }
    }

    /* Command: RESTART <name> */
    if (ctl_strcasecmp(cmd, "RESTART")) {
        if (!is_privileged) {
            ctl_append(res, "ERR permission denied: root or wheel required\n", res_cap);
            return -1;
        }
        if (!operand[0]) {
            ctl_append(res, "ERR missing service name for restart\n", res_cap);
            return -1;
        }
        size_t idx = 0;
        bool found = false;
        for (; idx < tbl->count; idx++) {
            if (ctl_strcasecmp(tbl->services[idx].name, operand)) {
                found = true;
                break;
            }
        }
        if (!found) {
            ctl_append(res, "ERR service '", res_cap);
            ctl_append(res, operand, res_cap);
            ctl_append(res, "' not found\n", res_cap);
            return -1;
        }
        int r = watchtower_restart_service(tbl, idx, now, ops);
        if (r == 0) {
            ctl_append(res, "OK restarting service '", res_cap);
            ctl_append(res, operand, res_cap);
            ctl_append(res, "'\n", res_cap);
            return 0;
        } else {
            ctl_append(res, "ERR failed to restart service '", res_cap);
            ctl_append(res, operand, res_cap);
            ctl_append(res, "'\n", res_cap);
            return -1;
        }
    }

    /* Unknown Command */
    ctl_append(res, "ERR unknown command '", res_cap);
    ctl_append(res, cmd, res_cap);
    ctl_append(res, "'\n", res_cap);
    return -1;
}
