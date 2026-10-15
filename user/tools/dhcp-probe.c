#include "types.h"
#include "syscall_abi.h"
#include "socket_abi.h"
#include "netctl_abi.h"
#include "../dhcp_codec.h"

static long sys_call(long nr, uintptr_t a, uintptr_t b, uintptr_t c,
                     uintptr_t d, uintptr_t e, uintptr_t f) {
    register uintptr_t r10 __asm__("r10") = d;
    register uintptr_t r8  __asm__("r8")  = e;
    register uintptr_t r9  __asm__("r9")  = f;
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c),
                     "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory", "cc");
    return nr;
}

static size_t str_len(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static void write_str(int fd, const char *s) {
    size_t n = str_len(s);
    size_t off = 0;
    while (off < n) {
        long w = sys_call(SYS_WRITE, (uintptr_t)fd, (uintptr_t)(s + off), n - off, 0, 0, 0);
        if (w <= 0) break;
        off += (size_t)w;
    }
}

static void print_u32(int fd, uint32_t val) {
    char buf[12];
    size_t i = 0;
    if (val == 0) {
        buf[i++] = '0';
    } else {
        while (val > 0) {
            buf[i++] = (char)('0' + (val % 10));
            val /= 10;
        }
    }
    char rev[12];
    for (size_t j = 0; j < i; j++) rev[j] = buf[i - 1 - j];
    rev[i] = 0;
    write_str(fd, rev);
}

static void print_ip(int fd, uint32_t ip_net_order) {
    const uint8_t *b = (const uint8_t *)&ip_net_order;
    for (int i = 0; i < 4; i++) {
        if (i > 0) write_str(fd, ".");
        print_u32(fd, b[i]);
    }
}

static inline uint16_t probe_htons(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}

static inline uint16_t probe_ntohs(uint16_t v) {
    return probe_htons(v);
}

int dhcp_probe_main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* 1. Get interface MAC address */
    netctl_ifget_t ifget;
    for (size_t i = 0; i < sizeof(ifget); i++) ((uint8_t *)&ifget)[i] = 0;
    ifget.struct_version = 1;
    ifget.reserved = 0;

    long ret = sys_call(SYS_NETCTL, NETCTL_IFGET, (uintptr_t)&ifget, sizeof(ifget), 0, 0, 0);
    if (ret != 0) {
        write_str(2, "dhcp-probe: failed to query network interface\n");
        return 1;
    }

    /* 2. Open UDP socket */
    long fd = sys_call(SYS_SOCKET, NET_AF_INET, NET_SOCK_DGRAM | NET_SOCK_CLOEXEC, 0, 0, 0, 0);
    if (fd < 0) {
        write_str(2, "dhcp-probe: failed to create UDP socket\n");
        return 1;
    }

    /* 3. Bind to 0.0.0.0:68 */
    net_sockaddr_in_t local_addr;
    for (size_t i = 0; i < sizeof(local_addr); i++) ((uint8_t *)&local_addr)[i] = 0;
    local_addr.family = NET_AF_INET;
    local_addr.port = probe_htons(DHCP_PORT_CLIENT);
    local_addr.address = 0;

    ret = sys_call(SYS_BIND, (uintptr_t)fd, (uintptr_t)&local_addr, sizeof(local_addr), 0, 0, 0);
    if (ret != 0) {
        write_str(2, "dhcp-probe: failed to bind 0.0.0.0:68\n");
        sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
        return 1;
    }

    /* 4. Encode DHCPDISCOVER */
    uint8_t tx_buf[DHCP_MAX_PACKET_LEN];
    size_t tx_len = 0;
    uint32_t xid = 0x39444850; /* '9DHP' */
    if (dhcp_encode_discover(tx_buf, sizeof(tx_buf), xid, ifget.mac, &tx_len) != 0) {
        write_str(2, "dhcp-probe: failed to encode DHCPDISCOVER\n");
        sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
        return 1;
    }

    /* 5. Sendto 255.255.255.255:67 */
    net_sockaddr_in_t bcast_addr;
    for (size_t i = 0; i < sizeof(bcast_addr); i++) ((uint8_t *)&bcast_addr)[i] = 0;
    bcast_addr.family = NET_AF_INET;
    bcast_addr.port = probe_htons(DHCP_PORT_SERVER);
    bcast_addr.address = 0xffffffffu;

    ret = sys_call(SYS_SENDTO, (uintptr_t)fd, (uintptr_t)tx_buf, tx_len, 0,
                   (uintptr_t)&bcast_addr, sizeof(bcast_addr));
    if (ret < 0) {
        write_str(2, "dhcp-probe: failed to send broadcast DHCPDISCOVER\n");
        sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
        return 1;
    }

    /* 6. Bounded poll for reply (up to 5 seconds) */
    sysinfo_t info;
    sys_call(SYS_SYSINFO, (uintptr_t)&info, 0, 0, 0, 0, 0);
    uint64_t start_ticks = info.uptime_ticks;
    uint64_t hz = info.tick_hz ? info.tick_hz : 100;
    uint64_t deadline = start_ticks + 5 * hz;

    uint8_t rx_buf[DHCP_MAX_PACKET_LEN];
    net_sockaddr_in_t src_addr;
    uint32_t src_len = sizeof(src_addr);
    bool received = false;

    while (1) {
        for (size_t i = 0; i < sizeof(src_addr); i++) ((uint8_t *)&src_addr)[i] = 0;
        src_len = sizeof(src_addr);
        long n = sys_call(SYS_RECVFROM, (uintptr_t)fd, (uintptr_t)rx_buf, sizeof(rx_buf),
                          NET_MSG_DONTWAIT, (uintptr_t)&src_addr, (uintptr_t)&src_len);
        if (n > 0) {
            write_str(1, "dhcp-probe: received packet from ");
            print_ip(1, src_addr.address);
            write_str(1, ":");
            print_u32(1, probe_ntohs(src_addr.port));
            write_str(1, "\n");
            received = true;
            break;
        }

        sys_call(SYS_SYSINFO, (uintptr_t)&info, 0, 0, 0, 0, 0);
        if (info.uptime_ticks >= deadline) {
            break;
        }
    }

    sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);

    if (!received) {
        write_str(2, "dhcp-probe: timeout waiting for reply\n");
        return 1;
    }

    return 0;
}
