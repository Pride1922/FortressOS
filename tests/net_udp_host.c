#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "udp.h"
#include "../src/include/net.h"
static unsigned sum(const unsigned char *p, size_t n, unsigned s) {
    while (n>=2) { s+=(p[0]<<8)|p[1]; p+=2; n-=2; }
    if (n) s+=p[0]<<8;
    while (s>>16) s=(s&65535)+(s>>16);
    return s;
}
static uint8_t bytes[1481], data[1473];
int main(void) {
    uint32_t src=htonl(0xc0a800a8), dst=htonl(0xc0a800de);
    uint8_t pseudo[12]={192,168,0,168,192,168,0,222,0,17,0,0};
    udp_header_t h; const uint8_t *p; size_t n;
    for (unsigned i=0; i<sizeof(data); ++i) data[i]=(uint8_t)i;
    unsigned sizes[]={0,1,2,9,1471,1472};
    for (unsigned i=0; i<6; ++i) {
        size_t len=sizes[i];
        assert(!udp_encode(bytes+1,1480,src,dst,49152,7777,data,len));
        pseudo[10]=(uint8_t)((len+8)>>8); pseudo[11]=(uint8_t)(len+8);
        assert(sum(bytes+1,len+8,sum(pseudo,12,0))==65535);
        assert(!udp_decode(bytes+1,len+8,src,dst,&h,&p,&n));
        assert(h.source==49152 && h.destination==7777 && n==len && !memcmp(p,data,n));
        for (size_t cut=0; cut<len+8; ++cut) assert(udp_decode(bytes+1,cut,src,dst,&h,&p,&n));
        assert(udp_decode(bytes+1,len+8,src^1,dst,&h,&p,&n));
        bytes[7]^=1; assert(udp_decode(bytes+1,len+8,src,dst,&h,&p,&n)); bytes[7]^=1;
        bytes[7]=bytes[8]=0; /* IPv4 omitted checksum. */
        assert(!udp_decode(bytes+1,len+9,src,dst,&h,&p,&n) && n==len);
    }
    assert(udp_encode(bytes,1480,src,dst,1,2,data,1473));
    assert(udp_encode(bytes,7,src,dst,1,2,NULL,0));
    assert(udp_encode(bytes,1480,src,dst,1,2,NULL,1));
    /* Independently solve the last word for computed checksum zero. */
    pseudo[10]=0; pseudo[11]=10;
    uint8_t header[8]={0,1,0,2,0,10,0,0};
    unsigned word=(~sum(header,8,sum(pseudo,12,0)))&65535;
    data[0]=(uint8_t)(word>>8); data[1]=(uint8_t)word;
    assert(!udp_encode(bytes,1480,src,dst,1,2,data,2));
    assert(bytes[6]==255 && bytes[7]==255);
    assert(!udp_decode(bytes,10,src,dst,&h,&p,&n));
    bytes[4]=0; bytes[5]=7; assert(udp_decode(bytes,10,src,dst,&h,&p,&n));
    puts("UDP codec ASan/UBSan PASS: independent checksum, odd/max/zero data, computed zero, truncation, omitted/corrupt checksum and bounds");
}
