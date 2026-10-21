#include "types.h"
#include "syscall_abi.h"
#include "socket_abi.h"
#include "netctl_abi.h"
#include "vfs.h"
#include "dhcp_codec.h"
#include "timezone.h"
#include "netconf.h"
#include "signal_abi.h"
#include "timeline_log.h"

#ifndef NET_IF_LINK_UP
#define NET_IF_LINK_UP NET_IF_LINK_ONLINE
#endif

#ifndef DHCPC_TEST_CALL
static long sys_call(long nr, uintptr_t a, uintptr_t b, uintptr_t c,
                     uintptr_t d, uintptr_t e, uintptr_t f) {
    register uintptr_t r10 __asm__("r10") = d;
    register uintptr_t r8  __asm__("r8")  = e;
    register uintptr_t r9  __asm__("r9")  = f;
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c),
                     "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory", "cc");
    return nr;
}
#else
#define sys_call DHCPC_TEST_CALL
#endif

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

static int mem_cmp(const void *a, const void *b, size_t n) {
    const uint8_t *p1 = (const uint8_t *)a;
    const uint8_t *p2 = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) return (int)p1[i] - (int)p2[i];
    }
    return 0;
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

static void dhcpc_log_lease(const char *action, const dhcp_offer_t *offer) {
    if (!offer) return;
    const uint8_t *b = (const uint8_t *)&offer->yiaddr;
    const uint8_t *r = (const uint8_t *)&offer->router;
    timeline_log("dhcpc", "Lease %s: IP %u.%u.%u.%u/%u, router %u.%u.%u.%u, lease %us",
                 action,
                 b[0], b[1], b[2], b[3], (unsigned)offer->prefix,
                 r[0], r[1], r[2], r[3], (unsigned)offer->lease_time);
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

static inline uint32_t dhcpc_ntohl(uint32_t v) {
    return dhcpc_htonl(v);
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

static void dhcpc_write_boot_log(const dhcp_offer_t *offer) {
    if (!offer) return;
    char msg[256];
    size_t pos = 0;
    append_str(msg, sizeof(msg), &pos, "eth0: DHCP lease acquired ");
    append_ip(msg, sizeof(msg), &pos, offer->yiaddr);
    append_str(msg, sizeof(msg), &pos, "/");
    append_u32(msg, sizeof(msg), &pos, offer->prefix);
    if (offer->router) {
        append_str(msg, sizeof(msg), &pos, ", gateway ");
        append_ip(msg, sizeof(msg), &pos, offer->router);
    }
    if (offer->dns_count > 0) {
        append_str(msg, sizeof(msg), &pos, ", DNS ");
        append_ip(msg, sizeof(msg), &pos, offer->dns_servers[0]);
    }
    append_str(msg, sizeof(msg), &pos, "\n");
    write_file("/run/dhcpc.log", msg, pos);
}

static long read_file(const char *path, char *buf, size_t cap) {
    long fd = sys_call(SYS_OPEN, (uintptr_t)path, VFS_O_RDONLY | VFS_O_CLOEXEC, 0, 0, 0, 0);
    if (fd < 0) return -1;
    size_t total = 0;
    while (total + 1 < cap) {
        long n = sys_call(SYS_READ, (uintptr_t)fd, (uintptr_t)(buf + total), cap - 1 - total, 0, 0, 0);
        if (n <= 0) break;
        total += (size_t)n;
    }
    sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
    buf[total] = '\0';
    return (long)total;
}

static void delete_file(const char *path) {
    sys_call(SYS_UNLINK, (uintptr_t)path, 0, 0, 0, 0, 0);
}

static uint64_t get_monotonic_sec(void) {
    timespec_t ts = {0, 0};
    sys_call(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (uintptr_t)&ts, 0, 0, 0, 0);
    return (uint64_t)ts.tv_sec;
}

static int64_t get_realtime_epoch(void) {
    timespec_t ts = {0, 0};
    long r = sys_call(SYS_CLOCK_GETTIME, CLOCK_REALTIME, (uintptr_t)&ts, 0, 0, 0, 0);
    if (r != 0 || ts.tv_sec <= 0) {
        return (int64_t)get_monotonic_sec();
    }
    return ts.tv_sec;
}

static void sleep_sec(uint64_t sec) {
    timespec_t req = {.tv_sec = (int64_t)sec, .tv_nsec = 0};
    sys_call(SYS_NANOSLEEP, (uintptr_t)&req, 0, 0, 0, 0, 0);
}

static void sleep_ms(uint64_t ms) {
    timespec_t req = {
        .tv_sec = (int64_t)(ms / 1000),
        .tv_nsec = (int64_t)((ms % 1000) * 1000000)
    };
    sys_call(SYS_NANOSLEEP, (uintptr_t)&req, 0, 0, 0, 0, 0);
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
    sys_call(SYS_MKDIR, (uintptr_t)"/etc/network", 0755, 0, 0, 0, 0);
    write_file("/run/resolv.conf", buf, pos);
    write_file("/etc/network/resolv.conf", buf, pos);
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

    sys_call(SYS_MKDIR, (uintptr_t)"/etc/network", 0755, 0, 0, 0, 0);
    write_file(NETCONF_INSTALLED_PATH, buf, pos);
    write_file("/tmp/network.conf", buf, pos);
    write_file(NETCONF_DEFAULT_PATH, buf, pos);
    return true;
}

static void save_timezone(const dhcp_offer_t *offer);

static bool save_dhcpc_lease(const dhcp_offer_t *offer, int64_t acquired_epoch) {
    save_timezone(offer);
    static char buf[512];
    size_t pos = 0;
    append_str(buf, sizeof(buf), &pos, "IP=");
    append_ip(buf, sizeof(buf), &pos, offer->yiaddr);
    append_str(buf, sizeof(buf), &pos, "\nPREFIX=");
    append_u32(buf, sizeof(buf), &pos, offer->prefix);
    append_str(buf, sizeof(buf), &pos, "\nNETMASK=");
    append_ip(buf, sizeof(buf), &pos, offer->subnet_mask);
    append_str(buf, sizeof(buf), &pos, "\nGATEWAY=");
    if (offer->router) append_ip(buf, sizeof(buf), &pos, offer->router);
    append_str(buf, sizeof(buf), &pos, "\nDNS1=");
    if (offer->dns_count > 0) append_ip(buf, sizeof(buf), &pos, offer->dns_servers[0]);
    append_str(buf, sizeof(buf), &pos, "\nDNS2=");
    if (offer->dns_count > 1) append_ip(buf, sizeof(buf), &pos, offer->dns_servers[1]);
    append_str(buf, sizeof(buf), &pos, "\nSERVER=");
    append_ip(buf, sizeof(buf), &pos, offer->server_ip);
    append_str(buf, sizeof(buf), &pos, "\nLEASE_TIME=");
    append_u32(buf, sizeof(buf), &pos, offer->lease_time);
    append_str(buf, sizeof(buf), &pos, "\nT1=");
    append_u32(buf, sizeof(buf), &pos, offer->t1);
    append_str(buf, sizeof(buf), &pos, "\nT2=");
    append_u32(buf, sizeof(buf), &pos, offer->t2);
    append_str(buf, sizeof(buf), &pos, "\nACQUIRED_EPOCH=");
    append_u32(buf, sizeof(buf), &pos, (uint32_t)acquired_epoch);
    append_str(buf, sizeof(buf), &pos, "\nEXPIRE_EPOCH=");
    append_u32(buf, sizeof(buf), &pos, (uint32_t)(acquired_epoch + offer->lease_time));
    append_str(buf, sizeof(buf), &pos, "\n");

    return write_file("/run/dhcpc.lease", buf, pos);
}

static bool save_dhcpc_pid(uint64_t pid) {
    char buf[32];
    size_t pos = 0;
    append_u32(buf, sizeof(buf), &pos, (uint32_t)pid);
    append_str(buf, sizeof(buf), &pos, "\n");
    bool ok = write_file("/run/dhcpc.pid", buf, pos);
    sys_call(SYS_SYNC, 0, 0, 0, 0, 0, 0);
    return ok;
}

static int64_t read_dhcpc_pid(void) {
    char buf[32];
    long len = read_file("/run/dhcpc.pid", buf, sizeof(buf));
    if (len <= 0) return -1;
    int64_t pid = 0;
    size_t i = 0;
    while (buf[i] >= '0' && buf[i] <= '9') {
        pid = pid * 10 + (buf[i] - '0');
        i++;
    }
    return pid > 0 ? pid : -1;
}

static bool is_process_alive(int64_t pid) {
    if (pid <= 0) return false;
    long ret = sys_call(SYS_KILL, (uintptr_t)pid, 0, 0, 0, 0, 0);
    return ret == 0 || ret == SYSCALL_EPERM || ret == SYSCALL_EACCES;
}

static const char *find_key_val(const char *buf, const char *key) {
    size_t klen = str_len(key);
    const char *p = buf;
    while (*p) {
        if ((p == buf || *(p - 1) == '\n') && mem_cmp(p, key, klen) == 0 && p[klen] == '=') {
            return p + klen + 1;
        }
        p++;
    }
    return NULL;
}

static bool extract_val_str(const char *buf, const char *key, char *out, size_t out_cap) {
    const char *v = find_key_val(buf, key);
    if (!v) return false;
    size_t i = 0;
    while (*v && *v != '\n' && *v != '\r' && i + 1 < out_cap) {
        out[i++] = *v++;
    }
    out[i] = '\0';
    return i > 0;
}

static bool parse_ip(const char *s, uint32_t *out_net_order) {
    uint32_t b[4] = {0, 0, 0, 0};
    int idx = 0;
    while (*s && idx < 4) {
        if (*s >= '0' && *s <= '9') {
            b[idx] = b[idx] * 10 + (*s - '0');
            if (b[idx] > 255) return false;
        } else if (*s == '.') {
            idx++;
        } else {
            break;
        }
        s++;
    }
    if (idx != 3) return false;
    uint8_t bytes[4] = {(uint8_t)b[0], (uint8_t)b[1], (uint8_t)b[2], (uint8_t)b[3]};
    *out_net_order = *(uint32_t *)bytes;
    return true;
}

static bool parse_u32(const char *s, uint32_t *out) {
    uint64_t val = 0;
    if (!*s || *s < '0' || *s > '9') return false;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        if (val > 0xffffffffu) return false;
        s++;
    }
    *out = (uint32_t)val;
    return true;
}

static bool extract_val_ip(const char *buf, const char *key, uint32_t *out_ip) {
    char tmp[32];
    if (!extract_val_str(buf, key, tmp, sizeof(tmp))) return false;
    return parse_ip(tmp, out_ip);
}

static bool extract_val_u32(const char *buf, const char *key, uint32_t *out_val) {
    char tmp[32];
    if (!extract_val_str(buf, key, tmp, sizeof(tmp)))
        return false;
    return parse_u32(tmp, out_val);
}

static void save_timezone(const dhcp_offer_t *offer) {
    const char *value = NULL;
    tz_result_t parsed;
    if (offer->timezone_iana[0] && tz_name(offer->timezone_iana) &&
        tz_spec(offer->timezone_iana, get_realtime_epoch(), &parsed))
        value = offer->timezone_iana;
    else if (offer->timezone_posix[0] &&
             tz_posix(offer->timezone_posix, get_realtime_epoch(), &parsed))
        value = offer->timezone_posix;
    if (value) {
        size_t len = 0;
        while (value[len])
            ++len;
        bool published = false;
        if (write_file("/run/timezone.new", value, len))
            published = sys_call(SYS_RENAME, (uintptr_t) "/run/timezone.new",
                                 (uintptr_t) "/run/timezone", 0, 0, 0, 0) == 0;
        if (!published)
            delete_file("/run/timezone");
        delete_file("/run/timezone.new");

        /* If /etc/timezone does not exist or /etc/timezone.dhcp exists, write /etc/timezone */
        long chk = sys_call(SYS_OPEN, (uintptr_t)"/etc/timezone", VFS_O_RDONLY, 0, 0, 0, 0);
        bool should_write = false;
        if (chk < 0) {
            should_write = true;
        } else {
            sys_call(SYS_CLOSE, chk, 0, 0, 0, 0, 0);
            long dhcp_opt = sys_call(SYS_OPEN, (uintptr_t)"/etc/timezone.dhcp", VFS_O_RDONLY, 0, 0, 0, 0);
            if (dhcp_opt >= 0) {
                sys_call(SYS_CLOSE, dhcp_opt, 0, 0, 0, 0, 0);
                should_write = true;
            }
        }
        if (should_write) {
            char tz_buf[160];
            if (len + 1 < sizeof(tz_buf)) {
                for (size_t i = 0; i < len; i++)
                    tz_buf[i] = value[i];
                tz_buf[len] = '\n';
                if (write_file("/etc/timezone.new", tz_buf, len + 1)) {
                    long ren = sys_call(SYS_RENAME_REPLACE, (uintptr_t)"/etc/timezone.new",
                                        (uintptr_t)"/etc/timezone", 0, 0, 0, 0);
                    if (ren < 0) {
                        sys_call(SYS_RENAME, (uintptr_t)"/etc/timezone.new",
                                 (uintptr_t)"/etc/timezone", 0, 0, 0, 0);
                    }
                }
                delete_file("/etc/timezone.new");
            }
        }
    } else
        delete_file("/run/timezone");
}

static bool apply_interface(const dhcp_offer_t *offer) {
    netctl_ifset_t req;
    for (size_t i = 0; i < sizeof(req); i++)
        ((uint8_t *)&req)[i] = 0;
    req.struct_version = 1;
    req.local_ipv4 = offer->yiaddr;
    req.netmask_ipv4 = offer->subnet_mask ? offer->subnet_mask : dhcpc_htonl(0xffffff00u);
    req.gateway_ipv4 = offer->router;

    long ret = sys_call(SYS_NETCTL, NETCTL_IFSET, (uintptr_t)&req, sizeof(req), 0, 0, 0);
    if(ret==0)save_timezone(offer);
    return ret == 0;
}

static void flush_interface(void) {
    delete_file("/run/timezone");
    netctl_ifset_t req;
    for (size_t i = 0; i < sizeof(req); i++) ((uint8_t *)&req)[i] = 0;
    req.struct_version = 1;
    req.local_ipv4 = 0;
    req.netmask_ipv4 = 0;
    req.gateway_ipv4 = 0;
    sys_call(SYS_NETCTL, NETCTL_IFSET, (uintptr_t)&req, sizeof(req), 0, 0, 0);
}

static int spawn_daemon(void) {
    spawn_fd_action_t actions[3];
    for (size_t i = 0; i < 3; i++) {
        for (size_t j = 0; j < sizeof(spawn_fd_action_t); j++) ((uint8_t *)&actions[i])[j] = 0;
    }
    actions[0].type = SPAWN_FD_ACTION_OPEN;
    actions[0].dst_fd = 0;
    actions[0].path = (uint64_t)"/dev/null";
    actions[0].flags = VFS_O_RDONLY;

    actions[1].type = SPAWN_FD_ACTION_OPEN;
    actions[1].dst_fd = 1;
    actions[1].path = (uint64_t)"/run/dhcpc.log";
    actions[1].flags = VFS_O_WRONLY | VFS_O_CREAT | VFS_O_APPEND;
    actions[1].mode = 0644;

    actions[2].type = SPAWN_FD_ACTION_DUP2;
    actions[2].src_fd = 1;
    actions[2].dst_fd = 2;

    const char *daemon_argv[] = {"/sbin/dhcpc", "--daemon", NULL};
    spawn_opts_t opts;
    for (size_t j = 0; j < sizeof(opts); j++) ((uint8_t *)&opts)[j] = 0;
    opts.size = sizeof(opts);
    opts.version = 2;
    opts.flags = SPAWN_SETPGROUP;
    opts.argv = (uint64_t)daemon_argv;
    opts.fd_actions = (uint64_t)actions;
    opts.action_count = 3;

    long pid = sys_call(SYS_SPAWN_EXT, (uintptr_t)"/sbin/dhcpc", (uintptr_t)&opts, sizeof(opts), 0, 0, 0);
    if (pid > 0) {
        save_dhcpc_pid((uint64_t)pid);
    }
    return (int)pid;
}

static volatile int g_term_requested = 0;
static volatile int g_renew_requested = 0;

static void on_signal(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        g_term_requested = 1;
    } else if (sig == SIGHUP) {
        g_renew_requested = 1;
    }
}

static void setup_signals(void) {
    signal_action_t sa = {.handler = (uintptr_t)on_signal, .mask = 0, .flags = 0, .reserved = 0};
    sys_call(SYS_SIGACTION, SIGTERM, (uintptr_t)&sa, 0, 0, 0, 0);
    sys_call(SYS_SIGACTION, SIGINT, (uintptr_t)&sa, 0, 0, 0, 0);
    sys_call(SYS_SIGACTION, SIGHUP, (uintptr_t)&sa, 0, 0, 0, 0);
}

static void dhcpc_apply_random_jitter(dhcp_offer_t *offer) {
    if (!offer || offer->lease_time == 0) return;
    int32_t d1 = 0, d2 = 0;
    uint32_t rnd[2] = {0, 0};
    long r = sys_call(SYS_GETRANDOM, (uintptr_t)rnd, sizeof(rnd), 0, 0, 0, 0);
    if (r <= 0) {
        timespec_t ts = {0, 0};
        sys_call(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (uintptr_t)&ts, 0, 0, 0, 0);
        rnd[0] = (uint32_t)ts.tv_nsec;
        rnd[1] = (uint32_t)(ts.tv_nsec ^ ((uint32_t)ts.tv_sec << 16));
    }
    d1 = (int32_t)(rnd[0] % 7) - 3;
    d2 = (int32_t)(rnd[1] % 7) - 3;
    dhcp_apply_jitter(offer->lease_time, &offer->t1, &offer->t2, d1, d2);
}

/* Perform full 4-way handshake (DISCOVER -> OFFER -> REQUEST -> ACK) */
static int dhcp_do_handshake(const uint8_t mac[6], uint64_t timeout_sec, bool dry_run, dhcp_offer_t *out_offer) {
    long fd = sys_call(SYS_SOCKET, NET_AF_INET, NET_SOCK_DGRAM | NET_SOCK_CLOEXEC, 0, 0, 0, 0);
    if (fd < 0) return -1;

    net_sockaddr_in_t local_addr;
    for (size_t i = 0; i < sizeof(local_addr); i++) ((uint8_t *)&local_addr)[i] = 0;
    local_addr.family = NET_AF_INET;
    local_addr.port = dhcpc_htons(DHCP_PORT_CLIENT);
    local_addr.address = 0;

    long ret = sys_call(SYS_BIND, (uintptr_t)fd, (uintptr_t)&local_addr, sizeof(local_addr), 0, 0, 0);
    if (ret != 0) {
        sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
        return -2;
    }

    net_sockaddr_in_t bcast_addr;
    for (size_t i = 0; i < sizeof(bcast_addr); i++) ((uint8_t *)&bcast_addr)[i] = 0;
    bcast_addr.family = NET_AF_INET;
    bcast_addr.port = dhcpc_htons(DHCP_PORT_SERVER);
    bcast_addr.address = 0xffffffffu;

    sysinfo_t info;
    sys_call(SYS_SYSINFO, (uintptr_t)&info, 0, 0, 0, 0, 0);
    uint32_t xid = (uint32_t)(info.uptime_ticks ^ 0x44484350);
    if (!xid) xid = 0x12345678;

    static uint8_t tx_buf[DHCP_MAX_PACKET_LEN];
    size_t tx_len = 0;
    if (dhcp_encode_discover(tx_buf, sizeof(tx_buf), xid, mac, &tx_len) != 0) {
        sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
        return -3;
    }

    ret = sys_call(SYS_SENDTO, (uintptr_t)fd, (uintptr_t)tx_buf, tx_len, 0,
                   (uintptr_t)&bcast_addr, sizeof(bcast_addr));
    if (ret < 0) {
        sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
        return -4;
    }

    /* Wait for DHCPOFFER */
    uint64_t deadline = get_monotonic_sec() + timeout_sec;
    static uint8_t rx_buf[DHCP_MAX_PACKET_LEN];
    net_sockaddr_in_t src_addr;
    uint32_t src_len = sizeof(src_addr);
    static dhcp_offer_t offer;
    bool got_offer = false;

    while (get_monotonic_sec() <= deadline && !g_term_requested) {
        for (size_t i = 0; i < sizeof(src_addr); i++) ((uint8_t *)&src_addr)[i] = 0;
        src_len = sizeof(src_addr);
        long n = sys_call(SYS_RECVFROM, (uintptr_t)fd, (uintptr_t)rx_buf, sizeof(rx_buf),
                          NET_MSG_DONTWAIT, (uintptr_t)&src_addr, (uintptr_t)&src_len);
        if (n > 0) {
            if (dhcp_decode_reply(rx_buf, (size_t)n, xid, mac, &offer) == 0 &&
                offer.msg_type == DHCP_MSG_OFFER) {
                got_offer = true;
                break;
            }
        }
        sleep_ms(50);
    }

    if (!got_offer) {
        sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
        return -5;
    }

    if (dry_run) {
        sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
        if (out_offer) *out_offer = offer;
        return 0;
    }

    /* Send DHCPREQUEST */
    tx_len = 0;
    if (dhcp_encode_request(tx_buf, sizeof(tx_buf), xid, mac,
                            offer.yiaddr, offer.server_ip, &tx_len) != 0) {
        sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
        return -6;
    }

    ret = sys_call(SYS_SENDTO, (uintptr_t)fd, (uintptr_t)tx_buf, tx_len, 0,
                   (uintptr_t)&bcast_addr, sizeof(bcast_addr));
    if (ret < 0) {
        sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
        return -7;
    }

    /* Wait for DHCPACK */
    deadline = get_monotonic_sec() + timeout_sec;
    static dhcp_offer_t ack;
    bool got_ack = false;

    while (get_monotonic_sec() <= deadline && !g_term_requested) {
        for (size_t i = 0; i < sizeof(src_addr); i++) ((uint8_t *)&src_addr)[i] = 0;
        src_len = sizeof(src_addr);
        long n = sys_call(SYS_RECVFROM, (uintptr_t)fd, (uintptr_t)rx_buf, sizeof(rx_buf),
                          NET_MSG_DONTWAIT, (uintptr_t)&src_addr, (uintptr_t)&src_len);
        if (n > 0) {
            if (dhcp_decode_reply(rx_buf, (size_t)n, xid, mac, &ack) == 0) {
                if (ack.msg_type == DHCP_MSG_ACK) {
                    got_ack = true;
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
                    if (ack.t1) offer.t1 = ack.t1;
                    for(size_t tz_i=0;tz_i<128;tz_i++){offer.timezone_iana[tz_i]=ack.timezone_iana[tz_i];offer.timezone_posix[tz_i]=ack.timezone_posix[tz_i];}
                    if (ack.t2) offer.t2 = ack.t2;
                    break;
                } else if (ack.msg_type == DHCP_MSG_NAK) {
                    sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
                    return 1;
                }
            }
        }
        sleep_ms(50);
    }

    sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
    if (!got_ack) return -8;

    if (!offer.subnet_mask) {
        offer.prefix = 24;
        offer.subnet_mask = dhcpc_htonl(0xffffff00u);
    }

    dhcpc_apply_random_jitter(&offer);

    if (out_offer) *out_offer = offer;
    return 0;
}

/* Unicast renewal DHCPREQUEST */
static int dhcp_do_renew(const dhcp_offer_t *offer, const uint8_t mac[6], uint64_t timeout_sec, dhcp_offer_t *out_ack) {
    long fd = sys_call(SYS_SOCKET, NET_AF_INET, NET_SOCK_DGRAM | NET_SOCK_CLOEXEC, 0, 0, 0, 0);
    if (fd < 0) return -1;

    net_sockaddr_in_t local_addr;
    for (size_t i = 0; i < sizeof(local_addr); i++) ((uint8_t *)&local_addr)[i] = 0;
    local_addr.family = NET_AF_INET;
    local_addr.port = dhcpc_htons(DHCP_PORT_CLIENT);
    local_addr.address = 0;

    if (sys_call(SYS_BIND, (uintptr_t)fd, (uintptr_t)&local_addr, sizeof(local_addr), 0, 0, 0) != 0) {
        sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
        return -2;
    }

    net_sockaddr_in_t srv_addr;
    for (size_t i = 0; i < sizeof(srv_addr); i++) ((uint8_t *)&srv_addr)[i] = 0;
    srv_addr.family = NET_AF_INET;
    srv_addr.port = dhcpc_htons(DHCP_PORT_SERVER);
    srv_addr.address = offer->server_ip;

    sysinfo_t info;
    sys_call(SYS_SYSINFO, (uintptr_t)&info, 0, 0, 0, 0, 0);
    uint32_t xid = (uint32_t)(info.uptime_ticks ^ 0x52454e57); /* 'RENW' */

    static uint8_t tx[DHCP_MAX_PACKET_LEN];
    size_t tx_len = 0;
    if (dhcp_encode_request_renew(tx, sizeof(tx), xid, mac, offer->yiaddr, &tx_len) != 0) {
        sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
        return -3;
    }

    if (sys_call(SYS_SENDTO, (uintptr_t)fd, (uintptr_t)tx, tx_len, 0, (uintptr_t)&srv_addr, sizeof(srv_addr)) < 0) {
        sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
        return -4;
    }

    uint64_t deadline = get_monotonic_sec() + timeout_sec;
    static uint8_t rx[DHCP_MAX_PACKET_LEN];
    net_sockaddr_in_t src;
    uint32_t srclen = sizeof(src);
    static dhcp_offer_t ack;

    while (get_monotonic_sec() <= deadline && !g_term_requested) {
        for (size_t i = 0; i < sizeof(src); i++) ((uint8_t *)&src)[i] = 0;
        srclen = sizeof(src);
        long n = sys_call(SYS_RECVFROM, (uintptr_t)fd, (uintptr_t)rx, sizeof(rx),
                          NET_MSG_DONTWAIT, (uintptr_t)&src, (uintptr_t)&srclen);
        if (n > 0) {
            if (dhcp_decode_reply(rx, (size_t)n, xid, mac, &ack) == 0) {
                if (ack.msg_type == DHCP_MSG_ACK) {
                    sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
                    if (out_ack) {
                        *out_ack = *offer;
                        if (ack.yiaddr) out_ack->yiaddr = ack.yiaddr;
                        if (ack.subnet_mask) { out_ack->subnet_mask = ack.subnet_mask; out_ack->prefix = ack.prefix; }
                        if (ack.router) out_ack->router = ack.router;
                        if (ack.dns_count > 0) {
                            for (int i = 0; i < ack.dns_count; i++) out_ack->dns_servers[i] = ack.dns_servers[i];
                            out_ack->dns_count = ack.dns_count;
                        }
                        if (ack.lease_time) out_ack->lease_time = ack.lease_time;
                        if (ack.t1) out_ack->t1 = ack.t1;
                        if (ack.t2) out_ack->t2 = ack.t2;
                        for(size_t tz_i=0;tz_i<128;tz_i++){out_ack->timezone_iana[tz_i]=ack.timezone_iana[tz_i];out_ack->timezone_posix[tz_i]=ack.timezone_posix[tz_i];}
                        dhcpc_apply_random_jitter(out_ack);
                    }
                    return 0;
                } else if (ack.msg_type == DHCP_MSG_NAK) {
                    sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
                    return 1;
                }
            }
        }
        sleep_ms(50);
    }

    sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
    return -5;
}

/* Broadcast rebind DHCPREQUEST */
static int dhcp_do_rebind(const dhcp_offer_t *offer, const uint8_t mac[6], uint64_t timeout_sec, dhcp_offer_t *out_ack) {
    long fd = sys_call(SYS_SOCKET, NET_AF_INET, NET_SOCK_DGRAM | NET_SOCK_CLOEXEC, 0, 0, 0, 0);
    if (fd < 0) return -1;

    net_sockaddr_in_t local_addr;
    for (size_t i = 0; i < sizeof(local_addr); i++) ((uint8_t *)&local_addr)[i] = 0;
    local_addr.family = NET_AF_INET;
    local_addr.port = dhcpc_htons(DHCP_PORT_CLIENT);
    local_addr.address = 0;

    if (sys_call(SYS_BIND, (uintptr_t)fd, (uintptr_t)&local_addr, sizeof(local_addr), 0, 0, 0) != 0) {
        sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
        return -2;
    }

    net_sockaddr_in_t bcast_addr;
    for (size_t i = 0; i < sizeof(bcast_addr); i++) ((uint8_t *)&bcast_addr)[i] = 0;
    bcast_addr.family = NET_AF_INET;
    bcast_addr.port = dhcpc_htons(DHCP_PORT_SERVER);
    bcast_addr.address = 0xffffffffu;

    sysinfo_t info;
    sys_call(SYS_SYSINFO, (uintptr_t)&info, 0, 0, 0, 0, 0);
    uint32_t xid = (uint32_t)(info.uptime_ticks ^ 0x52424e44); /* 'RBND' */

    static uint8_t tx[DHCP_MAX_PACKET_LEN];
    size_t tx_len = 0;
    if (dhcp_encode_request_rebind(tx, sizeof(tx), xid, mac, offer->yiaddr, &tx_len) != 0) {
        sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
        return -3;
    }

    if (sys_call(SYS_SENDTO, (uintptr_t)fd, (uintptr_t)tx, tx_len, 0, (uintptr_t)&bcast_addr, sizeof(bcast_addr)) < 0) {
        sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
        return -4;
    }

    uint64_t deadline = get_monotonic_sec() + timeout_sec;
    static uint8_t rx[DHCP_MAX_PACKET_LEN];
    net_sockaddr_in_t src;
    uint32_t srclen = sizeof(src);
    static dhcp_offer_t ack;

    while (get_monotonic_sec() <= deadline && !g_term_requested) {
        for (size_t i = 0; i < sizeof(src); i++) ((uint8_t *)&src)[i] = 0;
        srclen = sizeof(src);
        long n = sys_call(SYS_RECVFROM, (uintptr_t)fd, (uintptr_t)rx, sizeof(rx),
                          NET_MSG_DONTWAIT, (uintptr_t)&src, (uintptr_t)&srclen);
        if (n > 0) {
            if (dhcp_decode_reply(rx, (size_t)n, xid, mac, &ack) == 0) {
                if (ack.msg_type == DHCP_MSG_ACK) {
                    sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
                    if (out_ack) {
                        *out_ack = *offer;
                        if (ack.yiaddr) out_ack->yiaddr = ack.yiaddr;
                        if (ack.subnet_mask) { out_ack->subnet_mask = ack.subnet_mask; out_ack->prefix = ack.prefix; }
                        if (ack.router) out_ack->router = ack.router;
                        if (ack.dns_count > 0) {
                            for (int i = 0; i < ack.dns_count; i++) out_ack->dns_servers[i] = ack.dns_servers[i];
                            out_ack->dns_count = ack.dns_count;
                        }
                        if (ack.lease_time) out_ack->lease_time = ack.lease_time;
                        if (ack.t1) out_ack->t1 = ack.t1;
                        if (ack.t2) out_ack->t2 = ack.t2;
                        for(size_t tz_i=0;tz_i<128;tz_i++){out_ack->timezone_iana[tz_i]=ack.timezone_iana[tz_i];out_ack->timezone_posix[tz_i]=ack.timezone_posix[tz_i];}
                        dhcpc_apply_random_jitter(out_ack);
                    }
                    return 0;
                } else if (ack.msg_type == DHCP_MSG_NAK) {
                    sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
                    return 1;
                }
            }
        }
        sleep_ms(50);
    }

    sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
    return -5;
}

/* INIT-REBOOT DHCPREQUEST */
static int dhcp_do_reboot(const dhcp_offer_t *offer, const uint8_t mac[6], uint64_t timeout_sec, dhcp_offer_t *out_ack) {
    long fd = sys_call(SYS_SOCKET, NET_AF_INET, NET_SOCK_DGRAM | NET_SOCK_CLOEXEC, 0, 0, 0, 0);
    if (fd < 0) return -1;

    net_sockaddr_in_t local_addr;
    for (size_t i = 0; i < sizeof(local_addr); i++) ((uint8_t *)&local_addr)[i] = 0;
    local_addr.family = NET_AF_INET;
    local_addr.port = dhcpc_htons(DHCP_PORT_CLIENT);
    local_addr.address = 0;

    if (sys_call(SYS_BIND, (uintptr_t)fd, (uintptr_t)&local_addr, sizeof(local_addr), 0, 0, 0) != 0) {
        sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
        return -2;
    }

    net_sockaddr_in_t bcast_addr;
    for (size_t i = 0; i < sizeof(bcast_addr); i++) ((uint8_t *)&bcast_addr)[i] = 0;
    bcast_addr.family = NET_AF_INET;
    bcast_addr.port = dhcpc_htons(DHCP_PORT_SERVER);
    bcast_addr.address = 0xffffffffu;

    sysinfo_t info;
    sys_call(SYS_SYSINFO, (uintptr_t)&info, 0, 0, 0, 0, 0);
    uint32_t xid = (uint32_t)(info.uptime_ticks ^ 0x494e4954); /* 'INIT' */

    static uint8_t tx[DHCP_MAX_PACKET_LEN];
    size_t tx_len = 0;
    if (dhcp_encode_request_reboot(tx, sizeof(tx), xid, mac, offer->yiaddr, &tx_len) != 0) {
        sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
        return -3;
    }

    if (sys_call(SYS_SENDTO, (uintptr_t)fd, (uintptr_t)tx, tx_len, 0, (uintptr_t)&bcast_addr, sizeof(bcast_addr)) < 0) {
        sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
        return -4;
    }

    uint64_t deadline = get_monotonic_sec() + timeout_sec;
    static uint8_t rx[DHCP_MAX_PACKET_LEN];
    net_sockaddr_in_t src;
    uint32_t srclen = sizeof(src);
    static dhcp_offer_t ack;

    while (get_monotonic_sec() <= deadline && !g_term_requested) {
        for (size_t i = 0; i < sizeof(src); i++) ((uint8_t *)&src)[i] = 0;
        srclen = sizeof(src);
        long n = sys_call(SYS_RECVFROM, (uintptr_t)fd, (uintptr_t)rx, sizeof(rx),
                          NET_MSG_DONTWAIT, (uintptr_t)&src, (uintptr_t)&srclen);
        if (n > 0) {
            if (dhcp_decode_reply(rx, (size_t)n, xid, mac, &ack) == 0) {
                if (ack.msg_type == DHCP_MSG_ACK) {
                    sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
                    if (out_ack) {
                        *out_ack = *offer;
                        if (ack.yiaddr) out_ack->yiaddr = ack.yiaddr;
                        if (ack.subnet_mask) { out_ack->subnet_mask = ack.subnet_mask; out_ack->prefix = ack.prefix; }
                        if (ack.router) out_ack->router = ack.router;
                        if (ack.dns_count > 0) {
                            for (int i = 0; i < ack.dns_count; i++) out_ack->dns_servers[i] = ack.dns_servers[i];
                            out_ack->dns_count = ack.dns_count;
                        }
                        if (ack.lease_time) out_ack->lease_time = ack.lease_time;
                        if (ack.t1) out_ack->t1 = ack.t1;
                        if (ack.t2) out_ack->t2 = ack.t2;
                        for(size_t tz_i=0;tz_i<128;tz_i++){out_ack->timezone_iana[tz_i]=ack.timezone_iana[tz_i];out_ack->timezone_posix[tz_i]=ack.timezone_posix[tz_i];}
                        dhcpc_apply_random_jitter(out_ack);
                    }
                    return 0;
                } else if (ack.msg_type == DHCP_MSG_NAK) {
                    sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
                    return 1;
                }
            }
        }
        sleep_ms(50);
    }

    sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
    return -5;
}

/* Send DHCPRELEASE */
static void dhcp_do_release(const dhcp_offer_t *offer, const uint8_t mac[6]) {
    long fd = sys_call(SYS_SOCKET, NET_AF_INET, NET_SOCK_DGRAM | NET_SOCK_CLOEXEC, 0, 0, 0, 0);
    if (fd < 0) return;

    net_sockaddr_in_t local_addr;
    for (size_t i = 0; i < sizeof(local_addr); i++) ((uint8_t *)&local_addr)[i] = 0;
    local_addr.family = NET_AF_INET;
    local_addr.port = dhcpc_htons(DHCP_PORT_CLIENT);
    local_addr.address = 0;

    if (sys_call(SYS_BIND, (uintptr_t)fd, (uintptr_t)&local_addr, sizeof(local_addr), 0, 0, 0) == 0) {
        net_sockaddr_in_t srv_addr;
        for (size_t i = 0; i < sizeof(srv_addr); i++) ((uint8_t *)&srv_addr)[i] = 0;
        srv_addr.family = NET_AF_INET;
        srv_addr.port = dhcpc_htons(DHCP_PORT_SERVER);
        srv_addr.address = offer->server_ip;

        static uint8_t tx[DHCP_MAX_PACKET_LEN];
        size_t tx_len = 0;
        if (dhcp_encode_release(tx, sizeof(tx), 0x52454c53, mac, offer->yiaddr, offer->server_ip, &tx_len) == 0) {
            sys_call(SYS_SENDTO, (uintptr_t)fd, (uintptr_t)tx, tx_len, 0,
                     (uintptr_t)&srv_addr, sizeof(srv_addr));
        }
    }
    sys_call(SYS_CLOSE, (uintptr_t)fd, 0, 0, 0, 0, 0);
}

static int show_status(void) {
    static char lease_buf[512];
    long n = read_file("/run/dhcpc.lease", lease_buf, sizeof(lease_buf));
    if (n <= 0) {
        print_err("dhcpc: no active lease found (/run/dhcpc.lease missing)\n");
        return 1;
    }

    print_out("Active DHCPv4 Lease:\n");
    char val[64];
    if (extract_val_str(lease_buf, "IP", val, sizeof(val))) {
        print_out("  IP Address:     "); print_out(val);
        if (extract_val_str(lease_buf, "PREFIX", val, sizeof(val))) {
            print_out("/"); print_out(val);
        }
        print_out("\n");
    }
    if (extract_val_str(lease_buf, "NETMASK", val, sizeof(val))) {
        print_out("  Subnet Mask:    "); print_out(val); print_out("\n");
    }
    if (extract_val_str(lease_buf, "GATEWAY", val, sizeof(val))) {
        print_out("  Gateway:        "); print_out(val); print_out("\n");
    }
    if (extract_val_str(lease_buf, "DNS1", val, sizeof(val))) {
        print_out("  DNS Server 1:   "); print_out(val); print_out("\n");
    }
    if (extract_val_str(lease_buf, "DNS2", val, sizeof(val))) {
        print_out("  DNS Server 2:   "); print_out(val); print_out("\n");
    }
    if (extract_val_str(lease_buf, "SERVER", val, sizeof(val))) {
        print_out("  DHCP Server:    "); print_out(val); print_out("\n");
    }
    if (extract_val_str(lease_buf, "LEASE_TIME", val, sizeof(val))) {
        print_out("  Lease Duration: "); print_out(val); print_out(" seconds\n");
    }
    if (extract_val_str(lease_buf, "T1", val, sizeof(val))) {
        print_out("  Renewal (T1):   "); print_out(val); print_out(" seconds\n");
    }
    if (extract_val_str(lease_buf, "T2", val, sizeof(val))) {
        print_out("  Rebind (T2):    "); print_out(val); print_out(" seconds\n");
    }

    uint32_t exp_epoch = 0;
    if (extract_val_u32(lease_buf, "EXPIRE_EPOCH", &exp_epoch)) {
        int64_t now_epoch = get_realtime_epoch();
        print_out("  Expires in:     ");
        if (now_epoch > 0 && exp_epoch > (uint32_t)now_epoch) {
            char rem[16]; size_t rpos = 0;
            append_u32(rem, sizeof(rem), &rpos, exp_epoch - (uint32_t)now_epoch);
            print_out(rem); print_out(" seconds\n");
        } else {
            print_out("expired\n");
        }
    }

    int64_t pid = read_dhcpc_pid();
    if (pid > 0 && is_process_alive(pid)) {
        char pid_str[16]; size_t ppos = 0;
        append_u32(pid_str, sizeof(pid_str), &ppos, (uint32_t)pid);
        print_out("  Daemon Status:  running (PID "); print_out(pid_str); print_out(")\n");
    } else {
        print_out("  Daemon Status:  not running\n");
    }
    return 0;
}

static int do_renew_command(void) {
    int64_t pid = read_dhcpc_pid();
    if (pid <= 0 || !is_process_alive(pid)) {
        print_err("dhcpc: renewal failed: daemon is not running\n");
        return 1;
    }
    long ret = sys_call(SYS_KILL, (uintptr_t)pid, SIGHUP, 0, 0, 0, 0);
    if (ret != 0) {
        print_err("dhcpc: failed to send SIGHUP to daemon\n");
        return 1;
    }
    char msg[64]; size_t pos = 0;
    append_str(msg, sizeof(msg), &pos, "dhcpc: sent renewal signal (SIGHUP) to daemon PID ");
    append_u32(msg, sizeof(msg), &pos, (uint32_t)pid);
    append_str(msg, sizeof(msg), &pos, "\n");
    print_out(msg);
    return 0;
}

static int do_release_command(void) {
    int64_t pid = read_dhcpc_pid();
    if (pid > 0 && is_process_alive(pid)) {
        long ret = sys_call(SYS_KILL, (uintptr_t)pid, SIGTERM, 0, 0, 0, 0);
        if (ret != 0) {
            print_err("dhcpc: failed to send SIGTERM to daemon\n");
            return 1;
        }
        char msg[64]; size_t pos = 0;
        append_str(msg, sizeof(msg), &pos, "dhcpc: sent release signal (SIGTERM) to daemon PID ");
        append_u32(msg, sizeof(msg), &pos, (uint32_t)pid);
        append_str(msg, sizeof(msg), &pos, "\n");
        print_out(msg);
        return 0;
    }

    static char lease_buf[512];
    long n = read_file("/run/dhcpc.lease", lease_buf, sizeof(lease_buf));
    if (n > 0) {
        static dhcp_offer_t off;
        for (size_t i = 0; i < sizeof(off); i++) ((uint8_t *)&off)[i] = 0;
        extract_val_ip(lease_buf, "IP", &off.yiaddr);
        extract_val_ip(lease_buf, "SERVER", &off.server_ip);
        if (off.yiaddr && off.server_ip) {
            netctl_ifget_t ifget;
            for (size_t i = 0; i < sizeof(ifget); i++) ((uint8_t *)&ifget)[i] = 0;
            ifget.struct_version = 1;
            if (sys_call(SYS_NETCTL, NETCTL_IFGET, (uintptr_t)&ifget, sizeof(ifget), 0, 0, 0) == 0) {
                dhcp_do_release(&off, ifget.mac);
            }
        }
        flush_interface();
        delete_file("/run/dhcpc.lease");
        delete_file("/run/dhcpc.pid");
        print_out("dhcpc: released lease and flushed interface\n");
        return 0;
    }

    print_err("dhcpc: no active lease or daemon found to release\n");
    return 1;
}

static int dhcpc_daemon_main(void) {
    proc_info_t pinfo;
    for (size_t i = 0; i < sizeof(pinfo); i++) ((uint8_t *)&pinfo)[i] = 0;
    long pret = sys_call(SYS_PROCINFO, PROC_INFO_SELF, (uintptr_t)&pinfo, 0, 0, 0, 0);

    int64_t existing_pid = read_dhcpc_pid();
    if (existing_pid > 0 && is_process_alive(existing_pid)) {
        if (pret > 0 && pinfo.pid > 0 && pinfo.pid == existing_pid) {
            /* This is our own PID, saved by our parent during spawn */
        } else {
            print_err("dhcpc: daemon already running\n");
            return 0;
        }
    }

    if (pret > 0 && pinfo.pid > 0) {
        save_dhcpc_pid((uint64_t)pinfo.pid);
    }

    setup_signals();

    netctl_ifget_t ifget;
    for (size_t i = 0; i < sizeof(ifget); i++) ((uint8_t *)&ifget)[i] = 0;
    ifget.struct_version = 1;
    long ret = sys_call(SYS_NETCTL, NETCTL_IFGET, (uintptr_t)&ifget, sizeof(ifget), 0, 0, 0);
    if (ret != 0) {
        print_err("dhcpc: network device unavailable\n");
        delete_file("/run/dhcpc.pid");
        return 1;
    }

    typedef enum {
        STATE_INIT,
        STATE_BOUND,
        STATE_RENEWING,
        STATE_REBINDING,
        STATE_DISCONNECTED,
        STATE_INIT_REBOOT
    } daemon_state_t;

    daemon_state_t state = STATE_INIT;
    static dhcp_offer_t current_offer;
    for (size_t i = 0; i < sizeof(current_offer); i++) ((uint8_t *)&current_offer)[i] = 0;

    uint64_t t_ack = 0;
    uint64_t t_renew = 0;
    uint64_t t_rebind = 0;
    uint64_t t_expire = 0;

    static char lease_buf[512];
    if (read_file("/run/dhcpc.lease", lease_buf, sizeof(lease_buf)) > 0) {
        if (extract_val_ip(lease_buf, "IP", &current_offer.yiaddr) &&
            extract_val_ip(lease_buf, "SERVER", &current_offer.server_ip)) {
            extract_val_ip(lease_buf, "NETMASK", &current_offer.subnet_mask);
            extract_val_ip(lease_buf, "GATEWAY", &current_offer.router);
            uint32_t dns1 = 0, dns2 = 0;
            if (extract_val_ip(lease_buf, "DNS1", &dns1)) {
                current_offer.dns_servers[0] = dns1;
                current_offer.dns_count = 1;
            }
            if (extract_val_ip(lease_buf, "DNS2", &dns2)) {
                current_offer.dns_servers[1] = dns2;
                current_offer.dns_count = 2;
            }
            uint32_t lt = 86400, t1 = 43200, t2 = 75600;
            extract_val_u32(lease_buf, "LEASE_TIME", &lt);
            extract_val_u32(lease_buf, "T1", &t1);
            extract_val_u32(lease_buf, "T2", &t2);
            current_offer.lease_time = lt;
            current_offer.t1 = t1;
            current_offer.t2 = t2;
            current_offer.prefix = dhcp_mask_to_prefix(current_offer.subnet_mask);

            t_ack = get_monotonic_sec();
            t_renew = t_ack + current_offer.t1;
            t_rebind = t_ack + current_offer.t2;
            t_expire = t_ack + current_offer.lease_time;
            if (ifget.link_state == NET_IF_LINK_UP) {
                state = STATE_BOUND;
            } else {
                state = STATE_DISCONNECTED;
            }
        }
    }

    while (1) {
        if (g_term_requested) {
            if (current_offer.yiaddr && current_offer.server_ip) {
                dhcp_do_release(&current_offer, ifget.mac);
            }
            flush_interface();
            delete_file("/run/dhcpc.lease");
            delete_file("/run/dhcpc.pid");
            return 0;
        }

        if (g_renew_requested) {
            g_renew_requested = 0;
            if (state == STATE_BOUND) {
                state = STATE_RENEWING;
            }
        }

        switch (state) {
            case STATE_INIT: {
                for (size_t i = 0; i < sizeof(ifget); i++) ((uint8_t *)&ifget)[i] = 0;
                ifget.struct_version = 1;
                long r_if = sys_call(SYS_NETCTL, NETCTL_IFGET, (uintptr_t)&ifget, sizeof(ifget), 0, 0, 0);
                if (r_if != 0 || ifget.link_state != NET_IF_LINK_UP) {
                    sleep_sec(2);
                    break;
                }

                static dhcp_offer_t offer;
                if (dhcp_do_handshake(ifget.mac, 3, false, &offer) == 0) {
                    current_offer = offer;
                    apply_interface(&current_offer);
                    int64_t epoch = get_realtime_epoch();
                    save_dhcpc_lease(&current_offer, epoch);
                    save_resolv_conf(current_offer.dns_servers, current_offer.dns_count);
                    save_network_conf(current_offer.yiaddr, current_offer.prefix, current_offer.router,
                                      current_offer.dns_servers, current_offer.dns_count);
                    sys_call(SYS_SYNC, 0, 0, 0, 0, 0, 0);
                    dhcpc_log_lease("bound", &current_offer);
                    dhcpc_write_boot_log(&current_offer);

                    t_ack = get_monotonic_sec();
                    t_renew = t_ack + current_offer.t1;
                    t_rebind = t_ack + current_offer.t2;
                    t_expire = t_ack + current_offer.lease_time;
                    state = STATE_BOUND;
                } else {
                    sleep_sec(2);
                }
                break;
            }

            case STATE_BOUND: {
                for (size_t i = 0; i < sizeof(ifget); i++) ((uint8_t *)&ifget)[i] = 0;
                ifget.struct_version = 1;
                long r_if = sys_call(SYS_NETCTL, NETCTL_IFGET, (uintptr_t)&ifget, sizeof(ifget), 0, 0, 0);
                if (r_if != 0 || ifget.link_state != NET_IF_LINK_UP) {
                    state = STATE_DISCONNECTED;
                    break;
                }

                uint64_t now = get_monotonic_sec();
                if (now >= t_renew) {
                    state = STATE_RENEWING;
                    break;
                }

                uint64_t delta = t_renew - now;
                uint64_t chunk = delta > 2 ? 2 : delta;
                if (chunk == 0) chunk = 1;
                sleep_sec(chunk);
                break;
            }

            case STATE_DISCONNECTED: {
                sleep_sec(1);
                for (size_t i = 0; i < sizeof(ifget); i++) ((uint8_t *)&ifget)[i] = 0;
                ifget.struct_version = 1;
                long r_if = sys_call(SYS_NETCTL, NETCTL_IFGET, (uintptr_t)&ifget, sizeof(ifget), 0, 0, 0);
                if (r_if == 0 && ifget.link_state == NET_IF_LINK_UP) {
                    if (current_offer.yiaddr && current_offer.server_ip) {
                        state = STATE_INIT_REBOOT;
                    } else {
                        state = STATE_INIT;
                    }
                }
                break;
            }

            case STATE_INIT_REBOOT: {
                static dhcp_offer_t ack;
                int r = dhcp_do_reboot(&current_offer, ifget.mac, 3, &ack);
                if (r == 0) {
                    current_offer = ack;
                    int64_t epoch = get_realtime_epoch();
                    save_dhcpc_lease(&current_offer, epoch);
                    dhcpc_log_lease("rebound", &current_offer);
                    dhcpc_write_boot_log(&current_offer);
                    t_ack = get_monotonic_sec();
                    t_renew = t_ack + current_offer.t1;
                    t_rebind = t_ack + current_offer.t2;
                    t_expire = t_ack + current_offer.lease_time;
                    state = STATE_BOUND;
                } else {
                    flush_interface();
                    delete_file("/run/dhcpc.lease");
                    state = STATE_INIT;
                }
                break;
            }

            case STATE_RENEWING: {
                static dhcp_offer_t ack;
                int r = dhcp_do_renew(&current_offer, ifget.mac, 3, &ack);
                if (r == 0) {
                    current_offer = ack;
                    int64_t epoch = get_realtime_epoch();
                    save_dhcpc_lease(&current_offer, epoch);
                    dhcpc_log_lease("renewed", &current_offer);
                    t_ack = get_monotonic_sec();
                    t_renew = t_ack + current_offer.t1;
                    t_rebind = t_ack + current_offer.t2;
                    t_expire = t_ack + current_offer.lease_time;
                    state = STATE_BOUND;
                } else if (r == 1) {
                    flush_interface();
                    delete_file("/run/dhcpc.lease");
                    state = STATE_INIT;
                } else {
                    uint64_t now = get_monotonic_sec();
                    if (now >= t_rebind) {
                        state = STATE_REBINDING;
                    } else {
                        uint64_t rem = t_rebind - now;
                        sleep_sec(rem > 5 ? 5 : rem);
                    }
                }
                break;
            }

            case STATE_REBINDING: {
                static dhcp_offer_t ack;
                int r = dhcp_do_rebind(&current_offer, ifget.mac, 3, &ack);
                if (r == 0) {
                    current_offer = ack;
                    int64_t epoch = get_realtime_epoch();
                    save_dhcpc_lease(&current_offer, epoch);
                    dhcpc_log_lease("rebound (T2)", &current_offer);
                    t_ack = get_monotonic_sec();
                    t_renew = t_ack + current_offer.t1;
                    t_rebind = t_ack + current_offer.t2;
                    t_expire = t_ack + current_offer.lease_time;
                    state = STATE_BOUND;
                } else if (r == 1) {
                    flush_interface();
                    delete_file("/run/dhcpc.lease");
                    state = STATE_INIT;
                } else {
                    uint64_t now = get_monotonic_sec();
                    if (now >= t_expire) {
                        flush_interface();
                        delete_file("/run/dhcpc.lease");
                        state = STATE_INIT;
                    } else {
                        uint64_t rem = t_expire - now;
                        sleep_sec(rem > 5 ? 5 : rem);
                    }
                }
                break;
            }
        }
    }
}

static void print_usage(void) {
    print_err("usage: dhcpc [OPTIONS]\n"
              "Acquires a DHCP IPv4 lease and configures the network interface.\n"
              "\n"
              "Options:\n"
              "  -b, --boot      Early boot probe (<=3s), configure, spawn daemon, and exit\n"
              "  -d, --daemon    Run as background renewal daemon (RFC 2131 T1/T2 timers)\n"
              "  -n, --dry-run   Perform DHCP transaction without configuring interface or writing files\n"
              "  -s, --status    Display current lease information, deadlines, and daemon PID\n"
              "  -r, --renew     Send SIGHUP to running daemon to force immediate lease renewal\n"
              "  -k, --release   Send SIGTERM to running daemon to send DHCPRELEASE and stop\n"
              "  -h, --help      Show this help message\n"
              "\n"
              "Limitation (v1) note:\n"
              "  DHCP v1 was a one-shot client; DHCP v2 provides background renewal daemon.\n");
}

int dhcpc_main(int argc, char **argv) {
    bool dry_run = false;
    bool boot_mode = false;
    bool daemon_mode = false;
    bool status_mode = false;
    bool renew_mode = false;
    bool release_mode = false;

    for (int i = 1; i < argc; i++) {
        if (str_equals(argv[i], "--help") || str_equals(argv[i], "-h")) {
            print_usage();
            return 0;
        } else if (str_equals(argv[i], "--dry-run") || str_equals(argv[i], "-n")) {
            dry_run = true;
        } else if (str_equals(argv[i], "--boot") || str_equals(argv[i], "-b")) {
            boot_mode = true;
        } else if (str_equals(argv[i], "--daemon") || str_equals(argv[i], "-d") ||
                   str_equals(argv[i], "--foreground") || str_equals(argv[i], "-f")) {
            daemon_mode = true;
        } else if (str_equals(argv[i], "--status") || str_equals(argv[i], "-s")) {
            status_mode = true;
        } else if (str_equals(argv[i], "--renew") || str_equals(argv[i], "-r")) {
            renew_mode = true;
        } else if (str_equals(argv[i], "--release") || str_equals(argv[i], "-k")) {
            release_mode = true;
        } else {
            print_err("dhcpc: unrecognized option: ");
            print_err(argv[i]);
            print_err("\n");
            print_usage();
            return 1;
        }
    }

    if (daemon_mode) {
        return dhcpc_daemon_main();
    }

    if (status_mode) {
        return show_status();
    }

    if (renew_mode) {
        return do_renew_command();
    }

    if (release_mode) {
        return do_release_command();
    }

    /* Query network device and MAC address */
    netctl_ifget_t ifget;
    for (size_t i = 0; i < sizeof(ifget); i++) ((uint8_t *)&ifget)[i] = 0;
    ifget.struct_version = 1;

    long ret = sys_call(SYS_NETCTL, NETCTL_IFGET, (uintptr_t)&ifget, sizeof(ifget), 0, 0, 0);
    if (ret != 0) {
        if (boot_mode) {
            /* Absent NIC at boot: exit silently 0 */
            return 0;
        }
        print_err("dhcpc: network interface unavailable\n");
        return 1;
    }

    if (ifget.link_state != NET_IF_LINK_UP) {
        if (boot_mode) {
            write_file("/run/dhcpc.log", "eth0: link waiting / no DHCP response; continuing in background\n", 64);
            spawn_daemon();
            return 0;
        }
        if (ifget.link_state == NET_IF_LINK_WAITING) {
            print_err("dhcpc: link WAITING (no cable connected)\n");
        } else if (ifget.link_state == NET_IF_LINK_FAILED) {
            print_err("dhcpc: link FAILED (controller fault)\n");
        } else {
            print_err("dhcpc: link DOWN\n");
        }
        return 1;
    }

    /* Fast probe / handshake */
    uint64_t timeout_sec = boot_mode ? 3 : 5;
    static dhcp_offer_t offer;
    int hret = dhcp_do_handshake(ifget.mac, timeout_sec, dry_run, &offer);

    if (hret != 0) {
        if (boot_mode) {
            write_file("/run/dhcpc.log", "eth0: no DHCP response; continuing in background\n", 50);
            spawn_daemon();
            return 0;
        }
        print_err("dhcpc: no DHCP offer/ack received (timeout)\n");
        return 1;
    }

    /* Dry run reporting */
    if (dry_run) {
        char msg[256];
        size_t pos = 0;
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

    /* Apply configuration */
    if (!apply_interface(&offer)) {
        print_err("dhcpc: failed to apply interface configuration (SYS_NETCTL NETCTL_IFSET)\n");
        return 1;
    }

    int64_t epoch = get_realtime_epoch();
    save_dhcpc_lease(&offer, epoch);
    save_resolv_conf(offer.dns_servers, offer.dns_count);
    save_network_conf(offer.yiaddr, offer.prefix, offer.router,
                      offer.dns_servers, offer.dns_count);

    sys_call(SYS_SYNC, 0, 0, 0, 0, 0, 0);
    dhcpc_log_lease("bound", &offer);

    /* Output announcement */
    char msg[256];
    size_t pos = 0;
    if (boot_mode) {
        append_str(msg, sizeof(msg), &pos, "eth0: DHCP lease acquired ");
        append_ip(msg, sizeof(msg), &pos, offer.yiaddr);
        append_str(msg, sizeof(msg), &pos, "/");
        append_u32(msg, sizeof(msg), &pos, offer.prefix);
        if (offer.router) {
            append_str(msg, sizeof(msg), &pos, ", gateway ");
            append_ip(msg, sizeof(msg), &pos, offer.router);
        }
        if (offer.dns_count > 0) {
            append_str(msg, sizeof(msg), &pos, ", DNS ");
            append_ip(msg, sizeof(msg), &pos, offer.dns_servers[0]);
        }
        append_str(msg, sizeof(msg), &pos, "\n");
        write_file("/run/dhcpc.log", msg, pos);

        spawn_daemon();
        return 0;
    } else {
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

        /* If daemon is running, signal it to update timers; else spawn daemon */
        int64_t dpid = read_dhcpc_pid();
        if (dpid > 0 && is_process_alive(dpid)) {
            sys_call(SYS_KILL, (uintptr_t)dpid, SIGHUP, 0, 0, 0, 0);
        } else {
            spawn_daemon();
        }
        return 0;
    }
}
