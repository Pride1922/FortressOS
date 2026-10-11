#include "install_archive.h"
#include "root_boot.h"
#include "string.h"

static bool allzero(const uint8_t *p,size_t n) {
    for (size_t i=0;i<n;i++) if (p[i]) return false;
    return true;
}
static bool number(const uint8_t *p,size_t n,uint64_t *out) {
    uint64_t v=0;size_t i=0;bool digits=false;
    while (i<n && p[i]==' ') i++;
    while (i<n && p[i]>='0' && p[i]<='7') {
        if (v>(UINT64_MAX-7)/8) return false;
        v=v*8+p[i++]-'0';digits=true;
    }
    while (i<n && (p[i]==0 || p[i]==' ')) i++;
    if (!digits || i!=n) return false;
    *out=v;return true;
}
static bool text(const uint8_t *p,size_t capacity,size_t *length) {
    size_t i=0;while (i<capacity && p[i]) i++;
    if (i==capacity || !allzero(p+i,capacity-i)) return false;
    *length=i;return true;
}
static bool name(const uint8_t *h,char out[256]) {
    size_t n,p;
    if (!text(h,100,&n) || !text(h+345,155,&p) || !n || n+p+(p ? 1:0)>=256) return false;
    size_t total=0;
    if (p) {memcpy(out,h+345,p);total=p;out[total++]='/';}
    memcpy(out+total,h,n);total+=n;
    if (h[156]=='5' && out[total-1]=='/') total--;
    if (!total) return false;
    out[total]=0;
    for (size_t i=0;i<total;) {
        size_t start=i;
        while (i<total && out[i]!='/') {
            unsigned char c=out[i++];
            if (!((c>='a' && c<='z') || (c>='A' && c<='Z') ||
                  (c>='0' && c<='9') || c=='.' || c=='_' || c=='-')) return false;
        }
        size_t length=i-start;
        if (!length || length>=64 || (length==1 && out[start]=='.') ||
            (length==2 && out[start]=='.' && out[start+1]=='.')) return false;
        if (i<total && ++i==total) return false;
    }
    return strcmp(out,"bootstrap") && strncmp(out,"bootstrap/",10);
}
static bool header_valid(const uint8_t *h,uint64_t *size) {
    uint64_t mode,uid,gid,sum,stamp;
    if (memcmp(h+257,"ustar\0",6) || memcmp(h+263,"00",2) ||
        (h[156]!='0' && h[156]!=0 && h[156]!='5') || !allzero(h+157,100) ||
        !allzero(h+500,12) || !number(h+100,8,&mode) || mode>07777 ||
        !number(h+108,8,&uid) || uid || !number(h+116,8,&gid) || gid ||
        !number(h+124,12,size) || !number(h+136,12,&stamp) || !number(h+148,8,&sum)) return false;
    uint64_t actual=0;
    for (unsigned i=0;i<512;i++) actual+=i>=148 && i<156 ? ' ' : h[i];
    return actual==sum && (h[156]!='5' || !*size);
}
static void octal(uint8_t *out,size_t n,uint64_t value) {
    memset(out,'0',n-1);out[n-1]=0;
    for (size_t i=n-1;i && value;) {out[--i]=(uint8_t)('0'+(value&7));value>>=3;}
}
static void make_header(uint8_t *h,const char *path,unsigned mode,size_t size,bool dir) {
    memset(h,0,512);memcpy(h,path,strlen(path));octal(h+100,8,mode);
    octal(h+108,8,0);octal(h+116,8,0);octal(h+124,12,size);octal(h+136,12,0);
    h[156]=dir ? '5' : '0';memcpy(h+257,"ustar\00000",8);
    octal(h+329,8,0);octal(h+337,8,0);memset(h+148,' ',8);
    uint64_t sum=0;for (unsigned i=0;i<512;i++) sum+=h[i];
    octal(h+148,7,sum);h[155]=' ';
}
int install_archive_personalize(const void *input,size_t n,const void *record,size_t rn,
                               void *output,size_t capacity,size_t *written) {
    if (!written) return -22;
    *written=0;
    if (!input || !record || !output || n<1024 || n>INSTALL_ARCHIVE_MAX || n%512 ||
        capacity<2560 || capacity>INSTALL_ARCHIVE_MAX || !rn || rn>512) return -22;
    uintptr_t in=(uintptr_t)input,out=(uintptr_t)output,rec=(uintptr_t)record;
    if (in>UINTPTR_MAX-n || out>UINTPTR_MAX-capacity || rec>UINTPTR_MAX-rn ||
        (in<out+capacity && out<in+n) || (rec<out+capacity && out<rec+rn)) return -22;
    memset(output,0,capacity);
    root_recovery_t parsed;
    if (!root_recovery_parse(record,rn,&parsed)) return -22;
    uint32_t offsets[510];unsigned count=0;size_t pos=0;int error=-22;
    const uint8_t *src=input;uint8_t *dst=output;bool terminated=false;
    while (n-pos>=512) {
        if (allzero(src+pos,512)) {
            if (n-pos<1024 || !allzero(src+pos,n-pos)) goto failed;
            terminated=true;break;
        }
        uint64_t size;char path[256];
        if (count==510 || !header_valid(src+pos,&size) || !name(src+pos,path) ||
            size>INSTALL_ARCHIVE_MAX || size>n-pos-512) goto failed;
        uint64_t padded=(size+511)&~511ULL;
        if (padded>n-pos-512 || !allzero(src+pos+512+(size_t)size,(size_t)(padded-size))) goto failed;
        for (unsigned i=0;i<count;i++) {
            char previous[256];
            if (!name(src+offsets[i],previous) || !strcmp(path,previous)) goto failed;
            /* A regular file cannot be the parent of another archive member.
             * Reject both orders, rather than rely on extractor behavior. */
            size_t a=strlen(path),b=strlen(previous);
            if ((a>b && !memcmp(path,previous,b) && path[b]=='/' && src[offsets[i]+156]!='5') ||
                (b>a && !memcmp(path,previous,a) && previous[a]=='/' && src[pos+156]!='5')) goto failed;
        }
        offsets[count++]=(uint32_t)pos;pos+=512+(size_t)padded;
    }
    if (!terminated) goto failed;
    if (pos>capacity-2560) {error=-27;goto failed;}
    memcpy(dst,src,pos);
    make_header(dst+pos,"bootstrap",0700,0,true);pos+=512;
    make_header(dst+pos,"bootstrap/recovery.v1",0600,rn,false);pos+=512;
    memcpy(dst+pos,record,rn);pos+=512;
    *written=pos+1024;return 0;
failed:
    memset(output,0,capacity);return error;
}
