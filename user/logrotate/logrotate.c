/*
 * FortressOS /sbin/logrotate
 *
 * Implements bounded log rotation per LOGGING_FOUNDATION_PLAN.md:
 *
 * 1. Atomic Single-Instance Exclusion (SYS_MKDIR):
 *    Exclusion is enforced using atomic directory creation:
 *    sys_call(SYS_MKDIR, "/run/logrotate.lock", 0700).
 *    If the directory already exists, acquisition fails closed on SYSCALL_EEXIST (-15).
 *    Scheduled crond invocations pass --skip-if-locked and exit cleanly (0). Direct CLI
 *    invocations without the flag exit 1 with diagnostic warning. The lock directory
 *    records its owner PID in /run/logrotate.lock/pid and is cleanly removed on exit.
 *    Because /run is an ephemeral in-memory filesystem (runfs), abandoned locks are
 *    automatically cleared on system reboot.
 *
 * 2. Delaycompress Protocol & Residual Writer Risk:
 *    Rotating shifts the active file to file.1 and leaves file.1 UNCOMPRESSED.
 *    Compression is deferred to the subsequent cycle when file.1 shifts to file.2 and
 *    is compressed into file.2.gz.
 *    Residual Risk & Bounded Guarantee: FortressOS does not implement mandatory
 *    byte-range locking. Delaycompress provides best-effort protection against transient
 *    writer preemption: writers completing an append within one rotation cycle write
 *    safely to file.1. If a writer is suspended across >= 2 full rotation cycles, its
 *    write will append to file.2 or an unlinked vnode. Loss-free archival is guaranteed
 *    for writers that complete within one rotation cycle.
 *
 * 3. Immediate Active Recreate:
 *    Active files are immediately recreated with VFS_O_CREAT | VFS_O_WRONLY (no O_TRUNC)
 *    so concurrent writers do not lose data or encounter missing path errors.
 *
 * 4. Subsystem Isolation:
 *    /var/log/watchtower/ and /run/watchtower/ are reserved for Watchtower internal
 *    rotation and barred from external logrotate management.
 *    /var/log/boot.log and /run/boot.log are reserved for boot snapshot services alone.
 */

#include "logrotate.h"
#include "syscall_abi.h"
#include "signal_abi.h"
#include "vfs.h"

static bool lr_isblank(char c) {
    return (c == ' ' || c == '\t' || c == '\r');
}

static bool lr_isdigit(char c) {
    return (c >= '0' && c <= '9');
}

static size_t lr_strlen(const char *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static bool lr_streq(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (*a != *b) return false;
        a++;
        b++;
    }
    return *a == *b;
}

static bool lr_starts_with(const char *str, const char *prefix) {
    if (!str || !prefix) return false;
    while (*prefix) {
        if (*str != *prefix) return false;
        str++;
        prefix++;
    }
    return true;
}

static void lr_strlcpy(char *dst, const char *src, size_t cap) {
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

static void lr_strlcat(char *dst, const char *src, size_t cap) {
    if (!dst || cap == 0) return;
    size_t len = lr_strlen(dst);
    if (len >= cap) return;
    size_t i = 0;
    if (src) {
        while (src[i] && len + 1 < cap) {
            dst[len++] = src[i++];
        }
    }
    dst[len] = '\0';
}

static void lr_append_u64(char *dst, uint64_t val, size_t cap) {
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
    lr_strlcat(dst, rev, cap);
}

static void lr_format_err(char *err_msg, size_t err_cap, const char *msg, uint32_t line_num) {
    if (!err_msg || err_cap == 0) return;
    err_msg[0] = '\0';
    lr_strlcat(err_msg, "logrotate: error at line ", err_cap);
    lr_append_u64(err_msg, line_num, err_cap);
    lr_strlcat(err_msg, ": ", err_cap);
    lr_strlcat(err_msg, msg, err_cap);
}

static void lr_format_archive_path(char *dst, size_t cap, const char *base, uint32_t idx, const char *ext) {
    if (!dst || cap == 0) return;
    dst[0] = '\0';
    lr_strlcat(dst, base, cap);
    lr_strlcat(dst, ".", cap);
    lr_append_u64(dst, idx, cap);
    if (ext && ext[0]) {
        lr_strlcat(dst, ext, cap);
    }
}

void logrotate_config_init(logrotate_config_t *cfg) {
    if (!cfg) return;
    for (size_t i = 0; i < sizeof(logrotate_config_t); i++) {
        ((uint8_t *)cfg)[i] = 0;
    }
    cfg->count = 0;
}

int logrotate_parse_config(const char *buf, size_t len, logrotate_config_t *cfg,
                          char *err_msg, size_t err_cap) {
    if (!buf || !cfg) return -1;
    if (len > LOGROTATE_CONFIG_MAX_SIZE) {
        if (err_msg && err_cap > 0) {
            lr_strlcpy(err_msg, "logrotate: config file exceeds 4 KiB limit", err_cap);
        }
        return -1;
    }

    logrotate_config_init(cfg);

    bool in_block = false;
    bool pending_brace = false;
    logrotate_entry_t *curr = NULL;
    uint32_t line_num = 1;
    size_t pos = 0;

    while (pos < len) {
        /* Read next line */
        size_t line_start = pos;
        while (pos < len && buf[pos] != '\n') {
            pos++;
        }
        size_t line_end = pos;
        if (pos < len && buf[pos] == '\n') {
            pos++;
        }

        /* Trim trailing \r */
        if (line_end > line_start && buf[line_end - 1] == '\r') {
            line_end--;
        }

        /* Parse tokens on this line */
        size_t p = line_start;
        while (p < line_end && lr_isblank(buf[p])) p++;

        /* Empty line or comment */
        if (p >= line_end || buf[p] == '#' || buf[p] == ';') {
            line_num++;
            continue;
        }

        if (pending_brace) {
            /* We expect '{' as the first non-blank token */
            if (buf[p] != '{') {
                lr_format_err(err_msg, err_cap, "expected '{' after path", line_num);
                return -1;
            }
            p++;
            while (p < line_end && lr_isblank(buf[p])) p++;
            if (p < line_end && buf[p] != '#' && buf[p] != ';') {
                lr_format_err(err_msg, err_cap, "unexpected tokens after '{'", line_num);
                return -1;
            }
            pending_brace = false;
            in_block = true;
            line_num++;
            continue;
        }

        if (!in_block) {
            /* First token must be absolute path */
            char path_token[LOGROTATE_MAX_PATH];
            size_t path_len = 0;
            while (p < line_end && !lr_isblank(buf[p]) && buf[p] != '{') {
                if (path_len + 1 >= sizeof(path_token)) {
                    lr_format_err(err_msg, err_cap, "path exceeds 256 bytes", line_num);
                    return -1;
                }
                path_token[path_len++] = buf[p++];
            }
            path_token[path_len] = '\0';

            if (path_len == 0) {
                lr_format_err(err_msg, err_cap, "missing path before '{'", line_num);
                return -1;
            }
            if (path_token[0] != '/') {
                lr_format_err(err_msg, err_cap, "path must be absolute", line_num);
                return -1;
            }

            /* Subsystem Isolation: Bar paths reserved for Watchtower or Boot Snapshot */
            if (lr_starts_with(path_token, "/var/log/watchtower/") ||
                lr_starts_with(path_token, "/run/watchtower/")) {
                lr_format_err(err_msg, err_cap, "path is reserved for watchtower internal rotation", line_num);
                return -1;
            }
            if (lr_streq(path_token, "/var/log/boot.log") || lr_streq(path_token, "/run/boot.log")) {
                lr_format_err(err_msg, err_cap, "path is reserved for boot-log retention", line_num);
                return -1;
            }

            if (cfg->count >= LOGROTATE_MAX_ENTRIES) {
                lr_format_err(err_msg, err_cap, "exceeded max 10 entries", line_num);
                return -1;
            }

            curr = &cfg->entries[cfg->count];
            lr_strlcpy(curr->path, path_token, sizeof(curr->path));
            curr->size_limit = 0;
            curr->size_specified = false;
            curr->rotate = 4;
            curr->compress = false;
            curr->missingok = false;

            while (p < line_end && lr_isblank(buf[p])) p++;

            if (p >= line_end || buf[p] == '#' || buf[p] == ';') {
                /* '{' must appear on next line */
                pending_brace = true;
            } else if (buf[p] == '{') {
                p++;
                while (p < line_end && lr_isblank(buf[p])) p++;
                if (p < line_end && buf[p] != '#' && buf[p] != ';') {
                    lr_format_err(err_msg, err_cap, "unexpected tokens after '{'", line_num);
                    return -1;
                }
                in_block = true;
            } else {
                lr_format_err(err_msg, err_cap, "expected '{' after path", line_num);
                return -1;
            }

            line_num++;
            continue;
        }

        /* We are in a block: parse directive */
        if (buf[p] == '}') {
            p++;
            while (p < line_end && lr_isblank(buf[p])) p++;
            if (p < line_end && buf[p] != '#' && buf[p] != ';') {
                lr_format_err(err_msg, err_cap, "unexpected tokens after '}'", line_num);
                return -1;
            }
            in_block = false;
            cfg->count++;
            curr = NULL;
            line_num++;
            continue;
        }

        /* Extract directive keyword */
        char directive[32];
        size_t dlen = 0;
        while (p < line_end && !lr_isblank(buf[p])) {
            if (dlen + 1 < sizeof(directive)) {
                directive[dlen++] = buf[p];
            }
            p++;
        }
        directive[dlen] = '\0';

        while (p < line_end && lr_isblank(buf[p])) p++;

        if (lr_streq(directive, "size")) {
            if (p >= line_end || buf[p] == '#' || buf[p] == ';') {
                lr_format_err(err_msg, err_cap, "size directive requires value", line_num);
                return -1;
            }

            uint64_t val = 0;
            if (!lr_isdigit(buf[p])) {
                lr_format_err(err_msg, err_cap, "invalid numeric value for size", line_num);
                return -1;
            }
            while (p < line_end && lr_isdigit(buf[p])) {
                uint64_t next = val * 10 + (uint64_t)(buf[p] - '0');
                if (next < val) {
                    lr_format_err(err_msg, err_cap, "size value overflow", line_num);
                    return -1;
                }
                val = next;
                p++;
            }

            uint64_t mult = 1;
            if (p < line_end && !lr_isblank(buf[p]) && buf[p] != '#' && buf[p] != ';') {
                char suffix = buf[p++];
                if (suffix == 'k' || suffix == 'K') mult = 1024ULL;
                else if (suffix == 'm' || suffix == 'M') mult = 1024ULL * 1024ULL;
                else if (suffix == 'g' || suffix == 'G') mult = 1024ULL * 1024ULL * 1024ULL;
                else {
                    lr_format_err(err_msg, err_cap, "invalid size suffix", line_num);
                    return -1;
                }
            }

            /* Check remaining tokens on the line */
            while (p < line_end && lr_isblank(buf[p])) p++;
            if (p < line_end && buf[p] != '#' && buf[p] != ';') {
                lr_format_err(err_msg, err_cap, "unexpected arguments for size", line_num);
                return -1;
            }

            if (mult > 1 && val > UINT64_MAX / mult) {
                lr_format_err(err_msg, err_cap, "size value overflow", line_num);
                return -1;
            }

            if (curr) {
                curr->size_limit = val * mult;
                curr->size_specified = true;
            }
        } else if (lr_streq(directive, "rotate")) {
            if (p >= line_end || buf[p] == '#' || buf[p] == ';') {
                lr_format_err(err_msg, err_cap, "rotate directive requires count", line_num);
                return -1;
            }

            if (!lr_isdigit(buf[p])) {
                lr_format_err(err_msg, err_cap, "rotate count must be integer", line_num);
                return -1;
            }

            uint64_t val = 0;
            while (p < line_end && lr_isdigit(buf[p])) {
                val = val * 10 + (uint64_t)(buf[p] - '0');
                if (val > LOGROTATE_MAX_ROTATE) {
                    lr_format_err(err_msg, err_cap, "rotate count cannot exceed 10", line_num);
                    return -1;
                }
                p++;
            }

            while (p < line_end && lr_isblank(buf[p])) p++;
            if (p < line_end && buf[p] != '#' && buf[p] != ';') {
                lr_format_err(err_msg, err_cap, "unexpected arguments for rotate", line_num);
                return -1;
            }

            if (curr) {
                curr->rotate = (uint32_t)val;
            }
        } else if (lr_streq(directive, "compress")) {
            while (p < line_end && lr_isblank(buf[p])) p++;
            if (p < line_end && buf[p] != '#' && buf[p] != ';') {
                lr_format_err(err_msg, err_cap, "unexpected arguments for compress", line_num);
                return -1;
            }
            if (curr) {
                curr->compress = true;
            }
        } else if (lr_streq(directive, "missingok")) {
            while (p < line_end && lr_isblank(buf[p])) p++;
            if (p < line_end && buf[p] != '#' && buf[p] != ';') {
                lr_format_err(err_msg, err_cap, "unexpected arguments for missingok", line_num);
                return -1;
            }
            if (curr) {
                curr->missingok = true;
            }
        } else {
            lr_format_err(err_msg, err_cap, "unknown directive", line_num);
            return -1;
        }

        line_num++;
    }

    if (in_block || pending_brace) {
        lr_format_err(err_msg, err_cap, "unexpected end of file: unclosed block", line_num);
        return -1;
    }

    return 0;
}

int logrotate_load_config(const char *path, logrotate_config_t *cfg,
                         char *err_msg, size_t err_cap,
                         const logrotate_ops_t *ops) {
    if (!path || !cfg || !ops || !ops->open || !ops->read || !ops->close) return -1;

    int fd = ops->open(path, VFS_O_RDONLY, 0);
    if (fd < 0) {
        if (err_msg && err_cap > 0) {
            err_msg[0] = '\0';
            lr_strlcat(err_msg, "logrotate: cannot open config file: ", err_cap);
            lr_strlcat(err_msg, path, err_cap);
        }
        return -1;
    }

    static char s_config_buf[LOGROTATE_CONFIG_MAX_SIZE + 2];
    int64_t n = ops->read(fd, s_config_buf, sizeof(s_config_buf) - 1);
    ops->close(fd);

    if (n < 0) {
        if (err_msg && err_cap > 0) {
            err_msg[0] = '\0';
            lr_strlcat(err_msg, "logrotate: cannot read config file: ", err_cap);
            lr_strlcat(err_msg, path, err_cap);
        }
        return -1;
    }

    if (n > LOGROTATE_CONFIG_MAX_SIZE) {
        if (err_msg && err_cap > 0) {
            lr_strlcpy(err_msg, "logrotate: config file exceeds 4 KiB limit", err_cap);
        }
        return -1;
    }

    s_config_buf[n] = '\0';
    return logrotate_parse_config(s_config_buf, (size_t)n, cfg, err_msg, err_cap);
}

int logrotate_rotate_entry(const logrotate_entry_t *entry, bool force,
                          const logrotate_ops_t *ops) {
    if (!entry || !ops) return -1;
    if (!ops->lstat || !ops->rename || !ops->unlink || !ops->open || !ops->close) return -1;

    logrotate_stat_t st;
    int sret = ops->lstat(entry->path, &st);
    if (sret < 0) {
        if (entry->missingok) {
            return 0;
        }
        if (ops->log_err) {
            ops->log_err("logrotate: error: target file does not exist: ");
            ops->log_err(entry->path);
            ops->log_err("\n");
        }
        return -1;
    }

    /* Symlink Attack Prevention: Reject if target is a symlink */
    if ((st.mode & S_IFMT) == S_IFLNK) {
        if (ops->log_err) {
            ops->log_err("logrotate: error: target is a symlink: ");
            ops->log_err(entry->path);
            ops->log_err("\n");
        }
        return -1;
    }

    /* Reject directory targets */
    if ((st.mode & S_IFMT) == S_IFDIR) {
        if (ops->log_err) {
            ops->log_err("logrotate: error: target is a directory: ");
            ops->log_err(entry->path);
            ops->log_err("\n");
        }
        return -1;
    }

    /* Rotation Decision */
    bool should_rotate = force;
    if (!should_rotate && entry->size_specified) {
        if (st.size > entry->size_limit) {
            should_rotate = true;
        }
    }
    if (!should_rotate && !entry->size_specified) {
        if (st.size > 0) {
            should_rotate = true;
        }
    }

    if (!should_rotate) {
        return 0;
    }

    uint32_t N = entry->rotate;
    uint32_t orig_mode = st.mode;
    uint32_t perm = orig_mode & 07777;
    if (perm == 0) perm = 0640;

    /* 1. Handle rotate 0: unlink active and recreate empty with original mode */
    if (N == 0) {
        (void)ops->unlink(entry->path);
        int fd = ops->open(entry->path, VFS_O_CREAT | VFS_O_WRONLY, perm);
        if (fd >= 0) {
            ops->close(fd);
        } else {
            if (ops->log_err) {
                ops->log_err("logrotate: error: failed to recreate active file: ");
                ops->log_err(entry->path);
                ops->log_err("\n");
            }
            return -1;
        }
        return 0;
    }

    /* 2. Handle rotate 1:
     * Unlink oldest archive (.1.gz and .1).
     * Rename active -> .1.
     * Immediately recreate active (no O_TRUNC).
     * Retain .1 uncompressed even if compress configured (to preserve writer grace period). */
    if (N == 1) {
        char oldest_gz[LOGROTATE_MAX_PATH + 32];
        char oldest_raw[LOGROTATE_MAX_PATH + 32];
        lr_format_archive_path(oldest_gz, sizeof(oldest_gz), entry->path, 1, ".gz");
        lr_format_archive_path(oldest_raw, sizeof(oldest_raw), entry->path, 1, "");
        (void)ops->unlink(oldest_gz);
        (void)ops->unlink(oldest_raw);

        int r = ops->rename(entry->path, oldest_raw);
        if (r < 0) {
            if (ops->log_err) {
                ops->log_err("logrotate: error: failed to rename active file: ");
                ops->log_err(entry->path);
                ops->log_err("\n");
            }
            return -1;
        }

        int fd = ops->open(entry->path, VFS_O_CREAT | VFS_O_WRONLY, perm);
        if (fd >= 0) {
            ops->close(fd);
        } else {
            if (ops->log_err) {
                ops->log_err("logrotate: error: failed to recreate active file: ");
                ops->log_err(entry->path);
                ops->log_err("\n");
            }
            return -1;
        }
        return 0;
    }

    /* 3. General Case: rotate >= 2 with compress (Delaycompress) */
    if (entry->compress) {
        /* Step 4a: Unlink oldest (.N.gz and .N) */
        char oldest_gz[LOGROTATE_MAX_PATH + 32];
        char oldest_raw[LOGROTATE_MAX_PATH + 32];
        lr_format_archive_path(oldest_gz, sizeof(oldest_gz), entry->path, N, ".gz");
        lr_format_archive_path(oldest_raw, sizeof(oldest_raw), entry->path, N, "");
        (void)ops->unlink(oldest_gz);
        (void)ops->unlink(oldest_raw);

        /* Step 4b: Shift older compressed archives (N - 1 down to 2) */
        for (int i = (int)N - 1; i >= 2; i--) {
            char src_gz[LOGROTATE_MAX_PATH + 32];
            char dst_gz[LOGROTATE_MAX_PATH + 32];
            lr_format_archive_path(src_gz, sizeof(src_gz), entry->path, (uint32_t)i, ".gz");
            lr_format_archive_path(dst_gz, sizeof(dst_gz), entry->path, (uint32_t)(i + 1), ".gz");

            logrotate_stat_t probe_st;
            if (ops->lstat(src_gz, &probe_st) == 0) {
                (void)ops->rename(src_gz, dst_gz);
            }

            char src_raw[LOGROTATE_MAX_PATH + 32];
            char dst_raw[LOGROTATE_MAX_PATH + 32];
            lr_format_archive_path(src_raw, sizeof(src_raw), entry->path, (uint32_t)i, "");
            lr_format_archive_path(dst_raw, sizeof(dst_raw), entry->path, (uint32_t)(i + 1), "");

            if (ops->lstat(src_raw, &probe_st) == 0) {
                (void)ops->rename(src_raw, dst_raw);
            }
        }

        /* Step 4c: Shift and compress previous shift (active.1 -> active.2 -> active.2.gz) */
        char p1[LOGROTATE_MAX_PATH + 32];
        char p2[LOGROTATE_MAX_PATH + 32];
        char p1_gz[LOGROTATE_MAX_PATH + 32];
        lr_format_archive_path(p1, sizeof(p1), entry->path, 1, "");
        lr_format_archive_path(p2, sizeof(p2), entry->path, 2, "");
        lr_format_archive_path(p1_gz, sizeof(p1_gz), entry->path, 1, ".gz");

        logrotate_stat_t probe_st;
        if (ops->lstat(p1, &probe_st) == 0) {
            (void)ops->rename(p1, p2);

            bool compress_ok = false;
            if (ops->spawn && ops->waitpid) {
                const char *gzip_argv[] = {"/bin/gzip", "-f", p2, NULL};
                int64_t pid = ops->spawn("/bin/gzip", gzip_argv);
                if (pid > 0) {
                    int elapsed_ms = 0;
                    uint64_t status = 0;
                    while (elapsed_ms < LOGROTATE_GZIP_TIMEOUT_MS) {
                        int64_t w = ops->waitpid(pid, &status, WNOHANG);
                        if (w == pid) {
                            int exit_code = (int)((status >> 8) & 0xff);
                            if (exit_code == 0) {
                                compress_ok = true;
                            } else if (ops->log_err) {
                                ops->log_err("logrotate: error: /bin/gzip exited with error\n");
                            }
                            break;
                        }
                        if (ops->sleep_ms) {
                            ops->sleep_ms(50);
                        }
                        elapsed_ms += 50;
                    }
                    if (!compress_ok && elapsed_ms >= LOGROTATE_GZIP_TIMEOUT_MS) {
                        if (ops->kill) ops->kill(pid, SIGKILL);
                        (void)ops->waitpid(pid, &status, 0);
                        if (ops->log_err) {
                            ops->log_err("logrotate: error: /bin/gzip timed out\n");
                        }
                    }
                } else if (ops->log_err) {
                    ops->log_err("logrotate: error: failed to spawn /bin/gzip\n");
                }
            }
            if (!compress_ok && ops->log_err) {
                ops->log_err("logrotate: warning: left archive uncompressed: ");
                ops->log_err(p2);
                ops->log_err("\n");
            }
        } else if (ops->lstat(p1_gz, &probe_st) == 0) {
            char p2_gz[LOGROTATE_MAX_PATH + 32];
            lr_format_archive_path(p2_gz, sizeof(p2_gz), entry->path, 2, ".gz");
            (void)ops->rename(p1_gz, p2_gz);
        }

        /* Step 4d: Shift active file to active.1 */
        int r = ops->rename(entry->path, p1);
        if (r < 0) {
            if (ops->log_err) {
                ops->log_err("logrotate: error: failed to rename active file: ");
                ops->log_err(entry->path);
                ops->log_err("\n");
            }
            return -1;
        }

        /* Step 4e: Immediately recreate active file (no O_TRUNC) */
        int fd = ops->open(entry->path, VFS_O_CREAT | VFS_O_WRONLY, perm);
        if (fd >= 0) {
            ops->close(fd);
        } else {
            if (ops->log_err) {
                ops->log_err("logrotate: error: failed to recreate active file: ");
                ops->log_err(entry->path);
                ops->log_err("\n");
            }
            return -1;
        }

        /* Step 4f: active.1 remains uncompressed */
        return 0;
    }

    /* 4. General Case: rotate >= 2 without compress */
    {
        char oldest_gz[LOGROTATE_MAX_PATH + 32];
        char oldest_raw[LOGROTATE_MAX_PATH + 32];
        lr_format_archive_path(oldest_gz, sizeof(oldest_gz), entry->path, N, ".gz");
        lr_format_archive_path(oldest_raw, sizeof(oldest_raw), entry->path, N, "");
        (void)ops->unlink(oldest_gz);
        (void)ops->unlink(oldest_raw);

        for (int i = (int)N - 1; i >= 1; i--) {
            char src_gz[LOGROTATE_MAX_PATH + 32];
            char dst_gz[LOGROTATE_MAX_PATH + 32];
            lr_format_archive_path(src_gz, sizeof(src_gz), entry->path, (uint32_t)i, ".gz");
            lr_format_archive_path(dst_gz, sizeof(dst_gz), entry->path, (uint32_t)(i + 1), ".gz");
            logrotate_stat_t probe_st;
            if (ops->lstat(src_gz, &probe_st) == 0) {
                (void)ops->rename(src_gz, dst_gz);
            }

            char src_raw[LOGROTATE_MAX_PATH + 32];
            char dst_raw[LOGROTATE_MAX_PATH + 32];
            lr_format_archive_path(src_raw, sizeof(src_raw), entry->path, (uint32_t)i, "");
            lr_format_archive_path(dst_raw, sizeof(dst_raw), entry->path, (uint32_t)(i + 1), "");
            if (ops->lstat(src_raw, &probe_st) == 0) {
                (void)ops->rename(src_raw, dst_raw);
            }
        }

        char p1[LOGROTATE_MAX_PATH + 32];
        lr_format_archive_path(p1, sizeof(p1), entry->path, 1, "");
        int r = ops->rename(entry->path, p1);
        if (r < 0) {
            if (ops->log_err) {
                ops->log_err("logrotate: error: failed to rename active file: ");
                ops->log_err(entry->path);
                ops->log_err("\n");
            }
            return -1;
        }

        int fd = ops->open(entry->path, VFS_O_CREAT | VFS_O_WRONLY, perm);
        if (fd >= 0) {
            ops->close(fd);
        } else {
            if (ops->log_err) {
                ops->log_err("logrotate: error: failed to recreate active file: ");
                ops->log_err(entry->path);
                ops->log_err("\n");
            }
            return -1;
        }
        return 0;
    }
}

int logrotate_run_with_ops(int argc, char **argv, const logrotate_ops_t *ops) {
    if (!ops) return 1;

    bool force = false;
    bool skip_if_locked = false;
    const char *config_path = NULL;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (lr_streq(arg, "-f") || lr_streq(arg, "--force")) {
            force = true;
        } else if (lr_streq(arg, "--skip-if-locked")) {
            skip_if_locked = true;
        } else if (lr_streq(arg, "-h") || lr_streq(arg, "--help")) {
            if (ops->log_err) {
                ops->log_err("usage: logrotate [-f|--force] [--skip-if-locked] [<config_file>]\n");
            }
            return 0;
        } else if (arg[0] == '-') {
            if (ops->log_err) {
                ops->log_err("logrotate: unknown option: ");
                ops->log_err(arg);
                ops->log_err("\nusage: logrotate [-f|--force] [--skip-if-locked] [<config_file>]\n");
            }
            return 1;
        } else {
            if (config_path != NULL) {
                if (ops->log_err) {
                    ops->log_err("logrotate: multiple config files specified\n");
                }
                return 1;
            }
            config_path = arg;
        }
    }

    /* 1. Acquire Exclusion via atomic SYS_MKDIR */
    bool lock_acquired = false;
    if (ops->mkdir) {
        int mret = ops->mkdir(LOGROTATE_LOCK_DIR, 0700);
        if (mret != 0) {
            if (mret == -15 || mret == -SYSCALL_EEXIST) {
                /* Collision: read existing owner PID if present */
                int pfd = ops->open ? ops->open(LOGROTATE_LOCK_PID, VFS_O_RDONLY, 0) : -1;
                int64_t held_pid = -1;
                if (pfd >= 0) {
                    char pbuf[32];
                    int64_t pn = ops->read ? ops->read(pfd, pbuf, sizeof(pbuf) - 1) : -1;
                    if (ops->close) ops->close(pfd);
                    if (pn > 0) {
                        pbuf[pn] = '\0';
                        size_t pi = 0;
                        while (pi < (size_t)pn && lr_isblank(pbuf[pi])) pi++;
                        if (pi < (size_t)pn && lr_isdigit(pbuf[pi])) {
                            int64_t val = 0;
                            while (pi < (size_t)pn && lr_isdigit(pbuf[pi])) {
                                val = val * 10 + (pbuf[pi] - '0');
                                pi++;
                            }
                            held_pid = val;
                        }
                    }
                }

                if (ops->log_err) {
                    char lmsg[128];
                    lmsg[0] = '\0';
                    lr_strlcat(lmsg, "logrotate: locked by ", sizeof(lmsg));
                    lr_strlcat(lmsg, LOGROTATE_LOCK_DIR, sizeof(lmsg));
                    lr_strlcat(lmsg, " (pid ", sizeof(lmsg));
                    if (held_pid > 0) {
                        lr_append_u64(lmsg, (uint64_t)held_pid, sizeof(lmsg));
                    } else {
                        lr_strlcat(lmsg, "unknown", sizeof(lmsg));
                    }
                    lr_strlcat(lmsg, "); skipping\n", sizeof(lmsg));
                    ops->log_err(lmsg);
                }

                return skip_if_locked ? 0 : 1;
            } else {
                if (ops->log_err) {
                    ops->log_err("logrotate: failed to acquire lock directory\n");
                }
                return 1;
            }
        }
        lock_acquired = true;

        /* Write PID file */
        if (ops->open && ops->write && ops->close) {
            int pfd = ops->open(LOGROTATE_LOCK_PID, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC, 0644);
            if (pfd >= 0) {
                int64_t my_pid = ops->getpid ? ops->getpid() : 0;
                char pbuf[32];
                pbuf[0] = '\0';
                lr_append_u64(pbuf, (uint64_t)my_pid, sizeof(pbuf));
                lr_strlcat(pbuf, "\n", sizeof(pbuf));
                ops->write(pfd, pbuf, lr_strlen(pbuf));
                ops->close(pfd);
            }
        }
    }

    static logrotate_config_t config;
    char err_msg[256];
    err_msg[0] = '\0';
    int rc = 0;

    if (config_path) {
        rc = logrotate_load_config(config_path, &config, err_msg, sizeof(err_msg), ops);
        if (rc != 0) {
            if (ops->log_err) {
                ops->log_err(err_msg[0] ? err_msg : "logrotate: failed to load config");
                ops->log_err("\n");
            }
            if (lock_acquired && ops->unlink) {
                ops->unlink(LOGROTATE_LOCK_PID);
                ops->unlink(LOGROTATE_LOCK_DIR);
            }
            return 1;
        }
    } else {
        /* Try primary */
        rc = logrotate_load_config(LOGROTATE_CONFIG_PRIMARY, &config, err_msg, sizeof(err_msg), ops);
        if (rc != 0) {
            /* Try fallback */
            err_msg[0] = '\0';
            rc = logrotate_load_config(LOGROTATE_CONFIG_FALLBACK, &config, err_msg, sizeof(err_msg), ops);
            if (rc != 0) {
                if (ops->log_err) {
                    ops->log_err(err_msg[0] ? err_msg : "logrotate: cannot open config file (tried primary and fallback)");
                    ops->log_err("\n");
                }
                if (lock_acquired && ops->unlink) {
                    ops->unlink(LOGROTATE_LOCK_PID);
                    ops->unlink(LOGROTATE_LOCK_DIR);
                }
                return 1;
            }
        }
    }

    int overall_status = 0;
    for (size_t i = 0; i < config.count; i++) {
        int r = logrotate_rotate_entry(&config.entries[i], force, ops);
        if (r != 0) {
            overall_status = 1;
        }
    }

    /* Release exclusion */
    if (lock_acquired && ops->unlink) {
        ops->unlink(LOGROTATE_LOCK_PID);
        ops->unlink(LOGROTATE_LOCK_DIR);
    }

    return overall_status;
}

#ifndef LOGROTATE_HOST_TEST

static long sys_call(long nr, uintptr_t a, uintptr_t b, uintptr_t c,
                     uintptr_t d, uintptr_t e, uintptr_t f) {
    register uintptr_t r10 __asm__("r10") = d;
    register uintptr_t r8  __asm__("r8")  = e;
    register uintptr_t r9  __asm__("r9")  = f;
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c),
                     "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory", "cc");
    return nr;
}

static int prod_lstat(const char *path, logrotate_stat_t *st) {
    if (!path || !st) return -1;
    vfs_stat_t vst;
    for (size_t i = 0; i < sizeof(vst); i++) ((uint8_t *)&vst)[i] = 0;
    long ret = sys_call(SYS_LSTAT, (uintptr_t)path, (uintptr_t)&vst, 0, 0, 0, 0);
    if (ret != 0) return (int)ret;
    st->size = vst.size;
    st->mode = vst.mode;
    return 0;
}

static int prod_stat(const char *path, logrotate_stat_t *st) {
    if (!path || !st) return -1;
    vfs_stat_t vst;
    for (size_t i = 0; i < sizeof(vst); i++) ((uint8_t *)&vst)[i] = 0;
    long ret = sys_call(SYS_STAT, (uintptr_t)path, (uintptr_t)&vst, 0, 0, 0, 0);
    if (ret != 0) return (int)ret;
    st->size = vst.size;
    st->mode = vst.mode;
    return 0;
}

static int prod_rename(const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath) return -1;
    long ret = sys_call(SYS_RENAME, (uintptr_t)oldpath, (uintptr_t)newpath, 0, 0, 0, 0);
    return (int)ret;
}

static int prod_unlink(const char *path) {
    if (!path) return -1;
    long ret = sys_call(SYS_UNLINK, (uintptr_t)path, 0, 0, 0, 0, 0);
    return (int)ret;
}

static int prod_open(const char *path, int flags, uint32_t mode) {
    if (!path) return -1;
    long ret = sys_call(SYS_OPEN, (uintptr_t)path, (uintptr_t)flags, (uintptr_t)mode, 0, 0, 0);
    return (int)ret;
}

static int prod_close(int fd) {
    long ret = sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
    return (int)ret;
}

static int64_t prod_read(int fd, void *buf, size_t count) {
    if (!buf) return -1;
    long ret = sys_call(SYS_READ, (uintptr_t)fd, (uintptr_t)buf, count, 0, 0, 0);
    return (int64_t)ret;
}

static int64_t prod_write(int fd, const void *buf, size_t count) {
    if (!buf) return -1;
    long ret = sys_call(SYS_WRITE, (uintptr_t)fd, (uintptr_t)buf, count, 0, 0, 0);
    return (int64_t)ret;
}

static int64_t prod_spawn(const char *path, const char *const argv[]) {
    if (!path || !argv) return -1;
    spawn_opts_t opts;
    for (size_t i = 0; i < sizeof(opts); i++) ((uint8_t *)&opts)[i] = 0;
    opts.size = sizeof(opts);
    opts.version = 2;
    opts.flags = SPAWN_SETPGROUP;
    opts.argv = (uint64_t)argv;
    long pid = sys_call(SYS_SPAWN_EXT, (uintptr_t)path, (uintptr_t)&opts, sizeof(opts), 0, 0, 0);
    return (int64_t)pid;
}

static int64_t prod_waitpid(int64_t pid, uint64_t *status, int options) {
    long ret = sys_call(SYS_WAITPID, (uintptr_t)pid, (uintptr_t)status, (uintptr_t)options, 0, 0, 0);
    return (int64_t)ret;
}

static void prod_sleep_ms(uint32_t ms) {
    timespec_t req;
    req.tv_sec = (int64_t)(ms / 1000);
    req.tv_nsec = (int64_t)((ms % 1000) * 1000000ULL);
    timespec_t rem;
    (void)sys_call(SYS_NANOSLEEP, (uintptr_t)&req, (uintptr_t)&rem, 0, 0, 0, 0);
}

static void prod_log_err(const char *msg) {
    if (!msg) return;
    size_t len = lr_strlen(msg);
    if (len > 0) {
        (void)sys_call(SYS_WRITE, 2, (uintptr_t)msg, len, 0, 0, 0);
    }
}

static int prod_mkdir(const char *path, uint32_t mode) {
    if (!path) return -1;
    long ret = sys_call(SYS_MKDIR, (uintptr_t)path, (uintptr_t)mode, 0, 0, 0, 0);
    return (int)ret;
}

static int64_t prod_getpid(void) {
    proc_info_t pi;
    for (size_t i = 0; i < sizeof(pi); i++) ((uint8_t *)&pi)[i] = 0;
    long ret = sys_call(SYS_PROCINFO, PROC_INFO_SELF, (uintptr_t)&pi, 0, 0, 0, 0);
    if (ret < 0) return 1;
    return (int64_t)pi.pid;
}

static int prod_kill(int64_t pid, int sig) {
    if (pid <= 0) return -1;
    long ret = sys_call(SYS_KILL, (uintptr_t)pid, (uintptr_t)sig, 0, 0, 0, 0);
    return (int)ret;
}

int logrotate_main(int argc, char **argv) {
    static logrotate_ops_t ops;
    ops.lstat = prod_lstat;
    ops.stat = prod_stat;
    ops.rename = prod_rename;
    ops.unlink = prod_unlink;
    ops.open = prod_open;
    ops.close = prod_close;
    ops.read = prod_read;
    ops.write = prod_write;
    ops.spawn = prod_spawn;
    ops.waitpid = prod_waitpid;
    ops.sleep_ms = prod_sleep_ms;
    ops.log_err = prod_log_err;
    ops.mkdir = prod_mkdir;
    ops.getpid = prod_getpid;
    ops.kill = prod_kill;

    return logrotate_run_with_ops(argc, argv, &ops);
}

#endif /* !LOGROTATE_HOST_TEST */
