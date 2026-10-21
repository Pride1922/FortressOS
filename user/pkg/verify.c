#include "verify.h"
#include "../tools/gzip.h"
#include "elf.h"

static int equal(const uint8_t *a,const uint8_t *b,size_t n) {
    for(size_t i=0;i<n;i++) if(a[i]!=b[i]) return 0;
    return 1;
}
static int zero(const uint8_t *p,size_t n) {
    for(size_t i=0;i<n;i++) if(p[i]) return 0;
    return 1;
}
static uint64_t le(const uint8_t *p,unsigned n) {
    uint64_t v=0; for(unsigned i=0;i<n;i++) v|=(uint64_t)p[i]<<(8*i); return v;
}
static int octal(const uint8_t *p,unsigned n,uint64_t *v) {
    /* Canonical producer: n-1 octal digits followed by NUL, or checksum's space. */
    *v=0;
    for(unsigned i=0;i<n-1;i++) {
        if(p[i]<'0' || p[i]>'7') return -1;
        *v=*v*8+(p[i]-'0');
    }
    return p[n-1]? -1:0;
}
static int elf_header(fpk_verify_context *c,uint64_t size) {
    const uint8_t *h=c->elf_header;
    if(!equal(h,(const uint8_t *)"\177ELF\2\1\1",7) || le(h+16,2)!=ET_EXEC ||
       le(h+18,2)!=EM_X86_64 || le(h+20,4)!=1 || le(h+52,2)!=64 ||
       le(h+54,2)!=56 || !le(h+56,2) || le(h+56,2)>MAX_ELF_PHNUM) return -1;
    c->phoff=le(h+32,8);
    if(c->phoff<64 || c->phoff>size || le(h+56,2)*56>size-c->phoff) return -1;
    return 0;
}
static int elf_finish(fpk_verify_context *c,uint64_t size) {
    if(size<64 || elf_header(c,size)) return -1;
    uint64_t pages=0,entry=le(c->elf_header+24,8); unsigned loads=0,entry_ok=0;
    for(unsigned i=0;i<le(c->elf_header+56,2);i++) {
        const uint8_t *p=c->elf_phdrs+56*i;
        uint64_t type=le(p,4), flags=le(p+4,4), offset=le(p+8,8), addr=le(p+16,8);
        uint64_t files=le(p+32,8),mem=le(p+40,8);
        if(type==PT_INTERP || type==PT_DYNAMIC || type==PT_TLS) return -1;
        if(type!=PT_LOAD || !mem) continue;
        if((flags&(PF_W|PF_X))==(PF_W|PF_X) || files>mem || offset>size || files>size-offset ||
           addr<4096 || addr>=USER_SPACE_LIMIT || mem>USER_SPACE_LIMIT-addr ||
           (addr%4096)!=(offset%4096)) return -1;
        uint64_t start=addr&~4095ULL,end=(addr+mem-1)&~4095ULL;
        if((end>=USER_STACK_GUARD_VIRT && start<USER_STACK_TOP_VIRT) ||
           (end>=USER_SIGRESTORER_VIRT && start<USER_SIGRESTORER_VIRT+4096)) return -1;
        for(unsigned j=0;j<i;j++) {
            const uint8_t *q=c->elf_phdrs+56*j;
            if(le(q,4)!=PT_LOAD || !le(q+40,8)) continue;
            uint64_t s=le(q+16,8)&~4095ULL,e=(le(q+16,8)+le(q+40,8)-1)&~4095ULL;
            if(!(end<s || start>e)) return -1;
        }
        pages+=(end-start)/4096+1;
        if(pages>MAX_ELF_PAGES) return -1;
        if((flags&PF_X) && entry>=addr && entry<addr+mem) entry_ok=1;
        loads++;
    }
    return loads && entry_ok ? 0:-1;
}
static int tar_header(fpk_verify_context *c) {
    const uint8_t *p=c->tar;
    if(c->member==c->manifest.file_count) {
        if(!zero(p,512) || ++c->zero_blocks>2) return -1;
        return 0;
    }
    const fpk_file *f=c->manifest.files+c->member;
    uint64_t mode,uid,gid,size,mtime,sum;
    if(!equal(p,f->path.bytes,f->path.length) || !zero(p+f->path.length,100-f->path.length) ||
       octal(p+100,8,&mode) || octal(p+108,8,&uid) || octal(p+116,8,&gid) ||
       octal(p+124,12,&size) || octal(p+136,12,&mtime) ||
       octal(p+148,7,&sum) || p[155]!=' ' || p[156]!='0' ||
       !zero(p+157,100) || !equal(p+257,(const uint8_t *)"ustar\00000",8) ||
       !zero(p+265,247) || mode!=f->mode || uid || gid || mtime || size!=f->size) return -1;
    uint64_t actual=0;
    for(unsigned i=0;i<512;i++) actual+=(i>=148 && i<156)?32:p[i];
    if(sum!=actual) return -1;
    digest_init(&c->member_hash,DIGEST_SHA256); c->member_offset=0; c->phoff=0;
    return 0;
}
static long expanded(void *arg,const void *bytes,size_t n) {
    fpk_verify_context *c=arg; const uint8_t *p=bytes;
    if(n>c->envelope.expanded_bytes-c->expanded) { c->failed=1; return -1; }
    for(size_t i=0;i<n;i++) {
        uint8_t b=p[i]; uint64_t at=c->expanded++;
        /* A nonzero tar_used denotes header assembly. After its completion,
         * absolute block position and member_offset distinguish content/pad. */
        if(c->tar_used<512) {
            c->tar[c->tar_used++]=b;
            if(c->tar_used==512 && tar_header(c)) goto fail;
            if(c->tar_used==512 && c->member==c->manifest.file_count) c->tar_used=0;
        } else {
            const fpk_file *f=c->manifest.files+c->member;
            if(c->member_offset<f->size) {
                if(!digest_update(&c->member_hash,&b,1)) goto fail;
                if(f->role==1) {
                    if(c->member_offset<64) c->elf_header[c->member_offset]=b;
                    if(c->member_offset==63 && elf_header(c,f->size)) goto fail;
                    if(c->member_offset>=64 && c->member_offset>=c->phoff &&
                       c->member_offset-c->phoff<le(c->elf_header+56,2)*56)
                        c->elf_phdrs[c->member_offset-c->phoff]=b;
                }
            } else if(b) goto fail;
            c->member_offset++;
            if(c->member_offset>=f->size && (at+1)%512==0) {
                digest_final(&c->member_hash,c->hash);
                if(!equal(c->hash,f->sha256,32) || (f->role==1 && elf_finish(c,f->size))) goto fail;
                c->member++; c->tar_used=0;
            }
        }
    }
    return (long)n;
fail: c->failed=1; return -1;
}
static long input(fpk_verify_context *c,void *p,size_t n) {
    long r=c->read(c->reader,p,n);
    if(r<0 || (size_t)r>n) { c->failed=1; return -1; }
    if(r>0 && !digest_update(&c->artifact_hash,p,(size_t)r)) { c->failed=1; return -1; }
    return r;
}
static int exact(fpk_verify_context *c,void *bytes,size_t n) {
    uint8_t *p=bytes;
    while(n) { long r=input(c,p,n); if(r<=0) return -1; p+=r; n-=(size_t)r; }
    return 0;
}
static long compressed(void *arg,void *bytes,size_t n) {
    fpk_verify_context *c=arg;
    uint64_t left=c->envelope.compressed_bytes-c->compressed;
    if(n>left) n=(size_t)left;
    if(!n) return 0;
    long r=input(c,bytes,n);
    if(r<=0) { c->failed=1; return -1; }
    const uint8_t *p=bytes;
    for(long i=0;i<r;i++) if(c->compressed+(uint64_t)i==3 && p[i]) { c->failed=1; return -1; }
    c->compressed+=(uint64_t)r;
    if(!digest_update(&c->payload_hash,bytes,(size_t)r)) { c->failed=1; return -1; }
    return r;
}
int fpk_verify(fpk_verify_context *c,fpk_read_fn read,void *arg,uint64_t length,const uint8_t expected[32]) {
    if(!c) return -1;
    c->verified=0; c->failed=0; c->compressed=0; c->expanded=0;
    c->tar_used=0; c->member=0; c->zero_blocks=0; c->read=read; c->reader=arg;
    if(!read || !expected) return -1;
    digest_init(&c->artifact_hash,DIGEST_SHA256);
    if(exact(c,c->header,128) || fpk_parse_envelope(c->header,128,&c->envelope) ||
       c->envelope.container_bytes!=length || exact(c,c->manifest_bytes,c->envelope.manifest_bytes)) return -1;
    digest_init(&c->member_hash,DIGEST_SHA256);
    if(!digest_update(&c->member_hash,c->manifest_bytes,c->envelope.manifest_bytes)) return -1;
    digest_final(&c->member_hash,c->hash);
    if(!equal(c->hash,c->envelope.manifest_sha256,32) ||
       fpk_parse_manifest(c->manifest_bytes,c->envelope.manifest_bytes,&c->manifest) ||
       c->manifest.archive_bytes!=c->envelope.expanded_bytes) return -1;
    digest_init(&c->payload_hash,DIGEST_SHA256);
    if(gzip_decode_callbacks(compressed,expanded,c) || c->failed ||
       c->compressed!=c->envelope.compressed_bytes || c->expanded!=c->envelope.expanded_bytes ||
       c->member!=c->manifest.file_count || c->zero_blocks!=2 || c->tar_used) return -1;
    digest_final(&c->payload_hash,c->hash);
    if(!equal(c->hash,c->envelope.payload_sha256,32)) return -1;
    if(input(c,c->io,1)!=0 || c->failed) return -1;
    digest_final(&c->artifact_hash,c->hash);
    if(!equal(c->hash,expected,32)) return -1;
    c->verified=1; return 0;
}
