#include "types.h"
#include "syscall_abi.h"
#include "netctl_abi.h"
#include "netconf.h"

#ifndef IFUP_SYSCALL
static inline long ifup_default_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}
#define IFUP_SYSCALL ifup_default_syscall
#endif
#include "resolv_conf.h"
#include "machine_config.h"

static size_t ifup_strlen(const char *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static void ifup_print_out(const char *s) {
    size_t len = ifup_strlen(s);
    size_t off = 0;
    while (off < len) {
        long n = IFUP_SYSCALL(SYS_WRITE, 1, (uintptr_t)(s + off), len - off);
        if (n <= 0) break;
        off += (size_t)n;
    }
}

static void ifup_print_err(const char *s) {
    size_t len = ifup_strlen(s);
    size_t off = 0;
    while (off < len) {
        long n = IFUP_SYSCALL(SYS_WRITE, 2, (uintptr_t)(s + off), len - off);
        if (n <= 0) break;
        off += (size_t)n;
    }
}

static bool ifup_str_equals(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static void ifup_append_str(char *buf, size_t cap, size_t *pos, const char *s) {
    while (*s && *pos + 1 < cap) {
        buf[(*pos)++] = *s++;
    }
    if (*pos < cap) buf[*pos] = '\0';
}

static void ifup_append_u32(char *buf, size_t cap, size_t *pos, uint32_t val) {
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

static void ifup_append_ip(char *buf, size_t cap, size_t *pos, uint32_t be_ip) {
    const uint8_t *oct = (const uint8_t *)&be_ip;
    for (int i = 0; i < 4; i++) {
        ifup_append_u32(buf, cap, pos, oct[i]);
        if (i < 3) {
            if (*pos + 1 < cap) buf[(*pos)++] = '.';
        }
    }
    if (*pos < cap) buf[*pos] = '\0';
}

static bool ifup_write_to_file(const char *path, const char *buf, size_t pos) {
    char dir[32];
    size_t last_slash = 0;
    for (size_t i = 0; path[i] && i < sizeof(dir) - 1; i++) {
        dir[i] = path[i];
        if (path[i] == '/') last_slash = i;
    }
    if (last_slash > 0 && last_slash < sizeof(dir)) {
        dir[last_slash] = '\0';
        if (!ifup_str_equals(dir, "/tmp") && !ifup_str_equals(dir, "/etc")) {
            long rc = IFUP_SYSCALL(SYS_MKDIR, (uintptr_t)dir, 0755, 0);
            if (rc != 0 && rc != SYSCALL_EEXIST) return false;
        }
    }

    /* WRONLY | CREAT | TRUNC | CLOEXEC; checked short writes and close. */
    long fd = IFUP_SYSCALL(SYS_OPEN, (uintptr_t)path, 0x80241, 0644);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < pos) {
        long n = IFUP_SYSCALL(SYS_WRITE, (uintptr_t)fd, (uintptr_t)(buf + off), pos - off);
        if (n <= 0 || (size_t)n > pos - off) break;
        off += (size_t)n;
    }
    long closed = IFUP_SYSCALL(SYS_CLOSE, (uintptr_t)fd, 0, 0);
    return off == pos && closed == 0;
}

static bool ifup_write_resolv_conf(const uint32_t *servers, int count) {
    if (count <= 0) return true;
    char buf[128];
    size_t pos = 0;
    for (int i = 0; i < count; i++) {
        ifup_append_str(buf, sizeof(buf), &pos, "nameserver ");
        ifup_append_ip(buf, sizeof(buf), &pos, servers[i]);
        ifup_append_str(buf, sizeof(buf), &pos, "\n");
    }

    IFUP_SYSCALL(SYS_MKDIR, (uintptr_t)"/etc/network", 0755, 0);
    ifup_write_to_file("/etc/network/resolv.conf", buf, pos);
    ifup_write_to_file("/run/resolv.conf", buf, pos);
    const char *target = resolv_conf_path();
    if (target && !ifup_str_equals(target, "/etc/network/resolv.conf")) {
        ifup_write_to_file(target, buf, pos);
    }
    return true;
}

static bool ifup_save_config(const netctl_ifset_t *req, uint8_t prefix,
                             const uint32_t *servers, int count) {
    char buf[256];
    size_t pos = 0;
    ifup_append_str(buf, sizeof(buf), &pos, "address ");
    ifup_append_ip(buf, sizeof(buf), &pos, req->local_ipv4);
    ifup_append_str(buf, sizeof(buf), &pos, "/");
    ifup_append_u32(buf, sizeof(buf), &pos, prefix);
    ifup_append_str(buf, sizeof(buf), &pos, "\ngateway ");
    if (req->gateway_ipv4) ifup_append_ip(buf, sizeof(buf), &pos, req->gateway_ipv4);
    ifup_append_str(buf, sizeof(buf), &pos, "\n");
    for (int i = 0; i < count; i++) {
        ifup_append_str(buf, sizeof(buf), &pos, i == 0 ? "dns " : "dns2 ");
        ifup_append_ip(buf, sizeof(buf), &pos, servers[i]);
        ifup_append_str(buf, sizeof(buf), &pos, "\n");
    }
    return ifup_write_to_file(NETCONF_INSTALLED_PATH, buf, pos);
}

static void ifup_usage(void) {
    ifup_print_err("usage: ifup [--dry-run] [--no-save] [config-file]\n"
                   "       ifup [--dry-run] [--no-save] <ip>/<prefix> [gateway] [dns1] [dns2]\n"
                   "       ifup [--dry-run] [--no-save] <ip> <netmask> [gateway] [dns1] [dns2]\n"
                   "Installed systems save to /etc/network/network.conf; use sudo.\n");
}

int ifup_main(int argc, char **argv) {
    bool dry_run = false;
    bool no_save = false;
    const char *args[6];
    int arg_count = 0;

    for (int i = 1; i < argc; i++) {
        if (ifup_str_equals(argv[i], "--help") || ifup_str_equals(argv[i], "-h")) {
            ifup_usage();
            return 0;
        } else if (ifup_str_equals(argv[i], "--dry-run") || ifup_str_equals(argv[i], "-n")) {
            dry_run = true;
        } else if (ifup_str_equals(argv[i], "--no-save")) {
            no_save = true;
        } else {
            if (arg_count < 6) {
                args[arg_count++] = argv[i];
            } else {
                ifup_usage();
                return 1;
            }
        }
    }

    static netctl_ifset_t req;
    for (size_t i = 0; i < sizeof(req); i++) ((char *)&req)[i] = 0;
    req.struct_version = 1;

    uint8_t prefix = 0;
    uint32_t dns_servers[2] = {0, 0};
    int dns_count = 0;
    bool installed = machine_installed(IFUP_SYSCALL);

    if (arg_count == 0 || (arg_count == 1 && netconf_parse_cidr(args[0], &req.local_ipv4, &prefix, &req.netmask_ipv4) != 0 &&
                           !ifup_str_equals(args[0], "-"))) {
        /* Config file mode */
        const char *conf_path = (arg_count == 1) ? args[0] : installed ? NETCONF_INSTALLED_PATH : NETCONF_DEFAULT_PATH;
        static netconf_t conf;
        static char s_err_msg[256];
        s_err_msg[0] = '\0';

        if (netconf_parse_file(conf_path, "ifup", true, &conf, s_err_msg, sizeof(s_err_msg)) != 0) {
            static char s_out[300];
            size_t p = 0;
            ifup_append_str(s_out, sizeof(s_out), &p, "ifup: ");
            ifup_append_str(s_out, sizeof(s_out), &p, s_err_msg[0] ? s_err_msg : "error parsing configuration");
            ifup_append_str(s_out, sizeof(s_out), &p, "\n");
            ifup_print_err(s_out);
            return 1;
        }

        if (!conf.has_address) {
            static char s_out[300];
            size_t p = 0;
            ifup_append_str(s_out, sizeof(s_out), &p, "ifup: ");
            ifup_append_str(s_out, sizeof(s_out), &p, conf_path);
            ifup_append_str(s_out, sizeof(s_out), &p, ": missing required key \"address\"\n");
            ifup_print_err(s_out);
            return 1;
        }

        req.local_ipv4 = conf.address;
        req.netmask_ipv4 = conf.netmask;
        req.gateway_ipv4 = conf.gateway;
        prefix = conf.prefix;
        if (conf.has_dns && conf.dns) dns_servers[dns_count++] = conf.dns;
        if (conf.has_dns2 && conf.dns2) dns_servers[dns_count++] = conf.dns2;
    } else {
        /* CLI argument mode */
        if (netconf_parse_cidr(args[0], &req.local_ipv4, &prefix, &req.netmask_ipv4) == 0) {
            /* CIDR mode: ifup <cidr> [gateway] [dns1] [dns2] */
            if (arg_count == 1) {
                req.gateway_ipv4 = 0;
            } else if (arg_count >= 2 && arg_count <= 4) {
                if (netconf_parse_ipv4(args[1], &req.gateway_ipv4) != 0) {
                    ifup_print_err("ifup: invalid gateway\n");
                    return 1;
                }
                if (arg_count >= 3) {
                    if (netconf_parse_ipv4(args[2], &dns_servers[0]) != 0) {
                        ifup_print_err("ifup: invalid dns server\n");
                        return 1;
                    }
                    dns_count = 1;
                }
                if (arg_count == 4) {
                    if (netconf_parse_ipv4(args[3], &dns_servers[1]) != 0) {
                        ifup_print_err("ifup: invalid dns server\n");
                        return 1;
                    }
                    dns_count = 2;
                }
            } else {
                ifup_usage();
                return 1;
            }
        } else {
            /* Dotted decimal mode: ifup <ip> <netmask> [gateway] [dns1] [dns2] */
            if (arg_count == 2) {
                if (netconf_parse_ipv4(args[0], &req.local_ipv4) != 0 ||
                    netconf_parse_netmask(args[1], &prefix, &req.netmask_ipv4) != 0) {
                    ifup_print_err("ifup: invalid ip or netmask\n");
                    return 1;
                }
                req.gateway_ipv4 = 0;
            } else if (arg_count >= 3 && arg_count <= 5) {
                if (netconf_parse_ipv4(args[0], &req.local_ipv4) != 0 ||
                    netconf_parse_netmask(args[1], &prefix, &req.netmask_ipv4) != 0 ||
                    netconf_parse_ipv4(args[2], &req.gateway_ipv4) != 0) {
                    ifup_print_err("ifup: invalid ip, netmask, or gateway\n");
                    return 1;
                }
                if (arg_count >= 4) {
                    if (netconf_parse_ipv4(args[3], &dns_servers[0]) != 0) {
                        ifup_print_err("ifup: invalid dns server\n");
                        return 1;
                    }
                    dns_count = 1;
                }
                if (arg_count == 5) {
                    if (netconf_parse_ipv4(args[4], &dns_servers[1]) != 0) {
                        ifup_print_err("ifup: invalid dns server\n");
                        return 1;
                    }
                    dns_count = 2;
                }
            } else {
                ifup_usage();
                return 1;
            }
        }
    }

    if (dry_run) {
        static char s_msg[256];
        size_t p = 0;
        ifup_append_str(s_msg, sizeof(s_msg), &p, "eth0: address ");
        ifup_append_ip(s_msg, sizeof(s_msg), &p, req.local_ipv4);
        ifup_append_str(s_msg, sizeof(s_msg), &p, "/");
        ifup_append_u32(s_msg, sizeof(s_msg), &p, prefix);
        if (req.gateway_ipv4 != 0) {
            ifup_append_str(s_msg, sizeof(s_msg), &p, " gateway ");
            ifup_append_ip(s_msg, sizeof(s_msg), &p, req.gateway_ipv4);
        }
        ifup_append_str(s_msg, sizeof(s_msg), &p, " valid (dry-run)\n");
        ifup_print_out(s_msg);
        return 0;
    }

    long rc = IFUP_SYSCALL(SYS_NETCTL, NETCTL_IFSET, (uintptr_t)&req, sizeof(req));
    if (rc != 0) {
        if (rc == SYSCALL_EINVAL) {
            ifup_print_err("ifup: invalid configuration (rejected by kernel)\n");
        } else if (rc == SYSCALL_EIO) {
            ifup_print_err("ifup: network interface unavailable or not online\n");
        } else if (rc == SYSCALL_EOPNOTSUPP) {
            ifup_print_err("ifup: operation not supported on this CPU\n");
        } else if (rc == SYSCALL_EPERM || rc == SYSCALL_EACCES) {
            ifup_print_err("ifup: permission denied; use sudo ifup\n");
        } else {
            ifup_print_err("ifup: failed to apply interface configuration\n");
        }
        return 1;
    }

    if (!ifup_write_resolv_conf(dns_servers, dns_count)) {
        ifup_print_err("ifup: interface applied, but DNS configuration could not be saved\n");
        return 1;
    }
    if (installed && !no_save && (!ifup_save_config(&req, prefix, dns_servers, dns_count) ||
                       IFUP_SYSCALL(SYS_SYNC, 0, 0, 0) != 0)) {
        ifup_print_err("ifup: interface applied, but /etc/network/network.conf was not durably saved\n");
        return 1;
    }

    static char s_msg[256];
    size_t p = 0;
    ifup_append_str(s_msg, sizeof(s_msg), &p, "eth0: address ");
    ifup_append_ip(s_msg, sizeof(s_msg), &p, req.local_ipv4);
    ifup_append_str(s_msg, sizeof(s_msg), &p, "/");
    ifup_append_u32(s_msg, sizeof(s_msg), &p, prefix);
    if (req.gateway_ipv4 != 0) {
        ifup_append_str(s_msg, sizeof(s_msg), &p, " gateway ");
        ifup_append_ip(s_msg, sizeof(s_msg), &p, req.gateway_ipv4);
    }
    ifup_append_str(s_msg, sizeof(s_msg), &p, " applied\n");
    ifup_print_out(s_msg);
    return 0;
}
