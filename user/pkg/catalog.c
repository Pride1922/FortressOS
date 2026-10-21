#include "catalog.h"
#include "crypto.h"
static uint64_t le(const uint8_t *p,unsigned n) {
    uint64_t v=0; for(unsigned i=0;i<n;i++) v|=(uint64_t)p[i]<<(8*i); return v;
}
static int compare(const uint8_t *a,size_t an,const uint8_t *b,size_t bn) {
    size_t n=an<bn?an:bn;
    for(size_t i=0;i<n;i++) if(a[i]!=b[i]) return a[i]<b[i]?-1:1;
    return an<bn?-1:(an>bn?1:0);
}
static int zero(const uint8_t *p,size_t n) {
    for(size_t i=0;i<n;i++) if(p[i]) return 0;
    return 1;
}
int fpk_catalog_verify(const uint8_t *p,size_t n,const fpk_catalog_policy *policy,fpk_catalog *out) {
    static const uint8_t domain[16]="FORT-PKG-CAT-v1";
    if(!p || !policy || !out || n<72 || n>FPK_CATALOG_MAX ||
       policy->key_count<1 || policy->key_count>8 || !policy->threshold ||
       policy->threshold>policy->key_count || !policy->root_version ||
       zero(policy->repository,16) || policy->now<1767225600ULL || policy->now>4102444800ULL) return -1;
    for(unsigned i=0;i<policy->key_count;i++) {
        if(zero(policy->keys[i],32)) return -1;
        for(unsigned j=0;j<i;j++) if(!compare(policy->keys[i],32,policy->keys[j],32)) return -1;
    }
    unsigned count=(unsigned)le(p+64,2), signatures=(unsigned)le(p+66,2);
    uint64_t revision=le(p+40,8),issued=le(p+48,8),expires=le(p+56,8);
    size_t body=72+81u*count;
    if(compare(p,16,domain,16) || compare(p+16,16,policy->repository,16) ||
       le(p+32,8)!=policy->root_version || !revision || revision<policy->revision_floor ||
       issued<1767225600ULL || issued>policy->now || expires<=policy->now || expires>4102444800ULL ||
       !count || count>256 || !signatures || signatures>8 || !zero(p+68,4) ||
       n!=body+96u*signatures) return -1;
    for(unsigned i=0;i<count;i++) {
        const uint8_t *e=p+72+81*i;
        unsigned len=e[0];
        if(!fpk_identity_valid(e+1,len) || !zero(e+1+len,32-len) ||
           !le(e+33,8) || le(e+41,8)<128 || le(e+41,8)>128+FPK_MANIFEST_MAX+FPK_COMPRESSED_MAX) return -1;
        if(i) {
            const uint8_t *prev=e-81;
            if(compare(prev+1,prev[0],e+1,len)>=0) return -1;
        }
    }
    uint8_t hash[32],id[32];
    if(crypto_sha256(p,n,hash) || (revision==policy->revision_floor && crypto_verify32(hash,policy->floor_sha256))) return -1;
    unsigned valid=0;
    for(unsigned i=0;i<signatures;i++) {
        const uint8_t *s=p+body+96*i;
        if(i && compare(s-96,32,s,32)>=0) return -1;
        unsigned k=0;
        for(;k<policy->key_count;k++) {
            if(crypto_sha256(policy->keys[k],32,id)) return -1;
            if(!crypto_verify32(id,s)) break;
        }
        if(k==policy->key_count || crypto_ed25519_verify(s+32,policy->keys[k],p,body)) return -1;
        valid++;
    }
    if(valid<policy->threshold) return -1;
    out->entries=p+72; out->count=(uint16_t)count; out->revision=revision;
    for(unsigned i=0;i<32;i++) out->sha256[i]=hash[i];
    return 0;
}
int fpk_catalog_find(const fpk_catalog *catalog,const uint8_t *name,size_t n,fpk_catalog_target *out) {
    if(!catalog || !catalog->entries || !catalog->count || catalog->count>256 ||
       !out || !fpk_identity_valid(name,n)) return -1;
    for(unsigned i=0;i<catalog->count;i++) {
        const uint8_t *e=catalog->entries+81*i;
        if(!compare(name,n,e+1,e[0])) {
            out->package=(fpk_span){e+1,e[0]}; out->build=le(e+33,8);
            out->length=le(e+41,8); out->sha256=e+49; return 0;
        }
    }
    return -1;
}
int fpk_verify_authorized(fpk_verify_context *c,const uint8_t *p,size_t n,
                         const fpk_catalog_policy *policy,const uint8_t *name,size_t name_n,
                         fpk_read_fn read,void *reader) {
    if(!c) return -1;
    c->verified=0;
    fpk_catalog catalog; fpk_catalog_target target;
    if(fpk_catalog_verify(p,n,policy,&catalog) || fpk_catalog_find(&catalog,name,name_n,&target) ||
       fpk_verify(c,read,reader,target.length,target.sha256)) return -1;
    if(c->manifest.build!=target.build || compare(c->manifest.package.bytes,c->manifest.package.length,
                                                target.package.bytes,target.package.length)) {
        c->verified=0; return -1;
    }
    return 0;
}
