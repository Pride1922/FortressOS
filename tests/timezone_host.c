#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../user/timezone.h"
#include "../user/ntp.h"

static void put32(uint8_t *p,uint32_t v) {p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
int main(void) {
    tz_calendar_t c;
    assert(tz_calendar(-1,&c)&&c.year==1969&&c.month==12&&c.day==31&&c.second==59&&c.weekday==3);
    assert(tz_calendar(951782400,&c)&&c.year==2000&&c.month==2&&c.day==29);
    assert(tz_calendar(4107542400LL,&c)&&c.year==2100&&c.month==3&&c.day==1);
    assert(!tz_calendar(INT64_MAX,&c)&&!tz_calendar(INT64_MIN,&c));
    tz_result_t r;
    assert(tz_posix("UTC",0,&r)&&r.offset==0);
    assert(tz_posix("+05:45",0,&r)&&r.offset==20700);
    assert(tz_posix("UTC-2",0,&r)&&r.offset==7200);
    assert(tz_posix("EST5EDT,M3.2.0,M11.1.0",1782864000,&r)&&r.offset== -14400&&r.daylight);
    assert(tz_posix("EST5EDT,M3.2.0,M11.1.0",1767225600,&r)&&r.offset== -18000&&!r.daylight);
    assert(tz_posix("AAA-14BBB-15,J1/0,J182/0",1767182400,&r)&&r.offset==54000&&r.daylight);
    const char *bad[]={"+25","+02:99","A0","UTCgarbage","EST5EDT","EST5EDT,M0.2.0,M11.1.0","UTC00000","UTC0junk",NULL};
    for(int i=0;bad[i];i++)assert(!tz_posix(bad[i],0,&r));
    assert(tz_name("Europe/Brussels")&&!tz_name("../etc/shadow")&&!tz_name("/UTC")&&!tz_name("Europe//Paris"));
    uint8_t bytes[16384];FILE *f=fopen("build/zoneinfo/Europe/Brussels","rb");assert(f);
    size_t n=fread(bytes,1,sizeof(bytes),f);fclose(f);assert(n<sizeof(bytes));
    assert(tz_tzif(bytes,n,1782864000,&r)&&r.offset==7200);
    assert(tz_tzif(bytes,n,1767225600,&r)&&r.offset==3600);
    tz_result_t sentinel;memset(&sentinel,0x5a,sizeof(sentinel));
    for(size_t i=0;i<n;i++) {
        r=sentinel;
        if(!tz_tzif(bytes,i,1782864000,&r))assert(!memcmp(&r,&sentinel,sizeof(r)));
    }
    /* Deterministic malformed TZif mutation corpus, including count overflows. */
    unsigned seed=19;
    for(int i=0;i<20000;i++) {
        uint8_t mutated[16384];memcpy(mutated,bytes,n);
        seed=seed*1664525u+1013904223u;size_t at=seed%n;
        mutated[at]^=(uint8_t)(1u<<(i%8));r=sentinel;
        if(!tz_tzif(mutated,n,(i&1)?INT64_MAX:1782864000,&r))assert(!memcmp(&r,&sentinel,sizeof(r)));
    }
    uint8_t nonce[8]={1,2,3,4,5,6,7,8},request[48],reply[48]={0};
    ntp_request(request,nonce);assert(request[0]==0x23&&!memcmp(request+40,nonce,8));
    reply[0]=0x24;reply[1]=2;memcpy(reply+24,nonce,8);
    put32(reply+32,3991801600u);put32(reply+40,3991801600u);
    timespec_t t={-1,-1};assert(ntp_sample(reply,48,nonce,200000000,&t));
    assert(t.tv_sec==1782812800&&t.tv_nsec==100000000);
    for(int i=0;i<48;i++)assert(!ntp_sample(reply,i,nonce,1,&t));
    reply[0]=0xe4;assert(!ntp_sample(reply,48,nonce,1,&t));reply[0]=0x24;
    reply[1]=0;assert(!ntp_sample(reply,48,nonce,1,&t));reply[1]=16;assert(!ntp_sample(reply,48,nonce,1,&t));reply[1]=2;
    reply[24]^=1;assert(!ntp_sample(reply,48,nonce,1,&t));reply[24]^=1;
    assert(!ntp_sample(reply,48,nonce,5000000001LL,&t));
    timespec_t saved=t;
    reply[0]=0x23;assert(!ntp_sample(reply,48,nonce,1,&t));reply[0]=0x24;
    put32(reply+8,17u*65536u);assert(!ntp_sample(reply,48,nonce,1,&t));put32(reply+8,0);
    put32(reply+40,3991801601u);assert(!ntp_sample(reply,48,nonce,1,&t));
    put32(reply+40,3991801599u);assert(!ntp_sample(reply,48,nonce,1,&t));
    assert(t.tv_sec==saved.tv_sec&&t.tv_nsec==saved.tv_nsec);
    put32(reply+40,123010304u);put32(reply+32,123010304u); /* 2040 era 1 */
    assert(ntp_sample(reply,48,nonce,0,&t)&&t.tv_sec==2208988800LL);
    puts("PASS timezone calendar/POSIX/TZif malformed corpus and SNTP validation/era boundaries");
}
