/* BOOTP / DHCP Message Layout (RFC 2131 §2):
 *
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |     op (1)    |   htype (1)   |   hlen (1)    |   hops (1)    |
 * +---------------+---------------+---------------+---------------+
 * |                            xid (4)                            |
 * +---------------+---------------+---------------+---------------+
 * |           secs (2)            |           flags (2)           |
 * +---------------+---------------+---------------+---------------+
 * |                          ciaddr  (4)                          |
 * +---------------+---------------+---------------+---------------+
 * |                          yiaddr  (4)                          |
 * +---------------+---------------+---------------+---------------+
 * |                          siaddr  (4)                          |
 * +---------------+---------------+---------------+---------------+
 * |                          giaddr  (4)                          |
 * +---------------+---------------+---------------+---------------+
 * |                                                               |
 * |                          chaddr  (16)                         |
 * |                                                               |
 * |                                                               |
 * +---------------+---------------+---------------+---------------+
 * |                                                               |
 * |                          sname   (64)                         |
 * +---------------+---------------+---------------+---------------+
 * |                                                               |
 * |                          file    (128)                        |
 * +---------------+---------------+---------------+---------------+
 * |                    magic cookie (4) = 0x63825363              |
 * +---------------+---------------+---------------+---------------+
 * |                          options (variable) ...               |
 * +---------------+---------------+---------------+---------------+
 *
 * Total fixed BOOTP header size: 236 bytes.
 * Magic cookie: 4 bytes (offset 236..239).
 * Options start at offset 240.
 */

#include "types.h"
#include "syscall_abi.h"
#include "socket_abi.h"
#include "netctl_abi.h"
#include "vfs.h"
#include "dhcp_codec.h"
#include "netconf.h"

static long sys_call(long nr, uintptr_t a, uintptr_t b, uintptr_t c,
                     uintptr_t d, uintptr_t e, uintptr_t f) {
    register uintptr_t r10 __asm__("r10") = d;
    register uintptr_t r8  __asm__("r8")  = e;
    register uintptr_t r9  __asm__("r9")  = f;
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c),
                     "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory", "cc");
    return nr;
}

#define RESOLV_SYSCALL(nr, a, b, c) sys_call(nr, a, b, c, 0, 0, 0)
#include "resolv_conf.h"

static size_t str_len(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static bool str_equals(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void print_out(const char *s) {
    size_t n = str_len(s);
    size_t off = 0;
    while (off < n) {
        long w = sys_call(SYS_WRITE, 1, (uintptr_t)(s + off), n - off, 0, 0, 0);
        if (w <= 0) break;
        off += (size_t)w;
    }
}

static void print_err(const char *s) {
    size_t n = str_len(s);
    size_t off = 0;
    while (off < n) {
        long w = sys_call(SYS_WRITE, 2, (uintptr_t)(s + off), n - off, 0, 0, 0);
        if (w <= 0) break;
        off += (size_t)w;
    }
}

static void append_str(char *buf, size_t cap, size_t *pos, const char *s) {
    while (*s && *pos + 1 < cap) {
        buf[(*pos)++] = *s++;
    }
    buf[*pos] = '\0';
}

static void append_u32(char *buf, size_t cap, size_t *pos, uint32_t val) {
    char tmp[12];
    size_t i = 0;
    if (val == 0) {
        tmp[i++] = '0';
    } else {
        while (val > 0) {
            tmp[i++] = (char)('0' + (val % 10));
            val /= 10;
        }
    }
    while (i > 0 && *pos + 1 < cap) {
        buf[(*pos)++] = tmp[--i];
    }
    buf[*pos] = '\0';
}

static void append_ip(char *buf, size_t cap, size_t *pos, uint32_t ip_net_order) {
    const uint8_t *b = (const uint8_t *)&ip_net_order;
    for (int i = 0; i < 4; i++) {
        if (i > 0) append_str(buf, cap, pos, ".");
        append_u32(buf, cap, pos, b[i]);
    }
}

static inline uint16_t dhcpc_htons(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}

static inline uint32_t dhcpc_htonl(uint32_t v) {
    return ((v & 0x000000FFu) << 24) |
           ((v & 0x0000FF00u) << 8)  |
           ((v & 0x00FF0000u) >> 8)  |
           ((v & 0xFF000000u) >> 24);
}

static bool write_file(const char *path, const char *data, size_t len) {
    long fd = sys_call(SYS_OPEN, (uintptr_t)path,
                       VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC | VFS_O_CLOEXEC,
                       0644, 0, 0, 0);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < len) {
        long n = sys_call(SYS_WRITE, (uintptr_t)fd, (uintptr_t)(data + off), len - off, 0, 0, 0);
        if (n <= 0) break;
        off += (size_t)n;
    }
    sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
    return off == len;
}

static bool save_resolv_conf(const uint32_t *servers, int count) {
    if (count <= 0) return true;
    char buf[128];
    size_t pos = 0;
    for (int i = 0; i < count; i++) {
        append_str(buf, sizeof(buf), &pos, "nameserver ");
        append_ip(buf, sizeof(buf), &pos, servers[i]);
        append_str(buf, sizeof(buf), &pos, "\n");
    }
    const char *target = resolv_conf_path();
    return write_file(target, buf, pos);
}

static bool save_network_conf(uint32_t ip, uint8_t prefix, uint32_t gateway,
                              const uint32_t *servers, int count) {
    char buf[256];
    size_t pos = 0;
    append_str(buf, sizeof(buf), &pos, "address ");
    append_ip(buf, sizeof(buf), &pos, ip);
    append_str(buf, sizeof(buf), &pos, "/");
    append_u32(buf, sizeof(buf), &pos, prefix);
    append_str(buf, sizeof(buf), &pos, "\ngateway ");
    if (gateway) append_ip(buf, sizeof(buf), &pos, gateway);
    append_str(buf, sizeof(buf), &pos, "\n");
    for (int i = 0; i < count; i++) {
        append_str(buf, sizeof(buf), &pos, i == 0 ? "dns " : "dns2 ");
        append_ip(buf, sizeof(buf), &pos, servers[i]);
        append_str(buf, sizeof(buf), &pos, "\n");
    }

    /* Ensure /etc/network directory exists on writable filesystems */
    sys_call(SYS_MKDIR, (uintptr_t)"/etc/network", 0755, 0, 0, 0, 0);
    write_file(NETCONF_INSTALLED_PATH, buf, pos);

    /* Always write to /tmp/network.conf so live media has the active config */
    write_file("/tmp/network.conf", buf, pos);

    /* Also write to /mnt/.fortress/network.conf if live persistence directory exists */
    write_file(NETCONF_DEFAULT_PATH, buf, pos);
    return true;
}

static void print_usage(void) {
    print_err("usage: dhcpc [--dry-run] [--help]\n"
              "Acquires a DHCP IPv4 lease and configures the network interface.\n"
              "\n"
              "Flags:\n"
              "  --dry-run   Perform DHCP transaction without configuring interface or writing files\n"
              "  --help, -h  Show this help message\n"
              "\n"
              "Limitation (v1):\n"
              "  One-shot client using bounded busy-wait; no background renewal daemon.\n"
              "  The lease lasts until reboot. Requires sudo/root privileges.\n");
}

int dhcpc_main(int argc, char **argv) {
    bool dry_run = false;
    for (int i = 1; i < argc; i++) {
        if (str_equals(argv[i], "--help") || str_equals(argv[i], "-h")) {
            print_usage();
            return 0;
        } else if (str_equals(argv[i], "--dry-run")) {
            dry_run = true;
        } else {
            print_err("dhcpc: unrecognized option: ");
            print_err(argv[i]);
            print_err("\n");
            print_usage();
            return 1;
        }
    }

    /* 1. Query network device and MAC address */
    netctl_ifget_t ifget;
    for (size_t i = 0; i < sizeof(ifget); i++) ((uint8_t *)&ifget)[i] = 0;
    ifget.struct_version = 1;
    ifget.reserved = 0;

    long ret = sys_call(SYS_NETCTL, NETCTL_IFGET, (uintptr_t)&ifget, sizeof(ifget), 0, 0, 0);
    if (ret != 0) {
        print_err("dhcpc: network interface unavailable\n");
        return 1;
    }
    if (ifget.link_state == NET_IF_LINK_WAITING) {
        print_err("dhcpc: link WAITING (no cable connected)\n");
        return 1;
    } else if (ifget.link_state == NET_IF_LINK_FAILED) {
        print_err("dhcpc: link FAILED (controller fault)\n");
        return 1;
    } else if (ifget.link_state == NET_IF_LINK_DOWN) {
        print_err("dhcpc: link DOWN\n");
        return 1;
    }

    /* 2. Open UDP socket */
    long fd = sys_call(SYS_SOCKET, NET_AF_INET, NET_SOCK_DGRAM | NET_SOCK_CLOEXEC, 0, 0, 0, 0);
    if (fd < 0) {
        print_err("dhcpc: failed to create UDP socket\n");
        return 1;
    }

    /* 3. Bind to 0.0.0.0:68 */
    net_sockaddr_in_t local_addr;
    for (size_t i = 0; i < sizeof(local_addr); i++) ((uint8_t *)&local_addr)[i] = 0;
    local_addr.family = NET_AF_INET;
    local_addr.port = dhcpc_htons(DHCP_PORT_CLIENT);
    local_addr.address = 0;

    ret = sys_call(SYS_BIND, (uintptr_t)fd, (uintptr_t)&local_addr, sizeof(local_addr), 0, 0, 0);
    if (ret != 0) {
        if (ret == SYSCALL_EPERM || ret == SYSCALL_EACCES) {
            print_err("dhcpc: permission denied; use sudo dhcpc\n");
        } else {
            print_err("dhcpc: failed to bind port 68\n");
        }
        sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
        return 1;
    }

    /* 4. Prepare broadcast destination 255.255.255.255:67 */
    net_sockaddr_in_t bcast_addr;
    for (size_t i = 0; i < sizeof(bcast_addr); i++) ((uint8_t *)&bcast_addr)[i] = 0;
    bcast_addr.family = NET_AF_INET;
    bcast_addr.port = dhcpc_htons(DHCP_PORT_SERVER);
    bcast_addr.address = 0xffffffffu;

    /* Generate transaction ID */
    sysinfo_t info;
    sys_call(SYS_SYSINFO, (uintptr_t)&info, 0, 0, 0, 0, 0);
    uint32_t xid = (uint32_t)(info.uptime_ticks ^ 0x44484350);
    if (!xid) xid = 0x12345678;

    /* 5. Send DHCPDISCOVER */
    uint8_t tx_buf[DHCP_MAX_PACKET_LEN];
    size_t tx_len = 0;
    if (dhcp_encode_discover(tx_buf, sizeof(tx_buf), xid, ifget.mac, &tx_len) != 0) {
        print_err("dhcpc: failed to encode DHCPDISCOVER\n");
        sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
        return 1;
    }

    ret = sys_call(SYS_SENDTO, (uintptr_t)fd, (uintptr_t)tx_buf, tx_len, 0,
                   (uintptr_t)&bcast_addr, sizeof(bcast_addr));
    if (ret < 0) {
        print_err("dhcpc: failed to transmit DHCPDISCOVER\n");
        sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
        return 1;
    }

    /* 6. Wait for DHCPOFFER (5 seconds bounded busy-wait) */
    uint64_t hz = info.tick_hz ? info.tick_hz : 100;
    uint64_t deadline = info.uptime_ticks + 5 * hz;

    uint8_t rx_buf[DHCP_MAX_PACKET_LEN];
    net_sockaddr_in_t src_addr;
    uint32_t src_len = sizeof(src_addr);
    dhcp_offer_t offer;
    bool got_offer = false;

    while (1) {
        for (size_t i = 0; i < sizeof(src_addr); i++) ((uint8_t *)&src_addr)[i] = 0;
        src_len = sizeof(src_addr);
        long n = sys_call(SYS_RECVFROM, (uintptr_t)fd, (uintptr_t)rx_buf, sizeof(rx_buf),
                          NET_MSG_DONTWAIT, (uintptr_t)&src_addr, (uintptr_t)&src_len);
        if (n > 0) {
            if (dhcp_decode_reply(rx_buf, (size_t)n, xid, ifget.mac, &offer) == 0 &&
                offer.msg_type == DHCP_MSG_OFFER) {
                got_offer = true;
                break;
            }
        }

        sys_call(SYS_SYSINFO, (uintptr_t)&info, 0, 0, 0, 0, 0);
        if (info.uptime_ticks >= deadline) break;
    }

    if (!got_offer) {
        print_err("dhcpc: no DHCP offer received (timeout)\n");
        sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
        return 1;
    }

    /* 7. Send DHCPREQUEST */
    tx_len = 0;
    if (dhcp_encode_request(tx_buf, sizeof(tx_buf), xid, ifget.mac,
                            offer.yiaddr, offer.server_ip, &tx_len) != 0) {
        print_err("dhcpc: failed to encode DHCPREQUEST\n");
        sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
        return 1;
    }

    ret = sys_call(SYS_SENDTO, (uintptr_t)fd, (uintptr_t)tx_buf, tx_len, 0,
                   (uintptr_t)&bcast_addr, sizeof(bcast_addr));
    if (ret < 0) {
        print_err("dhcpc: failed to transmit DHCPREQUEST\n");
        sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
        return 1;
    }

    /* 8. Wait for DHCPACK (5 seconds bounded busy-wait) */
    sys_call(SYS_SYSINFO, (uintptr_t)&info, 0, 0, 0, 0, 0);
    deadline = info.uptime_ticks + 5 * hz;
    dhcp_offer_t ack;
    bool got_ack = false;

    while (1) {
        for (size_t i = 0; i < sizeof(src_addr); i++) ((uint8_t *)&src_addr)[i] = 0;
        src_len = sizeof(src_addr);
        long n = sys_call(SYS_RECVFROM, (uintptr_t)fd, (uintptr_t)rx_buf, sizeof(rx_buf),
                          NET_MSG_DONTWAIT, (uintptr_t)&src_addr, (uintptr_t)&src_len);
        if (n > 0) {
            if (dhcp_decode_reply(rx_buf, (size_t)n, xid, ifget.mac, &ack) == 0) {
                if (ack.msg_type == DHCP_MSG_ACK) {
                    got_ack = true;
                    /* Update offer parameters with any updated fields from ACK */
                    if (ack.yiaddr) offer.yiaddr = ack.yiaddr;
                    if (ack.subnet_mask) {
                        offer.subnet_mask = ack.subnet_mask;
                        offer.prefix = ack.prefix;
                    }
                    if (ack.router) offer.router = ack.router;
                    if (ack.dns_count > 0) {
                        for (int i = 0; i < ack.dns_count; i++) offer.dns_servers[i] = ack.dns_servers[i];
                        offer.dns_count = ack.dns_count;
                    }
                    if (ack.lease_time) offer.lease_time = ack.lease_time;
                    break;
                } else if (ack.msg_type == DHCP_MSG_NAK) {
                    print_err("dhcpc: received DHCPNAK from server\n");
                    sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
                    return 1;
                }
            }
        }

        sys_call(SYS_SYSINFO, (uintptr_t)&info, 0, 0, 0, 0, 0);
        if (info.uptime_ticks >= deadline) break;
    }

    sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);

    if (!got_ack) {
        print_err("dhcpc: no DHCPACK received (timeout)\n");
        return 1;
    }

    /* Fallback netmask if not provided */
    if (!offer.subnet_mask) {
        offer.prefix = 24;
        offer.subnet_mask = dhcpc_htonl(0xffffff00u);
    }

    /* 9. Handle dry-run or apply */
    char msg[256];
    size_t pos = 0;
    if (dry_run) {
        append_str(msg, sizeof(msg), &pos, "eth0: address ");
        append_ip(msg, sizeof(msg), &pos, offer.yiaddr);
        append_str(msg, sizeof(msg), &pos, "/");
        append_u32(msg, sizeof(msg), &pos, offer.prefix);
        if (offer.router) {
            append_str(msg, sizeof(msg), &pos, " gateway ");
            append_ip(msg, sizeof(msg), &pos, offer.router);
        }
        append_str(msg, sizeof(msg), &pos, " valid (dry-run)\n");
        print_out(msg);
        return 0;
    }

    /* 10. Apply configuration via SYS_NETCTL NETCTL_IFSET */
    netctl_ifset_t req;
    for (size_t i = 0; i < sizeof(req); i++) ((uint8_t *)&req)[i] = 0;
    req.struct_version = 1;
    req.local_ipv4 = offer.yiaddr;
    req.netmask_ipv4 = offer.subnet_mask;
    req.gateway_ipv4 = offer.router;

    ret = sys_call(SYS_NETCTL, NETCTL_IFSET, (uintptr_t)&req, sizeof(req), 0, 0, 0);
    if (ret != 0) {
        print_err("dhcpc: failed to apply interface configuration (SYS_NETCTL NETCTL_IFSET)\n");
        return 1;
    }

    /* 11. Write resolv.conf and network.conf */
    save_resolv_conf(offer.dns_servers, offer.dns_count);
    save_network_conf(offer.yiaddr, offer.prefix, offer.router,
                      offer.dns_servers, offer.dns_count);

    /* Sync filesystem */
    sys_call(SYS_SYNC, 0, 0, 0, 0, 0, 0);

    /* 12. Print success status */
    pos = 0;
    append_str(msg, sizeof(msg), &pos, "eth0: acquired lease ");
    append_ip(msg, sizeof(msg), &pos, offer.yiaddr);
    append_str(msg, sizeof(msg), &pos, "/");
    append_u32(msg, sizeof(msg), &pos, offer.prefix);
    if (offer.router) {
        append_str(msg, sizeof(msg), &pos, " gateway ");
        append_ip(msg, sizeof(msg), &pos, offer.router);
    }
    append_str(msg, sizeof(msg), &pos, "\n");
    print_out(msg);

    return 0;
}
