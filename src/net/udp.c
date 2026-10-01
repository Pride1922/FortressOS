#include "udp.h"
#include "checksum.h"
#include "string.h"
static uint16_t read16(const uint8_t *p) { return (uint16_t)((p[0]<<8)|p[1]); }
static void write16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v; }
static uint32_t pseudo(uint32_t src, uint32_t dst, uint16_t len) {
    uint8_t bytes[12];
    memcpy(bytes,&src,4); memcpy(bytes+4,&dst,4);
    bytes[8]=0; bytes[9]=17; write16(bytes+10,len);
    return net_checksum_accumulate(bytes,sizeof(bytes),0);
}
int udp_decode(const void *buf, size_t len, uint32_t src, uint32_t dst,
               udp_header_t *header, const uint8_t **data, size_t *data_len) {
    if (!buf || !header || !data || !data_len || len<8) return -1;
    const uint8_t *p=buf;
    uint16_t n=read16(p+4);
    if (n<8 || n>len || (unsigned)(n-8)>NET_UDP_DATA_MAX) return -1;
    if (read16(p+6) && net_checksum(p,n,pseudo(src,dst,n))) return -1;
    *header=(udp_header_t){read16(p),read16(p+2),n};
    *data=p+8; *data_len=n-8; return 0;
}
int udp_encode(void *buf, size_t capacity, uint32_t src, uint32_t dst,
               uint16_t sport, uint16_t dport, const void *data, size_t len) {
    if (!buf || (!data && len) || len>NET_UDP_DATA_MAX || capacity<len+8) return -1;
    uint8_t *p=buf;
    if (len) memmove(p+8,data,len);
    write16(p,sport); write16(p+2,dport); write16(p+4,(uint16_t)(len+8));
    write16(p+6,0);
    uint16_t sum=net_checksum(p,len+8,pseudo(src,dst,(uint16_t)(len+8)));
    write16(p+6,sum ? sum : 0xffff); return 0;
}
