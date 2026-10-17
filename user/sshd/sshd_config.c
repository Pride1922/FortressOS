#include "sshd_config.h"
#include "syscall_abi.h"
#include "vfs.h"
#include "string.h"

#ifdef CRYPTO_HOST_TEST
#include <fcntl.h>
#include <unistd.h>
#endif

#ifndef SSHD_SYSCALL
static inline long sshd_default_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c,
                                        uintptr_t d, uintptr_t e, uintptr_t f) {
#ifndef CRYPTO_HOST_TEST
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
#define SSHD_SYSCALL sshd_default_syscall
#endif

static void cfg_copy(char *dst, const char *src, size_t cap) {
    if (!dst || cap == 0) return;
    size_t i = 0;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static bool cfg_isspace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool cfg_strcasecmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        char c1 = *s1 >= 'A' && *s1 <= 'Z' ? (char)(*s1 + 32) : *s1;
        char c2 = *s2 >= 'A' && *s2 <= 'Z' ? (char)(*s2 + 32) : *s2;
        if (c1 != c2) return false;
        s1++;
        s2++;
    }
    return *s1 == '\0' && *s2 == '\0';
}

static int parse_ipv4(const char *str, uint32_t *out_ip) {
    uint32_t octets[4] = {0};
    int octet_idx = 0;
    const char *p = str;

    while (*p && octet_idx < 4) {
        if (*p < '0' || *p > '9') return -1;
        uint32_t val = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            val = val * 10 + (uint32_t)(*p - '0');
            digits++;
            p++;
        }
        if (digits == 0 || digits > 3 || val > 255) return -1;
        octets[octet_idx++] = val;

        if (octet_idx < 4) {
            if (*p != '.') return -1;
            p++;
        }
    }

    if (octet_idx != 4 || *p != '\0') return -1;

    *out_ip = (octets[0]) | (octets[1] << 8) | (octets[2] << 16) | (octets[3] << 24);
    return 0;
}

void sshd_config_init_defaults(sshd_config_t *cfg) {
    if (!cfg) return;
    cfg->enabled = true;
    cfg->port = 22;
    cfg->listen_address = 0; /* INADDR_ANY */
    cfg->permit_root_login = false;
    cfg->password_auth = true;
    cfg->pubkey_auth = true;
    cfg->max_sessions = 2;
}

int sshd_config_parse_buffer(const char *buf, size_t len, sshd_config_t *out_cfg,
                             char *err_msg, size_t err_cap) {
    if (!buf || !out_cfg) return -1;
    sshd_config_init_defaults(out_cfg);

    size_t pos = 0;
    while (pos < len) {
        /* Read next line */
        size_t line_start = pos;
        while (pos < len && buf[pos] != '\n') pos++;
        size_t line_end = pos;
        if (pos < len && buf[pos] == '\n') pos++;

        /* Trim comments starting with '#' */
        for (size_t i = line_start; i < line_end; i++) {
            if (buf[i] == '#') {
                line_end = i;
                break;
            }
        }

        /* Skip leading whitespace */
        while (line_start < line_end && cfg_isspace(buf[line_start])) line_start++;
        /* Trim trailing whitespace */
        while (line_end > line_start && cfg_isspace(buf[line_end - 1])) line_end--;

        if (line_start >= line_end) continue; /* empty line */

        /* Extract key and value separated by whitespace or '=' */
        size_t key_start = line_start;
        size_t key_end = key_start;
        while (key_end < line_end && !cfg_isspace(buf[key_end]) && buf[key_end] != '=') key_end++;

        size_t val_start = key_end;
        while (val_start < line_end && (cfg_isspace(buf[val_start]) || buf[val_start] == '=')) val_start++;

        char key[64];
        size_t klen = key_end - key_start;
        if (klen >= sizeof(key)) klen = sizeof(key) - 1;
        memcpy(key, buf + key_start, klen);
        key[klen] = '\0';

        char val[128];
        size_t vlen = line_end - val_start;
        if (vlen >= sizeof(val)) vlen = sizeof(val) - 1;
        memcpy(val, buf + val_start, vlen);
        val[vlen] = '\0';

        if (cfg_strcasecmp(key, "Enabled") || cfg_strcasecmp(key, "SSH_ENABLE")) {
            if (cfg_strcasecmp(val, "yes") || cfg_strcasecmp(val, "true") || cfg_strcasecmp(val, "1")) {
                out_cfg->enabled = true;
            } else if (cfg_strcasecmp(val, "no") || cfg_strcasecmp(val, "false") || cfg_strcasecmp(val, "0")) {
                out_cfg->enabled = false;
            }
        } else if (cfg_strcasecmp(key, "Port")) {
            uint32_t p = 0;
            const char *vp = val;
            while (*vp >= '0' && *vp <= '9') {
                p = p * 10 + (uint32_t)(*vp - '0');
                vp++;
            }
            if (*vp == '\0' && p > 0 && p <= 65535) {
                out_cfg->port = (uint16_t)p;
            } else {
                if (err_msg && err_cap > 0) {
                    cfg_copy(err_msg, "Invalid port number", err_cap);
                }
                return -1;
            }
        } else if (cfg_strcasecmp(key, "ListenAddress")) {
            uint32_t ip = 0;
            if (parse_ipv4(val, &ip) == 0) {
                out_cfg->listen_address = ip;
            } else {
                if (err_msg && err_cap > 0) {
                    cfg_copy(err_msg, "Invalid ListenAddress", err_cap);
                }
                return -1;
            }
        } else if (cfg_strcasecmp(key, "PermitRootLogin")) {
            if (cfg_strcasecmp(val, "yes")) {
                out_cfg->permit_root_login = true;
            } else {
                out_cfg->permit_root_login = false;
            }
        } else if (cfg_strcasecmp(key, "PasswordAuthentication")) {
            if (cfg_strcasecmp(val, "no") || cfg_strcasecmp(val, "0")) {
                out_cfg->password_auth = false;
            } else {
                out_cfg->password_auth = true;
            }
        } else if (cfg_strcasecmp(key, "PubkeyAuthentication")) {
            if (cfg_strcasecmp(val, "no") || cfg_strcasecmp(val, "0")) {
                out_cfg->pubkey_auth = false;
            } else {
                out_cfg->pubkey_auth = true;
            }
        } else if (cfg_strcasecmp(key, "MaxSessions")) {
            uint32_t s = 0;
            const char *vp = val;
            while (*vp >= '0' && *vp <= '9') {
                s = s * 10 + (uint32_t)(*vp - '0');
                vp++;
            }
            if (*vp == '\0' && s >= 1 && s <= 2) {
                out_cfg->max_sessions = s;
            }
        }
    }

    return 0;
}

int sshd_config_load(const char *path, sshd_config_t *out_cfg,
                     char *err_msg, size_t err_cap) {
    if (!out_cfg) return -1;
    sshd_config_init_defaults(out_cfg);

    const char *try_paths[] = {
        path,
        SSHD_DEFAULT_CONFIG_PATH,
        SSHD_INSTALLED_CONFIG_PATH
    };

    char buf[SSHD_MAX_CONFIG_FILE_SIZE];
    long nread = -1;

    for (size_t i = 0; i < sizeof(try_paths) / sizeof(try_paths[0]); i++) {
        if (!try_paths[i]) continue;
#ifndef CRYPTO_HOST_TEST
        long fd = SSHD_SYSCALL(SYS_OPEN, (uintptr_t)try_paths[i], VFS_O_RDONLY | VFS_O_CLOEXEC, 0, 0, 0, 0);
        if (fd >= 0) {
            nread = SSHD_SYSCALL(SYS_READ, (uintptr_t)fd, (uintptr_t)buf, sizeof(buf) - 1, 0, 0, 0);
            SSHD_SYSCALL(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
            if (nread > 0) {
                buf[nread] = '\0';
                break;
            }
        }
#else
        int fd = open(try_paths[i], O_RDONLY);
        if (fd >= 0) {
            nread = (long)read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (nread > 0) {
                buf[nread] = '\0';
                break;
            }
        }
#endif
    }

    if (nread <= 0) {
        /* Default configuration remains active */
        return 0;
    }

    return sshd_config_parse_buffer(buf, (size_t)nread, out_cfg, err_msg, err_cap);
}
