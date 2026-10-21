#include "format.h"

typedef struct { const uint8_t *p; size_t n; } cursor;
static const uint8_t *take(cursor *c, size_t n) {
    if (n>c->n) return NULL;
    const uint8_t *p=c->p; c->p+=n; c->n-=n; return p;
}
static uint16_t u16(const uint8_t *p) { return (uint16_t)((unsigned)p[0] | (unsigned)p[1]<<8); }
static uint64_t u64(const uint8_t *p) {
    uint64_t x=0;
    for (unsigned i=0;i<8;i++) x|=(uint64_t)p[i]<<(8*i);
    return x;
}
static int span(cursor *c, fpk_span *s, size_t min, size_t max) {
    const uint8_t *p=take(c,2); if (!p) return -1;
    size_t n=u16(p); if (n<min || n>max) return -1;
    p=take(c,n); if (!p) return -1;
    s->bytes=p; s->length=(uint16_t)n; return 0;
}
static int printable(fpk_span s) {
    for (unsigned i=0;i<s.length;i++) if (s.bytes[i]<32 || s.bytes[i]>126) return 0;
    return 1;
}
static int compare(fpk_span a, fpk_span b) {
    unsigned n=a.length<b.length?a.length:b.length;
    for (unsigned i=0;i<n;i++) if(a.bytes[i]!=b.bytes[i]) return a.bytes[i]<b.bytes[i]?-1:1;
    return a.length<b.length?-1:(a.length>b.length?1:0);
}
static int path_valid(fpk_span s) {
    unsigned start=0;
    for (unsigned i=0;i<=s.length;i++) {
        if(i==s.length || s.bytes[i]=='/') {
            unsigned n=i-start;
            if(!n || n>63 || (n==1 && s.bytes[start]=='.') ||
               (n==2 && s.bytes[start]=='.' && s.bytes[start+1]=='.')) return 0;
            start=i+1;
        } else {
            uint8_t b=s.bytes[i];
            if(b<33 || b>126 || b=='\\' || b==':') return 0;
        }
    }
    return 1;
}
/* Two identical passes avoid a large stack temporary and never publish partial
 * output. Caller must keep input immutable throughout both passes. */
static int parse(const uint8_t *p, size_t n, fpk_manifest *out) {
    cursor c={p,n}; const uint8_t *h=take(&c,64);
    if(!h || h[0]!='F' || h[1]!='M' || h[2]!='N' || h[3]!='1' || u16(h+4)!=1) return -1;
    uint16_t count=u16(h+6);
    uint64_t build=u64(h+8), lo=u64(h+16), hi=u64(h+24);
    if(!count || count>16 || !build || lo>hi) return -1;
    unsigned any=0; for(unsigned i=32;i<64;i++) any|=h[i];
    if(!any) return -1;
    fpk_span name,command,version,description;
    if(span(&c,&name,1,32) || span(&c,&command,1,32) || span(&c,&version,1,32) ||
       span(&c,&description,0,256)) return -1;
    if(!fpk_identity_valid(name.bytes,name.length) || !fpk_identity_valid(command.bytes,command.length) ||
       !printable(version) || !printable(description)) return -1;
    uint64_t archive=1024; unsigned executables=0; fpk_span previous={NULL,0}, paths[16];
    for(unsigned i=0;i<count;i++) {
        const uint8_t *r=take(&c,44); fpk_span path;
        if(!r || span(&c,&path,1,100)) return -1;
        uint64_t size=u64(r+4); uint16_t mode=u16(r+2);
        if(r[1] || !size || size>FPK_EXPANDED_MAX || !path_valid(path) ||
           (i && compare(previous,path)>=0)) return -1;
        if(r[0]==1) {
            if(mode!=0755 || size>4u*1024u*1024u || path.length!=4+command.length ||
               path.bytes[0]!='b' || path.bytes[1]!='i' || path.bytes[2]!='n' || path.bytes[3]!='/') return -1;
            for(unsigned j=0;j<command.length;j++) if(path.bytes[j+4]!=command.bytes[j]) return -1;
            executables++;
        } else if(r[0]==2) {
            if(mode!=0644 || path.length<5 || path.bytes[0]!='d' || path.bytes[1]!='o' ||
               path.bytes[2]!='c' || path.bytes[3]!='/') return -1;
        } else return -1;
        /* Directories are implicit: a regular file cannot be an ancestor. */
        for(unsigned k=0;k<i;k++) if(path.length>paths[k].length && path.bytes[paths[k].length]=='/') {
            unsigned j=0; while(j<paths[k].length && path.bytes[j]==paths[k].bytes[j]) j++;
            if(j==paths[k].length) return -1;
        }
        archive+=512+((size+511)/512)*512;
        if(archive>FPK_EXPANDED_MAX) return -1;
        if(out) out->files[i]=(fpk_file){r[0],mode,size,r+12,path};
        paths[i]=path; previous=path;
    }
    if(c.n || executables!=1) return -1;
    if(out) {
        out->build=build; out->base_min=lo; out->base_max=hi; out->archive_bytes=archive;
        out->platform_id=h+32; out->package=name; out->command=command;
        out->version=version; out->description=description; out->file_count=count;
        for(unsigned i=count;i<16;i++) out->files[i]=(fpk_file){0};
    }
    return 0;
}
int fpk_parse_manifest(const uint8_t *p, size_t n, fpk_manifest *out) {
    if(!p || !out || n>FPK_MANIFEST_MAX || parse(p,n,NULL)) return -1;
    return parse(p,n,out);
}
