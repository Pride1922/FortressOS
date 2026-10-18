#include "dns.h"
#include "ntp.h"
#include "signal_abi.h"
#include "udp_common.h"
static dns_context_t context;
static dns_result_t resolved;
static uint8_t packet[512], request[48], nonce[8];
static volatile bool stopped;
/* The isolated test fixture uses an unprivileged host UDP port. */
#ifndef TEST_NTP_PORT
#define NTP_SERVER_PORT 123
#else
#define NTP_SERVER_PORT TEST_NTP_PORT
#endif
static void stop(int signal) {
    (void)signal;
    stopped = true;
}
static bool text(const char *s) { return udp_write(s); }
static bool clock_read(timespec_t *t) {
    return udp_call(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (uintptr_t)t, 0, 0, 0, 0) == 0;
}
static int64_t ns(const timespec_t *t) { return t->tv_sec * 1000000000LL + t->tv_nsec; }
static bool synchronize(const char *host) {
    uint32_t ip;
    if (!udp_ip(host, &ip)) {
        dns_context_init(&context);
        dns_options_t opts = {0};
        if (dns_resolve_ipv4(&context, &opts, host, udp_length(host), &resolved) != DNS_OK ||
            !resolved.address_count)
            return false;
        ip = resolved.addresses[0];
    }
    if (udp_call(SYS_GETRANDOM, (uintptr_t)nonce, 8, 0, 0, 0, 0) != 8)
        return false;
    ntp_request(request, nonce);
    long fd = udp_call(SYS_SOCKET, NET_AF_INET, NET_SOCK_DGRAM, 0, 0, 0, 0);
    if (fd < 0)
        return false;
    net_sockaddr_in_t server = {
        .family = NET_AF_INET, .port = __builtin_bswap16(NTP_SERVER_PORT), .address = ip};
    timespec_t start, end;
    bool ok = false;
    if (!clock_read(&start))
        goto done;
    if (udp_call(SYS_SENDTO, fd, (uintptr_t)request, 48, 0, (uintptr_t)&server, sizeof(server)) !=
        48)
        goto done;
    while (!stopped) {
        net_sockaddr_in_t source = {0};
        uint32_t size = sizeof(source);
        long n = udp_call(SYS_RECVFROM, fd, (uintptr_t)packet, sizeof(packet), NET_MSG_DONTWAIT,
                          (uintptr_t)&source, (uintptr_t)&size);
        if (!clock_read(&end))
            break;
        int64_t elapsed = ns(&end) - ns(&start);
        if (elapsed < 0 || elapsed > 5000000000LL)
            break;
        if (n >= 0 && size == sizeof(source) && source.family == NET_AF_INET &&
            source.address == ip && source.port == server.port) {
            timespec_t sample;
            if (ntp_sample(packet, (size_t)n, nonce, elapsed, &sample)) {
                ok = udp_call(SYS_CLOCK_SETTIME, CLOCK_REALTIME, (uintptr_t)&sample, 0, 0, 0, 0) ==
                     0;
                break;
            }
        } else if (n < 0 && n != SYSCALL_EAGAIN && n != SYSCALL_EINTR)
            break;
        timespec_t pause = {0, 20000000};
        udp_call(SYS_NANOSLEEP, (uintptr_t)&pause, 0, 0, 0, 0, 0);
    }
done:
    udp_call(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    return ok;
}
int ntpd_main(int argc, char **argv) {
    bool foreground = argc == 3 && udp_equal(argv[1], "-f");
    bool once = argc == 3 && udp_equal(argv[1], "--once");
    if (!foreground && !once) {
        text("usage: ntpd --once SERVER | ntpd -f SERVER\nSNTP over UDP 123; CAP_SYS_ADMIN "
             "required. No NTS authentication.\n");
        return argc == 2 && udp_equal(argv[1], "--help") ? 0 : 2;
    }
    long caps = udp_call(SYS_CAPGET, 0, 0, 0, 0, 0, 0);
    if (caps < 0 || !(caps & (1 << 8))) {
        text("ntpd: CAP_SYS_ADMIN required\n");
        return 1;
    }
    signal_action_t action = {.handler = (uintptr_t)stop};
    udp_call(SYS_SIGACTION, SIGTERM, (uintptr_t)&action, 0, 0, 0, 0);
    udp_call(SYS_SIGACTION, SIGINT, (uintptr_t)&action, 0, 0, 0, 0);
    do {
        bool ok = synchronize(argv[2]);
        text(ok ? "ntpd: UTC synchronized\n" : "ntpd: synchronization failed; clock unchanged\n");
        if (once)
            return ok ? 0 : 1;
        /* Sleep in bounded pieces for signal-responsive supervisor shutdown. */
        for (int i = 0; i < (ok ? 3600 : 60) && !stopped; i++) {
            timespec_t wait = {1, 0};
            udp_call(SYS_NANOSLEEP, (uintptr_t)&wait, 0, 0, 0, 0, 0);
        }
    } while (!stopped);
    return 0;
}
