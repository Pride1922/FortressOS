#include "dns_codec.h"
#include "udp_common.h"
#ifndef DNS_CALL
#define DNS_CALL udp_call
#endif
#include "resolv_conf.h"
#define DNS_MAGIC 0x444e5301u
/* Character access to declared uint64_t storage is legal C11 aliasing. */
static uint32_t get32(const uint8_t *w,unsigned at) { uint32_t n; dns_copy(&n,w+at,4); return n; }
static void put32(uint8_t *w,unsigned at,uint32_t n) { dns_copy(w+at,&n,4); }
static int64_t error(uint8_t *w,long result) {
    if(result<0) dns_copy(w+16,&result,8);
    return result;
}
void dns_context_init(dns_context_t *ctx) {
    dns_zero(ctx,sizeof(*ctx)); put32((uint8_t *)ctx,0,DNS_MAGIC);
}
int64_t dns_last_syscall_error(const dns_context_t *ctx) {
    int64_t result; dns_copy(&result,(const uint8_t *)ctx+16,8); return result;
}
const char *dns_status_name(int status) {
    static const char *const names[]={"DNS_OK","DNS_INVALID","DNS_BUSY","DNS_TIMEOUT",
        "DNS_INTERRUPTED","DNS_NXDOMAIN","DNS_NO_DATA","DNS_SERVER_FAILURE",
        "DNS_MALFORMED","DNS_LIMIT","DNS_IO","DNS_UNSUPPORTED","DNS_TCP_QUIET"};
    return status>=0 && status<=DNS_TCP_QUIET ? names[status] : "DNS_INVALID";
}
static int failure(long r) {
    if(r==SYSCALL_EINTR) return DNS_INTERRUPTED;
    if(r==SYSCALL_ETIMEDOUT) return DNS_TIMEOUT;
    return DNS_IO;
}
static int clock_read(uint8_t *w,uint64_t *now,uint64_t *hz) {
    sysinfo_t info;
    long r=DNS_CALL(SYS_SYSINFO,(uintptr_t)&info,0,0,0,0,0);
    if(error(w,r)<0) return failure(r);
    if(!info.tick_hz) return DNS_IO;
    *now=info.uptime_ticks; *hz=info.tick_hz; return DNS_OK;
}
static int budget(uint8_t *w,uint64_t deadline,uint64_t expected_hz) {
    uint64_t now,hz; int status=clock_read(w,&now,&hz); if(status) return status;
    if(hz!=expected_hz) return DNS_IO;
    return now>=deadline ? DNS_TIMEOUT : DNS_OK;
}
static long closefd(uint8_t *w,long fd) {
    int64_t saved=dns_last_syscall_error((const dns_context_t *)w);
    long r=DNS_CALL(SYS_CLOSE,(uintptr_t)fd,0,0,0,0,0);
    if(r<0 && !saved) error(w,r);
    return r;
}
static int tcp_transfer(uint8_t *w,long fd,bool receive,uint8_t *data,size_t len,
                        uint64_t deadline,uint64_t hz) {
    size_t at=0;
    while(at<len) {
        int status=budget(w,deadline,hz); if(status) return status;
        long r=DNS_CALL(receive ? SYS_RECV_UNTIL : SYS_SEND_UNTIL,
            (uintptr_t)fd,(uintptr_t)(data+at),len-at,0,deadline,0);
        if(error(w,r)<0) return failure(r);
        status=budget(w,deadline,hz); if(status) return status;
        if(!r || (size_t)r>len-at) return DNS_IO;
        at+=(size_t)r;
    }
    return DNS_OK;
}
static int tcp_query(uint8_t *w,const net_sockaddr_in_t *server,size_t query_len,
                     uint16_t id,uint64_t deadline,uint64_t hz,unsigned *hops,unsigned *visited) {
    int status=budget(w,deadline,hz); if(status) return status;
    long fd=DNS_CALL(SYS_SOCKET,NET_AF_INET,NET_SOCK_STREAM|NET_SOCK_CLOEXEC,6,0,0,0);
    if(error(w,fd)<0) return failure(fd);
    status=budget(w,deadline,hz);
    if(!status) {
        long r=DNS_CALL(SYS_CONNECT_UNTIL,(uintptr_t)fd,(uintptr_t)server,16,deadline,0,0);
        if(error(w,r)<0) status=r==SYSCALL_EAGAIN ? DNS_TCP_QUIET : failure(r);
        else status=budget(w,deadline,hz);
    }
    uint8_t prefix[2]={(uint8_t)(query_len>>8),(uint8_t)query_len};
    if(!status) status=tcp_transfer(w,fd,false,prefix,2,deadline,hz);
    if(!status) status=tcp_transfer(w,fd,false,w+DNS_QUERY,query_len,deadline,hz);
    if(!status) status=tcp_transfer(w,fd,true,prefix,2,deadline,hz);
    size_t len=(size_t)prefix[0]*256+prefix[1];
    if(!status && len<12) status=DNS_MALFORMED;
    if(!status && len>4096) status=DNS_LIMIT;
    if(!status) status=tcp_transfer(w,fd,true,w+DNS_WIRE,len,deadline,hz);
    if(!status) {
        status=dns_decode(w,len,id,hops,visited);
        if(status==DNS_TRUNCATED || status==DNS_UNMATCHED) status=DNS_MALFORMED;
    }
    closefd(w,fd); return status;
}
static bool unicast(uint32_t ip) {
    uint8_t b[4]; dns_copy(b,&ip,4);
    return b[0] && b[0]!=127 && b[0]<224 && ip!=UINT32_MAX;
}
uint32_t dns_server_used(const dns_context_t *ctx) {
    return ctx ? get32((const uint8_t *)ctx, 24) : 0;
}
uint32_t dns_servers_configured(const dns_context_t *ctx) {
    return ctx ? get32((const uint8_t *)ctx, 28) : 0;
}
static unsigned parse_resolv_file(uint8_t *w, const char *path, uint32_t *servers, unsigned max_servers) {
    long fd = DNS_CALL(SYS_OPEN, (uintptr_t)path, 0, 0, 0, 0, 0);
    if (fd < 0) return 0;
    char buf[512];
    long n = DNS_CALL(SYS_READ, (uintptr_t)fd, (uintptr_t)buf, sizeof(buf) - 1, 0, 0, 0);
    closefd(w, fd);
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
            if (ip_len > 0 && dns_ipv4(line + ip_start, ip_len, &ip) && unicast(ip)) {
                servers[count++] = ip;
            }
        }
    }
    return count;
}
static unsigned load_dns_servers(uint8_t *w, uint32_t *servers, unsigned max_servers) {
    return parse_resolv_file(w, resolv_conf_path(), servers, max_servers);
}
int dns_resolve_ipv4(dns_context_t *ctx,const dns_options_t *opt,
                     const char *name,size_t n,dns_result_t *result) {
    uint8_t *w=(uint8_t *)ctx;
    if(get32(w,0)!=DNS_MAGIC) return DNS_INVALID;
    if(get32(w,4)) return DNS_BUSY;
    put32(w,4,1); dns_zero(w+16,8);
    put32(w,24,0); put32(w,28,0);
    int status=DNS_INVALID; long fd=-1; bool tcp_used=false,numeric_bypass=false;
    uint64_t now=0,hz=0,deadline=opt->deadline_ticks;
    if(opt->reserved || !n || n>254) goto done;
    uint32_t numeric;
    if(dns_ipv4(name,n,&numeric)) {
        dns_zero(w+DNS_STAGE,296); dns_format_ipv4(numeric,(char *)w+DNS_STAGE);
        dns_copy(w+DNS_STAGE+256,&numeric,4); put32(w,DNS_STAGE+288,1);
        put32(w,DNS_STAGE+292,DNS_RESULT_NUMERIC); numeric_bypass=true; status=DNS_OK; goto publish;
    }
    bool digits=true;
    for(size_t i=0;i<n;++i) if((name[i]<'0' || name[i]>'9') && name[i]!='.') digits=false;
    if(digits || dns_normalize(name,n,(char *)w+DNS_CURRENT)) goto done;

    uint32_t servers[4];
    unsigned server_count = 0;
    if (opt->server_ipv4 != 0) {
        if (!unicast(opt->server_ipv4)) goto done;
        servers[0] = opt->server_ipv4;
        server_count = 1;
    } else {
        server_count = load_dns_servers(w, servers, 4);
        if (server_count == 0) {
            status = DNS_TIMEOUT;
            goto done;
        }
    }
    put32(w, 28, server_count);

    status = clock_read(w, &now, &hz);
    if (status) goto done;
    if (hz > UINT64_MAX / 30 || (!deadline && now > UINT64_MAX - 30 * hz)) {
        status = DNS_INVALID;
        goto done;
    }
    if (!deadline) deadline = now + 30 * hz;
    if (deadline <= now) {
        status = DNS_TIMEOUT;
        goto done;
    }
    if (deadline - now > 30 * hz) {
        status = DNS_INVALID;
        goto done;
    }

    char initial_name[DNS_NAME_CAP];
    dns_copy(initial_name, w + DNS_CURRENT, dns_length((char *)w + DNS_CURRENT) + 1);

    for (unsigned s_idx = 0; s_idx < server_count; s_idx++) {
        uint32_t current_server = servers[s_idx];
        net_sockaddr_in_t server = {.family = NET_AF_INET, .port = __builtin_bswap16(53), .address = current_server};

        dns_copy(w + DNS_CURRENT, initial_name, dns_length(initial_name) + 1);
        unsigned hops = 0, visited = 1, noise = 0;
        dns_zero(w + DNS_VISITED, 9 * 256);
        dns_copy(w + DNS_VISITED, w + DNS_CURRENT, dns_length((char *)w + DNS_CURRENT) + 1);

        bool server_timed_out = false;
        for (unsigned question = 0; question < 9; ++question) {
            status = budget(w, deadline, hz);
            if (status) {
                if (status == DNS_TIMEOUT) { server_timed_out = true; break; }
                goto done;
            }
            uint32_t sequence = get32(w, 8) + 1;
            put32(w, 8, sequence);
            uint16_t id = (uint16_t)(sequence ^ (uint32_t)now ^ (uint32_t)(now >> 16));
            size_t qlen;
            status = dns_encode((char *)w + DNS_CURRENT, id, w + DNS_QUERY, &qlen);
            if (status) goto done;
            fd = DNS_CALL(SYS_SOCKET, NET_AF_INET, NET_SOCK_DGRAM | NET_SOCK_CLOEXEC, 17, 0, 0, 0);
            if (error(w, fd) < 0) {
                status = failure(fd);
                fd = -1;
                goto done;
            }
            bool next = false;
            for (unsigned attempt = 0; attempt < 3 && !next; ++attempt) {
                status = budget(w, deadline, hz);
                if (status) {
                    if (status == DNS_TIMEOUT) { server_timed_out = true; break; }
                    goto done;
                }
                long r = DNS_CALL(SYS_SENDTO, (uintptr_t)fd, (uintptr_t)(w + DNS_QUERY), qlen, 0, (uintptr_t)&server, 16);
                if (error(w, r) < 0) {
                    status = failure(r);
                    goto done;
                }
                if ((size_t)r != qlen) {
                    status = DNS_IO;
                    goto done;
                }
                for (;;) {
                    status = budget(w, deadline, hz);
                    if (status) {
                        if (status == DNS_TIMEOUT) { server_timed_out = true; break; }
                        goto done;
                    }
                    net_sockaddr_in_t from;
                    uint32_t size = 16;
                    r = DNS_CALL(SYS_RECVFROM, (uintptr_t)fd, (uintptr_t)(w + DNS_WIRE), 1472, 0, (uintptr_t)&from, (uintptr_t)&size);
                    if (r < 0) {
                        error(w, r);
                        if (r == SYSCALL_EAGAIN) {
                            status = budget(w, deadline, hz);
                            if (status) {
                                if (status == DNS_TIMEOUT) server_timed_out = true;
                                goto done_server_loop;
                            }
                            break;
                        }
                        status = failure(r);
                        goto done;
                    }
                    status = budget(w, deadline, hz);
                    if (status) {
                        if (status == DNS_TIMEOUT) { server_timed_out = true; break; }
                        goto done;
                    }
                    if ((size_t)r > 1472 || size != 16) {
                        status = DNS_IO;
                        goto done;
                    }
                    const uint8_t *p = w + DNS_WIRE;
                    if (from.family != NET_AF_INET || from.address != server.address || from.port != server.port ||
                        r < 2 || (uint16_t)((unsigned)p[0] * 256 + p[1]) != id)
                        status = DNS_UNMATCHED;
                    else if (r > 512)
                        status = DNS_LIMIT;
                    else
                        status = dns_decode(w, (size_t)r, id, &hops, &visited);

                    if (status == DNS_UNMATCHED) {
                        if (++noise > 32) { status = DNS_LIMIT; goto done; }
                        continue;
                    }
                    if (status == DNS_TRUNCATED) {
                        closefd(w, fd);
                        fd = -1;
                        tcp_used = true;
                        status = tcp_query(w, &server, qlen, id, deadline, hz, &hops, &visited);
                    }
                    if (status == DNS_BUSY) { next = true; break; }
                    if (status) goto done;
                    put32(w, 24, current_server);
                    goto publish;
                }
            }
        done_server_loop:
            if (fd >= 0) { closefd(w, fd); fd = -1; }
            if (server_timed_out) break;
            if (!next) {
                server_timed_out = true;
                break;
            }
        }
        if (fd >= 0) { closefd(w, fd); fd = -1; }
        if (server_timed_out) {
            status = DNS_TIMEOUT;
            continue;
        }
    }
    status = DNS_TIMEOUT;
    goto done;
publish:
    if (!numeric_bypass) { status = budget(w, deadline, hz); if (status) goto done; }
    if (tcp_used) put32(w, DNS_STAGE + 292, DNS_RESULT_TCP);
    dns_copy(result, w + DNS_STAGE, sizeof(*result));
done:
    if (fd >= 0) closefd(w, fd);
    put32(w, 4, 0);
    return status;
}
