#include "pkg_selector_candidate.h"
#include "crypto.h"
#include "string.h"
static uint64_t ps_le(const uint8_t *p,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
static bool ps_zero(const uint8_t *p,size_t n){for(size_t i=0;i<n;i++)if(p[i])return false;return true;}
static bool ps_range(const void *p,size_t n){return p&&n<=UINTPTR_MAX-(uintptr_t)p;}
static bool ps_apart(const void *a,size_t an,const void *b,size_t bn){return ps_range(a,an)&&ps_range(b,bn)&&((uintptr_t)a+an<=(uintptr_t)b||(uintptr_t)b+bn<=(uintptr_t)a);}
static bool ps_hash(const uint8_t *p,size_t n,const uint8_t *expected){uint8_t h[32];return !crypto_sha256(p,n,h)&&!crypto_verify32(h,expected);}
static bool ps_keys(const uint8_t keys[8][32],unsigned count,unsigned threshold){
 if(!count||count>8||!threshold||threshold>count)return false;
 for(unsigned i=0;i<count;i++){if(ps_zero(keys[i],32))return false;for(unsigned j=0;j<i;j++)if(!memcmp(keys[i],keys[j],32))return false;}
 for(unsigned i=count;i<8;i++)if(!ps_zero(keys[i],32))return false;
 return true;
}
static int ps_auth(const uint8_t *s,size_t n,const ps_policy *p){
 static const uint8_t domain[16]="FORT-PKG-SEL-v1";
 if(n<PS_BODY||n>PS_MAX||memcmp(s,domain,16)||ps_le(s+16,4)!=n||ps_le(s+20,2)!=1||ps_le(s+22,2)!=0||
    ps_le(s+480,4)!=0x4c455354u||ps_le(s+488,4)!=1||!ps_zero(s+486,2)||!ps_zero(s+492,276))return -1;
 unsigned count=(unsigned)ps_le(s+484,2);uint64_t epoch=ps_le(s+40,8),issued=ps_le(s+24,8),expires=ps_le(s+32,8);
 if(!count||count>8||n!=PS_BODY+96*count||!epoch||epoch==UINT64_MAX||epoch<p->epoch_floor||
    p->now<1767225600ULL||p->now>4102444800ULL||issued<1767225600ULL||expires>4102444800ULL||issued>p->now||expires<=p->now||issued>=expires||
    ps_le(s+48,4)!=2||ps_le(s+52,4)!=2||ps_le(s+56,8)!=p->root_version||
    ps_le(s+64,8)!=p->catalog_revision_floor||ps_le(s+72,8)!=p->base_version||!ps_zero(s+80,16)||
    memcmp(s+96,p->uuid,80)||memcmp(s+176,p->profile,32)||memcmp(s+304,p->root_hash,32)||
    memcmp(s+336,p->catalog_hash,32)||memcmp(s+368,p->platform,32)||memcmp(s+400,p->kernel,32)||
    memcmp(s+432,p->bootstrap,32)||ps_zero(s+464,16))return -1;
 for(unsigned i=0;i<5;i++)if(ps_zero(p->uuid[i],16))return -1;
 for(unsigned i=0;i<9;i++)if(ps_zero(s+176+32*i,32))return -1;
 if(!ps_keys(p->selector_keys,p->selector_count,p->selector_threshold)||
    !ps_keys(p->enrollment_keys,p->enrollment_count,p->enrollment_threshold)||
    !ps_keys(p->catalog_keys,p->catalog_count,p->catalog_threshold)||!p->epoch_floor||!p->root_version||!p->catalog_revision_floor||!p->base_version)return -1;
 /* Roles must have disjoint key sets; a catalog/enrollment key is not selector
  * authority even if someone changes the envelope's role bytes. */
 for(unsigned i=0;i<p->selector_count;i++){
  for(unsigned j=0;j<p->enrollment_count;j++)if(!memcmp(p->selector_keys[i],p->enrollment_keys[j],32))return -1;
  for(unsigned j=0;j<p->catalog_count;j++)if(!memcmp(p->selector_keys[i],p->catalog_keys[j],32))return -1;
 }
 for(unsigned i=0;i<p->enrollment_count;i++)for(unsigned j=0;j<p->catalog_count;j++)if(!memcmp(p->enrollment_keys[i],p->catalog_keys[j],32))return -1;
 if(epoch==p->epoch_floor&&!ps_hash(s,n,p->selector_floor_hash))return -1;
 for(unsigned i=0;i<count;i++){
  const uint8_t *row=s+PS_BODY+96*i;if(i&&memcmp(row-96,row,32)>=0)return -1;
  unsigned key=0;for(;key<p->selector_count;key++)if(ps_hash(p->selector_keys[key],32,row))break;
  if(key==p->selector_count||crypto_ed25519_verify(row+32,p->selector_keys[key],s,PS_BODY))return -1;
 }
 return count>=p->selector_threshold?0:-1;
}
static int ps_unpack(const uint8_t *b,size_t n,ps_workspace *w){
 static const uint8_t domain[16]="FORT-PKG-BND-v1";
 static const unsigned sizes[8]={0,512,1024,8192,1024,1024,1024,1024};
 if(n<96||n>PS_BUNDLE_MAX||memcmp(b,domain,16)||ps_le(b+16,4)!=n||ps_le(b+20,2)!=1||ps_le(b+22,2)!=8||!ps_zero(b+24,8))return -1;
 size_t at=96;
 for(unsigned i=0;i<8;i++){
  const uint8_t *row=b+32+8*i;size_t bytes=(size_t)ps_le(row,4);
  if(ps_le(row+4,2)!=i+1||ps_le(row+6,2)||bytes>n-at||
     (i?bytes!=sizes[i]:bytes<PE_BODY||bytes>PE_MAX))return -1;
  w->parts[i]=(ps_blob){b+at,bytes};at+=bytes;
 }
 return at==n?0:-1;
}
static int ps_records(const uint8_t *s,const ps_policy *p,ps_workspace *w){
 const uint8_t *en=w->parts[0].p,*a=w->parts[4].p,*o=w->parts[5].p,*trust=w->parts[7].p;
 pe_policy *ep=&w->enrollment_policy;memset(ep,0,sizeof(*ep));memcpy(ep->keys,p->enrollment_keys,256);memcpy(ep->uuid,p->uuid,80);
 memcpy(ep->descriptor_sha256,s+240,32);ep->key_count=p->enrollment_count;ep->threshold=p->enrollment_threshold;ep->epoch=ps_le(s+40,8);
 if(pe_verify(en,w->parts[0].n,ep,&w->enrollment)||memcmp(en+152,p->profile,32)||memcmp(en+184,p->kernel,32)||memcmp(en+216,p->bootstrap,32)||
    !ps_hash(w->parts[1].p,512,s+208)||!ps_hash(trust,1024,en+248))return -1;
 /* Enrollment starts at the exact empty account operation, not a pending redo
  * or a convenient checksummed pair. Database contents/maps remain an adapter
  * obligation; this pure reader binds the signed prepared baseline only. */
 if(memcmp(a,"FORT-ACC-GEN-v1\0",16)||ps_le(a+16,4)!=1024||ps_le(a+20,2)!=3||ps_le(a+22,2)||ps_le(a+24,8)!=1||ps_le(a+32,8)!=1||
    ps_le(a+40,8)!=ep->epoch||memcmp(a+48,p->uuid[4],16)||memcmp(a+64,p->uuid[0],16)||memcmp(a+80,en+312,32)||
    ps_le(a+144,8)!=1||!ps_zero(a+152,176)||!ps_hash(a,992,a+992)||!ps_hash(o,1024,a+112))return -1;
 for(unsigned i=0;i<3;i++)if(ps_zero(a+328+32*i,32)||!ps_le(a+424+4*i,4)||ps_le(a+424+4*i,4)>8192)return -1;
 pe_origin origin;if(pe_origin_decode(o,1024,&w->enrollment,1,1,&origin)||origin.active)return -1;
 fpk_catalog_policy *cp=&w->catalog_policy;memset(cp,0,sizeof(*cp));memcpy(cp->repository,p->uuid[3],16);memcpy(cp->keys,p->catalog_keys,256);
 cp->key_count=p->catalog_count;cp->threshold=p->catalog_threshold;cp->root_version=p->root_version;cp->revision_floor=p->catalog_revision_floor;cp->now=p->now;memcpy(cp->floor_sha256,p->catalog_hash,32);
 size_t cn=(size_t)ps_le(trust+96,4);fpk_catalog cat;
 if(cn>736||fpk_catalog_verify(trust+256,cn,cp,&cat)||cat.revision!=p->catalog_revision_floor)return -1;
 pm_input in={.marker=w->parts[1].p,.ledger=w->parts[2].p,.owners=w->parts[3].p,.state=w->parts[6].p,.trust=trust,
   .account=a,.enrollment=&w->enrollment,.policy=cp,.base_version=p->base_version,.platform=p->platform};
 if(pm_validate(&in,&w->mount,&w->view)||w->view.grant.phase!=PG_IDLE||w->view.grant.sequence!=1||w->view.state_generation!=1||w->view.installed)return -1;
 return 0;
}
int ps_validate(const uint8_t *s,size_t sn,const uint8_t *b,size_t bn,const ps_policy *p,ps_workspace *w,ps_result *out){
 if(sn<PS_BODY||sn>PS_MAX||bn<96||bn>PS_BUNDLE_MAX||!ps_range(s,sn)||!ps_range(b,bn)||!ps_range(p,sizeof(*p))||!ps_range(w,sizeof(*w))||!ps_range(out,sizeof(*out))||
    !ps_apart(w,sizeof(*w),s,sn)||!ps_apart(w,sizeof(*w),b,bn)||!ps_apart(w,sizeof(*w),p,sizeof(*p))||
    !ps_apart(w,sizeof(*w),out,sizeof(*out))||!ps_apart(out,sizeof(*out),s,sn)||!ps_apart(out,sizeof(*out),b,bn)||!ps_apart(out,sizeof(*out),p,sizeof(*p)))return -1;
 if(ps_auth(s,sn,p)||!ps_hash(b,bn,s+272)||ps_unpack(b,bn,w)||ps_records(s,p,w))return -1;
 ps_result value={.epoch=ps_le(s+40,8)};
 if(crypto_sha256(s,sn,value.selector_sha256)||crypto_sha256(b,bn,value.bundle_sha256))return -1;
 *out=value;return 0;
}
unsigned ps_admission(bool required,unsigned artifacts,bool valid){
 if(artifacts==PS_ABSENT)return required?PS_REFUSE:PS_LEGACY;
 return artifacts==PS_PRESENT&&required&&valid?PS_PROTECTED:PS_REFUSE;
}
