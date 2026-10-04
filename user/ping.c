#include "types.h"
#include "syscall_abi.h"
#include "dns.h"
#include "dns_codec.h"

static net_ping_v1_t s_ping;
static dns_context_t s_dns_context;
static dns_result_t s_dns_result;
static char s_text[256];
static size_t s_pos;
static long call(long number, uintptr_t a, uintptr_t b, uintptr_t c) {
    __asm__ volatile("syscall" : "+a"(number) : "D"(a), "S"(b), "d"(c) : "rcx","r11","memory","cc");
    return number;
}
static void str(const char *p) { while (*p && s_pos<sizeof(s_text)-1) s_text[s_pos++]=*p++; }
static void num(uint64_t n) {
    if (n>=10) num(n/10);
    if (s_pos<sizeof(s_text)-1) s_text[s_pos++]=(char)('0'+n%10);
}
static bool print(int fd) {
    size_t pos=0;
    while (pos<s_pos) {
        long n=call(SYS_WRITE,(uintptr_t)fd,(uintptr_t)(s_text+pos),s_pos-pos);
        if (n<=0) return false;
        pos+=(size_t)n;
    }
    s_pos=0; return true;
}
static bool equal(const char *a, const char *b) { while (*a && *a==*b) { ++a; ++b; } return *a==*b; }
static bool integer(const char **p, unsigned limit, unsigned *out) {
    unsigned n=0, digits=0;
    while (**p>='0' && **p<='9') {
        if (++digits>3) return false;
        n=n*10+(unsigned)(*(*p)++-'0'); if (n>limit) return false;
    }
    *out=n; return digits!=0;
}
static bool address(const char *p, uint32_t *out) {
    uint32_t value=0;
    for (unsigned i=0; i<4; ++i) {
        unsigned octet;
        if (!integer(&p,255,&octet)) return false;
        value=(value<<8)|octet;
        if (i<3 && *p++!='.') return false;
    }
    if (*p) return false;
    *out=__builtin_bswap32(value); return true;
}
int ping_main(int argc, char **argv) {
    unsigned count=4, timeout=1, received=0, attempted=0;
    uint64_t sum=0, minimum=~0ull, maximum=0;
    const char *dest=NULL;
    for (int i=1; i<argc; ++i) {
        if (equal(argv[i],"-c") || equal(argv[i],"-W")) {
            bool is_count=equal(argv[i],"-c"); unsigned value;
            if (++i>=argc) goto usage;
            const char *p=argv[i];
            if (!integer(&p,is_count ? 100 : 5,&value) || *p || !value) goto usage;
            if (is_count) count=value; else timeout=value;
        } else if (!dest && argv[i][0]!='-') dest=argv[i];
        else goto usage;
    }
    if (!dest) goto usage;

    uint32_t ip = 0;
    bool is_hostname = false;
    char ip_str[16];

    if (address(dest, &ip)) {
        is_hostname = false;
    } else {
        is_hostname = true;
        dns_options_t options = {0};
        dns_context_init(&s_dns_context);
        size_t len = 0;
        while (dest[len]) len++;
        int status = dns_resolve_ipv4(&s_dns_context, &options, dest, len, &s_dns_result);
        if (status != DNS_OK || s_dns_result.address_count == 0) {
            str("ping: cannot resolve ");
            str(dest);
            str("\n");
            print(2);
            return 1;
        }
        ip = s_dns_result.addresses[0];
        dns_format_ipv4(ip, ip_str);
    }

    if (is_hostname) {
        str("PING "); str(dest); str(" ("); str(ip_str); str(") (32 data bytes)\n");
    } else {
        str("PING "); str(dest); str(" (32 data bytes)\n");
    }
    if (!print(1)) return 1;

    const char *reply_host = is_hostname ? ip_str : dest;
    for (unsigned i=0; i<count; ++i) {
        s_ping=(net_ping_v1_t){.version=1,.destination=ip,.timeout_seconds=timeout,.sequence=i+1,.start_delay_ms=i ? 1000 : 0};
        long ret=call(SYS_NETCTL,NETCTL_PING,(uintptr_t)&s_ping,sizeof(s_ping));
        if (ret<0) { str("ping: control error "); num((uint64_t)-ret); str("\n"); print(2); return 1; }
        ++attempted;
        if (s_ping.outcome==NETPING_REPLY && s_ping.tick_hz) {
            uint64_t ms=s_ping.rtt_ticks*1000/s_ping.tick_hz;
            ++received; sum+=ms; if (ms<minimum) minimum=ms; if (ms>maximum) maximum=ms;
            num(s_ping.echoed_bytes); str(" bytes from "); str(reply_host); str(": icmp_seq="); num(i+1); str(" time="); num(ms); str(" ms\n");
        } else {
            str("icmp_seq="); num(i+1);
            str(s_ping.outcome==NETPING_ARP_TIMEOUT ? " ARP timeout\n" : s_ping.outcome==NETPING_ECHO_TIMEOUT ? " echo timeout\n" : " transmit failed\n");
        }
        if (!print(1)) return 1;
    }
    num(attempted); str(" probes, "); num(received); str(" replies, "); num((attempted-received)*100/attempted); str("% loss\n");
    if (received) { str("rtt min/avg/max = "); num(minimum); str("/"); num(sum/received); str("/"); num(maximum); str(" ms\n"); }
    if (!print(1)) return 1;
    return received ? 0 : 1;
usage:
    str("usage: ping [-c 1..100] [-W 1..5] <IPv4|hostname>\n"); print(2); return 2;
}
