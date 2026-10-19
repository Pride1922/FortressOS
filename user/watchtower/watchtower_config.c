#include "watchtower_config.h"
#include "syscall_abi.h"
#include "vfs.h"

#ifdef WATCHTOWER_HOST_TEST
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#ifndef WT_SYSCALL
static inline long wt_config_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c,
                                     uintptr_t d, uintptr_t e, uintptr_t f) {
#ifndef WATCHTOWER_HOST_TEST
    register uintptr_t r10 __asm__("r10") = d;
    register uintptr_t r8  __asm__("r8")  = e;
    register uintptr_t r9  __asm__("r9")  = f;
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c),
                     "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory", "cc");
    return nr;
#else
    (void)nr; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    return -1;
#endif
}
#define WT_SYSCALL wt_config_syscall
#endif

static size_t cfg_strlen(const char *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static bool cfg_streq(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (*a != *b) return false;
        a++;
        b++;
    }
    return *a == *b;
}

static void cfg_copy(char *dst, const char *src, size_t cap) {
    if (!dst || cap == 0) return;
    size_t i = 0;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void cfg_append(char *dst, const char *src, size_t cap) {
    if (!dst || cap == 0) return;
    size_t len = 0;
    while (dst[len] && len < cap) len++;
    size_t i = 0;
    while (src && src[i] && len + 1 < cap) {
        dst[len++] = src[i++];
    }
    dst[len] = '\0';
}

static void cfg_append_u32(char *dst, uint32_t val, size_t cap) {
    char tmp[16];
    int idx = 0;
    if (val == 0) {
        tmp[idx++] = '0';
    } else {
        while (val > 0) {
            tmp[idx++] = (char)('0' + (val % 10));
            val /= 10;
        }
    }
    char rev[16];
    for (int i = 0; i < idx; i++) {
        rev[i] = tmp[idx - 1 - i];
    }
    rev[idx] = '\0';
    cfg_append(dst, rev, cap);
}

static bool cfg_isspace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool cfg_strcasecmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        char c1 = (*s1 >= 'A' && *s1 <= 'Z') ? (char)(*s1 + 32) : *s1;
        char c2 = (*s2 >= 'A' && *s2 <= 'Z') ? (char)(*s2 + 32) : *s2;
        if (c1 != c2) return false;
        s1++;
        s2++;
    }
    return *s1 == '\0' && *s2 == '\0';
}

static bool cfg_parse_bool(const char *val, bool *out) {
    if (cfg_strcasecmp(val, "yes") || cfg_strcasecmp(val, "true") ||
        cfg_strcasecmp(val, "1")   || cfg_strcasecmp(val, "on")) {
        *out = true;
        return true;
    }
    if (cfg_strcasecmp(val, "no")  || cfg_strcasecmp(val, "false") ||
        cfg_strcasecmp(val, "0")   || cfg_strcasecmp(val, "off")) {
        *out = false;
        return true;
    }
    return false;
}

static bool cfg_parse_u32(const char *val, uint32_t *out) {
    if (!val || !val[0]) return false;
    uint64_t acc = 0;
    for (size_t i = 0; val[i]; i++) {
        if (val[i] < '0' || val[i] > '9') return false;
        acc = acc * 10 + (uint32_t)(val[i] - '0');
        if (acc > 0xFFFFFFFFULL) return false;
    }
    *out = (uint32_t)acc;
    return true;
}

static void cfg_format_err(char *err_msg, size_t err_cap, const char *msg,
                           const char *val, uint32_t line) {
    if (!err_msg || err_cap == 0) return;
    err_msg[0] = '\0';
    cfg_append(err_msg, "watchtower: ", err_cap);
    cfg_append(err_msg, msg, err_cap);
    if (val && val[0]) {
        cfg_append(err_msg, " '", err_cap);
        cfg_append(err_msg, val, err_cap);
        cfg_append(err_msg, "'", err_cap);
    }
    cfg_append(err_msg, " at line ", err_cap);
    cfg_append_u32(err_msg, line, err_cap);
}

/* Parse command line into exec path and arguments without shell interpretation */
static bool parse_exec_line(const char *line, char *exec_path, size_t exec_cap,
                            char args[WATCHTOWER_MAX_ARGS][WATCHTOWER_MAX_ARG_LEN],
                            int *out_argc, char *err_msg, size_t err_cap, uint32_t line_num) {
    size_t i = 0;
    while (line[i] && cfg_isspace(line[i])) i++;
    if (!line[i]) {
        cfg_format_err(err_msg, err_cap, "empty Exec directive", "", line_num);
        return false;
    }

    /* Extract executable path */
    size_t p = 0;
    char quote = 0;
    if (line[i] == '"' || line[i] == '\'') {
        quote = line[i++];
    }
    while (line[i]) {
        if (quote) {
            if (line[i] == quote) { i++; break; }
        } else {
            if (cfg_isspace(line[i])) break;
        }
        if (p + 1 >= exec_cap) {
            cfg_format_err(err_msg, err_cap, "Exec path exceeds maximum length", "", line_num);
            return false;
        }
        exec_path[p++] = line[i++];
    }
    exec_path[p] = '\0';
    if (p == 0) {
        cfg_format_err(err_msg, err_cap, "missing executable path in Exec", "", line_num);
        return false;
    }

    /* Parse remaining arguments */
    int argc = 0;
    while (line[i]) {
        while (line[i] && cfg_isspace(line[i])) i++;
        if (!line[i]) break;

        if (argc >= WATCHTOWER_MAX_ARGS) {
            cfg_format_err(err_msg, err_cap, "exceeded maximum argument count (16)", "", line_num);
            return false;
        }

        quote = 0;
        if (line[i] == '"' || line[i] == '\'') {
            quote = line[i++];
        }

        size_t a = 0;
        while (line[i]) {
            if (quote) {
                if (line[i] == quote) { i++; break; }
            } else {
                if (cfg_isspace(line[i])) break;
            }
            if (a + 1 >= WATCHTOWER_MAX_ARG_LEN) {
                cfg_format_err(err_msg, err_cap, "argument exceeds maximum length", "", line_num);
                return false;
            }
            args[argc][a++] = line[i++];
        }
        args[argc][a] = '\0';
        argc++;
    }

    *out_argc = argc;
    return true;
}

int watchtower_config_parse_service(const char *buf, size_t len, const char *svc_name,
                                    service_t *out_svc, char *err_msg, size_t err_cap) {
    if (!buf || !out_svc) return -1;
    if (err_msg && err_cap > 0) err_msg[0] = '\0';

    /* Initialize service struct with default values */
    for (size_t i = 0; i < sizeof(service_t); i++) ((uint8_t *)out_svc)[i] = 0;

    if (svc_name && svc_name[0]) {
        cfg_copy(out_svc->name, svc_name, sizeof(out_svc->name));
    }
    out_svc->order = 100;
    out_svc->enabled = true;
    out_svc->type = SVC_TYPE_SIMPLE;
    out_svc->working_dir[0] = '\0';
    out_svc->pid_file[0] = '\0';
    out_svc->env_file[0] = '\0';
    out_svc->envc = 0;
    out_svc->initial_pid = 0;
    cfg_copy(out_svc->user, "root", sizeof(out_svc->user));
    cfg_copy(out_svc->group, "root", sizeof(out_svc->group));
    out_svc->restart_policy = RESTART_ON_FAILURE;
    out_svc->restart_limit = 3;
    out_svc->restart_delay_sec = 5;
    out_svc->timeout_start_sec = 5;
    out_svc->timeout_stop_sec = (svc_name && cfg_streq(svc_name, "sshd")) ? 10 : 5;
    out_svc->readiness_mode = READINESS_NONE;
    out_svc->pipe_fd = -1;

    bool has_exec = false;
    bool has_order = false;
    bool has_enabled = false;
    bool has_type = false;
    bool has_working_dir = false;
    bool has_pid_file = false;
    bool has_env_file = false;
    bool has_user = false;
    bool has_group = false;
    bool has_restart = false;
    bool has_restart_limit = false;
    bool has_restart_delay = false;
    bool has_timeout_start = false;
    bool has_timeout_stop = false;
    bool has_readiness = false;

    size_t pos = 0;
    uint32_t line_num = 1;

    while (pos < len) {
        /* Read next line */
        char line[WATCHTOWER_MAX_LINE_LEN];
        size_t lpos = 0;
        bool overflow = false;

        while (pos < len && buf[pos] != '\n') {
            if (lpos + 1 < sizeof(line)) {
                line[lpos++] = buf[pos];
            } else {
                overflow = true;
            }
            pos++;
        }
        if (pos < len && buf[pos] == '\n') pos++;
        line[lpos] = '\0';

        if (overflow) {
            cfg_format_err(err_msg, err_cap, "line exceeds maximum length", "", line_num);
            return -1;
        }

        /* Trim leading whitespace */
        size_t start = 0;
        while (line[start] && cfg_isspace(line[start])) start++;

        /* Trim trailing whitespace */
        size_t end = lpos;
        while (end > start && cfg_isspace(line[end - 1])) end--;
        line[end] = '\0';

        /* Ignore empty lines and comments (# or ;) */
        if (start >= end || line[start] == '#' || line[start] == ';') {
            line_num++;
            continue;
        }

        /* Ignore section headers like [Service], [Unit] */
        if (line[start] == '[' && line[end - 1] == ']') {
            line_num++;
            continue;
        }

        /* Find key-value delimiter ('=' or ':') */
        char *eq = NULL;
        for (size_t k = start; k < end; k++) {
            if (line[k] == '=' || line[k] == ':') {
                eq = &line[k];
                break;
            }
        }
        if (!eq) {
            cfg_format_err(err_msg, err_cap, "missing '=' delimiter", &line[start], line_num);
            return -1;
        }

        /* Split key and value */
        *eq = '\0';
        char *key = &line[start];
        char *val = eq + 1;

        /* Trim key */
        size_t klen = 0;
        while (key[klen]) klen++;
        while (klen > 0 && cfg_isspace(key[klen - 1])) key[--klen] = '\0';

        /* Trim value */
        while (*val && cfg_isspace(*val)) val++;
        size_t vlen = 0;
        while (val[vlen]) vlen++;
        while (vlen > 0 && cfg_isspace(val[vlen - 1])) val[--vlen] = '\0';

        /* Parse Directives */
        if (cfg_strcasecmp(key, "Exec") || cfg_strcasecmp(key, "ExecStart")) {
            if (has_exec) {
                cfg_format_err(err_msg, err_cap, "duplicate directive", key, line_num);
                return -1;
            }
            if (!parse_exec_line(val, out_svc->exec_path, sizeof(out_svc->exec_path),
                                 out_svc->args, &out_svc->argc, err_msg, err_cap, line_num)) {
                return -1;
            }
            has_exec = true;
        } else if (cfg_strcasecmp(key, "Order")) {
            if (has_order) {
                cfg_format_err(err_msg, err_cap, "duplicate directive", key, line_num);
                return -1;
            }
            uint32_t ord = 0;
            if (!cfg_parse_u32(val, &ord) || ord == 0 || ord > 999) {
                cfg_format_err(err_msg, err_cap, "out-of-range value (1..999) for Order", val, line_num);
                return -1;
            }
            out_svc->order = (int)ord;
            has_order = true;
        } else if (cfg_strcasecmp(key, "Enabled")) {
            if (has_enabled) {
                cfg_format_err(err_msg, err_cap, "duplicate directive", key, line_num);
                return -1;
            }
            bool b = false;
            if (!cfg_parse_bool(val, &b)) {
                cfg_format_err(err_msg, err_cap, "invalid boolean for Enabled", val, line_num);
                return -1;
            }
            out_svc->enabled = b;
            has_enabled = true;
        } else if (cfg_strcasecmp(key, "User")) {
            if (has_user) {
                cfg_format_err(err_msg, err_cap, "duplicate directive", key, line_num);
                return -1;
            }
            if (vlen == 0 || vlen >= sizeof(out_svc->user)) {
                cfg_format_err(err_msg, err_cap, "invalid User identifier", val, line_num);
                return -1;
            }
            cfg_copy(out_svc->user, val, sizeof(out_svc->user));
            has_user = true;
        } else if (cfg_strcasecmp(key, "Group")) {
            if (has_group) {
                cfg_format_err(err_msg, err_cap, "duplicate directive", key, line_num);
                return -1;
            }
            if (vlen == 0 || vlen >= sizeof(out_svc->group)) {
                cfg_format_err(err_msg, err_cap, "invalid Group identifier", val, line_num);
                return -1;
            }
            cfg_copy(out_svc->group, val, sizeof(out_svc->group));
            has_group = true;
        } else if (cfg_strcasecmp(key, "Restart")) {
            if (has_restart) {
                cfg_format_err(err_msg, err_cap, "duplicate directive", key, line_num);
                return -1;
            }
            if (cfg_strcasecmp(val, "never") || cfg_strcasecmp(val, "no") ||
                cfg_strcasecmp(val, "false") || cfg_strcasecmp(val, "off")) {
                out_svc->restart_policy = RESTART_NEVER;
            } else if (cfg_strcasecmp(val, "on-failure") || cfg_strcasecmp(val, "on_failure")) {
                out_svc->restart_policy = RESTART_ON_FAILURE;
            } else if (cfg_strcasecmp(val, "always")) {
                out_svc->restart_policy = RESTART_ALWAYS;
            } else {
                cfg_format_err(err_msg, err_cap, "invalid value for Restart (never, on-failure, always)", val, line_num);
                return -1;
            }
            has_restart = true;
        } else if (cfg_strcasecmp(key, "RestartLimit")) {
            if (has_restart_limit) {
                cfg_format_err(err_msg, err_cap, "duplicate directive", key, line_num);
                return -1;
            }
            uint32_t lim = 0;
            if (!cfg_parse_u32(val, &lim) || lim == 0 || lim > 100) {
                cfg_format_err(err_msg, err_cap, "out-of-range value (1..100) for RestartLimit", val, line_num);
                return -1;
            }
            out_svc->restart_limit = lim;
            has_restart_limit = true;
        } else if (cfg_strcasecmp(key, "RestartDelay")) {
            if (has_restart_delay) {
                cfg_format_err(err_msg, err_cap, "duplicate directive", key, line_num);
                return -1;
            }
            uint32_t del = 0;
            if (!cfg_parse_u32(val, &del) || del > 3600) {
                cfg_format_err(err_msg, err_cap, "out-of-range value (0..3600) for RestartDelay", val, line_num);
                return -1;
            }
            out_svc->restart_delay_sec = del;
            has_restart_delay = true;
        } else if (cfg_strcasecmp(key, "TimeoutStart")) {
            if (has_timeout_start) {
                cfg_format_err(err_msg, err_cap, "duplicate directive", key, line_num);
                return -1;
            }
            uint32_t t = 0;
            if (!cfg_parse_u32(val, &t) || t == 0 || t > 3600) {
                cfg_format_err(err_msg, err_cap, "out-of-range value (1..3600) for TimeoutStart", val, line_num);
                return -1;
            }
            out_svc->timeout_start_sec = t;
            has_timeout_start = true;
        } else if (cfg_strcasecmp(key, "TimeoutStop")) {
            if (has_timeout_stop) {
                cfg_format_err(err_msg, err_cap, "duplicate directive", key, line_num);
                return -1;
            }
            uint32_t t = 0;
            if (!cfg_parse_u32(val, &t) || t == 0 || t > 3600) {
                cfg_format_err(err_msg, err_cap, "out-of-range value (1..3600) for TimeoutStop", val, line_num);
                return -1;
            }
            out_svc->timeout_stop_sec = t;
            has_timeout_stop = true;
        } else if (cfg_strcasecmp(key, "Readiness")) {
            if (has_readiness) {
                cfg_format_err(err_msg, err_cap, "duplicate directive", key, line_num);
                return -1;
            }
            if (cfg_strcasecmp(val, "none")) {
                out_svc->readiness_mode = READINESS_NONE;
                out_svc->ready_path[0] = '\0';
            } else if (val[0] == 'f' && val[1] == 'i' && val[2] == 'l' && val[3] == 'e' && val[4] == ':') {
                const char *rpath = val + 5;
                while (*rpath && cfg_isspace(*rpath)) rpath++;
                if (!rpath[0] || vlen >= sizeof(out_svc->ready_path)) {
                    cfg_format_err(err_msg, err_cap, "invalid path for Readiness=file:", rpath, line_num);
                    return -1;
                }
                out_svc->readiness_mode = READINESS_FILE;
                cfg_copy(out_svc->ready_path, rpath, sizeof(out_svc->ready_path));
            } else {
                cfg_format_err(err_msg, err_cap, "invalid Readiness value (none, file:<path>)", val, line_num);
                return -1;
            }
            has_readiness = true;
        } else if (cfg_strcasecmp(key, "Type")) {
            if (has_type) {
                cfg_format_err(err_msg, err_cap, "duplicate directive", key, line_num);
                return -1;
            }
            if (cfg_strcasecmp(val, "simple")) {
                out_svc->type = SVC_TYPE_SIMPLE;
            } else if (cfg_strcasecmp(val, "forking")) {
                out_svc->type = SVC_TYPE_FORKING;
            } else {
                cfg_format_err(err_msg, err_cap, "invalid value for Type (simple, forking)", val, line_num);
                return -1;
            }
            has_type = true;
        } else if (cfg_strcasecmp(key, "WorkingDirectory")) {
            if (has_working_dir) {
                cfg_format_err(err_msg, err_cap, "duplicate directive", key, line_num);
                return -1;
            }
            if (val[0] != '/' || vlen >= sizeof(out_svc->working_dir)) {
                cfg_format_err(err_msg, err_cap, "invalid WorkingDirectory (must be absolute path)", val, line_num);
                return -1;
            }
            cfg_copy(out_svc->working_dir, val, sizeof(out_svc->working_dir));
            has_working_dir = true;
        } else if (cfg_strcasecmp(key, "PIDFile")) {
            if (has_pid_file) {
                cfg_format_err(err_msg, err_cap, "duplicate directive", key, line_num);
                return -1;
            }
            if (val[0] != '/' || vlen >= sizeof(out_svc->pid_file)) {
                cfg_format_err(err_msg, err_cap, "invalid PIDFile (must be absolute path)", val, line_num);
                return -1;
            }
            cfg_copy(out_svc->pid_file, val, sizeof(out_svc->pid_file));
            has_pid_file = true;
        } else if (cfg_strcasecmp(key, "Environment")) {
            if (out_svc->envc >= WATCHTOWER_MAX_ENV) {
                cfg_format_err(err_msg, err_cap, "exceeded maximum Environment variables (16)", val, line_num);
                return -1;
            }
            const char *eq_pos = NULL;
            for (size_t i = 0; val[i]; i++) {
                if (val[i] == '=') { eq_pos = &val[i]; break; }
            }
            if (!eq_pos || eq_pos == val) {
                cfg_format_err(err_msg, err_cap, "Environment must be KEY=VAL", val, line_num);
                return -1;
            }
            if (vlen >= WATCHTOWER_MAX_ENV_LEN) {
                cfg_format_err(err_msg, err_cap, "Environment variable exceeds max length (128)", val, line_num);
                return -1;
            }
            cfg_copy(out_svc->env[out_svc->envc], val, WATCHTOWER_MAX_ENV_LEN);
            out_svc->envc++;
        } else if (cfg_strcasecmp(key, "EnvironmentFile")) {
            if (has_env_file) {
                cfg_format_err(err_msg, err_cap, "duplicate directive", key, line_num);
                return -1;
            }
            if (val[0] != '/' || vlen >= sizeof(out_svc->env_file)) {
                cfg_format_err(err_msg, err_cap, "invalid EnvironmentFile (must be absolute path)", val, line_num);
                return -1;
            }
            cfg_copy(out_svc->env_file, val, sizeof(out_svc->env_file));
            has_env_file = true;
        } else {
            /* Strict fail-closed on any unknown directive */
            cfg_format_err(err_msg, err_cap, "unknown directive", key, line_num);
            return -1;
        }

        line_num++;
    }

    /* Mandatory directive check */
    if (!has_exec) {
        cfg_format_err(err_msg, err_cap, "missing mandatory Exec directive", "", line_num);
        return -1;
    }

    return 0;
}

static long read_file_to_buf(const char *path, char *buf, size_t cap) {
#ifdef WATCHTOWER_HOST_TEST
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return (long)n;
#else
    long fd = WT_SYSCALL(SYS_OPEN, (uintptr_t)path, VFS_O_RDONLY, 0, 0, 0, 0);
    if (fd < 0) return -1;
    long n = WT_SYSCALL(SYS_READ, (uintptr_t)fd, (uintptr_t)buf, cap - 1, 0, 0, 0);
    (void)WT_SYSCALL(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
    if (n < 0) return -1;
    buf[n] = '\0';
    return n;
#endif
}

int watchtower_config_load_file(const char *path, const char *svc_name,
                                service_t *out_svc, char *err_msg, size_t err_cap) {
    if (!path || !out_svc) return -1;

    static char s_buf[WATCHTOWER_MAX_CONFIG_SIZE];
    long nread = read_file_to_buf(path, s_buf, sizeof(s_buf));
    if (nread < 0) {
        if (err_msg && err_cap > 0) {
            err_msg[0] = '\0';
            cfg_append(err_msg, "watchtower: cannot open configuration file '", err_cap);
            cfg_append(err_msg, path, err_cap);
            cfg_append(err_msg, "'", err_cap);
        }
        return -2;
    }

    int parse_rc = watchtower_config_parse_service(s_buf, (size_t)nread, svc_name, out_svc, err_msg, err_cap);
    if (parse_rc != 0) return parse_rc;

    /* If EnvironmentFile is specified, load variables from it */
    if (out_svc->env_file[0] != '\0') {
        static char env_buf[WATCHTOWER_MAX_CONFIG_SIZE];
        long env_read = read_file_to_buf(out_svc->env_file, env_buf, sizeof(env_buf));
        if (env_read < 0) {
            if (err_msg && err_cap > 0) {
                err_msg[0] = '\0';
                cfg_append(err_msg, "watchtower: cannot open EnvironmentFile '", err_cap);
                cfg_append(err_msg, out_svc->env_file, err_cap);
                cfg_append(err_msg, "'", err_cap);
            }
            return -1; /* Fail-closed on missing/unreadable EnvironmentFile */
        }
        /* Parse key-value lines */
        size_t pos = 0;
        while (pos < (size_t)env_read) {
            size_t lstart = pos;
            while (pos < (size_t)env_read && env_buf[pos] != '\n') pos++;
            size_t lend = pos;
            if (pos < (size_t)env_read && env_buf[pos] == '\n') pos++;

            while (lstart < lend && cfg_isspace(env_buf[lstart])) lstart++;
            while (lend > lstart && cfg_isspace(env_buf[lend - 1])) lend--;
            if (lstart >= lend || env_buf[lstart] == '#') continue;

            env_buf[lend] = '\0';
            char *line = &env_buf[lstart];
            char *eq = NULL;
            for (char *p = line; *p; p++) {
                if (*p == '=') { eq = p; break; }
            }
            if (eq && eq > line && out_svc->envc < WATCHTOWER_MAX_ENV) {
                if (lend - lstart < WATCHTOWER_MAX_ENV_LEN) {
                    cfg_copy(out_svc->env[out_svc->envc++], line, WATCHTOWER_MAX_ENV_LEN);
                }
            }
        }
    }

    return 0;
}

int watchtower_config_load_service(const char *installed_path, const char *live_path,
                                   const char *svc_name, service_t *out_svc,
                                   char *err_msg, size_t err_cap) {
    if (!out_svc) return -1;

    /* Rule 1: Installed root is authoritative (/etc/watchtower/<service>.service) */
    if (installed_path) {
        int r = watchtower_config_load_file(installed_path, svc_name, out_svc, err_msg, err_cap);
        if (r == 0) return 0;
        if (r == -1) {
            /* File existed but had a syntax error -> fail closed immediately, do not fall back */
            return -1;
        }
    }

    /* Rule 2: Live media fallback (/mnt/.fortress/watchtower/<service>.service) */
    if (live_path) {
        int r = watchtower_config_load_file(live_path, svc_name, out_svc, err_msg, err_cap);
        if (r == 0) return 0;
        if (r == -1) {
            return -1;
        }
    }

    if (err_msg && err_cap > 0) {
        err_msg[0] = '\0';
        cfg_append(err_msg, "watchtower: no configuration file found for service '", err_cap);
        if (svc_name) cfg_append(err_msg, svc_name, err_cap);
        cfg_append(err_msg, "'", err_cap);
    }
    return -1;
}

static bool is_enabled_line(const char *line, size_t len) {
    size_t i = 0;
    while (i < len && cfg_isspace(line[i])) i++;
    const char *key = "enabled";
    size_t k = 0;
    while (i < len && key[k]) {
        char c = (line[i] >= 'A' && line[i] <= 'Z') ? (char)(line[i] + 32) : line[i];
        if (c != key[k]) return false;
        i++;
        k++;
    }
    if (key[k] != '\0') return false;
    while (i < len && cfg_isspace(line[i])) i++;
    if (i < len && line[i] == '=') return true;
    return false;
}

int watchtower_config_set_enabled(const char *installed_path, const char *live_path,
                                  bool enabled, char *err_msg, size_t err_cap) {
    static char in_buf[WATCHTOWER_MAX_CONFIG_SIZE];
    long nread = -1;
    const char *target_path = NULL;

    if (installed_path) {
        nread = read_file_to_buf(installed_path, in_buf, sizeof(in_buf));
        if (nread >= 0) {
            target_path = installed_path;
        }
    }
    if (nread < 0 && live_path) {
        nread = read_file_to_buf(live_path, in_buf, sizeof(in_buf));
        if (nread >= 0) {
            target_path = live_path;
        }
    }
    if (!target_path || nread < 0) {
        if (err_msg && err_cap > 0) {
            err_msg[0] = '\0';
            cfg_append(err_msg, "watchtower: configuration file not found", err_cap);
        }
        return -1;
    }

    static char out_buf[WATCHTOWER_MAX_CONFIG_SIZE + 64];
    size_t out_len = 0;
    bool has_enabled = false;
    size_t pos = 0;

    while (pos < (size_t)nread) {
        size_t line_start = pos;
        while (pos < (size_t)nread && in_buf[pos] != '\n') pos++;
        size_t line_end = pos;
        if (pos < (size_t)nread && in_buf[pos] == '\n') pos++;

        const char *line = &in_buf[line_start];
        size_t line_len = line_end - line_start;

        if (is_enabled_line(line, line_len)) {
            const char *replacement = enabled ? "Enabled=yes\n" : "Enabled=no\n";
            size_t rlen = cfg_strlen(replacement);
            if (out_len + rlen >= sizeof(out_buf) - 1) {
                if (err_msg && err_cap > 0) {
                    err_msg[0] = '\0';
                    cfg_append(err_msg, "watchtower: configuration exceeds maximum size", err_cap);
                }
                return -1;
            }
            for (size_t k = 0; k < rlen; k++) out_buf[out_len++] = replacement[k];
            has_enabled = true;
        } else {
            size_t copy_len = (line_end < (size_t)nread && in_buf[line_end] == '\n') ? (line_len + 1) : line_len;
            if (out_len + copy_len >= sizeof(out_buf) - 1) {
                if (err_msg && err_cap > 0) {
                    err_msg[0] = '\0';
                    cfg_append(err_msg, "watchtower: configuration exceeds maximum size", err_cap);
                }
                return -1;
            }
            for (size_t k = 0; k < copy_len; k++) out_buf[out_len++] = line[k];
        }
    }

    if (!has_enabled) {
        if (out_len > 0 && out_buf[out_len - 1] != '\n') {
            if (out_len + 1 < sizeof(out_buf) - 1) {
                out_buf[out_len++] = '\n';
            }
        }
        const char *to_add = enabled ? "Enabled=yes\n" : "Enabled=no\n";
        size_t alen = cfg_strlen(to_add);
        if (out_len + alen >= sizeof(out_buf) - 1) {
            if (err_msg && err_cap > 0) {
                err_msg[0] = '\0';
                cfg_append(err_msg, "watchtower: configuration exceeds maximum size", err_cap);
            }
            return -1;
        }
        for (size_t k = 0; k < alen; k++) out_buf[out_len++] = to_add[k];
    }
    out_buf[out_len] = '\0';

    char tmp_path[WATCHTOWER_MAX_PATH_LEN + 16];
    tmp_path[0] = '\0';
    cfg_copy(tmp_path, target_path, sizeof(tmp_path));
    cfg_append(tmp_path, ".tmp", sizeof(tmp_path));

#ifdef WATCHTOWER_HOST_TEST
    int fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        if (err_msg && err_cap > 0) {
            err_msg[0] = '\0';
            cfg_append(err_msg, "watchtower: cannot create temporary file", err_cap);
        }
        return -1;
    }
    ssize_t wr = write(fd, out_buf, out_len);
    close(fd);
    if (wr < 0 || (size_t)wr != out_len) {
        unlink(tmp_path);
        if (err_msg && err_cap > 0) {
            err_msg[0] = '\0';
            cfg_append(err_msg, "watchtower: failed to write temporary file", err_cap);
        }
        return -1;
    }
    if (rename(tmp_path, target_path) != 0) {
        unlink(tmp_path);
        if (err_msg && err_cap > 0) {
            err_msg[0] = '\0';
            cfg_append(err_msg, "watchtower: failed to rename temporary file", err_cap);
        }
        return -1;
    }
    return 0;
#else
    long fd = WT_SYSCALL(SYS_OPEN, (uintptr_t)tmp_path, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC, 0644, 0, 0, 0);
    if (fd < 0) {
        if (err_msg && err_cap > 0) {
            err_msg[0] = '\0';
            cfg_append(err_msg, "watchtower: cannot create temporary file", err_cap);
        }
        return -1;
    }
    long wr = WT_SYSCALL(SYS_WRITE, (uintptr_t)fd, (uintptr_t)out_buf, out_len, 0, 0, 0);
    (void)WT_SYSCALL(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
    if (wr < 0 || (size_t)wr != out_len) {
        (void)WT_SYSCALL(SYS_UNLINK, (uintptr_t)tmp_path, 0, 0, 0, 0, 0);
        if (err_msg && err_cap > 0) {
            err_msg[0] = '\0';
            cfg_append(err_msg, "watchtower: failed to write temporary file", err_cap);
        }
        return -1;
    }
    long ren_r = WT_SYSCALL(SYS_RENAME_REPLACE, (uintptr_t)tmp_path, (uintptr_t)target_path, 0, 0, 0, 0);
    if (ren_r < 0) {
        ren_r = WT_SYSCALL(SYS_RENAME, (uintptr_t)tmp_path, (uintptr_t)target_path, 0, 0, 0, 0);
    }
    if (ren_r < 0) {
        (void)WT_SYSCALL(SYS_UNLINK, (uintptr_t)tmp_path, 0, 0, 0, 0, 0);
        if (err_msg && err_cap > 0) {
            err_msg[0] = '\0';
            cfg_append(err_msg, "watchtower: failed to atomic replace service file", err_cap);
        }
        return -1;
    }
    return 0;
#endif
}
