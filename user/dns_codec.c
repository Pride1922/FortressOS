#include "dns_codec.h"
void dns_copy(void *d, const void *s, size_t n) {
    uint8_t *a=d; const uint8_t *b=s; for (size_t i=0;i<n;++i) a[i]=b[i];
}
void dns_zero(void *d, size_t n) { uint8_t *a=d; for (size_t i=0;i<n;++i) a[i]=0; }
size_t dns_length(const char *s) { size_t n=0; while(s[n]) ++n; return n; }
static unsigned lower(unsigned c) { return c>='A' && c<='Z' ? c+32 : c; }
bool dns_equal(const char *a,const char *b) {
    while (*a && lower((uint8_t)*a)==lower((uint8_t)*b)) { ++a; ++b; }
    return lower((uint8_t)*a)==lower((uint8_t)*b);
}
static uint16_t u16(const uint8_t *p) { return (uint16_t)((unsigned)p[0]*256+p[1]); }
static void p16(uint8_t *p,unsigned n) { p[0]=(uint8_t)(n>>8); p[1]=(uint8_t)n; }
int dns_normalize(const char *name,size_t n,char out[256]) {
    if (!n || n>254) return DNS_INVALID;
    if (name[n-1]=='.') --n;
    if (!n) return DNS_INVALID;
    unsigned label=0; size_t wire=1;
    for (size_t i=0;i<n;++i) {
        unsigned c=lower((uint8_t)name[i]);
        if (c=='.') {
            if (!label || name[i-1]=='-') return DNS_INVALID;
            wire+=label+1; label=0;
        } else {
            if (!((c>='a' && c<='z') || (c>='0' && c<='9') || c=='-') ||
                (!label && c=='-') || ++label>63) return DNS_INVALID;
        }
        out[i]=(char)c;
    }
    if (!label || name[n-1]=='-' || wire+label+1>255) return DNS_INVALID;
    out[n]=0; return DNS_OK;
}
bool dns_ipv4(const char *s,size_t len,uint32_t *ip) {
    size_t i=0; uint8_t octets[4];
    for (unsigned j=0;j<4;++j) {
        unsigned value=0,digits=0;
        while(i<len && s[i]>='0' && s[i]<='9') {
            if (++digits>5) return false;
            value=value*10+(unsigned)(s[i++]-'0'); if(value>255) return false;
        }
        if (!digits) return false;
        octets[j]=(uint8_t)value;
        if(j<3 && (i>=len || s[i++]!='.')) return false;
    }
    if(i!=len) return false;
    dns_copy(ip,octets,4); return true;
}
void dns_format_ipv4(uint32_t ip,char out[16]) {
    uint8_t bytes[4]; dns_copy(bytes,&ip,4); unsigned at=0;
    for(unsigned i=0;i<4;++i) {
        unsigned v=bytes[i]; if(v>=100) out[at++]=(char)('0'+v/100);
        if(v>=10) out[at++]=(char)('0'+(v/10)%10);
        out[at++]=(char)('0'+v%10); if(i<3) out[at++]='.';
    }
    out[at]=0;
}
int dns_encode(const char *name,uint16_t id,uint8_t *p,size_t *len) {
    dns_zero(p,12); p16(p,id); p[2]=1; p[5]=1; size_t at=12,start=0;
    bool ended=false;
    for(size_t i=0;i<255;++i) if(name[i]=='.' || !name[i]) {
        size_t n=i-start; if (!n || n>63 || at+1+n+5>272) return DNS_INVALID;
        p[at++]=(uint8_t)n; dns_copy(p+at,name+start,n); at+=n; start=i+1;
        if(!name[i]) { ended=true; break; }
    }
    if(!ended) return DNS_INVALID;
    p[at++]=0; p16(p+at,1); p16(p+at+2,1); *len=at+4; return DNS_OK;
}
/* Iterative bounded expansion; consumed length describes only original bytes.
 * Only backward pointers are accepted (RFC compression references prior names). */
static int name_read(const uint8_t *p,size_t len,size_t *pos,char *out) {
    size_t at=*pos, end=0, count=0, expanded=1; unsigned steps=0;
    while (++steps<=128) {
        if(at>=len) return DNS_MALFORMED;
        unsigned c=p[at++];
        if((c&0xc0)==0xc0) {
            if(at>=len) return DNS_MALFORMED;
            size_t target=(c&63)*256+p[at++];
            if(target>=at-2 || target<12) return DNS_MALFORMED;
            if(!end) end=at;
            at=target; continue;
        }
        if(c&0xc0) return DNS_MALFORMED;
        if(!c) { out[count]=0; *pos=end ? end : at; return DNS_OK; }
        if(c>63 || c>len-at || expanded+c+1>255) return DNS_MALFORMED;
        expanded+=c+1;
        if(count) out[count++]='.';
        for(unsigned i=0;i<c;++i) {
            unsigned v=p[at++];
            /* Embedded NUL/dot would change presentation label boundaries. */
            if(!v || v=='.' || v>=128) return DNS_UNSUPPORTED;
            out[count++]=(char)lower(v);
        }
    }
    return DNS_MALFORMED;
}
/* RR metadata uses memcpy, not struct casts into opaque context storage. */
typedef struct { uint32_t owner,rdata; uint16_t type,klass,length,pad; } rr_t;
_Static_assert(sizeof(rr_t)==16,"RR metadata");
static void rr_get(uint8_t *w,unsigned i,rr_t *r) { dns_copy(r,w+DNS_RRS+i*16,16); }
static int stage_address(uint8_t *w,const uint8_t *address) {
    uint32_t count; dns_copy(&count,w+DNS_STAGE+288,4);
    for(unsigned i=0;i<count;++i) {
        bool same=true; for(unsigned j=0;j<4;++j) if(w[DNS_STAGE+256+i*4+j]!=address[j]) same=false;
        if(same) return DNS_OK;
    }
    if(count==8) return DNS_LIMIT;
    dns_copy(w+DNS_STAGE+256+count*4,address,4); ++count;
    dns_copy(w+DNS_STAGE+288,&count,4); return DNS_OK;
}
int dns_decode(uint8_t *w,size_t len,uint16_t id,unsigned *hops,unsigned *visited) {
    const uint8_t *p=w+DNS_WIRE;
    char *current=(char *)w+DNS_CURRENT,*owner=(char *)w+DNS_OWNER,*target=(char *)w+DNS_TARGET;
    if(len<12 || u16(p)!=id || !(p[2]&128) || u16(p+4)!=1) return DNS_MALFORMED;
    if(p[2]&0x78) return DNS_UNSUPPORTED;
    if(p[3]&0x40) return DNS_MALFORMED; /* Reserved Z bit; AD/CD not negotiated. */
    size_t at=12; int status=name_read(p,len,&at,owner); if(status) return status;
    if(at+4>len) return DNS_MALFORMED;
    if(!dns_equal(owner,current) || u16(p+at)!=1 || u16(p+at+2)!=1) return DNS_UNMATCHED;
    at+=4;
    if(p[2]&2) return DNS_TRUNCATED; /* Only matching envelope required. */
    unsigned counts[3]={u16(p+6),u16(p+8),u16(p+10)}, total=0;
    for(unsigned i=0;i<3;++i) { if(counts[i]>64-total) return DNS_LIMIT; total+=counts[i]; }
    for(unsigned i=0;i<total;++i) {
        rr_t r={.owner=(uint32_t)at};
        status=name_read(p,len,&at,owner); if(status) return status;
        if(at+10>len) return DNS_MALFORMED;
        r.type=u16(p+at); r.klass=u16(p+at+2); r.length=u16(p+at+8); at+=10; r.rdata=(uint32_t)at;
        if(r.length>len-at) return DNS_MALFORMED;
        if(r.type==41) return DNS_UNSUPPORTED; /* No EDNS/extended RCODE support. */
        if(r.klass==1 && r.type==1 && r.length!=4) return DNS_MALFORMED;
        if(r.type==5) {
            size_t cp=at; status=name_read(p,len,&cp,target);
            if(status || cp!=at+r.length) return status ? status : DNS_MALFORMED;
        }
        dns_copy(w+DNS_RRS+i*16,&r,16); at+=r.length;
    }
    if(at!=len) return DNS_MALFORMED;
    unsigned rcode=p[3]&15;
    if(rcode==3) return DNS_NXDOMAIN;
    if(rcode) return DNS_SERVER_FAILURE;
    dns_zero(w+DNS_STAGE,296); bool progressed=false;
    for(;;) {
        bool alias=false, addresses=false;
        /* TARGET remains canonical next alias while OWNER is scratch. */
        target[0]=0;
        for(unsigned i=0;i<counts[0];++i) {
            rr_t r; rr_get(w,i,&r); size_t cp=r.owner;
            status=name_read(p,len,&cp,owner); if(status) return status;
            if(r.klass!=1 || !dns_equal(owner,current)) continue;
            if(r.type==1) { addresses=true; status=stage_address(w,p+r.rdata); if(status) return status; }
            if(r.type==5) {
                cp=r.rdata; status=name_read(p,len,&cp,owner); if(status) return status;
                if(alias && !dns_equal(owner,target)) return DNS_MALFORMED;
                dns_copy(target,owner,dns_length(owner)+1); alias=true;
            }
        }
        if(alias && addresses) return DNS_MALFORMED;
        if(addresses) {
            status=dns_normalize(current,dns_length(current),owner); if(status) return DNS_UNSUPPORTED;
            dns_copy(w+DNS_STAGE,owner,dns_length(owner)+1); return DNS_OK;
        }
        if(!alias) return progressed ? DNS_BUSY : DNS_NO_DATA;
        if(*hops==8 || *visited>=9) return DNS_LIMIT;
        for(unsigned i=0;i<*visited;++i) if(dns_equal(target,(char *)w+DNS_VISITED+i*256)) return DNS_MALFORMED;
        dns_copy(current,target,dns_length(target)+1);
        dns_zero(w+DNS_VISITED+*visited*256,256);
        dns_copy(w+DNS_VISITED+*visited*256,current,dns_length(current)+1);
        ++*visited; ++*hops; progressed=true;
    }
}
