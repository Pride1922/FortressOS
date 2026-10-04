#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "idt.h"
#include "thread.h"
#include "../src/net/net.h"
#include "netctl_abi.h"
#include "syscall_abi.h"

/* Mock device and driver callbacks */
static net_dev_t dev;
static bool carrier = true;
static uint64_t mock_rx_pkts = 1234;
static uint64_t mock_tx_pkts = 567;
static uint32_t mock_link_state = NET_IF_LINK_ONLINE;

uint32_t e1000_link_state_abi(const net_dev_t *d) {
    (void)d;
    return mock_link_state;
}

void e1000_get_stats(const net_dev_t *d, uint64_t *rx, uint64_t *tx) {
    (void)d;
    if (rx) *rx = mock_rx_pkts;
    if (tx) *tx = mock_tx_pkts;
}

bool e1000_network_online(net_dev_t *d) {
    return d == &dev && carrier;
}

bool e1000_service_link(net_dev_t *d, uint64_t now, uint64_t hz) {
    (void)d; (void)now; (void)hz;
    return carrier;
}

size_t smp_get_cpu_count(void) { return 1; }
int64_t net_socket_syscall(interrupt_frame_t *frame) { (void)frame; return -14; }
void serial_puts(const char *s) { (void)s; }
void serial_print_hex(uint64_t n) { (void)n; }
uint64_t apic_timer_get_bsp_ticks(void) { return 100; }
uint64_t apic_timer_get_frequency(void) { return 100; }
uint64_t apic_poll_clock_hz(void) { return 0; }
uint64_t apic_poll_clock_read(void) { return 0; }
bool net_tcp_idle(void) { return true; }
bool net_tcp_receiving(void) { return false; }
void net_tcp_input(uint32_t source, const uint8_t *data, size_t len) {
    (void)source; (void)data; (void)len;
}
void net_tcp_tick(uint64_t ticks, bool online) { (void)ticks; (void)online; }
void net_socket_init(net_dev_t *d, const net_config_t *cfg) { (void)d; (void)cfg; }
void net_socket_enable(void) {}
void net_socket_worker_tick(uint64_t now, bool online) { (void)now; (void)online; }
void net_socket_input(uint32_t ip, uint16_t sp, uint16_t dp, const uint8_t *data, size_t len) {
    (void)ip; (void)sp; (void)dp; (void)data; (void)len;
}

static uint32_t s_mock_socket_local_ip = 0;
static uint32_t s_mock_tcp_local_ip = 0;
void net_socket_set_local(uint32_t ip) { s_mock_socket_local_ip = ip; }
void net_tcp_set_local_ip(uint32_t ip) { s_mock_tcp_local_ip = ip; }

tcb_t *thread_current(void) { static tcb_t worker; return &worker; }
net_dev_t *e1000_get_net_device(void) { return &dev; }
tcb_t *thread_create_on_cpu(size_t cpu, const char *name, void (*entry)(void *), void *arg) {
    (void)cpu; (void)name; (void)entry; (void)arg;
    return (tcb_t *)&dev;
}
void thread_yield(void) {}
void sched_wake_all(const void *channel) { (void)channel; }
void sched_wait_until(const void *channel, bool (*ready)(void *), void *arg) {
    (void)channel; (void)ready; (void)arg;
}

static int mock_send(net_dev_t *d, const void *buf, size_t len) {
    (void)d; (void)buf; (void)len;
    return 0;
}

static char s_captured_stdout[2048];
static size_t s_captured_stdout_len;
static char s_captured_stderr[2048];
static size_t s_captured_stderr_len;
static long s_mock_netctl_ret = 0;

static const char *s_mock_file_path = NULL;
static const char *s_mock_file_content = NULL;
static size_t s_mock_file_pos = 0;

static char s_captured_resolv[512];
static size_t s_captured_resolv_len = 0;

static long mock_ifconfig_syscall(long nr, uintptr_t a, uintptr_t b, uintptr_t c) {
    if (nr == SYS_MKDIR) {
        return 0;
    } else if (nr == SYS_WRITE) {
        int fd = (int)a;
        const char *buf = (const char *)b;
        size_t count = (size_t)c;
        if (fd == 1) {
            if (s_captured_stdout_len + count < sizeof(s_captured_stdout)) {
                memcpy(s_captured_stdout + s_captured_stdout_len, buf, count);
                s_captured_stdout_len += count;
                s_captured_stdout[s_captured_stdout_len] = '\0';
            }
            return (long)count;
        } else if (fd == 2) {
            if (s_captured_stderr_len + count < sizeof(s_captured_stderr)) {
                memcpy(s_captured_stderr + s_captured_stderr_len, buf, count);
                s_captured_stderr_len += count;
                s_captured_stderr[s_captured_stderr_len] = '\0';
            }
            return (long)count;
        } else if (fd == 4) {
            if (s_captured_resolv_len + count < sizeof(s_captured_resolv)) {
                memcpy(s_captured_resolv + s_captured_resolv_len, buf, count);
                s_captured_resolv_len += count;
                s_captured_resolv[s_captured_resolv_len] = '\0';
            }
            return (long)count;
        }
        return -1;
    } else if (nr == SYS_OPEN) {
        const char *path = (const char *)a;
        if (s_mock_file_path && strcmp(path, s_mock_file_path) == 0) {
            s_mock_file_pos = 0;
            return 3;
        }
        if (!strcmp(path, "/tmp/resolv.conf") || !strcmp(path, "/mnt/.fortress/resolv.conf")) {
            s_captured_resolv_len = 0;
            return 4;
        }
        return -1;
    } else if (nr == SYS_READ) {
        if (a == 3 && s_mock_file_content) {
            size_t total = strlen(s_mock_file_content);
            if (s_mock_file_pos >= total) return 0;
            size_t avail = total - s_mock_file_pos;
            size_t chunk = (c < avail) ? c : avail;
            memcpy((void *)b, s_mock_file_content + s_mock_file_pos, chunk);
            s_mock_file_pos += chunk;
            return (long)chunk;
        }
        return -1;
    } else if (nr == SYS_CLOSE) {
        if (a == 3 || a == 4) return 0;
        return -1;
    } else if (nr == SYS_NETCTL) {
        if (s_mock_netctl_ret != 0) return s_mock_netctl_ret;
        uint32_t cmd = (uint32_t)a;
        if (cmd == NETCTL_IFGET) {
            netctl_ifget_t *out = (netctl_ifget_t *)b;
            if (c != sizeof(netctl_ifget_t)) return SYSCALL_EINVAL;
            if (out->struct_version != 1 || out->reserved != 0) return SYSCALL_EINVAL;
            return net_get_ifconfig(out);
        } else if (cmd == NETCTL_IFSET) {
            netctl_ifset_t *in = (netctl_ifset_t *)b;
            if (c != sizeof(netctl_ifset_t)) return SYSCALL_EINVAL;
            net_config_t new_cfg;
            int vrc = net_validate_ifset(in, &new_cfg);
            if (vrc != 0) return vrc;
            net_set_config(&new_cfg);
            return 0;
        }
        return SYSCALL_EINVAL;
    }
    return -1;
}

#define IFCONFIG_SYSCALL mock_ifconfig_syscall
#include "../user/ifconfig.c"

#define NETCONF_SYSCALL mock_ifconfig_syscall
#include "../user/netconf.c"

#define IFUP_SYSCALL mock_ifconfig_syscall
#include "../user/ifup.c"

int main(void) {
    /* 1. Compile-time & runtime ABI struct verification */
    assert(sizeof(netctl_ifget_t) == 64);
    assert(sizeof(netctl_ifset_t) == 32);

    netctl_ifget_t req;
    memset(&req, 0, sizeof(req));
    req.struct_version = 1;
    req.reserved = 0;

    /* 2. Absent device check */
    int rc = net_get_ifconfig(&req);
    assert(rc == SYSCALL_EIO);

    /* Null pointer check */
    assert(net_get_ifconfig(NULL) == SYSCALL_EINVAL);

    /* 3. Initialize interface */
    memset(&dev, 0, sizeof(dev));
    memcpy(dev.name, "eth0", 5);
    dev.mac_addr[0] = 0xc8;
    dev.mac_addr[1] = 0xf7;
    dev.mac_addr[2] = 0x50;
    dev.mac_addr[3] = 0x0e;
    dev.mac_addr[4] = 0x35;
    dev.mac_addr[5] = 0x80;
    dev.mtu = 1500;
    dev.send_packet = mock_send;

    net_config_t cfg = {
        .local_ip = htonl(0xc0a800a8),  /* 192.168.0.168 */
        .gateway  = htonl(0xc0a80001),  /* 192.168.0.1 */
        .prefix   = 24
    };
    net_init(&dev, &cfg);

    /* 4. Query interface state */
    memset(&req, 0xaa, sizeof(req));
    req.struct_version = 1;
    req.reserved = 0;

    rc = net_get_ifconfig(&req);
    assert(rc == 0);
    assert(req.struct_version == 1);
    assert(req.reserved == 0);
    assert(memcmp(req.mac, dev.mac_addr, 6) == 0);
    assert(req.reserved2 == 0);
    assert(req.local_ipv4 == cfg.local_ip);
    assert(req.netmask_ipv4 == htonl(0xffffff00));
    assert(req.gateway_ipv4 == cfg.gateway);
    assert(req.mtu == 1500);
    assert(req.link_state == NET_IF_LINK_ONLINE);
    assert(req.reserved3 == 0);
    assert(req.rx_packets == 1234);
    assert(req.tx_packets == 567);
    assert(req.reserved4[0] == 0 && req.reserved4[1] == 0);

    /* 5. Link state mapping checks */
    mock_link_state = NET_IF_LINK_WAITING;
    rc = net_get_ifconfig(&req);
    assert(rc == 0 && req.link_state == NET_IF_LINK_WAITING);

    mock_link_state = NET_IF_LINK_DOWN;
    rc = net_get_ifconfig(&req);
    assert(rc == 0 && req.link_state == NET_IF_LINK_DOWN);

    mock_link_state = NET_IF_LINK_FAILED;
    rc = net_get_ifconfig(&req);
    assert(rc == 0 && req.link_state == NET_IF_LINK_FAILED);

    /* 6. Counters monotonic update */
    mock_rx_pkts = 1234;
    mock_tx_pkts = 567;
    rc = net_get_ifconfig(&req);
    assert(rc == 0);
    assert(req.rx_packets == 1234);
    assert(req.tx_packets == 567);

    /* 7. Test /bin/ifconfig Ring 3 tool output - ONLINE */
    mock_link_state = NET_IF_LINK_ONLINE;
    s_captured_stdout_len = 0;
    s_captured_stderr_len = 0;
    rc = ifconfig_main(1, NULL);
    assert(rc == 0);
    const char *expected_online =
        "eth0  HWaddr c8:f7:50:0e:35:80\n"
        "      inet 192.168.0.168/24  netmask 255.255.255.0  broadcast 192.168.0.255\n"
        "      gateway 192.168.0.1\n"
        "      mtu 1500\n"
        "      link UP\n"
        "      RX 1234  TX 567\n";
    assert(strcmp(s_captured_stdout, expected_online) == 0);

    /* 8. Test /bin/ifconfig Ring 3 tool output - WAITING */
    mock_link_state = NET_IF_LINK_WAITING;
    s_captured_stdout_len = 0;
    s_captured_stderr_len = 0;
    rc = ifconfig_main(1, NULL);
    assert(rc == 0);
    const char *expected_waiting =
        "eth0  HWaddr c8:f7:50:0e:35:80\n"
        "      link WAITING (no cable)\n";
    assert(strcmp(s_captured_stdout, expected_waiting) == 0);

    /* 9. Test /bin/ifconfig Ring 3 tool output - FAILED */
    mock_link_state = NET_IF_LINK_FAILED;
    s_captured_stdout_len = 0;
    s_captured_stderr_len = 0;
    rc = ifconfig_main(1, NULL);
    assert(rc == 0);
    const char *expected_failed =
        "eth0  HWaddr c8:f7:50:0e:35:80\n"
        "      link FAILED (controller fault; quarantine active)\n";
    assert(strcmp(s_captured_stdout, expected_failed) == 0);

    /* 10. Test /bin/ifconfig Ring 3 tool output - ERROR / UNAVAILABLE */
    s_mock_netctl_ret = SYSCALL_EIO;
    s_captured_stdout_len = 0;
    s_captured_stderr_len = 0;
    rc = ifconfig_main(1, NULL);
    assert(rc == 1);
    assert(strcmp(s_captured_stderr, "ifconfig: network interface unavailable\n") == 0);
    s_mock_netctl_ret = 0;
    mock_link_state = NET_IF_LINK_ONLINE;

    /* =====================================================================
     * 11. NETCTL_IFSET validation tests (net_validate_ifset)
     * ===================================================================== */
    netctl_ifset_t set_req;
    net_config_t parsed_cfg;

    /* Base valid request: 192.168.0.199/24, gw 192.168.0.1 */
    memset(&set_req, 0, sizeof(set_req));
    set_req.struct_version = 1;
    set_req.local_ipv4 = htonl(0xc0a800c7);  /* 192.168.0.199 */
    set_req.netmask_ipv4 = htonl(0xffffff00); /* 255.255.255.0 */
    set_req.gateway_ipv4 = htonl(0xc0a80001); /* 192.168.0.1 */

    assert(net_validate_ifset(NULL, &parsed_cfg) == SYSCALL_EINVAL);
    assert(net_validate_ifset(&set_req, NULL) == SYSCALL_EINVAL);

    /* Valid base request passes */
    assert(net_validate_ifset(&set_req, &parsed_cfg) == 0);
    assert(parsed_cfg.local_ip == set_req.local_ipv4);
    assert(parsed_cfg.prefix == 24);
    assert(parsed_cfg.gateway == set_req.gateway_ipv4);

    /* struct_version != 1 */
    set_req.struct_version = 2;
    assert(net_validate_ifset(&set_req, &parsed_cfg) == SYSCALL_EINVAL);
    set_req.struct_version = 1;

    /* flags != 0 */
    set_req.flags = 1;
    assert(net_validate_ifset(&set_req, &parsed_cfg) == SYSCALL_EINVAL);
    set_req.flags = 0;

    /* reserved fields != 0 */
    set_req.reserved = 1;
    assert(net_validate_ifset(&set_req, &parsed_cfg) == SYSCALL_EINVAL);
    set_req.reserved = 0;

    set_req.reserved2[0] = 1;
    assert(net_validate_ifset(&set_req, &parsed_cfg) == SYSCALL_EINVAL);
    set_req.reserved2[0] = 0;

    set_req.reserved2[1] = 1;
    assert(net_validate_ifset(&set_req, &parsed_cfg) == SYSCALL_EINVAL);
    set_req.reserved2[1] = 0;

    /* local_ipv4: 0.0.0.0 */
    set_req.local_ipv4 = 0;
    assert(net_validate_ifset(&set_req, &parsed_cfg) == SYSCALL_EINVAL);

    /* local_ipv4: loopback 127.0.0.1 */
    set_req.local_ipv4 = htonl(0x7f000001);
    assert(net_validate_ifset(&set_req, &parsed_cfg) == SYSCALL_EINVAL);

    /* local_ipv4: multicast 224.0.0.1 */
    set_req.local_ipv4 = htonl(0xe0000001);
    assert(net_validate_ifset(&set_req, &parsed_cfg) == SYSCALL_EINVAL);

    /* local_ipv4: 255.255.255.255 */
    set_req.local_ipv4 = 0xffffffffu;
    assert(net_validate_ifset(&set_req, &parsed_cfg) == SYSCALL_EINVAL);

    /* Restore local_ipv4 */
    set_req.local_ipv4 = htonl(0xc0a800c7);

    /* netmask: non-contiguous (e.g. 255.255.0.255) */
    set_req.netmask_ipv4 = htonl(0xffff00ff);
    assert(net_validate_ifset(&set_req, &parsed_cfg) == SYSCALL_EINVAL);

    /* netmask: prefix 0 (0.0.0.0) */
    set_req.netmask_ipv4 = 0;
    assert(net_validate_ifset(&set_req, &parsed_cfg) == SYSCALL_EINVAL);

    /* netmask: prefix 31 (255.255.255.254) */
    set_req.netmask_ipv4 = htonl(0xfffffffe);
    assert(net_validate_ifset(&set_req, &parsed_cfg) == SYSCALL_EINVAL);

    /* netmask: prefix 32 (255.255.255.255) */
    set_req.netmask_ipv4 = htonl(0xffffffff);
    assert(net_validate_ifset(&set_req, &parsed_cfg) == SYSCALL_EINVAL);

    /* Restore netmask */
    set_req.netmask_ipv4 = htonl(0xffffff00);

    /* Subnet network address as local_ipv4 (192.168.0.0/24) */
    set_req.local_ipv4 = htonl(0xc0a80000);
    assert(net_validate_ifset(&set_req, &parsed_cfg) == SYSCALL_EINVAL);

    /* Subnet broadcast address as local_ipv4 (192.168.0.255/24) */
    set_req.local_ipv4 = htonl(0xc0a800ff);
    assert(net_validate_ifset(&set_req, &parsed_cfg) == SYSCALL_EINVAL);

    /* Restore local_ipv4 */
    set_req.local_ipv4 = htonl(0xc0a800c7);

    /* gateway: 0 (valid: unconfigured gateway) */
    set_req.gateway_ipv4 = 0;
    assert(net_validate_ifset(&set_req, &parsed_cfg) == 0);
    assert(parsed_cfg.gateway == 0);

    /* gateway == local_ipv4 */
    set_req.gateway_ipv4 = set_req.local_ipv4;
    assert(net_validate_ifset(&set_req, &parsed_cfg) == SYSCALL_EINVAL);

    /* gateway: loopback */
    set_req.gateway_ipv4 = htonl(0x7f000001);
    assert(net_validate_ifset(&set_req, &parsed_cfg) == SYSCALL_EINVAL);

    /* gateway: multicast */
    set_req.gateway_ipv4 = htonl(0xe0000001);
    assert(net_validate_ifset(&set_req, &parsed_cfg) == SYSCALL_EINVAL);

    /* gateway: off-link (192.168.1.1 on 192.168.0.x/24) */
    set_req.gateway_ipv4 = htonl(0xc0a80101);
    assert(net_validate_ifset(&set_req, &parsed_cfg) == SYSCALL_EINVAL);

    /* gateway: subnet network address (192.168.0.0) */
    set_req.gateway_ipv4 = htonl(0xc0a80000);
    assert(net_validate_ifset(&set_req, &parsed_cfg) == SYSCALL_EINVAL);

    /* gateway: subnet broadcast address (192.168.0.255) */
    set_req.gateway_ipv4 = htonl(0xc0a800ff);
    assert(net_validate_ifset(&set_req, &parsed_cfg) == SYSCALL_EINVAL);

    /* Restore gateway */
    set_req.gateway_ipv4 = htonl(0xc0a80001);

    /* Link state != ONLINE returns EIO */
    mock_link_state = NET_IF_LINK_WAITING;
    assert(net_validate_ifset(&set_req, &parsed_cfg) == SYSCALL_EIO);

    mock_link_state = NET_IF_LINK_DOWN;
    assert(net_validate_ifset(&set_req, &parsed_cfg) == SYSCALL_EIO);

    mock_link_state = NET_IF_LINK_FAILED;
    assert(net_validate_ifset(&set_req, &parsed_cfg) == SYSCALL_EIO);

    mock_link_state = NET_IF_LINK_ONLINE;
    assert(net_validate_ifset(&set_req, &parsed_cfg) == 0);

    /* =====================================================================
     * 12. net_set_config application and multi-layer state sync
     * ===================================================================== */
    net_set_config(&parsed_cfg);
    assert(s_mock_socket_local_ip == parsed_cfg.local_ip);
    assert(s_mock_tcp_local_ip == parsed_cfg.local_ip);

    /* Query state via IFGET to verify change took effect */
    memset(&req, 0, sizeof(req));
    req.struct_version = 1;
    rc = net_get_ifconfig(&req);
    assert(rc == 0);
    assert(req.local_ipv4 == parsed_cfg.local_ip);
    assert(req.netmask_ipv4 == htonl(0xffffff00));
    assert(req.gateway_ipv4 == parsed_cfg.gateway);

    /* =====================================================================
     * 13. Config file parser tests (user/netconf.c)
     * ===================================================================== */
    netconf_t conf;
    char err_msg[256];

    /* Basic valid config */
    const char *cfg_valid =
        "# Configuration file\n"
        "address 192.168.0.168/24\n"
        "gateway 192.168.0.1\n"
        "dns     1.1.1.1\n";
    assert(netconf_parse_buffer(cfg_valid, strlen(cfg_valid), "test.conf", "test", false, &conf, err_msg, sizeof(err_msg)) == 0);
    assert(conf.has_address && conf.address == htonl(0xc0a800a8) && conf.prefix == 24);
    assert(conf.has_gateway && conf.gateway == htonl(0xc0a80001));
    assert(conf.has_dns && conf.dns == htonl(0x01010101));

    /* Key-value with '=' and CRLF */
    const char *cfg_crlf =
        "address=10.0.2.15/24\r\n"
        "gateway = 10.0.2.2\r\n"
        "dns= 8.8.8.8\r\n";
    assert(netconf_parse_buffer(cfg_crlf, strlen(cfg_crlf), "test.conf", "test", false, &conf, err_msg, sizeof(err_msg)) == 0);
    assert(conf.has_address && conf.address == htonl(0x0a00020f) && conf.prefix == 24);
    assert(conf.has_gateway && conf.gateway == htonl(0x0a000202));
    assert(conf.has_dns && conf.dns == htonl(0x08080808));

    /* Duplicate key: last wins */
    const char *cfg_dup =
        "address 10.0.0.1/8\n"
        "address 192.168.1.50/24\n";
    assert(netconf_parse_buffer(cfg_dup, strlen(cfg_dup), "test.conf", "test", false, &conf, err_msg, sizeof(err_msg)) == 0);
    assert(conf.address == htonl(0xc0a80132) && conf.prefix == 24);

    /* Unknown key: ignored */
    const char *cfg_unknown =
        "hostname myhost\n"
        "address 192.168.0.10/24\n";
    assert(netconf_parse_buffer(cfg_unknown, strlen(cfg_unknown), "test.conf", "test", false, &conf, err_msg, sizeof(err_msg)) == 0);
    assert(conf.has_address);

    /* Malformed address */
    const char *cfg_bad_addr = "address 192.168.0.x\n";
    assert(netconf_parse_buffer(cfg_bad_addr, strlen(cfg_bad_addr), "test.conf", "test", false, &conf, err_msg, sizeof(err_msg)) == -1);

    /* Oversized file > 4096 */
    char big_file[4100];
    memset(big_file, ' ', sizeof(big_file));
    assert(netconf_parse_buffer(big_file, sizeof(big_file), "big.conf", "test", false, &conf, err_msg, sizeof(err_msg)) == -1);

    /* Oversized line > 256 */
    char long_line[300];
    memset(long_line, 'a', 260);
    long_line[260] = '\n';
    long_line[261] = '\0';
    assert(netconf_parse_buffer(long_line, strlen(long_line), "long.conf", "test", false, &conf, err_msg, sizeof(err_msg)) == -1);

    /* =====================================================================
     * 14. /bin/ifup tool execution tests
     * ===================================================================== */
    /* --help */
    s_captured_stdout_len = 0;
    s_captured_stderr_len = 0;
    char *h_argv[] = {"ifup", "--help"};
    assert(ifup_main(2, h_argv) == 0);
    assert(strstr(s_captured_stderr, "usage: ifup") != NULL);

    /* CLI form: CIDR + gateway */
    s_captured_stdout_len = 0;
    s_captured_stderr_len = 0;
    char *cli_argv[] = {"ifup", "192.168.0.222/24", "192.168.0.1"};
    assert(ifup_main(3, cli_argv) == 0);
    assert(strcmp(s_captured_stdout, "eth0: address 192.168.0.222/24 gateway 192.168.0.1 applied\n") == 0);

    /* Verify kernel received it */
    req.struct_version = 1;
    assert(net_get_ifconfig(&req) == 0);
    assert(req.local_ipv4 == htonl(0xc0a800de));

    /* CLI form: dotted-decimal */
    s_captured_stdout_len = 0;
    s_captured_stderr_len = 0;
    char *cli_dotted[] = {"ifup", "192.168.0.223", "255.255.255.0", "192.168.0.1"};
    assert(ifup_main(4, cli_dotted) == 0);
    assert(strcmp(s_captured_stdout, "eth0: address 192.168.0.223/24 gateway 192.168.0.1 applied\n") == 0);

    /* CLI form: CIDR + gateway + 1 DNS */
    s_captured_stdout_len = 0;
    s_captured_stderr_len = 0;
    s_captured_resolv_len = 0;
    char *cli_dns1[] = {"ifup", "192.168.0.222/24", "192.168.0.1", "1.1.1.1"};
    assert(ifup_main(4, cli_dns1) == 0);
    assert(strcmp(s_captured_stdout, "eth0: address 192.168.0.222/24 gateway 192.168.0.1 applied\n") == 0);
    assert(strcmp(s_captured_resolv, "nameserver 1.1.1.1\n") == 0);

    /* CLI form: CIDR + gateway + 2 DNS */
    s_captured_stdout_len = 0;
    s_captured_stderr_len = 0;
    s_captured_resolv_len = 0;
    char *cli_dns2[] = {"ifup", "192.168.0.222/24", "192.168.0.1", "1.1.1.1", "8.8.8.8"};
    assert(ifup_main(5, cli_dns2) == 0);
    assert(strcmp(s_captured_stdout, "eth0: address 192.168.0.222/24 gateway 192.168.0.1 applied\n") == 0);
    assert(strcmp(s_captured_resolv, "nameserver 1.1.1.1\nnameserver 8.8.8.8\n") == 0);

    /* CLI form: dotted-decimal + gateway + 2 DNS */
    s_captured_stdout_len = 0;
    s_captured_stderr_len = 0;
    s_captured_resolv_len = 0;
    char *cli_dotted_dns[] = {"ifup", "192.168.0.223", "255.255.255.0", "192.168.0.1", "1.1.1.1", "8.8.8.8"};
    assert(ifup_main(6, cli_dotted_dns) == 0);
    assert(strcmp(s_captured_stdout, "eth0: address 192.168.0.223/24 gateway 192.168.0.1 applied\n") == 0);
    assert(strcmp(s_captured_resolv, "nameserver 1.1.1.1\nnameserver 8.8.8.8\n") == 0);

    /* CLI dry-run */
    s_captured_stdout_len = 0;
    s_captured_stderr_len = 0;
    char *cli_dry[] = {"ifup", "--dry-run", "192.168.0.224/24", "192.168.0.1"};
    assert(ifup_main(4, cli_dry) == 0);
    assert(strcmp(s_captured_stdout, "eth0: address 192.168.0.224/24 gateway 192.168.0.1 valid (dry-run)\n") == 0);
    /* Kernel unchanged */
    assert(net_get_ifconfig(&req) == 0);
    assert(req.local_ipv4 == htonl(0xc0a800df)); /* still 223 */

    /* Mock config file execution */
    s_mock_file_path = "/mnt/.fortress/network.conf";
    s_mock_file_content =
        "# Fortress Network Configuration\n"
        "address 192.168.0.250/24\n"
        "gateway 192.168.0.1\n"
        "dns 1.1.1.1\n";

    s_captured_stdout_len = 0;
    s_captured_stderr_len = 0;
    char *file_argv[] = {"ifup"};
    assert(ifup_main(1, file_argv) == 0);
    assert(strcmp(s_captured_stdout, "eth0: address 192.168.0.250/24 gateway 192.168.0.1 applied\n") == 0);

    /* Verify kernel received new address from file */
    assert(net_get_ifconfig(&req) == 0);
    assert(req.local_ipv4 == htonl(0xc0a800fa));

    /* Test netconf_read_dns helper */
    uint32_t dns_ip = 0;
    assert(netconf_read_dns("/mnt/.fortress/network.conf", &dns_ip) == 0);
    assert(dns_ip == htonl(0x01010101));

    puts("NETCTL_IFGET, NETCTL_IFSET, netconf parser & ifup/ifconfig host ASan/UBSan PASS: full validation suite, multi-layer sync, CIDR/dotted CLI, file parser, dry-run, and tool outputs verified");
    return 0;
}
