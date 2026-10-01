#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "icmp.h"
#include "ipv4.h"
#include "checksum.h"
int main(void) {
    uint8_t buf[1500];
    const uint8_t independent[]={8,0,0xb5,0xa2,0x12,0x34,0,1,0x30,0x28};
    icmp_echo_t echo; const uint8_t *data; size_t len;
    assert(net_checksum(independent,sizeof(independent),0)==0);
    assert(!icmp_echo_decode(independent,sizeof(independent),&echo,&data,&len));
    assert(echo.identifier==0x1234 && echo.sequence==1 && len==2);
    for (unsigned n=0; n<=1472; ++n) {
        memset(buf,0xa5,sizeof(buf));
        assert(!icmp_echo_encode(buf,sizeof(buf),8,123,456,buf+8,n));
        assert(!icmp_echo_decode(buf,n+8,&echo,&data,&len) && len==n);
        assert(!icmp_echo_encode(buf,sizeof(buf),0,echo.identifier,echo.sequence,data,len));
        assert(!icmp_echo_decode(buf,n+8,&echo,&data,&len) && echo.type==0);
        buf[2]^=1; assert(icmp_echo_decode(buf,n+8,&echo,NULL,NULL));
    }
    for (unsigned n=0; n<8; ++n) {
        uint8_t tiny[n+1]; memset(tiny,0,sizeof(tiny));
        assert(icmp_echo_decode(tiny,n,&echo,NULL,NULL));
        assert(icmp_echo_encode(tiny,n,8,0,0,NULL,0));
    }
    assert(icmp_echo_encode(buf,sizeof(buf),3,0,0,NULL,0));
    assert(icmp_echo_encode(buf,sizeof(buf),8,0,0,buf,1473));
    assert(ipv4_encode(buf,sizeof(buf),0,0,1,65535,64,NULL));
    puts("ICMP host ASan/UBSan PASS: independent vector, 1473 payload lengths, checksums, bounds, IPv4 overflow");
}
