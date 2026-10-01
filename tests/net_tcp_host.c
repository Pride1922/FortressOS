#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "tcp.h"
#include "net.h"

/* Independent byte-wise checksum oracle and packet construction. */
static unsigned sum(const uint8_t *p, size_t n, unsigned s) {
    while (n>=2) { s+=((unsigned)p[0]<<8)|p[1]; p+=2; n-=2; }
    if (n) s+=(unsigned)p[0]<<8;
    while (s>>16) s=(s&65535)+(s>>16);
    return s;
}
static uint32_t src, dst;
static unsigned psum(size_t n) {
    uint8_t p[12]={192,168,0,168,192,168,0,222,0,6,(uint8_t)(n>>8),(uint8_t)n};
    return sum(p,12,0);
}
static void seal(uint8_t *p, size_t n) {
    p[16]=p[17]=0;
    unsigned s=(~sum(p,n,psum(n)))&65535;
    p[16]=(uint8_t)(s>>8); p[17]=(uint8_t)s;
}
static uint8_t storage[TCP_IPV4_SEGMENT_MAX+2], payload[TCP_IPV4_SEGMENT_MAX];
static tcp_header_t h;
static const uint8_t *data;
static size_t data_len;
static int decode(uint8_t *p, size_t n) { return tcp_decode(p,n,src,dst,&h,&data,&data_len); }
static void rejected(uint8_t *p, size_t n) {
    memset(&h,0xa5,sizeof(h)); tcp_header_t old=h;
    data=payload; data_len=123;
    assert(decode(p,n)==-1 && !memcmp(&h,&old,sizeof(h)) && data==payload && data_len==123);
}
static void options(uint8_t *p, const uint8_t *opts, size_t n) {
    memset(p,0,20+n); p[12]=(uint8_t)(((20+n)/4)<<4); p[13]=TCP_SYN;
    memcpy(p+20,opts,n); seal(p,20+n);
}
int main(void) {
    src=htonl(0xc0a800a8); dst=htonl(0xc0a800de);
    uint8_t *p=storage+1; /* deliberately unaligned */
    for (size_t i=0; i<sizeof(payload); ++i) payload[i]=(uint8_t)(i*31);
    /* Fixed SYN independently constructed. */
    const uint8_t syn[24]={0xc0,0,0,80,0x12,0x34,0x56,0x78,0,0,0,0,
        0x60,2,0x20,0,0,0,0,0,2,4,5,0xb4};
    memcpy(p,syn,24); seal(p,24);
    for (size_t cut=0; cut<24; ++cut) rejected(p,cut);
    assert(!decode(p,24) && h.source==49152 && h.destination==80 &&
        h.sequence==0x12345678 && h.flags==TCP_SYN && h.window==8192 &&
        h.header_length==24 && h.has_mss && h.mss==1460 && !data_len);
    tcp_header_t send=h;
    assert(!tcp_encode(storage+40,24,src,dst,&send,NULL,0));
    assert(!memcmp(p,storage+40,24));
    send=(tcp_header_t){.source=65535,.destination=1,.sequence=0xffffffff,
        .acknowledgment=0x80000000,.window=8192,.flags=TCP_ACK|TCP_PSH,.urgent=7};
    size_t sizes[]={0,1,2,9,1459,1460,8192,TCP_IPV4_SEGMENT_MAX-20};
    for (unsigned j=0; j<sizeof(sizes)/sizeof(sizes[0]); ++j) {
        size_t n=sizes[j]+20;
        assert(!tcp_encode(p,n,src,dst,&send,payload,sizes[j]));
        assert(sum(p,n,psum(n))==65535);
        assert(!decode(p,n) && h.sequence==0xffffffff && h.acknowledgment==0x80000000 &&
            data_len==sizes[j] && !memcmp(data,payload,data_len));
        assert(tcp_decode(p,n,src^1,dst,&h,&data,&data_len));
        p[n-1]^=1; rejected(p,n); p[n-1]^=1;
        assert(tcp_encode(p,n-1,src,dst,&send,payload,sizes[j]));
    }
    assert(tcp_encode(p,sizeof(storage)-1,src,dst,&send,payload,SIZE_MAX));
    assert(tcp_encode(p,sizeof(storage)-1,src,dst,&send,payload,TCP_IPV4_SEGMENT_MAX-19));
    assert(tcp_encode(p,20,src,dst,&send,NULL,1));
    assert(tcp_encode(NULL,20,src,dst,&send,NULL,0));
    assert(tcp_decode(NULL,20,src,dst,&h,&data,&data_len));
    assert(tcp_decode(p,20,src,dst,NULL,&data,&data_len));
    assert(tcp_decode(p,20,src,dst,&h,NULL,&data_len));
    assert(tcp_decode(p,20,src,dst,&h,&data,NULL));
    assert(tcp_encode(p,20,src,dst,NULL,NULL,0));
    for (size_t n=0; n<20; ++n) rejected(p,n);
    rejected(p,TCP_IPV4_SEGMENT_MAX+1);
    /* Offset bounds independent of checksum rejection. */
    for (unsigned offset=0; offset<16; ++offset) {
        memset(p,0,60); p[12]=(uint8_t)(offset<<4); seal(p,60);
        if (offset<5) rejected(p,60); else assert(!decode(p,60));
    }
    memset(p,0,20); p[12]=0xf0; seal(p,20); rejected(p,20);
    const uint8_t valid[12]={1,2,4,5,0xb4,3,3,255,0,0xfe,0xfe,0xfe};
    options(p,valid,12); assert(!decode(p,32) && h.mss==1460 && h.wscale==14);
    const uint8_t unknown[4]={30,4,0xaa,0xbb};
    options(p,unknown,4); assert(!decode(p,24) && !h.has_mss && !h.has_wscale);
    const uint8_t bad[][8]={
        {2,3,5,0}, {2,4,0,0}, {3,4,1,0}, {30,0,0,0},
        {30,1,0,0}, {30,9,0,0}, {1,1,1,1,1,1,1,2},
        {2,4,5,0xb4,2,4,5,0xb4}, {3,3,1,3,3,2,0,0}};
    for (unsigned i=0; i<sizeof(bad)/sizeof(bad[0]); ++i) {
        options(p,bad[i],8); rejected(p,28);
    }
    send.has_mss=true; assert(tcp_encode(p,24,src,dst,&send,NULL,0));
    send.flags=TCP_SYN; send.mss=0; assert(tcp_encode(p,24,src,dst,&send,NULL,0));
    send.mss=1460; send.has_wscale=true; assert(tcp_encode(p,24,src,dst,&send,NULL,0));
    send.has_wscale=false; send.has_mss=false;
    /* In-place payload move preserves bytes. */
    memcpy(p,payload,1460); assert(!tcp_encode(p,1480,src,dst,&send,p,1460));
    assert(!decode(p,1480) && !memcmp(data,payload,1460));
    /* Unlike UDP, checksum zero is neither omission nor rewritten to ffff. */
    assert(!tcp_encode(p,22,src,dst,&send,payload,2));
    p[20]=p[21]=0; p[16]=p[17]=0;
    unsigned word=(~sum(p,22,psum(22)))&65535;
    uint8_t zero[2]={(uint8_t)(word>>8),(uint8_t)word};
    assert(!tcp_encode(p,22,src,dst,&send,zero,2) && !p[16] && !p[17]);
    assert(!decode(p,22)); p[20]^=1; rejected(p,22);
    /* Exhaustive length/offset bounds with valid checksum, then random fuzz. */
    for (size_t n=20; n<=128; ++n) for (unsigned off=0; off<16; ++off) {
        memset(p,0,n); p[12]=(uint8_t)(off<<4); seal(p,n);
        if (off<5 || off*4>n) rejected(p,n); else assert(!decode(p,n));
    }
    uint32_t random=0x12345678;
    for (unsigned i=0; i<20000; ++i) {
        size_t n=i%129;
        for (size_t j=0; j<n; ++j) { random=random*1664525+1013904223; p[j]=(uint8_t)(random>>24); }
        (void)decode(p,n);
        if (n>=20) { seal(p,n); (void)decode(p,n); }
    }
    puts("TCP codec ASan/UBSan PASS: independent wire/checksum, options, zero checksum, odd/max lengths, bounds, unaligned/overlap, deterministic fuzz");
}
