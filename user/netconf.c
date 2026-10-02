#include "netconf.h"
#include "syscall_abi.h"

#ifndef NETCONF_SYSCALL
static inline long netconf_default_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}
#define NETCONF_SYSCALL netconf_default_syscall
#endif

static size_t nc_strlen(const char *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static void nc_write_stderr(const char *s) {
    size_t len = nc_strlen(s);
    size_t off = 0;
    while (off < len) {
        long n = NETCONF_SYSCALL(SYS_WRITE, 2, (uintptr_t)(s + off), len - off);
        if (n <= 0) break;
        off += (size_t)n;
    }
}

static bool nc_isspace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool nc_isdigit(char c) {
    return c >= '0' && c <= '9';
}

static void nc_append_str(char *buf, size_t cap, size_t *pos, const char *s) {
    while (*s && *pos + 1 < cap) {
        buf[(*pos)++] = *s++;
    }
    if (*pos < cap) buf[*pos] = '\0';
}

static void nc_append_u32(char *buf, size_t cap, size_t *pos, uint32_t val) {
    char tmp[12];
    int idx = 0;
    if (val == 0) {
        tmp[idx++] = '0';
    } else {
        while (val > 0) {
            tmp[idx++] = (char)('0' + (val % 10));
            val /= 10;
        }
    }
    for (int i = idx - 1; i >= 0 && *pos + 1 < cap; i--) {
        buf[(*pos)++] = tmp[i];
    }
    if (*pos < cap) buf[*pos] = '\0';
}

int netconf_parse_ipv4(const char *str, uint32_t *out_ip) {
    if (!str || !out_ip) return -1;
    uint32_t acc = 0;
    const char *p = str;
    for (int i = 0; i < 4; i++) {
        if (!nc_isdigit(*p)) return -1;
        uint32_t oct = 0;
        int digits = 0;
        while (nc_isdigit(*p)) {
            oct = oct * 10 + (uint32_t)(*p - '0');
            digits++;
            p++;
            if (oct > 255 || digits > 3) return -1;
        }
        acc = (acc << 8) | oct;
        if (i < 3) {
            if (*p != '.') return -1;
            p++;
        }
    }
    if (*p != '\0') return -1;
    *out_ip = __builtin_bswap32(acc);
    return 0;
}

int netconf_parse_cidr(const char *str, uint32_t *out_ip, uint8_t *out_prefix, uint32_t *out_mask) {
    if (!str || !out_ip || !out_prefix || !out_mask) return -1;
    char ip_part[64];
    size_t i = 0;
    while (str[i] && str[i] != '/' && i + 1 < sizeof(ip_part)) {
        ip_part[i] = str[i];
        i++;
    }
    if (str[i] != '/') return -1;
    ip_part[i] = '\0';

    if (netconf_parse_ipv4(ip_part, out_ip) != 0) return -1;

    const char *pref_str = str + i + 1;
    if (!nc_isdigit(*pref_str)) return -1;
    uint32_t p = 0;
    while (nc_isdigit(*pref_str)) {
        p = p * 10 + (uint32_t)(*pref_str - '0');
        pref_str++;
        if (p > 32) return -1;
    }
    if (*pref_str != '\0' || p < 1 || p > 30) return -1;

    *out_prefix = (uint8_t)p;
    uint32_t m = (0xffffffffu << (32 - p));
    *out_mask = __builtin_bswap32(m);
    return 0;
}

int netconf_parse_netmask(const char *str, uint8_t *out_prefix, uint32_t *out_mask) {
    if (!str || !out_prefix || !out_mask) return -1;
    uint32_t raw_ip;
    if (netconf_parse_ipv4(str, &raw_ip) != 0) return -1;
    uint32_t m = __builtin_bswap32(raw_ip);
    unsigned p = 0;
    uint32_t cur = m;
    while (cur & 0x80000000u) {
        p++;
        cur <<= 1;
    }
    if (cur != 0 || p < 1 || p > 30) return -1;
    *out_prefix = (uint8_t)p;
    *out_mask = raw_ip;
    return 0;
}

static bool str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

int netconf_parse_buffer(const char *buf, size_t len, const char *source_name,
                         const char *tool_name, bool verbose_warnings,
                         netconf_t *out_conf, char *err_msg, size_t err_cap) {
    if (!buf || !source_name || !out_conf) return -1;
    for (size_t i = 0; i < sizeof(*out_conf); i++) {
        ((char *)out_conf)[i] = 0;
    }
    if (err_msg && err_cap > 0) err_msg[0] = '\0';

    if (len > NETCONF_MAX_FILE_SIZE) {
        if (err_msg && err_cap > 0) {
            size_t pos = 0;
            nc_append_str(err_msg, err_cap, &pos, source_name);
            nc_append_str(err_msg, err_cap, &pos, ": file exceeds 4096 bytes");
        }
        return -1;
    }

    size_t at = 0;
    uint32_t line_num = 0;

    while (at < len) {
        line_num++;
        size_t line_start = at;
        while (at < len && buf[at] != '\n') {
            at++;
        }
        size_t raw_len = at - line_start;
        if (at < len && buf[at] == '\n') at++; /* skip \n */

        if (raw_len > NETCONF_MAX_LINE_LEN) {
            if (err_msg && err_cap > 0) {
                size_t pos = 0;
                nc_append_str(err_msg, err_cap, &pos, source_name);
                nc_append_str(err_msg, err_cap, &pos, ":");
                nc_append_u32(err_msg, err_cap, &pos, line_num);
                nc_append_str(err_msg, err_cap, &pos, ": line exceeds 256 bytes");
            }
            return -1;
        }

        static char s_line[NETCONF_MAX_LINE_LEN + 1];
        for (size_t i = 0; i < raw_len; i++) {
            s_line[i] = buf[line_start + i];
        }
        s_line[raw_len] = '\0';

        /* Strip comment starting at '#' */
        for (size_t i = 0; i < raw_len; i++) {
            if (s_line[i] == '#') {
                s_line[i] = '\0';
                break;
            }
        }

        /* Trim leading whitespace */
        char *p = s_line;
        while (*p && nc_isspace(*p)) p++;

        /* Trim trailing whitespace */
        size_t pl = nc_strlen(p);
        while (pl > 0 && nc_isspace(p[pl - 1])) {
            p[pl - 1] = '\0';
            pl--;
        }
        if (pl == 0) continue; /* empty line */

        /* Extract key: alphanumeric until whitespace or '=' */
        char key[64];
        size_t klen = 0;
        while (*p && !nc_isspace(*p) && *p != '=' && klen + 1 < sizeof(key)) {
            key[klen++] = *p++;
        }
        key[klen] = '\0';

        /* Skip separator */
        while (*p && nc_isspace(*p)) p++;
        if (*p == '=') {
            p++;
            while (*p && nc_isspace(*p)) p++;
        }

        /* Value is rest of line */
        const char *val = p;

        if (str_eq(key, "address") || str_eq(key, "adress")) {
            if (out_conf->has_address && verbose_warnings) {
                char w[256];
                size_t pos = 0;
                if (tool_name) {
                    nc_append_str(w, sizeof(w), &pos, tool_name);
                    nc_append_str(w, sizeof(w), &pos, ": ");
                }
                nc_append_str(w, sizeof(w), &pos, source_name);
                nc_append_str(w, sizeof(w), &pos, ":");
                nc_append_u32(w, sizeof(w), &pos, line_num);
                nc_append_str(w, sizeof(w), &pos, ": duplicate key \"address\" (overwriting)\n");
                nc_write_stderr(w);
            }
            if (netconf_parse_cidr(val, &out_conf->address, &out_conf->prefix, &out_conf->netmask) != 0) {
                if (err_msg && err_cap > 0) {
                    size_t pos = 0;
                    nc_append_str(err_msg, err_cap, &pos, source_name);
                    nc_append_str(err_msg, err_cap, &pos, ":");
                    nc_append_u32(err_msg, err_cap, &pos, line_num);
                    nc_append_str(err_msg, err_cap, &pos, ": invalid address \"");
                    nc_append_str(err_msg, err_cap, &pos, val);
                    nc_append_str(err_msg, err_cap, &pos, "\"");
                }
                return -1;
            }
            out_conf->has_address = true;
        } else if (str_eq(key, "gateway")) {
            if (out_conf->has_gateway && verbose_warnings) {
                char w[256];
                size_t pos = 0;
                if (tool_name) {
                    nc_append_str(w, sizeof(w), &pos, tool_name);
                    nc_append_str(w, sizeof(w), &pos, ": ");
                }
                nc_append_str(w, sizeof(w), &pos, source_name);
                nc_append_str(w, sizeof(w), &pos, ":");
                nc_append_u32(w, sizeof(w), &pos, line_num);
                nc_append_str(w, sizeof(w), &pos, ": duplicate key \"gateway\" (overwriting)\n");
                nc_write_stderr(w);
            }
            if (val[0] == '\0') {
                out_conf->gateway = 0;
            } else if (netconf_parse_ipv4(val, &out_conf->gateway) != 0) {
                if (err_msg && err_cap > 0) {
                    size_t pos = 0;
                    nc_append_str(err_msg, err_cap, &pos, source_name);
                    nc_append_str(err_msg, err_cap, &pos, ":");
                    nc_append_u32(err_msg, err_cap, &pos, line_num);
                    nc_append_str(err_msg, err_cap, &pos, ": invalid gateway \"");
                    nc_append_str(err_msg, err_cap, &pos, val);
                    nc_append_str(err_msg, err_cap, &pos, "\"");
                }
                return -1;
            }
            out_conf->has_gateway = true;
        } else if (str_eq(key, "dns")) {
            if (out_conf->has_dns && verbose_warnings) {
                char w[256];
                size_t pos = 0;
                if (tool_name) {
                    nc_append_str(w, sizeof(w), &pos, tool_name);
                    nc_append_str(w, sizeof(w), &pos, ": ");
                }
                nc_append_str(w, sizeof(w), &pos, source_name);
                nc_append_str(w, sizeof(w), &pos, ":");
                nc_append_u32(w, sizeof(w), &pos, line_num);
                nc_append_str(w, sizeof(w), &pos, ": duplicate key \"dns\" (overwriting)\n");
                nc_write_stderr(w);
            }
            if (val[0] == '\0') {
                out_conf->dns = 0;
            } else if (netconf_parse_ipv4(val, &out_conf->dns) != 0) {
                if (err_msg && err_cap > 0) {
                    size_t pos = 0;
                    nc_append_str(err_msg, err_cap, &pos, source_name);
                    nc_append_str(err_msg, err_cap, &pos, ":");
                    nc_append_u32(err_msg, err_cap, &pos, line_num);
                    nc_append_str(err_msg, err_cap, &pos, ": invalid dns \"");
                    nc_append_str(err_msg, err_cap, &pos, val);
                    nc_append_str(err_msg, err_cap, &pos, "\"");
                }
                return -1;
            }
            out_conf->has_dns = true;
        } else {
            if (verbose_warnings) {
                char w[256];
                size_t pos = 0;
                if (tool_name) {
                    nc_append_str(w, sizeof(w), &pos, tool_name);
                    nc_append_str(w, sizeof(w), &pos, ": ");
                }
                nc_append_str(w, sizeof(w), &pos, source_name);
                nc_append_str(w, sizeof(w), &pos, ":");
                nc_append_u32(w, sizeof(w), &pos, line_num);
                nc_append_str(w, sizeof(w), &pos, ": unknown key \"");
                nc_append_str(w, sizeof(w), &pos, key);
                nc_append_str(w, sizeof(w), &pos, "\" (ignored)\n");
                nc_write_stderr(w);
            }
        }
    }
    return 0;
}

static char s_file_buf[NETCONF_MAX_FILE_SIZE + 2];

int netconf_parse_file(const char *path, const char *tool_name, bool verbose_warnings,
                       netconf_t *out_conf, char *err_msg, size_t err_cap) {
    if (!path || !out_conf) return -1;
    if (err_msg && err_cap > 0) err_msg[0] = '\0';

    long fd = NETCONF_SYSCALL(SYS_OPEN, (uintptr_t)path, 0, 0);
    if (fd < 0) {
        if (err_msg && err_cap > 0) {
            size_t pos = 0;
            nc_append_str(err_msg, err_cap, &pos, "cannot open file \"");
            nc_append_str(err_msg, err_cap, &pos, path);
            nc_append_str(err_msg, err_cap, &pos, "\"");
        }
        return -1;
    }

    size_t total_read = 0;
    while (total_read < sizeof(s_file_buf)) {
        long n = NETCONF_SYSCALL(SYS_READ, (uintptr_t)fd, (uintptr_t)(s_file_buf + total_read), sizeof(s_file_buf) - total_read);
        if (n < 0) {
            NETCONF_SYSCALL(SYS_CLOSE, (uintptr_t)fd, 0, 0);
            if (err_msg && err_cap > 0) {
                size_t pos = 0;
                nc_append_str(err_msg, err_cap, &pos, "read error on file \"");
                nc_append_str(err_msg, err_cap, &pos, path);
                nc_append_str(err_msg, err_cap, &pos, "\"");
            }
            return -1;
        }
        if (n == 0) break;
        total_read += (size_t)n;
    }
    NETCONF_SYSCALL(SYS_CLOSE, (uintptr_t)fd, 0, 0);

    if (total_read > NETCONF_MAX_FILE_SIZE) {
        if (err_msg && err_cap > 0) {
            size_t pos = 0;
            nc_append_str(err_msg, err_cap, &pos, path);
            nc_append_str(err_msg, err_cap, &pos, ": file exceeds 4096 bytes");
        }
        return -1;
    }

    return netconf_parse_buffer(s_file_buf, total_read, path, tool_name, verbose_warnings,
                                out_conf, err_msg, err_cap);
}

int netconf_read_dns(const char *path, uint32_t *out_dns) {
    if (!out_dns) return -1;
    const char *p = path ? path : NETCONF_DEFAULT_PATH;
    netconf_t conf;
    int rc = netconf_parse_file(p, NULL, false, &conf, NULL, 0);
    if (rc != 0 || !conf.has_dns || conf.dns == 0) {
        return -1;
    }
    *out_dns = conf.dns;
    return 0;
}
