#include "types.h"
#include "syscall_abi.h"
#include "netctl_abi.h"

static netctl_ifget_t s_req;
static char s_buf[512];

#ifndef IFCONFIG_SYSCALL
static long sys_call(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory", "cc");
    return nr;
}
#define IFCONFIG_SYSCALL sys_call
#endif
#include "resolv_conf.h"

static bool write_all(int fd, const char *buf, size_t count) {
    while (count > 0) {
        long w = IFCONFIG_SYSCALL(SYS_WRITE, (uintptr_t)fd, (uintptr_t)buf, count);
        if (w <= 0) return false;
        buf += w;
        count -= (size_t)w;
    }
    return true;
}

static void append_str(char *buf, size_t *pos, const char *s) {
    while (*s && *pos < sizeof(s_buf) - 1) {
        buf[(*pos)++] = *s++;
    }
}

static void append_u64(char *buf, size_t *pos, uint64_t val) {
    char tmp[24];
    size_t i = 0;
    if (val == 0) {
        tmp[i++] = '0';
    } else {
        while (val > 0) {
            tmp[i++] = (char)('0' + (val % 10));
            val /= 10;
        }
    }
    while (i > 0 && *pos < sizeof(s_buf) - 1) {
        buf[(*pos)++] = tmp[--i];
    }
}

static void append_hex_byte(char *buf, size_t *pos, uint8_t byte) {
    const char hex[] = "0123456789abcdef";
    if (*pos < sizeof(s_buf) - 2) {
        buf[(*pos)++] = hex[(byte >> 4) & 0x0f];
        buf[(*pos)++] = hex[byte & 0x0f];
    }
}

static void append_mac(char *buf, size_t *pos, const uint8_t *mac) {
    for (int i = 0; i < 6; ++i) {
        if (i > 0 && *pos < sizeof(s_buf) - 1) buf[(*pos)++] = ':';
        append_hex_byte(buf, pos, mac[i]);
    }
}

static void append_ip(char *buf, size_t *pos, uint32_t ip) {
    uint32_t h = __builtin_bswap32(ip);
    for (int i = 3; i >= 0; --i) {
        if (i < 3 && *pos < sizeof(s_buf) - 1) buf[(*pos)++] = '.';
        append_u64(buf, pos, (h >> (i * 8)) & 0xff);
    }
}

static unsigned count_prefix(uint32_t mask_net) {
    uint32_t m = __builtin_bswap32(mask_net);
    unsigned count = 0;
    while (m & 0x80000000u) {
        count++;
        m <<= 1;
    }
    return count;
}

static bool parse_ipv4(const char *s, size_t len, uint32_t *out) {
    uint32_t val = 0;
    size_t i = 0;
    for (int oct = 0; oct < 4; ++oct) {
        if (i >= len || s[i] < '0' || s[i] > '9') return false;
        unsigned num = 0;
        int digits = 0;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            num = num * 10 + (unsigned)(s[i++] - '0');
            if (++digits > 3 || num > 255) return false;
        }
        val = (val << 8) | (num & 0xff);
        if (oct < 3) {
            if (i >= len || s[i++] != '.') return false;
        }
    }
    if (i != len) return false;
    *out = __builtin_bswap32(val);
    return true;
}

static bool is_unicast(uint32_t ip) {
    uint32_t h = __builtin_bswap32(ip);
    uint32_t b0 = (h >> 24) & 0xff;
    return b0 != 0 && b0 != 127 && b0 < 224 && ip != 0xffffffffu;
}

static unsigned ifconfig_load_dns(uint32_t *servers, unsigned max_servers) {
    const char *path = resolv_conf_path();
    long fd = IFCONFIG_SYSCALL(SYS_OPEN, (uintptr_t)path, 0, 0);
    if (fd < 0) return 0;
    static char buf[512];
    long n = IFCONFIG_SYSCALL(SYS_READ, (uintptr_t)fd, (uintptr_t)buf, sizeof(buf) - 1);
    IFCONFIG_SYSCALL(SYS_CLOSE, (uintptr_t)fd, 0, 0);
    if (n <= 0) return 0;
    buf[n] = '\0';

    unsigned count = 0;
    size_t at = 0;
    while (at < (size_t)n && count < max_servers) {
        while (at < (size_t)n && (buf[at] == ' ' || buf[at] == '\t' || buf[at] == '\r' || buf[at] == '\n')) at++;
        if (at >= (size_t)n) break;
        if (buf[at] == '#' || buf[at] == ';') {
            while (at < (size_t)n && buf[at] != '\n') at++;
            continue;
        }
        const char *line = buf + at;
        size_t line_len = 0;
        while (at + line_len < (size_t)n && line[line_len] != '\r' && line[line_len] != '\n') line_len++;
        at += line_len;

        if (line_len > 11 &&
            line[0] == 'n' && line[1] == 'a' && line[2] == 'm' && line[3] == 'e' &&
            line[4] == 's' && line[5] == 'e' && line[6] == 'r' && line[7] == 'v' &&
            line[8] == 'e' && line[9] == 'r' && (line[10] == ' ' || line[10] == '\t')) {
            size_t p = 11;
            while (p < line_len && (line[p] == ' ' || line[p] == '\t')) p++;
            size_t ip_start = p;
            while (p < line_len && line[p] != ' ' && line[p] != '\t') p++;
            size_t ip_len = p - ip_start;
            uint32_t ip = 0;
            if (ip_len > 0 && parse_ipv4(line + ip_start, ip_len, &ip) && is_unicast(ip)) {
                servers[count++] = ip;
            }
        }
    }
    return count;
}

int ifconfig_main(int argc, char **argv) {
    (void)argc; (void)argv;
    s_req.struct_version = 1;
    s_req.reserved = 0;

    long ret = IFCONFIG_SYSCALL(SYS_NETCTL, NETCTL_IFGET, (uintptr_t)&s_req, sizeof(s_req));
    if (ret != 0) {
        const char err[] = "ifconfig: network interface unavailable\n";
        write_all(2, err, sizeof(err) - 1);
        return 1;
    }

    size_t pos = 0;
    append_str(s_buf, &pos, "eth0  HWaddr ");
    append_mac(s_buf, &pos, s_req.mac);
    append_str(s_buf, &pos, "\n");

    if (s_req.link_state == NET_IF_LINK_WAITING) {
        append_str(s_buf, &pos, "      link WAITING (no cable)\n");
    } else if (s_req.link_state == NET_IF_LINK_FAILED) {
        append_str(s_buf, &pos, "      link FAILED (controller fault; quarantine active)\n");
    } else if (s_req.link_state == NET_IF_LINK_DOWN) {
        append_str(s_buf, &pos, "      link DOWN\n");
    } else if (s_req.link_state == NET_IF_LINK_UNINITIALIZED) {
        append_str(s_buf, &pos, "      link UNINITIALIZED\n");
    } else {
        /* NET_IF_LINK_ONLINE */
        append_str(s_buf, &pos, "      inet ");
        append_ip(s_buf, &pos, s_req.local_ipv4);
        append_str(s_buf, &pos, "/");
        append_u64(s_buf, &pos, count_prefix(s_req.netmask_ipv4));
        append_str(s_buf, &pos, "  netmask ");
        append_ip(s_buf, &pos, s_req.netmask_ipv4);
        append_str(s_buf, &pos, "  broadcast ");
        uint32_t bcast = s_req.local_ipv4 | ~s_req.netmask_ipv4;
        append_ip(s_buf, &pos, bcast);
        append_str(s_buf, &pos, "\n");

        if (s_req.gateway_ipv4 != 0) {
            append_str(s_buf, &pos, "      gateway ");
            append_ip(s_buf, &pos, s_req.gateway_ipv4);
            append_str(s_buf, &pos, "\n");
        }

        uint32_t dns_srv[4];
        unsigned dns_count = ifconfig_load_dns(dns_srv, 4);
        if (dns_count > 0) {
            append_str(s_buf, &pos, "      dns ");
            for (unsigned i = 0; i < dns_count; ++i) {
                if (i > 0) append_str(s_buf, &pos, ", ");
                append_ip(s_buf, &pos, dns_srv[i]);
            }
            append_str(s_buf, &pos, "\n");
        }

        append_str(s_buf, &pos, "      mtu ");
        append_u64(s_buf, &pos, s_req.mtu);
        append_str(s_buf, &pos, "\n      link UP\n      RX ");
        append_u64(s_buf, &pos, s_req.rx_packets);
        append_str(s_buf, &pos, "  TX ");
        append_u64(s_buf, &pos, s_req.tx_packets);
        append_str(s_buf, &pos, "\n");
    }

    write_all(1, s_buf, pos);
    return 0;
}
