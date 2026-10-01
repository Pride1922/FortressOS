#include "tcp.h"
#include "checksum.h"
#include "string.h"

static uint16_t r16(const uint8_t *p) { return (uint16_t)((p[0]<<8)|p[1]); }
static uint32_t r32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static void w16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v; }
static void w32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16);
    p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v;
}
static uint32_t pseudo(uint32_t src, uint32_t dst, size_t len) {
    uint8_t p[12];
    memcpy(p,&src,4); memcpy(p+4,&dst,4);
    p[8]=0; p[9]=6; w16(p+10,(uint16_t)len);
    return net_checksum_accumulate(p,sizeof(p),0);
}
int tcp_decode(const void *buf, size_t len, uint32_t src, uint32_t dst,
               tcp_header_t *header, const uint8_t **data, size_t *data_len) {
    if (!buf || !header || !data || !data_len || len<TCP_HEADER_MIN ||
        len>TCP_IPV4_SEGMENT_MAX) return -1;
    const uint8_t *p=buf;
    size_t n=(size_t)(p[12]>>4)*4;
    if (n<TCP_HEADER_MIN || n>len || net_checksum(p,len,pseudo(src,dst,len))) return -1;
    tcp_header_t h={.source=r16(p),.destination=r16(p+2),
        .sequence=r32(p+4),.acknowledgment=r32(p+8),.window=r16(p+14),
        .urgent=r16(p+18),.flags=p[13],.header_length=(uint8_t)n};
    for (size_t i=20; i<n;) {
        uint8_t kind=p[i];
        if (!kind) break; /* EOL: remaining padding is not an option stream. */
        if (kind==1) { ++i; continue; }
        if (n-i<2 || p[i+1]<2 || p[i+1]>n-i) return -1;
        size_t size=p[i+1];
        if (kind==2) {
            if (size!=4 || h.has_mss || !r16(p+i+2)) return -1;
            h.has_mss=true; h.mss=r16(p+i+2);
        } else if (kind==3) {
            if (size!=3 || h.has_wscale) return -1;
            h.has_wscale=true; h.wscale=p[i+2]>14 ? 14 : p[i+2];
        }
        i+=size; /* Unknown well-formed options are skipped. */
    }
    *header=h; *data=p+n; *data_len=len-n; return 0;
}
int tcp_encode(void *buf, size_t capacity, uint32_t src, uint32_t dst,
               const tcp_header_t *header, const void *data, size_t len) {
    if (!buf || !header || (!data && len)) return -1;
    tcp_header_t h=*header;
    if (h.has_wscale || (h.has_mss && (!(h.flags&TCP_SYN) || !h.mss))) return -1;
    size_t n=h.has_mss ? 24 : 20;
    if (len>TCP_IPV4_SEGMENT_MAX-n || capacity<n+len) return -1;
    uint8_t *p=buf;
    if (len) memmove(p+n,data,len);
    memset(p,0,n);
    w16(p,h.source); w16(p+2,h.destination);
    w32(p+4,h.sequence); w32(p+8,h.acknowledgment);
    p[12]=(uint8_t)((n/4)<<4); p[13]=h.flags;
    w16(p+14,h.window); w16(p+18,h.urgent);
    if (h.has_mss) { p[20]=2; p[21]=4; w16(p+22,h.mss); }
    w16(p+16,net_checksum(p,n+len,pseudo(src,dst,n+len)));
    return 0;
}
