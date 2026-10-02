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

static void ifup_usage(void) {
    ifup_print_err("usage: ifup [--dry-run] [config-file]\n"
                   "       ifup [--dry-run] <ip>/<prefix> [gateway]\n"
                   "       ifup [--dry-run] <ip> <netmask> [gateway]\n");
}

int ifup_main(int argc, char **argv) {
    bool dry_run = false;
    const char *args[4];
    int arg_count = 0;

    for (int i = 1; i < argc; i++) {
        if (ifup_str_equals(argv[i], "--help") || ifup_str_equals(argv[i], "-h")) {
            ifup_usage();
            return 0;
        } else if (ifup_str_equals(argv[i], "--dry-run") || ifup_str_equals(argv[i], "-n")) {
            dry_run = true;
        } else {
            if (arg_count < 4) {
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

    if (arg_count == 0 || (arg_count == 1 && netconf_parse_cidr(args[0], &req.local_ipv4, &prefix, &req.netmask_ipv4) != 0 &&
                           !ifup_str_equals(args[0], "-"))) {
        /* Config file mode */
        const char *conf_path = (arg_count == 1) ? args[0] : NETCONF_DEFAULT_PATH;
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
    } else {
        /* CLI argument mode */
        if (arg_count == 1) {
            /* args[0] is CIDR */
            if (netconf_parse_cidr(args[0], &req.local_ipv4, &prefix, &req.netmask_ipv4) != 0) {
                ifup_print_err("ifup: invalid address/prefix\n");
                return 1;
            }
            req.gateway_ipv4 = 0;
        } else if (arg_count == 2) {
            if (netconf_parse_cidr(args[0], &req.local_ipv4, &prefix, &req.netmask_ipv4) == 0) {
                /* <cidr> <gateway> */
                if (netconf_parse_ipv4(args[1], &req.gateway_ipv4) != 0) {
                    ifup_print_err("ifup: invalid gateway\n");
                    return 1;
                }
            } else {
                /* <ip> <netmask> */
                if (netconf_parse_ipv4(args[0], &req.local_ipv4) != 0 ||
                    netconf_parse_netmask(args[1], &prefix, &req.netmask_ipv4) != 0) {
                    ifup_print_err("ifup: invalid ip or netmask\n");
                    return 1;
                }
                req.gateway_ipv4 = 0;
            }
        } else if (arg_count == 3) {
            /* <ip> <netmask> <gateway> */
            if (netconf_parse_ipv4(args[0], &req.local_ipv4) != 0 ||
                netconf_parse_netmask(args[1], &prefix, &req.netmask_ipv4) != 0 ||
                netconf_parse_ipv4(args[2], &req.gateway_ipv4) != 0) {
                ifup_print_err("ifup: invalid ip, netmask, or gateway\n");
                return 1;
            }
        } else {
            ifup_usage();
            return 1;
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
        } else {
            ifup_print_err("ifup: failed to apply interface configuration\n");
        }
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
