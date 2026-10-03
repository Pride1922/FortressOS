#include "icmp.h"
#include "checksum.h"
#include "string.h"

static uint16_t word(const uint8_t *p) { return (uint16_t)((uint16_t)p[0]<<8)|p[1]; }
static void put(uint8_t *p, uint16_t v) { p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v; }
int icmp_quote_decode(const void *buf, size_t len, icmp_quote_t *out) {
    if (!buf || !out || len<36 || len>1480) return -1;
    const uint8_t *p=buf, *ip=p+8;
    if (!((p[0]==ICMP_TIME_EXCEEDED && p[1]==0) ||
          (p[0]==ICMP_DEST_UNREACHABLE && p[1]<=5)) || net_checksum(p,len,0)) return -1;
    size_t ihl=(ip[0]&15u)*4u;
    if ((ip[0]>>4)!=4 || ihl<20 || ihl>len-16 ||
        word(ip+2)<ihl+8 || (word(ip+6)&0xbfffu) || ip[9]!=1 ||
        net_checksum(ip,ihl,0) || ip[ihl]!=ICMP_ECHO_REQUEST || ip[ihl+1]) return -1;
    icmp_quote_t q={.type=p[0],.code=p[1],.identifier=word(ip+ihl+4),.sequence=word(ip+ihl+6),
                    .ip_identifier=word(ip+4)};
    memcpy(&q.source,ip+12,4); memcpy(&q.destination,ip+16,4);
    *out=q; return 0;
}
int icmp_echo_decode(const void *buf, size_t len, icmp_echo_t *out,
                     const uint8_t **data, size_t *data_len) {
    if (!buf || !out || len<ICMP_ECHO_HEADER || len>1480) return -1;
    const uint8_t *p=buf;
    if ((p[0]!=ICMP_ECHO_REPLY && p[0]!=ICMP_ECHO_REQUEST) || p[1] || net_checksum(p,len,0)) return -1;
    out->type=p[0]; out->identifier=word(p+4); out->sequence=word(p+6);
    if (data) *data=p+8;
    if (data_len) *data_len=len-8;
    return 0;
}
int icmp_echo_encode(void *buf, size_t cap, uint8_t type, uint16_t identifier,
                     uint16_t sequence, const void *data, size_t len) {
    if (!buf || (len && !data) || len>1472 || cap<8 || len>cap-8 ||
        (type!=ICMP_ECHO_REPLY && type!=ICMP_ECHO_REQUEST)) return -1;
    uint8_t *p=buf;
    /* Copy first permits exact in-place data echo. */
    if (len) memmove(p+8,data,len);
    p[0]=type; p[1]=0; put(p+2,0); put(p+4,identifier); put(p+6,sequence);
    put(p+2,net_checksum(p,len+8,0));
    return 0;
}
