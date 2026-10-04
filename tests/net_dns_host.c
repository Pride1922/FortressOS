/* Literal RFC wire vectors and actual resolver with deterministic syscalls.
 * No encoder is used to manufacture peer answers. Host-only evidence. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../user/dns_codec.h"
#include "syscall_abi.h"
#include "socket_abi.h"
static long mock_call(long,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t);
#define DNS_CALL mock_call
#include "../user/dns.c"
static dns_context_t ctx;
static dns_result_t out, untouched;
static uint8_t response[4096], query[272], tcp_bytes[4098];
static size_t response_len,query_len,tcp_at;
static unsigned mode,calls,sockets,closes,sends,receives,tcp_sends;
static uint64_t ticks,hz=100,observed_deadline;
/* example.test, compressed A owner, 192.0.2.7. */
static const uint8_t golden[]={0x12,0x34,0x81,0x80,0,1,0,1,0,0,0,0,
  7,'e','x','a','m','p','l','e',4,'t','e','s','t',0,0,1,0,1,
  0xc0,0x0c,0,1,0,1,0,0,0,60,0,4,192,0,2,7};
static const uint8_t alias_golden[]={0x12,0x34,0x81,0x80,0,1,0,2,0,0,0,0,
  5,'a','l','i','a','s',4,'t','e','s','t',0,0,1,0,1,
  0xc0,0x0c,0,5,0,1,0,0,0,60,0,14,7,'s','e','r','v','i','c','e',4,'t','e','s','t',0,
  0xc0,0x28,0,1,0,1,0,0,0,60,0,4,10,0,2,2};
static const char *s_mock_resolv = NULL;
static size_t s_mock_resolv_pos = 0;
static uint32_t last_send_server = 0;
static void reset(unsigned scenario) {
    dns_context_init(&ctx); memset(&out,0xa5,sizeof(out)); untouched=out;
    calls=sockets=closes=sends=receives=tcp_sends=0; ticks=13000;
    response_len=sizeof(golden); memcpy(response,golden,sizeof(golden));
    mode=scenario; tcp_at=0; observed_deadline=0; last_send_server=0;
}
static long mock_call(long nr,uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t e,uintptr_t f) {
    ++calls;
    if(nr==SYS_OPEN) {
        const char *path = (const char *)a;
        if(s_mock_resolv && (!strcmp(path,"/tmp/resolv.conf") || !strcmp(path,"/mnt/.fortress/resolv.conf"))) {
            s_mock_resolv_pos = 0;
            return 25;
        }
        return -1;
    }
    if(nr==SYS_READ) {
        if(a==25 && s_mock_resolv) {
            size_t total = strlen(s_mock_resolv);
            if(s_mock_resolv_pos >= total) return 0;
            size_t avail = total - s_mock_resolv_pos;
            size_t chunk = (c < avail) ? c : avail;
            memcpy((void *)b, s_mock_resolv + s_mock_resolv_pos, chunk);
            s_mock_resolv_pos += chunk;
            return (long)chunk;
        }
        return -1;
    }
    if(nr==SYS_SYSINFO) { sysinfo_t info={.uptime_ticks=ticks,.tick_hz=hz}; memcpy((void *)a,&info,sizeof(info)); return 0; }
    if(nr==SYS_SOCKET) { ++sockets; assert(b&NET_SOCK_CLOEXEC); return c==17 ? 10 : 11; }
    if(nr==SYS_CLOSE) { if(a==25) return 0; assert(a==10 || a==11); ++closes; return 0; }
    if(nr==SYS_SENDTO) {
        ++sends; assert(a==10 && c<=272 && d==0 && f==16);
        const net_sockaddr_in_t *s=(void *)e; assert(s->port==__builtin_bswap16(53));
        last_send_server=s->address;
        query_len=c; memcpy(query,(void *)b,c); response[0]=query[0]; response[1]=query[1];
        if(mode==10) {
            memcpy(response,query,query_len); response[2]=0x81; response[3]=0x80; response[7]=1;
            if(sends==1) {
                memcpy(response+query_len,alias_golden+28,26); response_len=query_len+26;
            } else {
                assert(!memcmp(query+12,alias_golden+40,14));
                memcpy(response+query_len,golden+30,16); response_len=query_len+16;
            }
        }
        return (long)c;
    }
    if(nr==SYS_RECVFROM) {
        ++receives; assert(a==10 && c==1472 && !d);
        if(mode==1) { ticks+=5*hz; return SYSCALL_EAGAIN; }
        if(mode==2) return SYSCALL_EINTR;
        if(mode==20) {
            if(last_send_server == __builtin_bswap32(0x01010101)) { ticks+=5*hz; return SYSCALL_EAGAIN; }
            net_sockaddr_in_t s={.family=2,.port=__builtin_bswap16(53),.address=__builtin_bswap32(0x08080808)};
            memcpy((void *)e,&s,16); *(uint32_t *)f=16;
            memcpy((void *)b,response,response_len);
            return (long)response_len;
        }
        if(mode==21) { ticks+=5*hz; return SYSCALL_EAGAIN; }
        net_sockaddr_in_t s={.family=2,.port=__builtin_bswap16(53),.address=__builtin_bswap32(0xc0000201)};
        memcpy((void *)e,&s,16); *(uint32_t *)f=16;
        if(mode==3 || (mode>=5 && mode<=9) || mode==12) { response[2]|=2; response_len=query_len; }
        memcpy((void *)b,response,response_len);
        if(mode==4) ((uint8_t *)b)[1]^=1;
        if(mode==11 && receives==1) ((uint8_t *)b)[13]='z'; /* wrong question, matching ID */
        if(mode==13 && receives==1) ((net_sockaddr_in_t *)e)->port=__builtin_bswap16(54);
        if(mode==10) ticks+=2*hz;
        if(mode==8) ticks+=31*hz;
        return (long)response_len;
    }
    if(nr==SYS_CONNECT_UNTIL) {
        assert(a==11 && c==16); observed_deadline=d;
        if(mode==5) return SYSCALL_EAGAIN;
        if(mode==6) return SYSCALL_ETIMEDOUT;
        tcp_bytes[0]=0; tcp_bytes[1]=sizeof(golden); memcpy(tcp_bytes+2,golden,sizeof(golden));
        tcp_bytes[2]=query[0]; tcp_bytes[3]=query[1]; return 0;
    }
    if(nr==SYS_SEND_UNTIL) { assert(e==observed_deadline && !d); ++tcp_sends; return c>3 ? 3 : (long)c; }
    if(nr==SYS_RECV_UNTIL) {
        assert(e==observed_deadline && !d);
        if(mode==7) return 0;
        if(mode==9) { tcp_bytes[0]=0x10; tcp_bytes[1]=1; } /* cap + 1 */
        if(mode==12) { tcp_bytes[0]=0; tcp_bytes[1]=11; } /* below header size */
        if(tcp_at>=sizeof(golden)+2) return 0;
        size_t n=c>1 ? 1 : c; memcpy((void *)b,tcp_bytes+tcp_at,n); tcp_at+=n; ++ticks;
        return (long)n;
    }
    assert(!"unexpected DNS syscall"); return -999;
}
static int resolve(void) {
    dns_options_t options={.server_ipv4=__builtin_bswap32(0xc0000201)};
    return dns_resolve_ipv4(&ctx,&options,"example.test",12,&out);
}
static int resolve_auto(void) {
    dns_options_t options={0};
    return dns_resolve_ipv4(&ctx,&options,"example.test",12,&out);
}
static void codec(void) {
    uint8_t *w=(uint8_t *)&ctx; unsigned hops=0,visited=1;
    dns_context_init(&ctx); dns_copy(w+DNS_CURRENT,"example.test",13);
    dns_copy(w+DNS_VISITED,"example.test",13); memcpy(w+DNS_WIRE,golden,sizeof(golden));
    assert(dns_decode(w,sizeof(golden),0x1234,&hops,&visited)==DNS_OK);
    memcpy(&out,w+DNS_STAGE,sizeof(out)); assert(out.address_count==1 && !strcmp(out.canonical_name,"example.test"));
    memcpy(w+DNS_WIRE+46,golden+30,16); w[DNS_WIRE+7]=2;
    assert(!dns_decode(w,62,0x1234,&hops,&visited));
    memcpy(&out,w+DNS_STAGE,sizeof(out)); assert(out.address_count==1); /* dedup */
    w[DNS_WIRE+61]=8;
    assert(!dns_decode(w,62,0x1234,&hops,&visited));
    memcpy(&out,w+DNS_STAGE,sizeof(out)); assert(out.address_count==2);
    assert(((uint8_t *)out.addresses)[3]==7 && ((uint8_t *)out.addresses)[7]==8);
    memcpy(w+DNS_WIRE,golden,sizeof(golden)); w[DNS_WIRE+31]=20; /* unrelated owner "test" */
    assert(dns_decode(w,sizeof(golden),0x1234,&hops,&visited)==DNS_NO_DATA);
    memcpy(w+DNS_WIRE,golden,sizeof(golden)); w[DNS_WIRE+7]=0; w[DNS_WIRE+11]=1;
    assert(dns_decode(w,sizeof(golden),0x1234,&hops,&visited)==DNS_NO_DATA); /* additional A */
    memcpy(w+DNS_WIRE,golden,sizeof(golden)); w[DNS_WIRE+13]='E';
    assert(!dns_decode(w,sizeof(golden),0x1234,&hops,&visited)); /* wire case */
    memcpy(w+DNS_WIRE,golden,sizeof(golden)); w[DNS_WIRE+33]=41;
    assert(dns_decode(w,sizeof(golden),0x1234,&hops,&visited)==DNS_UNSUPPORTED);
    memcpy(w+DNS_WIRE,golden,sizeof(golden));
    for(size_t n=0;n<sizeof(golden);++n) {
        hops=0; visited=1; dns_copy(w+DNS_CURRENT,"example.test",13);
        assert(dns_decode(w,n,0x1234,&hops,&visited)!=DNS_OK);
    }
    memcpy(w+DNS_WIRE,golden,sizeof(golden)); w[DNS_WIRE+30]=0xc0; w[DNS_WIRE+31]=30;
    assert(dns_decode(w,sizeof(golden),0x1234,&hops,&visited)==DNS_MALFORMED);
    memcpy(w+DNS_WIRE,golden,sizeof(golden)); w[DNS_WIRE+7]=65;
    assert(dns_decode(w,sizeof(golden),0x1234,&hops,&visited)==DNS_LIMIT);
    memcpy(w+DNS_WIRE,alias_golden,sizeof(alias_golden));
    dns_copy(w+DNS_CURRENT,"alias.test",11); dns_copy(w+DNS_VISITED,"alias.test",11); hops=0; visited=1;
    assert(!dns_decode(w,sizeof(alias_golden),0x1234,&hops,&visited) && hops==1 && visited==2);
    memcpy(&out,w+DNS_STAGE,sizeof(out)); assert(!strcmp(out.canonical_name,"service.test") && out.address_count==1);
    /* Same-wire CNAME loop: alias.test -> alias.test via question pointer. */
    memcpy(w+DNS_WIRE,alias_golden,40); w[DNS_WIRE+7]=1; w[DNS_WIRE+39]=2;
    w[DNS_WIRE+40]=0xc0; w[DNS_WIRE+41]=12;
    dns_copy(w+DNS_CURRENT,"alias.test",11); hops=0; visited=1;
    assert(dns_decode(w,42,0x1234,&hops,&visited)==DNS_MALFORMED);
    dns_copy(w+DNS_CURRENT,"example.test",13); dns_copy(w+DNS_VISITED,"example.test",13);
    memcpy(w+DNS_WIRE,golden,sizeof(golden)); w[DNS_WIRE+3]=0x83; hops=0; visited=1;
    assert(dns_decode(w,sizeof(golden),0x1234,&hops,&visited)==DNS_NXDOMAIN);
    memcpy(w+DNS_WIRE,golden,30); w[DNS_WIRE+7]=9;
    for(unsigned i=0;i<9;++i) { memcpy(w+DNS_WIRE+30+16*i,golden+30,16); w[DNS_WIRE+45+16*i]=(uint8_t)i; }
    assert(dns_decode(w,30+9*16,0x1234,&hops,&visited)==DNS_LIMIT);
    char name[256]; assert(!dns_normalize("ExAmPlE.TeSt.",13,name) && !strcmp(name,"example.test"));
    assert(dns_normalize("-bad.test",9,name)==DNS_INVALID);
    char maximum[255]; unsigned pos=0;
    for(unsigned label=0;label<4;++label) {
        unsigned size=label==3 ? 61 : 63; memset(maximum+pos,'a',size); pos+=size;
        if(label<3) maximum[pos++]='.';
    }
    maximum[pos]=0; assert(pos==253 && !dns_normalize(maximum,pos,name));
    size_t maxlen; assert(!dns_encode(name,0x1234,query,&maxlen) && maxlen==271);
    size_t n; assert(!dns_encode("example.test",0x1234,query,&n));
    assert(n==30 && query[0]==0x12 && query[1]==0x34 && query[2]==1 && query[5]==1);
    /* Deterministic hostile inputs: no successful output is inferred from fuzz. */
    uint32_t seed=1;
    for(unsigned i=0;i<20000;++i) {
        size_t len=12+i%100; for(size_t j=0;j<len;++j) { seed=seed*1664525+1013904223; w[DNS_WIRE+j]=(uint8_t)(seed>>24); }
        hops=0; visited=1; (void)dns_decode(w,len,0x1234,&hops,&visited);
    }
}
int main(void) {
    codec();
    for(unsigned frequency=0;frequency<2;++frequency) {
        hz=frequency ? 1000 : 100;
        reset(0); assert(resolve()==DNS_OK && out.address_count==1 && closes==1);
        reset(1); assert(resolve()==DNS_TIMEOUT && sends==3 && closes==1 && !memcmp(&out,&untouched,sizeof(out)));
        reset(2); assert(resolve()==DNS_INTERRUPTED && dns_last_syscall_error(&ctx)==SYSCALL_EINTR && closes==1);
        reset(3); assert(resolve()==DNS_OK && out.flags==DNS_RESULT_TCP && closes==2 && tcp_sends>2);
        assert(observed_deadline==13000+30*hz);
        reset(4); assert(resolve()==DNS_LIMIT && receives==33 && closes==1);
        reset(5); assert(resolve()==DNS_TCP_QUIET && sockets==2 && closes==2);
        reset(6); assert(resolve()==DNS_TIMEOUT && closes==2);
        reset(7); assert(resolve()==DNS_IO && closes==2);
        reset(8); assert(resolve()==DNS_TIMEOUT && sockets==1 && closes==1);
        reset(9); assert(resolve()==DNS_LIMIT && tcp_at==2 && closes==2 && !memcmp(&out,&untouched,sizeof(out)));
        reset(10); assert(resolve()==DNS_OK && sends==2 && closes==2 && !strcmp(out.canonical_name,"service.test"));
        reset(11); assert(resolve()==DNS_OK && receives==2 && sends==1);
        reset(12); assert(resolve()==DNS_MALFORMED && tcp_at==2 && closes==2);
        reset(13); assert(resolve()==DNS_OK && receives==2 && sends==1);
    }
    reset(0); dns_options_t opt={0};
    assert(!dns_resolve_ipv4(&ctx,&opt,"192.000.2.7",11,&out) && !calls && out.flags==DNS_RESULT_NUMERIC);
    opt.reserved=1; assert(dns_resolve_ipv4(&ctx,&opt,"example.test",12,&out)==DNS_INVALID && !calls);
    assert(dns_resolve_ipv4(&ctx,&opt,"192.0.2.7",9,&out)==DNS_INVALID && !calls);
    dns_context_init(&ctx); assert(!dns_last_syscall_error(&ctx));
    put32((uint8_t *)&ctx,4,1); error((uint8_t *)&ctx,SYSCALL_EINTR);
    assert(dns_resolve_ipv4(&ctx,&opt,"example.test",12,&out)==DNS_BUSY && dns_last_syscall_error(&ctx)==SYSCALL_EINTR);
    put32((uint8_t *)&ctx,4,0); opt.reserved=0;
    assert(!dns_resolve_ipv4(&ctx,&opt,"192.0.2.7",9,&out) && !dns_last_syscall_error(&ctx));

    /* Test resolv.conf reading and server fallback (server 1 times out, server 2 succeeds) */
    s_mock_resolv = "nameserver 1.1.1.1\nnameserver 8.8.8.8\n";
    reset(20);
    assert(resolve_auto() == DNS_OK);
    assert(out.address_count == 1);
    assert(dns_server_used(&ctx) == __builtin_bswap32(0x08080808));
    assert(dns_servers_configured(&ctx) == 2);

    /* Test all servers timeout in resolv.conf */
    reset(21);
    assert(resolve_auto() == DNS_TIMEOUT);
    assert(dns_servers_configured(&ctx) == 2);

    /* Test missing resolv.conf */
    s_mock_resolv = NULL;
    reset(22);
    assert(resolve_auto() == DNS_TIMEOUT);
    assert(dns_servers_configured(&ctx) == 0);

    puts("DNS host PASS: literal vectors, bounded codec/fuzz, actual UDP/TCP resolver, exact deadlines at 100/1000 Hz, noise, interruption/quiet/EOF/cleanup/numeric bypass");
    return 0;
}
